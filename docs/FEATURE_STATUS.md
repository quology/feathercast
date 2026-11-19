# Feature status — 19 November 2026

Implemented features and environment-specific verification are distinct.

| Area | Implemented / verified | Limits |
|---|---|---|
| Engine | Native C++20, Qt 6.8.3/MinGW 13.1, OBS 33 C ABI, D3D11 | Windows 11 x64; selected non-frontend OBS modules only |
| UI/capture | Dockable scenes/sources, real options, safe Color startup | Actual desktop/window/game visual capture and protected games unverified |
| Preview | Native HWND display and gate | QWidget screenshots exclude HWND; visibility unverified |
| Audio | Advanced Audio Properties, live peaks, application-only capture and six assignable MKV mixers | Streaming/replay use track 1. App capture is process-level. Actual Windows application isolation and acoustic monitoring remain unverified. |
| Microphone filters | Native libobs compressor, gate and Speex/RNNoise suppression; live themed controls, bypass, ordering and persistence | Optional module/method availability is detected; unavailable saved rows remain dormant. No real microphone listening or noise-quality comparison claimed. |
| Outputs | Shared H.264/AAC pair; x264 and RTX 4070 NVENC simultaneous streaming/record/replay tested | Independent resolution/quality profiles absent; AMF/QSV unverified |
| Delay | 0–300 s target; default 256 MiB payload-capacity and 100,000 packet caps; live changes tested | No disk spill; cuts lose content, holds can time out; cap is not total RSS |
| Replay | Time/memory settings and 15/30/60/120/300 s presets | Uses OBS replay implementation, distinct from delay accounting |
| Recording/remux | MKV; asynchronous MP4 stream copy, temp then rename; decode/preservation verified | No splitting; disk check before start only; cancellation cleanup inspected, not runtime-tested |
| Automation | Visual OR-of-AND editor, typed conditions/actions, priority, ordered actions, timing, override, fallback, import/export | Process snapshots polled at 500 ms; external flags need integration; examples disabled |
| Hotkeys | Registration, conflict handling and native callback tests passed | Local WM_HOTKEY dispatch does not prove physical chord from another foreground app |
| Persistence | Atomic QSaveFile JSON, 30 s autosave, layout/theme/source state; invalid config preserves scenes | DPAPI keys separate, custom config directory has isolated namespace |
| Plugins | v1 custom actions and working 90 s delay example; module gates | No arbitrary sources/panels/conditions/hotkeys/outputs in v1; trusted in-process DLLs can crash; no Qt frontend compatibility |
| Network | Local RTMP and receiver-stop/restart reconnect verified | RTMPS tls_verify=1 applied, external TLS/platform session untested |
| Diagnostics | Media validation, UI self-test and short matched idle CPU/RSS sample | UI utilization unavailable; no rigorous gaming FPS/1% low/frame-time comparison |
| Delivery | Debug/Release tests, packaged UI/media verification and final-code CI passed | Portable development preview; no installer/stable-v1 claim; public license/source audit incomplete |

See [test results](../TEST_RESULTS.md) for evidence.
