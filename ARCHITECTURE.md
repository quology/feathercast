# FeatherCast architecture

FeatherCast owns a libobs process, capture graph, encoders, outputs and Qt interface. It does not automate a separately running OBS Studio. This document distinguishes the implemented portable core from application integrations, which must be verified separately. Research and source links are in [docs/RESEARCH.md](docs/RESEARCH.md).

## Boundaries and ownership

| Component | Responsibility | Execution context |
|---|---|---|
| Qt application/controller | Settings, commands, scenes, module lifecycle, decision execution | GUI thread |
| libobs | D3D11 rendering, capture sources, audio mixing, hardware/software encoders | OBS-owned threads |
| Encoded output adapter | Copy bounded packets, map codec/timebase metadata, signal encoder errors | OBS callback; never perform network I/O here |
| `DelayBuffer` | Bound payload/packet count/time span, gate/pacer, keyframe cuts | Mutex protected; one consumer |
| Relay/muxer | FLV/RTMP(S) network writes, reconnect, codec headers, interrupts | Dedicated worker |
| Recording/replay outputs | MKV files and replay save through OBS output modules | OBS-managed output workers |
| `AutomationEngine` | Pure snapshot evaluation and deterministic command decisions | Controller thread |
| `ModuleRegistry` | Dependency-aware activation and rollback | Controller thread |
| Plugin host | ABI validation, action calls and unload lifetime | Controller thread |

Capture and compatible encoders should be shared among streaming, recording and replay. Separate encoders are necessary for different codec, resolution, rate control or quality settings. Each output has independent lifetime/error reporting. Never stop a shared encoder explicitly while another output uses it. Preview disabling removes display rendering work without disabling program composition. There are no claimed performance improvements until equivalent workloads are measured.

Qt owns widgets. libobs handles are reference-counted and must be released with the matching OBS functions; allocations must not cross incompatible C runtimes. Do not exchange C++ objects between MinGW Qt and MSVC libraries. libobs C ABI calls can be tested with pinned headers/import libraries; OBS frontend Qt plugins are not automatically compatible with this host. Only explicitly selected non-frontend modules should load.

## Implemented delay core

`include/feathercast/core/delay_buffer.hpp` and `src/core/delay_buffer.cpp` implement an encoded packet queue. `set_target_delay`, `push`, `pop_ready`, `disconnect`, `reconnect`, `reset`, and `stats` are its public API. It does not mux or transmit by itself.

The adapter supplies a common microsecond epoch, globally nondecreasing DTS, strictly increasing DTS per track, and nonnegative DTS/PTS. Track zero is the one video stream; tracks 1–8 are audio. Negative composition offsets are rejected. For the first integration use H.264 closed GOP, B-frames disabled, and AAC; do not infer decoder random access solely from an arbitrary packet flag. Payload ownership moves into the queue. `arrival_us` and `pop_ready(now_us)` use one monotonic steady clock. Delay measures packet age after encoding, not end-to-end viewer latency.

- Start waits for a video keyframe; earlier audio and dependent frames are discarded.
- Increasing the target holds packets until their age reaches it. There is no duplicate-frame generator or audio filler. An ingest server can time out during the hold. The application must display this limitation and reconnect on a failed transport.
- Decreasing selects the newest available keyframe old enough for the target. It drops earlier video and audio together and applies one timestamp offset to all remaining tracks. Output resumes after the maximum previous presentation end, preserving composition offsets and avoiding timestamp rollback. Content is intentionally removed. A keyframe interval makes the effective target approximate.
- Release is paced by source DTS intervals; a backlog is not sent in a large burst. Late scheduling can increase effective delay. The first packet after a cut has `discontinuity=true`; the transport must validate whether headers or a new connection are required.
- The default payload-capacity cap is 256 MiB, with a separate 100,000-packet count cap and 310-second DTS span. Target delay is capped at 300 seconds. Packet capacities are counted to prevent a small size with a huge reserved allocation. Allocator/deque metadata is additional, bounded by the packet count; this is not an exact process-RSS ceiling. The caller's incoming and popped packet storage is outside the queue limit.
- Overflow clears the queue and waits for a keyframe. Oversized or invalid packets fail closed too; dependent frames cannot silently continue across a missing packet. Counters expose loss. There is no disk spill in this implementation; disk-buffer controls must not pretend otherwise.
- `disconnect` stops release while encoding may continue within caps. `reconnect(KeepBuffered)` discards leading non-keyframes and resets output timestamps for a **new connection**. `DiscardBuffered` waits for a new keyframe. `reset` starts a new encoder epoch; ordinary reconnection retains input ordering history. Do not invoke reconnect on the same muxer timeline.

The queue does not retry a popped packet: after write failure the adapter calls `disconnect` and creates a new muxer/connection before `reconnect`. Credentials never enter the core. A higher level stop action must explicitly choose immediate drop or paced drain; shutdown must not silently wait the full configured delay.

## Real relay integration contract

Register an `obs_output_info` with `OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED`, optionally multi-track audio. Its start routine uses `obs_output_initialize_encoders` and `obs_output_begin_data_capture`, after connection and codec parameters are ready. `encoded_packet` copies packet bytes immediately, converts the packet timebase with checked arithmetic, maps video/audio track IDs, and pushes into `DelayBuffer`. Null input means an encoder error, not an empty frame. End capture before destroying callback state.

libavformat is the selected feasible transport: create an output context for FLV; populate H.264/AAC stream codec parameters and copied encoder extradata; `avio_open2`; `avformat_write_header`; rescale each outgoing microsecond timestamp into the stream's **post-header** timebase; write via `av_interleaved_write_frame`; write trailer/close when possible. Account for Annex B versus AVCC payload/extradata representation. Use an interrupt callback and bounded I/O deadlines so stop and disconnect cannot hang indefinitely. A fresh connection needs codec headers and a video random access frame. RTMP typically has one AAC track; multitrack recording is a separate output.

The standard OBS delay API is not a substitute for this relay. In OBS 33.0.0, `obs_output_set_delay` updates configured fields; active delay is maintained separately in the delay pipeline. A fixed pre-start delay or changing a UI number is not proof of live dynamic delay.

Pending transport verification: local RTMP receiver recording; decode validation of cuts and A/V sync; increase beyond ingest idle timeout; disconnect during packet write; reconnect keyframe/header behavior; full-duration memory pressure; and Twitch/YouTube server behavior. No seamless-transition claim is made.

## Implemented automation

`AutomationEngine::set_rules` validates transactionally and sorts by descending numeric priority, then lexical ID. Rules contain OR-of-AND condition groups, multiple actions, debounce, cooldown and optional repeat mode. Conditions cover process/foreground/title, stream/record/replay state, audio level, elapsed time, hotkeys and external flags. Process names must be normalized by the Windows adapter; title matching is case-sensitive substring matching. Timers are elapsed threshold conditions, not persistent calendar schedules.

Evaluate every rule from one `AutomationSnapshot`, then execute only the highest-priority debounced match. A winner in cooldown retains arbitration so lower-priority rules cannot oscillate scenes. Default rules fire when they become the winner, rather than every tick. Repeat mode requires positive cooldown. Manual override blocks all automatic actions until its steady-clock deadline. Fallback applies only when nothing matches and is suppressed while debounce is pending. Redundant actions are filtered against a projected sequential state, preserving intentional start/stop action sequences.

`evaluate` produces values, never executes callbacks. The controller executes actions once, logs results, and obtains a fresh snapshot next tick. It must not recursively call evaluate in response to its own scene change. Failed actions are reported, not retried in a tight loop. Hotkey pulses must remain present long enough for debounce or use a zero-debounce hotkey rule. `matches` provides side-effect-free rule testing. Snapshot acquisition, JSON import/export, the macro editor and OS hooks belong to the application layer.

## Modules and plugin ABI

`ModuleRegistry` requires dependencies to be registered first, preventing cycles. Enable starts dependencies and rolls back only modules activated by that attempt. Disable refuses required modules or modules with active dependents. Shutdown uses reverse registration order; failed stops leave dependencies alive. Start callbacks must undo partially acquired resources on failure, and callbacks cannot reenter the registry. DLL unload is separate from toggling a module.

`plugin_api.h` is a versioned C ABI with structure sizes, UTF-8 borrowed strings, initialization/shutdown and action descriptors. Version 1 supports custom automation actions and a host delay request. Other requested extension types (sources, panels, conditions, hotkeys and outputs) require future ABI versions or the separate libobs module API; they are not falsely exposed as implemented v1 capabilities. Trusted native plugins run in-process and can crash the host; exception handling is not process isolation.

See [docs/PLUGINS.md](docs/PLUGINS.md). The example plugin calls the host to request a 90-second delay. The core test exercises its ABI in-process; application DLL loading is a separate integration test.

## Build integration and verified core checks

The root CMake project should call `include(CTest)` and `add_subdirectory(src/core)`. Link application code to `feathercast_core`. This defines `feathercast_core_tests` when `BUILD_TESTING` is on and the `feathercast_example_plugin` module target. No Qt, libobs, JSON library or external test framework is required by the core.

The core test executable was compiled with MinGW GCC 13.1, C++20, `-Wall -Wextra -Wpedantic -Werror -O2`, and all 12 scenarios passed on Windows. See `tests/core_tests.cpp` for exact cases. This is not validation of real capture, encoders, RTMP, plugin DLL loading, or viewer playback. Integration results should be recorded separately as those components are completed.
