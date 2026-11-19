"""Decode real media and check packet timestamps; never substitutes for listening/viewing."""
import argparse
import json
from pathlib import Path
import subprocess


def inspect(media, probe, decoder):
    data = json.loads(subprocess.check_output([str(probe), "-v", "error", "-show_streams", "-show_format", "-show_packets", "-show_entries", "format=duration,size:stream=index,codec_name,codec_type,width,height,sample_rate:packet=stream_index,dts_time,pts_time,duration_time,flags", "-of", "json", str(media)], text=True))
    streams = {stream["index"]: stream for stream in data["streams"]}
    by_stream = {}
    for packet in data.get("packets", []):
        by_stream.setdefault(packet["stream_index"], []).append(packet)
    if not any(s["codec_type"] == "video" for s in streams.values()):
        raise ValueError("Missing video stream")
    if not any(s["codec_type"] == "audio" for s in streams.values()):
        raise ValueError("Missing audio stream")
    summaries = []
    ends = {}
    for index, stream in streams.items():
        packets = by_stream.get(index, [])
        if not packets:
            raise ValueError(f"Stream {index} has no packets")
        dts = [float(p["dts_time"]) for p in packets]
        if not all(a < b for a, b in zip(dts, dts[1:])):
            raise ValueError(f"Non-increasing DTS in stream {index}")
        if stream["codec_type"] == "video" and "K" not in packets[0]["flags"]:
            raise ValueError("Video does not begin with a keyframe")
        ends[index] = float(packets[-1]["pts_time"]) + float(packets[-1].get("duration_time", 0))
        summaries.append({**stream, "packet_count": len(packets), "first_dts_seconds": dts[0], "last_dts_seconds": dts[-1], "strictly_increasing_dts": True})
    skew = max(abs(ends[v] - ends[a]) for v, video in streams.items() if video["codec_type"] == "video"
               for a, audio in streams.items() if audio["codec_type"] == "audio")
    if skew > 0.25:
        raise ValueError(f"Audio/video end skew exceeds 250 ms: {skew}")
    result = subprocess.run([str(decoder), "-nostdin", "-v", "error", "-xerror", "-i", str(media), "-map", "0:v", "-map", "0:a", "-f", "null", "-"], capture_output=True, text=True)
    if result.returncode:
        raise ValueError("Full decode failed: " + result.stderr[:2000])
    return {"file": media.name, "bytes": media.stat().st_size, "format": data.get("format"), "streams": summaries, "audio_video_end_skew_seconds": skew, "full_decode": "passed", "result": "passed"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffprobe", required=True, type=Path)
    parser.add_argument("--ffmpeg", required=True, type=Path)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    reports = []
    for media in sorted(args.directory.iterdir()):
        if media.suffix.lower() not in (".mkv", ".mp4", ".flv"):
            continue
        try:
            report = inspect(media, args.ffprobe, args.ffmpeg)
        except (ValueError, KeyError, subprocess.SubprocessError) as error:
            report = {"file": media.name, "result": "failed", "error": str(error)}
        reports.append(report)
        print(media.name, report["result"], flush=True)
    if not reports:
        raise SystemExit("No media files were found")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({"validation": reports, "limitations": "Packet/end-time checks and decoding do not establish perceptual lip sync or platform playback behavior."}, indent=2) + "\n", encoding="utf-8")
    if any(r["result"] != "passed" for r in reports):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
