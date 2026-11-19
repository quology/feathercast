# FeatherCast

FeatherCast is a native Windows broadcasting application using C++20, Qt 6 and standalone libobs. No running OBS Studio instance is required. This private **0.1.0-dev.1 development preview** is not stable v1 or full OBS feature parity.

## Implemented features

- Dockable scenes/sources, real capture/device options, volume/mute and actual libobs audio peak meters.
- Optional application-only audio for display/window/game capture. Advanced Audio Properties controls gain, mono, balance, sync, monitoring and six independent recording tracks.
- Shared H.264/AAC encoders for independent streaming, MKV recording and replay; simultaneous outputs tested separately with x264 and RTX 4070 NVENC.
- Live 0–300 second packet delay with bounded memory and keyframe cuts. Increasing delay holds output and can time out ingest; decreasing it intentionally removes content. Effective delay is approximate.
- Visual priority automation with OR-of-AND conditions, ordered actions, debounce, cooldown, manual override, fallback and transactional JSON import/export. Process snapshots poll every 500 ms.
- Global Windows hotkeys, settings/layout persistence, dark/light themes, module gates and separately DPAPI-protected stream keys.
- Asynchronous MKV-to-MP4 remux without re-encoding; native plugin ABI v1 custom actions and a working 90-second delay example.

See [feature status](docs/FEATURE_STATUS.md), [user guide](docs/USER_GUIDE.md), [test results](TEST_RESULTS.md) and [release notes](docs/RELEASE_NOTES.md).

## Build and run

Target Windows 11 x64; Qt 6.8.3, MinGW 13.1 and official OBS 33.0.0 C ABI runtime with D3D11. Official OBS DLLs may require the Microsoft Visual C++ runtime. Follow [BUILD.md](BUILD.md) for pinned dependencies, Debug/Release builds and packaging.

Extract the entire portable ZIP and run `FeatherCast.exe`; keep runtime/data directories and helpers together. The package passed UI and media tests without development SDK paths. First launch opens a safe Color scene. Capture and outputs start only after explicit actions. Configure sources, output directory and stream endpoint/key before starting outputs.

## Limits and license

Outputs share resolution and video quality. Streaming and replay use stereo AAC track 1. MKV recordings support six assignable stereo AAC tracks. Application audio captures the app, not an individual tab, and actual Windows application isolation remains unverified. Recording splitting, live disk-pressure protection, disk-backed delay, broad plugin extension types and plugin isolation are absent. External platforms, visual capture/preview, protected games, AMF/QSV hardware and comparable gaming performance remain unverified. CPU/GPU utilization is unavailable.

FeatherCast is GPL-3.0-or-later. OBS, Qt, FFmpeg and other dependencies retain their licenses; preserve supplied notices and source URLs. A complete public redistribution/license/corresponding-source audit remains pending. Source is in private `quology/feathercast`; no public stable redistribution is claimed.
