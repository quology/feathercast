# VELA 0.1.0-dev.4-workspace-r1 — development preview

19 November 2026. Local development preview; source checkpoint `4dcf30f`.

Scene editing and native source groups, locks, ordering, properties and transforms are available, with three video filter types and existing microphone filters. The mixer provides real aggregate peaks, gain, mute, monitoring, scope, hide/show and Advanced Audio Properties. Studio Mode has two native views, Cut/Fade and duration controls; preview-off recovery, CPU/FPS and output timers are available. Four Twitch docks support channel, chat, activity and statistics/goals with optional permissions. Existing layouts are retained; View → Reset Workspace Layout shows the new docks.

Grouped coordinates remain parent-local and may rebase while world placement stays fixed. Repeated unchanged Apply, nested placement, duplication and reload passed the strengthened synthetic checks. Release build and CTest 5/5 passed; coordinate checks passed 9/9, production UI 14/14 and restart 4/4. The r1 timer fix latches first native output activation, survives initial NVENC retry and reconnect, and returns zero after stop. Release CTest 5/5 and six finalized media validations passed for this fix. Current source `4dcf30f` [CI passed](https://github.com/quology/feathercast/actions/runs/44560702657); packaged r1 UI passed 14/14 and all three finalized media files passed full decode/timestamp/keyframe/end-skew validation. Native NVENC smoke passed with stream duration 8983.9369 ms, relay connected and relayError 0; prior workspace source `e82f380` passed [44558221420](https://github.com/quology/feathercast/actions/runs/44558221420). The earlier foundation run [44549637152](https://github.com/quology/feathercast/actions/runs/44549637152) passed for its own checkpoint.

No virtual camera or full OBS parity is claimed. Real Twitch account behavior, actual capture and physical monitoring remain manual checks. Lifetime channel views are unavailable; local activity dismissal does not skip external alerts. One profile/collection is supported; game/window global audio uses Application Audio, and capture-audio topology is creation-only. GPU utilization, broad performance improvements and public redistribution sign-off remain unavailable or unverified.

See [test results](../TEST_RESULTS.md) for exact evidence and historical validation.

---

# FeatherCast 0.1.0-dev.1 — development preview

18 November 2026. Private milestone, not stable v1 or full OBS feature parity.

Native Qt scenes/sources, standalone libobs, shared streaming/recording/replay, bounded live delay, visual automation, global hotkeys, protected stream keys, real audio meters and MP4 remux are implemented. First launch uses a safe Color scene; capture and outputs require explicit actions.

Synthetic x264/RTX 4070 NVENC outputs, local RTMP, live delay changes and full decode passed. Debug and Release CTest passed all three targets, including isolated DPAPI credential storage and key-free exports. Debug, Release and portable UI self-tests report all 12 Boolean checks true with clean shutdown. Remux passed decode, source/existing-destination preservation and invalid-input cleanup. Local receiver stop/restart verified one reconnect and valid recovered media. Packaged NVENC recording, replay and relay artifacts decoded successfully. Hotkey testing posts WM_HOTKEY locally, without proving physical keyboard delivery from another foreground app.

The portable ZIP was tested with SDK paths removed; selected OBS/Qt/graphics/capture libraries loaded from its own files. Final-code CI passed Linux core, Cppcheck and Windows build/test/package jobs: [run 44304834193](https://github.com/quology/feathercast/actions/runs/44304834193). See [TEST_RESULTS.md](../TEST_RESULTS.md). Extract the complete ZIP; no installer is supplied. A short [idle process sample](BENCHMARKS.md) is provided without a general performance claim.

Gaps include one AAC stereo track, shared quality/resolution, no splitting or disk-backed delay, pre-start disk-space check only, limited in-process plugin actions and unavailable CPU/GPU counters. Visual capture/preview, protected games, external platforms, AMF/QSV, long-duration operation and gaming performance remain unverified. Delay holds can time out ingest; reductions intentionally cut content. Public redistribution requires a complete license/corresponding-source audit.

See [feature status](FEATURE_STATUS.md), [user guide](USER_GUIDE.md) and [build instructions](../BUILD.md).
