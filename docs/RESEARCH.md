# Research record

Research date: 2026-11-17. Architectural decisions below are independent implementation choices; external capabilities do not imply FeatherCast already implements them. OBS application integration is pinned to official tag `33.0.0`, commit `02acc6a4cf08eba4ce2eab9342c5dd305b41e9e5` (coordinator-provisioned source), rather than changing `master` documentation.

## OBS Studio/libobs

OBS modules implement sources, encoders, outputs and services; this lets a separate frontend own libobs without launching OBS Studio. The source tree has Windows D3D11 and WinRT components. We reuse capture/render/audio/encode infrastructure, not duplicate it. [OBS plugin documentation](https://docs.obsproject.com/plugins), [official source](https://github.com/obsproject/obs-studio).

The output API accepts encoded packets through `encoded_packet`, delivered in monotonic timestamp order. Outputs attach video/audio encoders. Null packets report encoding failure. This is the correct boundary for a bounded packet relay; blocking network operations must move off that callback. [Output API](https://docs.obsproject.com/reference-outputs).

OBS's FFmpeg mux implementation includes a packet-based replay buffer and output lifecycle. Prefer its existing recording/replay implementations where compatible. Separate output references can share compatible encoders, but this must be verified with the chosen combination. [Pinned mux/replay source](https://github.com/obsproject/obs-studio/blob/33.0.0/plugins/obs-ffmpeg/obs-ffmpeg-mux.c).

Pinned delay source stores configured delay separately from active buffering state. Merely setting delay while active does not implement the required cut/rebase policy. FeatherCast therefore implements its own queue and requires an actual output transport adapter. [Pinned OBS delay source](https://github.com/obsproject/obs-studio/blob/33.0.0/libobs/obs-output-delay.c).

The current official Windows build guide uses Visual Studio/MSVC and CMake presets. FeatherCast's MinGW Qt frontend is a pragmatic local toolchain choice; it needs a validated C ABI boundary and selected compatible OBS modules. It does not establish general binary compatibility with third-party Qt frontend plugins. [Official Windows build guide](https://github.com/obsproject/obs-studio/wiki/Build-Instructions-For-Windows).

## Advanced Scene Switcher

The project documents macros as condition sets followed by action lists. Public locale definitions expose process/focus/title, audio, stream, recording, timers, hotkeys, AND/OR operations, action trigger modes and import/export. We implement a smaller independent controller with explicit numeric priority, deterministic ties, debounce, cooldown and manual override. We do not reuse its UI or promise its macro file format. [Macro documentation](https://github.com/WarmUpTill/SceneSwitcher/wiki/Macros), [public condition/action definitions](https://github.com/WarmUpTill/SceneSwitcher/blob/master/data/locale/en-US.ini), [repository](https://github.com/WarmUpTill/SceneSwitcher).

Window/process snapshots belong to the Windows adapter. A foreground-process condition must refer to the actual foreground process rather than any running process with a matching name. Snapshot-based evaluation avoids mixed observations inside one decision. JSON persistence and editable automation models belong to the frontend/controller rather than the portable core.

## InstantDelay

The public home page title advertises changing stream delay while streaming. On this research attempt the page body returned only a loading state; direct HTTP access met a browser challenge, and `/faq` was unavailable. Thus no internal architecture, quantitative performance, exact feature inventory or seamlessness claim was verified. The requested behavior is implemented independently from the user's specification. No proprietary source was obtained or copied. [InstantDelay public site](https://instant-delay.com/).

## Transport feasibility

libavformat muxers accept encoded packets. Stream codec parameters/extradata must be populated, a header written, packet timestamps converted to the muxer's resulting timebase, and a trailer written on successful closure. A new muxer connection provides an explicit timestamp reset boundary. This supports an in-process OBS encoded-output → queue → FLV/RTMP(S) path without re-encoding. Network protocol success and decoder correctness remain integration tests, not consequences of this design. [FFmpeg muxing API](https://ffmpeg.org/doxygen/trunk/group__lavf__encoding.html).

Increasing delay without synthesizing replacement audio/video creates an output hold. Decreasing without re-encoding requires a decodable cut, removing content. Sending packets faster can affect server buffers and does not guarantee faster viewer playback; this core deliberately uses keyframe cuts and normal pacing. These are engineering implications of the chosen design, not attributed claims about InstantDelay.

## Licensing and release obligations

| Dependency | Evidence and release policy |
|---|---|
| libobs/OBS Studio | Source headers say GPL version 2 **or later**. Preserve notices/license text and provide the complete corresponding source/build material for the distributed combined work. Select GPL-3.0-or-later for new FeatherCast code to align with Qt6's open-source options; do not describe libobs as a permissively licensed SDK. [Source license header](https://github.com/obsproject/obs-studio/blob/33.0.0/libobs/obs.h), [COPYING](https://github.com/obsproject/obs-studio/blob/33.0.0/COPYING). |
| Advanced Scene Switcher | Repository lists GPL-2.0; this stage copies no code. Any future incorporation must preserve applicable notices and verify individual file version clauses. [Repository license](https://github.com/WarmUpTill/SceneSwitcher/blob/master/LICENSE). |
| Qt6 | Qt has commercial, LGPLv3 and GPLv3 options; some modules are GPL-only, and bundled third-party licenses differ. Use dynamically linked Core/Gui/Widgets from a pinned distribution, preserve replacement/relink rights and notices, and ship corresponding required source/build information. Audit the exact shipped modules. [Qt licensing](https://doc.qt.io/qt-6/licensing.html). |
| FFmpeg | Licensing depends on configured components/build flags. Preserve the actual binary distribution's notices and source information; inspect `av*_configuration` and `av*_license` from the shipped build. Do not assume every FFmpeg binary is LGPL-only. [FFmpeg source licensing file](https://github.com/FFmpeg/FFmpeg/blob/n8.0/LICENSE.md). |
| InstantDelay | Public functionality reference only. No proprietary code, assets, name or license key bypass is included. |

Release packaging must enumerate bundled DLLs and plugins, copy their license/copyright files, record exact versions and hashes, and provide corresponding source including any local changes and build scripts. Exclude unrelated OBS frontend/Qt binaries, browser runtime and optional modules unless tested and intentionally distributed. A source-repository license alone is not sufficient fulfillment of bundled binary obligations.

No benchmark against stock OBS has yet been performed by this core stage. Claims about reduced CPU/GPU/RAM or gaming overhead require measured equivalent scenes, output settings, hardware and driver versions.
