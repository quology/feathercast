# Verified test results

## Workspace UI — 19 November 2026

- `scripts/build.ps1 -SkipBootstrap -Configuration Release` passed; CTest passed 5/5. The existing integrated UI self-test passed all 12 Boolean checks in an isolated profile.
- The final native D3D11 workspace fixture used only a synthetic color and test tone and passed 14 Boolean checks, including composed world placement and repeated unchanged Apply, plus: hierarchy/selection, visibility/locking, source property and transform Apply, video filter add/bypass/reorder/edit/remove, mixer gain/mute/global/hide/show, Studio take, transition controls, preview-off recovery and configuration save. A second process passed 4 restart checks for persisted hierarchy, audio, Studio transition and world placement. Monitoring-device changes and real capture targets were not exercised by this fixture.
- Dark and light QWidget captures were saved under `outputs/workspace-ui/handoff-*.png` and the later `offscreen-check.png` for 1920×1080 and 1366×768. QWidget capture excludes the native preview/program HWND content. Twitch was disconnected and no account activity or messages were generated. At the smaller size, dock contents may require scrolling.
- The earlier grouped-color failure is resolved: legitimate parent-local rebasing was distinguished from repeated-Apply and immediate-reload drift, and both defects were fixed. The fixture again tests real position edits. Original evidence remains in `outputs/workspace-ui/review-dark-1920x1080-profile/engine.json` and its JSON result; see “Group coordinates and idempotent Properties Apply” below for preserved reproductions and final 9/9 coordinate, 14/14 UI and 4/4 restart results.
- Actual authenticated Twitch behavior and live application capture remain manual verification items. The fixture does not replace the engine's separate native integration tests.


## Workspace menu — 19 November 2026

- Release build passed and CTest passed 5/5. An isolated UI self-test passed all 12 Boolean checks.
- Dark and light widget screenshots were captured at default size and at 884 pixels wide; the menu row fits. Widget captures exclude the native title bar and preview. Native menu interaction and keyboard checks remain pending coordinator review.

## Twitch integration and VELA branding — 19 November 2026

- The Windows Release build produced `VELA.exe` with the VELA icon and product metadata. CTest passed 5/5. The main window passed all 12 integrated checks in each theme. The header and icon were inspected in the running app.
- The production Twitch service passed local HTTP and WebSocket fixtures for device authorization, DPAPI storage, profile locking, startup validation, token rotation and concurrent 401 recovery, rate-limit handling, scoped authorization rejection, API requests, chat receive/dedup/send and disconnect cleanup. The production dock fixture verified live viewer/uptime display, floating and resizing, dirty edit preservation and bounded plain-text chat.
- Staged Qt deployment included Qt Network, Qt WebSockets and Schannel. With SDK directories off PATH, a Qt probe received the expected unauthenticated HTTP 401 from Twitch token validation and a secure EventSub welcome. This verifies TLS connectivity and the WebSocket handshake.
- In an isolated profile, VELA opened a scene and retained a recording folder saved by FeatherCast. The existing profile and DPAPI vault identifiers are retained. The DPAPI regression test uses synthetic credentials.
- Authenticated device consent, channel edits on Twitch, receiving and sending real chat, server-directed reconnect handoff, and viewer/uptime reporting during a real stream require an authorized Twitch account. The test run sent no authorized request to Twitch.

## Microphone filters — 19 November 2026

- Release build in `build/MicrophoneFilters` passed with Qt 6.8.3, MinGW 13.1 and the pinned OBS runtime. CTest passed 3/3. The main window passed all 12 integrated checks in both themes.
- The production microphone dialog fixture passed add, remove, bypass, reorder, Defaults, and synchronized Speex slider and numeric entry checks. It also checked the window after microphone removal. Dark and light dialog captures were reviewed.
- Native tests verified real libobs filter registration, order, retained filter identity, and persisted settings and bypass. Invalid edits/imports preserved the original graph. Legacy microphone and video sources loaded without adding filters.
- Injected synthetic samples verified compressor attenuation and measured a change in gate output when swapping filter order. Bypass restored the input as expected. Suppression checks covered finite processed samples for Speex and exercised RNNoise when advertised by the runtime.
- The fixture passed with a runtime missing `obs-filters`. The saved chain loaded without native filters attached. Dormant settings survived changes and reload, and Add was disabled in the dialog.
- Existing source audio and Advanced Audio Properties smoke tests passed. The decoded track routing check still found 440 Hz on track 1, 880 Hz on track 2 and both on shared track 3. Acoustic quality on a physical microphone was not evaluated. The fixture used an intentionally nonexistent device and synthetic samples.

## Full Advanced Audio Properties — 19 November 2026

- Full Release build in `build/AdvancedAudio` passed; CTest 3/3 passed. Existing application instances were left running in their original directories.
- Independent `feathercast_source_audio_smoke` and `feathercast_advanced_audio_ui_smoke` runs both exited 0. Native checks cover atomic validation, volume/mute, mono, balance, signed sync-offset conversion, empty/shared track masks, monitor-only suppression and restoration, recording routing guards, companion ownership, canonical persistence and legacy migration. Monitoring capability checks ran without a SKIP on this machine, using muted synthetic sources.
- The production dialog fixture exercised all eight columns, dB/percent conversion without gain changes, zero-gain display, mute/mono/balance/sync/monitoring controls, each track checkbox, active/inactive filtering, editor focus retention during polling, recording locks and Close. Its dark-theme screenshot was reviewed: all columns and six checkboxes per source fit at host scaling.
- Main application integrated UI self-test passed all 12 Boolean checks in both dark and light themes, with clean timed exits.
- Six native fixture media files passed every-track decode, strictly increasing per-stream DTS, first video keyframe and all-track A/V end-skew checks. Independent decoded PCM checks confirmed 440 Hz only on track 1, 880 Hz only on track 2, and both on shared track 3. Unwanted tone suppression exceeded 140 dB in this synthetic fixture (40 dB required). The retained mixed/isolated fixture also passed. The standard-library validator now reproduces the three-track check with `--routing`.
- An isolated application startup separately verified `ui.ini` migration: legacy gain/mute and isolated assignment were imported, existing canonical JSON took precedence over conflicting INI values, and the stale audio INI group was removed only after a successful save.
- Acoustic playback was not listened to, and real Windows application sound isolation remains unverified. Native capture ownership uses nonexistent targets; recordings use explicit synthetic tones rather than the user's application audio. These checks do not establish live-platform or hardware-encoder behavior.

## Application audio and isolated recording tracks — 19 November 2026

- Full Release build in `build/SourceAudio` passed with pinned Qt 6.8.3/MinGW 13.1; the existing `build/Release` executable was in use and was not replaced. Existing `obs_add_data_path` deprecation warning remains.
- CTest 3/3 passed. Integrated UI self-test passed all 12 Boolean checks in each theme; both timed launches exited 0. Add source was visually inspected with the new capture-audio and separate-recording-track controls.
- `feathercast_source_audio_smoke` passed: explicit target validation; private display-audio companion ownership, mute/gain/visibility/removal and namesake handling; native window/game routing; normal source defaults; configuration roundtrips and invalid-import preservation; five-track capacity; recording topology guards; concurrent recording/stream/replay; recording restart; physical track-number gaps; clean shutdown with no OBS error-level logs.
- Five generated media files passed every-track FFmpeg decode, strictly increasing per-stream DTS, initial video keyframe and all-track A/V end-skew checks: initial recording, restarted recording, gap recording, local FLV relay and replay.
- `scripts/validate_source_audio.py` independently decoded the initial MKV: mix track contains 440/880 Hz at comparable levels, isolated track contains 440 Hz with 880 Hz approximately 149 dB lower in this synthetic fixture (required suppression: 40 dB). Track labels and silent physical gap track were checked with FFprobe.
- Initial native validation caught synthesized null JSON option keys breaking reload; optional reads now preserve absent keys, and the final fixture includes this regression check.
- Actual sound capture/isolation from a real Windows application remains unverified. Native capture ownership tests use nonexistent windows in an inactive scene, then remove them before recording; media tests use explicit synthetic tones. No user's application audio or live external platform was recorded.

## UI refresh — 19 November 2026

- Full Release build with pinned Qt 6.8.3 and MinGW 13.1: passed.
- Release CTest: 3/3 passed (credential storage, automation configuration, core).
- Integrated `--ui-self-test` with isolated configurations: all 12 Boolean checks passed in both dark and light themes; both timed launches exited 0.
- Main-window widget screenshots reviewed in both themes at the default 1280 × 800 logical size (approximately 1600 × 1000 at host scaling). No clipped main-window controls observed. Native libobs preview content is not captured by `QWidget::grab`.
- Interactive settings/rule-editor inspection and smaller display sizes remain unverified: the test window disappeared before computer-use interaction could complete. Backend performance and live broadcasting were not retested for this UI change.

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
| Portable UI without SDK paths | Passed | Packaged app exit 0, all 12 checks; six selected OBS/Qt/graphics/capture modules loaded from package paths, not the development SDK |
| Packaged-runtime simultaneous NVENC outputs | Passed | Recording, replay and delayed FLV exit 0; all three media files decoded with valid timestamps; final delay 811.548 ms at zero target, relayError 0 |
| Final-source CI, commit 22b0d78 | Passed | [Run 44304834193](https://github.com/quology/feathercast/actions/runs/44304834193): Linux core, Cppcheck core/example analysis, Windows Debug/Release tests and portable packaging |

RTMPS tls_verify=1 is applied; no external RTMPS/Twitch/YouTube session was tested. AMF/QSV hardware, actual desktop/window/game visual capture, protected games, perceptual lip sync and long-duration stability remain unverified. Native preview is implemented, but QWidget grabs exclude its HWND and do not prove visibility.

The packaged-runtime media fixture was placed beside the delivered application for testing and is excluded from the release ZIP. The archive contains no development SDK or OBS frontend executable.

A matched short idle CPU/RSS sample is published in [BENCHMARKS.md](docs/BENCHMARKS.md), with raw engine counters in a separate artifact. It is not a production/gaming benchmark or performance-superiority claim. No comparable gaming FPS/1% low/frame-time measurements exist. Disk checking is pre-start only, without a live full-disk guarantee. UI utilization counters remain unavailable. Public redistribution/license/corresponding-source audit remains incomplete. See [feature status](docs/FEATURE_STATUS.md).


## Workspace backend verification, 19 November 2026

Release build and CTest passed with `scripts/build.ps1 -SkipBootstrap
-Configuration Release` (five CTest targets). The explicit GPU fixture
`feathercast_workspace_engine_smoke OBS_RUNTIME OUTPUT_DIRECTORY` passed using
OBS 33, 640×360/30 fps and synthetic color/tone sources. It is deliberately not
registered with headless CTest.

- Real native nested groups, source/scene renaming and ordering, descendant locks,
  unlocked visibility, property-schema rejection, native transforms, independent
  duplication (including matching transforms), and group-cycle rejection passed.
- Native Color Correction/Crop filters were attached and their processing order
  verified. Layout, locks, source properties, video filters and Global/hidden mixer
  state survived an exact JSON roundtrip. Malformed parents and an intentionally
  failed native candidate construction preserved the prior graph and program.
- Studio preview selection retained the native program source. Fade completed and
  Cut selected the expected output. Actual output PCM RMS established that a
  preview-only tone stayed out of program, became audible after Take, remained
  audible off-scene with Global scope, and did not double mix when present in both
  program and Global. Duplicating/removing a scene containing a global tone did
  not duplicate its output level. Scene scope restored normal off-program inactivity.
- Two native HWND displays were created, attached, resized, preview-disabled,
  reenabled and detached before window destruction. This establishes API/lifetime
  behavior, not final desktop layout or a visual pixel comparison.
- A synthetic MKV recording exercised output timers. CPU and status keys were
  present. Shutdown completed with zero unexpected OBS error-level logs and no
  remaining-source/double-destruction diagnostic. Deliberate Engine errors covered
  locked edits, unsupported property, cyclic group, invalid import and failed
  candidate construction; those are expected fixture assertions.

Evidence: `work/workspace-build.log`, `work/workspace-engine-tests.log`, and
`work/workspace-engine-tests/workspace.json` (ignored local artifacts).

The existing source-audio smoke also passed (`work/workspace-source-audio.log`),
including six-track routing/lifecycle and private capture-audio ownership checks.
`validate_source_audio.py` passed on its `isolated.mkv` and `routing.mkv`: the
isolated 440 Hz track rejected 880 Hz by about 149.8 dB; the manually routed tracks
contained their expected 440/880 Hz frequencies. Reports are
`work/workspace-source-audio/isolated-validation.json` and
`routing-validation.json`. Full media decode/timestamp/end-skew validation also
passed for the workspace recording and all six source-audio media artifacts
(`work/workspace-engine-tests/media-validation.json` and
`work/workspace-source-audio/media-validation.json`). Existing microphone-filter native/UI smoke passed in
`work/workspace-microphone-filters.log`; its device ID is deliberately nonexistent
and its injected samples are synthetic.

This stage validates the backend only. Workspace UI wiring, interactive editing,
visible program/preview rendering, real application capture and grouped companion
capture under a running application still need frontend/device testing. Global
scope intentionally excludes window/game video sources (use Application Audio).
Source capture-audio topology remains a creation-time option. Video filter support
is the advertised three native types, not every OBS filter. No virtual-camera
implementation, long soak or performance improvement is claimed.


## 2026-11-19 — Twitch activity feed and statistics workspace stage

- `./scripts/build.ps1 -SkipBootstrap -Configuration Release`: Release native app/service/UI fixture compilation passed; CTest **5/5 passed**. Expanded loopback fixtures use fake tokens and local HTTP/WebSocket only; no Twitch account was authorized and no real chat was sent.
- Protocol evidence: original base-only token retained (DPAPI/profile locking unchanged), base follower total available, optional subscriber/points totals unavailable without scopes; all nine activity types; follow v2 condition; bounded payload strings; duplicate/foreign broadcaster suppression; optional subscription denial/revocation leaves chat healthy; all authorized types resubscribe on a fresh socket; dedup survives reconnect. HTTP totals/goals success and unavailable failures covered. Delayed goal snapshot cannot overwrite a newer progress event.
- Production Qt evidence: four optional docks, existing metadata/chat behavior retained; 500-entry literal plain-text activity list; working type filter/dismiss/clear; zero versus unavailable totals; account disconnect clears activity/statistics/goals. Existing credential storage, automation and core tests also passed.
- `git diff --check`: passed. CI push branch filter now includes `codex/**`.
- Limitations: no authenticated Twitch/account eligibility or real activity test; Twitch-directed WSS transfer retains existing code path and still needs real-account handoff validation. Feed is live only and local Dismiss does not control third-party alert playback. Lifetime channel views unavailable because Twitch no longer provides them.
- Model log records requested overrides with null runtime evidence; execution identity was not independently exposed.


## 2026-11-19 — Group coordinates and idempotent Properties Apply

The earlier grouped-position issue was reproduced, diagnosed and fixed. It was
not resolved by substituting a rotation assertion. In an offscreen production
Properties dialog, changing fitted color width1280→1281 and local X=37 retained
world X≈37 after the first Apply. OBS correctly rebased the group to X=37 and the
child to X≈0. An unchanged third Apply then moved world X to≈74 because the old
editor compared a stale X=37 widget against its refreshed native X≈0 baseline.
Preserved failure: `work/coordinate-before-fix/coordinates.json`.

The editor now refreshes clean controls and their displayed baselines together,
including native group rebasing, and preserves unsaved user edits. The fixture
again tests actual X=37, with four unchanged Applies and native composed matrices.
A second regression test found pending-save reload loss: coherent saved parent115
plus child11 (intended world126) restored to parent115/child0/world115 even after
100ms. OBS33 treats a newly created scene's first render as a canvas resize and
omits the compensating group-origin move in that path. Initializing the empty
scene's native canvas before population fixes this without event-loop reentry or
capture activation. Preserved failure: `work/coordinate-pending-repro/coordinates.json`.
Nested group restoration also required forcing its native draw matrix before child
bounds recalculation. Duplication now finishes all reparenting before restoring
transforms, preventing later child insertion from moving restored ancestors.
The existing workspace fixture now compares composed rendered leaf placement and
exact non-position state, rather than requiring group-local origins to stay
identical after legitimate native rebasing. It caught a duplicate-placement failure
before that ordering fix; both strengthened duplicate and reload checks now pass.

Final checks:

- Release build and CTest5/5 passed (`work/coordinate-build.log`).
- New explicit `feathercast_workspace_coordinates_smoke` passed: four stable
  Applies, pending editor changes preserved, 144 coherent parent/child saves during
  active render, six immediate-save/reload cases retaining intended placement,
  independent native matrix composition through nested rotated/scaled groups,
  duplicate/reload placement, and100 immediate CPU reads returning one finite
  cached sample. Evidence: `work/coordinate-final4/coordinates.json`
  and `.log` alongside that directory (`work/coordinate-final4.log`).
- Fresh hidden production UI fixture passed14 Boolean checks, including world
  position, repeated Apply, refreshed local coordinate, and all preceding workspace
  controls. Separate restart passed4 checks including saved world placement:
  `work/coordinate-ui-final-20261119c/ui.json` and `reload.json`.
- Existing native workspace engine fixture passed with zero unexpected OBS errors
  and clean shutdown (`work/coordinate-final4-workspace.log`).

Fixtures used only generated color/tone sources and isolated profiles. Dialogs and
fixture application windows stayed offscreen; these checks establish geometry and
controller behavior, not a new visible-desktop layout or real-capture benchmark.


## 2026-11-19 — relay live-duration lifecycle regression

The final packaged NVENC smoke exposed an advancing stream with a zero live timer.
A preserved native signal trace (`work/relay-timer-repro.log`) reproduced the defect:
the relay initially retried before capture, then OBS emitted `activate` and
`reconnect_success`, without `start`. Engine clocks now latch the first capture
activation in a stable per-output atomic callback context, retain that epoch during
reconnect, and reset it when a new output session is connected. Stopped outputs
report zero. Relay transport and encoder behavior were not changed.

- Release build and all 5 CTest tests passed (`work/relay-timer-build.log`).
- Strengthened native output smoke passed with NVENC: at approximately 11 seconds,
  streaming duration was 8995 ms after the initial 2-second retry; recording/replay
  were 10997/10997 ms. It asserts all live durations exceed 8 seconds, stream time
  advances by at least 4 seconds between the 6-second and final samples, and all
  three durations return to zero after stop (`work/relay-timer-nvenc.log`).
- An intentionally terminated/restarted local RTMP receiver produced one reconnect;
  the same clock assertions passed with streaming duration 8982 ms, retaining the
  original capture epoch (`work/relay-timer-reconnect-20261119/reconnect-result.json`).
- Full decode, strictly increasing packet DTS, initial keyframes, and audio/video
  end-skew checks passed for all 6 finalized media files across these two runs;
  reports are `media-validation.json` in their respective output directories.
  The intentionally interrupted receiver partial file is excluded.

Both fixtures shut down normally with no OBS error-level messages. Logs retain
non-admin GPU priority diagnostics and OBS debug packet-timing messages during
shared-encoder shutdown; these are not suppressed. These checks use synthetic
color/tone and a local receiver, not a Twitch session or performance benchmark.
The prior package/ZIP was preserved; these results cover the newly built source
binary and provisioned OBS33 runtime, pending coordinator package regeneration.
