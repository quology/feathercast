"""Decode the source_audio_smoke two-tone recording and verify track isolation."""
import argparse
import json
import math
from array import array
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--routing", action="store_true", help="Check routing.mkv: isolated 440/880 Hz tracks and a shared third track")
    args = parser.parse_args()
    probe = json.loads(subprocess.check_output([args.ffprobe, "-v", "error", "-show_streams", "-of", "json", str(args.media)]))
    streams = [s for s in probe["streams"] if s["codec_type"] == "audio"]
    expected_tracks = [{440}, {880}, {440, 880}] if args.routing else [{440, 880}, {440}]
    if len(streams) != len(expected_tracks):
        raise SystemExit(f"Expected {len(expected_tracks)} audio streams, got {len(streams)}")
    result = {"file": args.media.name, "tracks": []}
    for index, stream in enumerate(streams):
        pcm = subprocess.check_output([args.ffmpeg, "-nostdin", "-v", "error", "-ss", "1", "-i", str(args.media), "-map", f"0:a:{index}", "-t", "2", "-ac", "1", "-ar", "48000", "-f", "f32le", "-"])
        samples = array("f")
        samples.frombytes(pcm)
        if sys.byteorder != "little":
            samples.byteswap()
        if len(samples) < 48000:
            raise SystemExit("Insufficient decoded audio")
        # Measure each known fixture tone using a Hann-windowed projection.
        amplitudes = {}
        for frequency in (440, 880):
            real = imaginary = 0.0
            for n, sample in enumerate(samples):
                weighted = sample * (0.5 - 0.5 * math.cos(2 * math.pi * n / (len(samples) - 1)))
                phase = 2 * math.pi * frequency * n / 48000
                real += weighted * math.cos(phase)
                imaginary += weighted * math.sin(phase)
            amplitudes[str(frequency)] = math.hypot(real, imaginary)
        ratio = 20 * math.log10(max(amplitudes["880"], 1e-12) / max(amplitudes["440"], 1e-12))
        expected = expected_tracks[index]
        result["tracks"].append({"audio_index": index, "codec": stream["codec_name"], "expected_hz": sorted(expected), "amplitudes": amplitudes, "880_relative_to_440_db": ratio})
        if stream["codec_name"] != "aac" or any(amplitudes[str(hz)] < 1 for hz in expected):
            raise SystemExit(f"Expected AAC and tones {sorted(expected)} on track {index + 1}")
        if expected == {440, 880} and not -12 < ratio < 12:
            raise SystemExit("Shared track must contain both tones at comparable levels")
        if (expected == {440} and ratio > -40) or (expected == {880} and ratio < 40):
            raise SystemExit(f"Isolated track {index + 1} leaks the other tone above -40 dB")
    result["result"] = "passed"
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
