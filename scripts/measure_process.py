"""Read process CPU/RAM and Windows GPU engine counters without changing its workload."""
import argparse
import datetime
import json
import os
from pathlib import Path
import statistics
import subprocess
import time
import psutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--seconds", type=int, default=12)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not 2 <= args.seconds <= 300:
        raise SystemExit("Measurement duration must be between 2 and 300 seconds")
    process = psutil.Process(args.pid)
    # A separate read-only GPU counter sampler runs during the same interval.
    # English Windows counter names may be unavailable on a localized host.
    gpu = None
    if os.name == "nt":
        ps = "$ErrorActionPreference='Stop'; $rows=Get-Counter -Counter '\\GPU Engine(*)\\Utilization Percentage' -SampleInterval 1 -MaxSamples " + str(args.seconds) + "; $rows.CounterSamples | Where-Object { $_.InstanceName -like 'pid_" + str(args.pid) + "_*' } | Select-Object InstanceName,CookedValue | ConvertTo-Json -Compress"
        gpu = subprocess.Popen(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", ps], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
    process.cpu_percent(None)
    samples = []
    start = time.monotonic()
    while time.monotonic() - start < args.seconds and process.is_running():
        time.sleep(0.5)
        try:
            samples.append({"elapsed_seconds": round(time.monotonic() - start, 3), "cpu_percent_one_core_scale": process.cpu_percent(None), "rss_bytes": process.memory_info().rss, "threads": process.num_threads()})
        except psutil.NoSuchProcess:
            break
    gpu_values, gpu_error = [], None
    if gpu:
        try:
            stdout, stderr = gpu.communicate(timeout=10)
            if gpu.returncode == 0 and stdout.strip():
                result = json.loads(stdout)
                gpu_values = result if isinstance(result, list) else [result]
            else:
                gpu_error = "GPU performance counters unavailable; no utilization claim"
        except (subprocess.TimeoutExpired, json.JSONDecodeError):
            gpu.kill()
            gpu.communicate()
            gpu_error = "GPU counter sampling failed; no utilization claim"
    if not samples:
        raise SystemExit("No process samples were collected")
    result = {"label": args.label, "measured_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "pid": args.pid, "duration_seconds": round(time.monotonic() - start, 3), "logical_cpus": psutil.cpu_count(), "cpu_percent_one_core_scale_mean": statistics.mean(s["cpu_percent_one_core_scale"] for s in samples), "cpu_percent_machine_scale_mean": statistics.mean(s["cpu_percent_one_core_scale"] for s in samples) / psutil.cpu_count(), "rss_peak_bytes": max(s["rss_bytes"] for s in samples), "samples": samples, "gpu_engine_samples": gpu_values, "gpu_error": gpu_error, "limitations": "Observational process-only data. Children, GPU VRAM, gaming FPS, frame-time percentiles and equivalent OBS workload are not inferred. CPU 100% on one-core scale means one fully occupied logical core."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: result[k] for k in ("label", "cpu_percent_machine_scale_mean", "rss_peak_bytes", "gpu_error")}, indent=2))


if __name__ == "__main__":
    main()
