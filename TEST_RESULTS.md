# Verified test results

Updated: 18 November 2026. Backend tests below were performed on 17 November unless stated otherwise. System: Windows x64, AMD Ryzen 7 9800X3D, NVIDIA GeForce RTX
4070. This record lists actual completed checks; pending items have not passed.

| Check | Result | Evidence |
|---|---|---|
| Core GCC 13.1/C++20 build | Passed | Current Release CTest core target; initial manual core build also used warnings as errors |
| Bounded delay, keyframe cuts, A/V timestamp rebasing and pacing | Passed | Core delay scenarios including randomized timestamp checks |
| Memory/packet bounds, invalid inputs and reconnect state | Passed | Core failure scenarios |
| Concurrent producer/consumer queue access | Passed | Core concurrency scenario |
| Automation priority, logic, debounce/cooldown, override and fallback | Passed | Core automation scenarios |
| Module dependency rollback and failed-stop handling | Passed | Core lifecycle scenarios |
| Example plugin ABI and DLL export | Passed | Core plugin scenario and `fc_plugin_query` export inspection |
| Total portable core tests | 13/13 passed | Independently rerun by coordinator; added timer-jitter pacing regression |
| MinGW C++ executable linked to official libobs runtime | Passed | Native executable returned `33.0.0` |
| Official OBS runtime checksum | Passed | SHA-256 `3369a3ecd67b5669a8a08d77357f87cc786d1fba42fbbd1054d841a69f6271a7` |
| CMake portable core build and CTest | Passed | `build/CoreOnly`, CTest 1/1; 13 internal scenarios |
| Native plugin DLL loading and ABI mismatch | Passed | DLL accepted ABI 1 and rejected ABI 999 |
| Simultaneous recording, replay and packet relay (software H.264) | Passed | Real synthetic video/tone, recording/replay/FLV artifacts decoded |
| Shared NVIDIA NVENC outputs | Passed | RTX 4070; `work/engine-nvenc.log`, relay error 0 |
| Local RTMP streaming while recording and replay run | Passed | `work/engine-rtmp.log`; FFmpeg receiver captured decodable MKV |
| Live delay increase and keyframe decrease | Passed for synthetic local test | 0.5 s -> 1.8 s -> 0 target; effective final delay about 0.8 s with 1 s keyframes |
| Actual media decode, strict per-stream DTS and A/V end alignment | Passed | Coordinator independently validated six revised NVENC/RTMP files; end skew below 250 ms threshold |
| Shutdown lifetime handling | Passed strengthened final smoke | OBS 33 canvas-reference cleanup fixed; final2 log has no error, remaining source or double destruction |
| Qt GUI Debug build/timed smoke | Passed 18 November | Clean timed launch/exit; final package pending |
| Equivalent OBS performance comparison and gaming FPS | Pending | No improvement claim |

Core tests do not prove real encoder, network, viewer, disk-failure or GPU behavior.
No Twitch/YouTube credentials were requested or used. No live public broadcast has
been performed. Automated media checks use synthetic scenes/audio where possible.

The native media tests above separately exercise real encoders and a local network
receiver. They do not establish perceptual lip sync, RTMPS platform acceptance,
reconnect recovery under all failures, full-disk safety, protected-game capture,
or long-duration stability. AAC priming, cumulative pacing drift, and an intermittent
OBS 33 canvas-reference shutdown race were corrected. The final strengthened smoke
and its three decoded media artifacts passed after that ownership fix.

## Application and final verification, 18 November 2026

| Check | Result | Evidence and boundary |
|---|---|---|
| Debug and Release builds and CTest | Passed | 3/3 targets: 13 internal core cases, automation JSON and isolated credential storage |
| Application DPAPI storage and exports | Passed | Real Engine storage/readback, fresh instance, encrypted UTF-8/UTF-16 plaintext exclusion, key-free JSON export and empty-key clearing; no capture or user vault used |
| Automation JSON | Passed | All fields/action enums roundtrip; 10 malformed documents rejected transactionally |
| Debug UI self-test | Passed all 12 Boolean checks | work/final-verification/results.json: serialization, invalid rule/engine config preservation, real scene/delay/ordered actions/no-repeat, override, hotkey registration/conflict/dispatch, real audio meter |
| Audio meter | Passed | Explicit 440 Hz tone: -21.9382 dBFS |
| Hotkey event dispatch | Passed local native event test | WM_HOTKEY posted locally; physical chords from another foreground app unverified |
| MP4 remux | Passed | work/final-verification/remux-results.json: full decode, original/existing destination preserved, invalid-input output/temp cleanup |
| Remux cancellation | Inspected only | Runtime cancellation not exercised |
| Local RTMP receiver stop/restart | Passed | work/reconnect-verified-20261118/reconnect-result.json: exit 0, reconnects 1, relayError 0, connected true, effective 873.676 ms at target zero |
| Reconnect media | Passed | Same directory media-validation.json: recording/replay/recovered receiver MKV fully decoded, strict DTS/end-skew checks passed |
| Baseline CI 964815a | Passed | [Run 44145255499](https://github.com/quology/feathercast/actions/runs/44145255499): Linux core, Windows Debug/Release, portable package; predates latest macro/remux changes |
| Latest Release UI self-test | Passed, exit 0, all 12 Boolean checks | work/release-verified-20261118/results.json and stdout.log; real meter -21.9382 dBFS, native event dispatch, clean shutdown without double destruction |
| Final-source portable package smoke | Pending coordinator update | Baseline CI package does not validate latest executable |
| Final-source CI | Pending coordinator update | Latest changes require synchronization |

RTMPS tls_verify=1 is applied; no external RTMPS/Twitch/YouTube session was tested. AMF/QSV hardware, actual desktop/window/game visual capture, protected games, perceptual lip sync and long-duration stability remain unverified. Native preview is implemented, but QWidget grabs exclude its HWND and do not prove visibility.

No rigorous comparable OBS gaming FPS/1% low/frame-time measurements exist; incomplete preliminary synthetic measurements are not published. Disk checking is pre-start only, without a live full-disk guarantee. CPU/GPU counters remain unavailable. Public redistribution/license/corresponding-source audit remains incomplete. See [feature status](docs/FEATURE_STATUS.md).
