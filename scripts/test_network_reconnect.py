"""Interrupt a loopback RTMP receiver and verify the real publisher reconnects."""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--encoder", default="obs_x264")
    parser.add_argument("--port", type=int, default=19358)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise SystemExit("Use a fresh output directory to preserve previous evidence")
    output.mkdir(parents=True)
    url = f"rtmp://127.0.0.1:{args.port}/live/test"
    children, streams = [], []

    def receiver(name):
        log = (output / f"{name}.log").open("w", encoding="utf-8")
        streams.append(log)
        process = subprocess.Popen([str(args.ffmpeg.resolve()), "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-listen", "1", "-i", url, "-c", "copy", str(output / name)], stdout=log, stderr=log, creationflags=subprocess.CREATE_NO_WINDOW)
        children.append(process)
        return process

    try:
        first = receiver("interrupted-receiver.mkv")
        time.sleep(0.7)
        engine_log = (output / "engine.log").open("w", encoding="utf-8")
        streams.append(engine_log)
        engine = subprocess.Popen([str(args.engine.resolve()), str(args.runtime.resolve()), str(output), url, args.encoder], stdout=engine_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        children.append(engine)
        time.sleep(3.7)
        first.terminate()
        first.wait(timeout=5)
        # This receiver file is deliberately interrupted and is not a media
        # integrity assertion. The finalized recording/replay/second receiver are.
        partial = output / "interrupted-receiver.mkv"
        if partial.exists():
            partial.rename(output / "interrupted-receiver.partial")
        time.sleep(0.8)
        second = receiver("received-after-reconnect.mkv")
        engine_exit = engine.wait(timeout=35)
        try:
            second.wait(timeout=8)
        except subprocess.TimeoutExpired:
            second.terminate()
            second.wait(timeout=5)
        engine_log.flush()
        text = (output / "engine.log").read_text(encoding="utf-8", errors="replace")
        stats = [json.loads(line.removeprefix("STATS ")) for line in text.splitlines() if line.startswith("STATS ")]
        result = {"engine_exit": engine_exit, "stats": stats[-1] if stats else None, "receiver_failure_injected": True, "passed": bool(engine_exit == 0 and stats and stats[-1].get("reconnects", 0) >= 1 and (output / "received-after-reconnect.mkv").stat().st_size > 0)}
        (output / "reconnect-result.json").write_bytes((json.dumps(result, indent=2) + "\n").encode())
        print(json.dumps(result, indent=2))
        if not result["passed"]:
            raise SystemExit(1)
    finally:
        for process in children:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        for stream in streams:
            stream.close()


if __name__ == "__main__":
    main()
