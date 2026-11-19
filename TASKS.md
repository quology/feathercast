# Current workspace checkpoint — 19 November 2026

Source `4dcf30f` on `codex/obs-workspace-features` is frozen. Release build/CTest 5/5, native coordinate 9/9, hidden workspace UI 14/14 and restart 4/4 passed. Current source [CI passed](https://github.com/quology/feathercast/actions/runs/44560702657); Final artifact checksums/report are in `outputs/VELA-workspace-verification.md`. Distribution is local only; the user confirmed local-only binary distribution. No virtual camera is included. Actual capture, authenticated Twitch behavior, physical monitoring and broad performance comparison remain manual/unverified. Portable package preparation is recorded in `work/workspace-delivery-handoff.md`.

The checkpoint below is historical and refers to dev.1.

# Development checkpoint

Private repository `quology/feathercast`, branch `main`. Code/tooling commit `22b0d78` passed all CI jobs; later documentation changes preserve that tested application source.

- [x] Research, architecture and license strategy.
- [x] Bounded delay, priority automation, module lifecycle and plugin action ABI.
- [x] 13 portable core scenarios and transactional automation JSON tests.
- [x] Qt 6.8.3/MinGW 13.1/OBS 33 provisioning and D3D11 integration.
- [x] Simultaneous x264/NVENC record/replay/local RTMP and media validation.
- [x] Qt UI, macro editor, settings, real meters and module gates.
- [x] Windows hotkeys, DPAPI secrets, atomic persistence and plugin loading.
- [x] MP4 remux success/decode/preservation/invalid-input cleanup.
- [x] Debug and Release CTest, 3/3 targets; Debug UI self-test 12/12 Boolean checks.
- [x] Local RTMP failure/reconnect injection and recovered media validation.
- [x] Documentation of implementation and limitations.
- [x] Final Release UI self-test: exit 0, all 12 Boolean checks and clean shutdown.
- [x] Portable packaged executable and runtime-media verification, with SDK paths removed.
- [x] Final-code CI and synchronization.
- [x] Short matched idle process CPU/RSS measurement, with explicit limits.
- [x] Create and verify private development prerelease assets (coordinator).
- [ ] Equivalent OBS/application gaming FPS, 1% lows and frame times.
- [ ] External platforms, visual capture/preview, AMF/QSV and long-duration validation.
- [ ] Public redistribution/license/corresponding-source audit.

Application source is frozen and verified. Coordinator owns release artifacts. Named agent spawning and Luna overrides were not exposed by the refreshed session, although the native catalog still lists Luna; documentation used a verified Sol/low fallback. Runtime model evidence is in docs/MODEL_USAGE.jsonl. No simultaneous source development; not all requested requirements are complete.

Private prerelease [v0.1.0-dev.1](https://github.com/quology/feathercast/releases/tag/v0.1.0-dev.1) uses source commit `873c62c647a264401a02f0369a17e941c0a8ed04`; all nine assets are present and the downloaded portable ZIP SHA-256 was verified.
