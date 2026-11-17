# Verified test results

Date: 17 November 2026. System: Windows x64, AMD Ryzen 7 9800X3D, NVIDIA GeForce RTX
4070. This record lists actual completed checks; pending items have not passed.

| Check | Result | Evidence |
|---|---|---|
| Core GCC 13.1/C++20 build with warnings treated as errors | Passed | `work/core_tests.exe` compiled from current core sources |
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
| Qt GUI and packaged executable | Pending | Interface stage follows adapter validation |
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
