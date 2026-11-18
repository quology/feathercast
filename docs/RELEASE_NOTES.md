# FeatherCast 0.1.0-dev — development preview

18 November 2026. Private milestone, not stable v1 or full OBS feature parity.

Native Qt scenes/sources, standalone libobs, shared streaming/recording/replay, bounded live delay, visual automation, global hotkeys, protected stream keys, real audio meters and MP4 remux are implemented. First launch uses a safe Color scene; capture and outputs require explicit actions.

Synthetic x264/RTX 4070 NVENC outputs, local RTMP, live delay changes and full decode passed. Debug and Release CTest passed both targets. Debug and Release UI self-tests report all 12 Boolean checks true with clean shutdown. Remux passed decode, source/existing-destination preservation and invalid-input cleanup. Local receiver stop/restart verified one reconnect and valid recovered media. Hotkey testing posts WM_HOTKEY locally, without proving physical keyboard delivery from another foreground app.

Final portable package and updated CI sign-off remain coordinator-owned in [TEST_RESULTS.md](../TEST_RESULTS.md). Distribution is intended as an extracted portable ZIP after package testing; no installer is supplied.

Gaps include one AAC stereo track, shared quality/resolution, no splitting or disk-backed delay, pre-start disk-space check only, limited in-process plugin actions and unavailable CPU/GPU counters. Visual capture/preview, protected games, external platforms, AMF/QSV, long-duration operation and gaming performance remain unverified. Delay holds can time out ingest; reductions intentionally cut content. Public redistribution requires a complete license/corresponding-source audit.

See [feature status](FEATURE_STATUS.md), [user guide](USER_GUIDE.md) and [build instructions](../BUILD.md).
