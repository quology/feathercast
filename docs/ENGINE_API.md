# Engine integration handoff

`include/feathercast/engine/engine.hpp` is the Qt controller API. All calls belong on the object's Qt thread. `initialize()` finds packaged `appDir/core`, then adjacent `appDir/obs`, then the build's manifest-pinned development runtime. An explicit runtime path overrides this. `defaultSettings()` supplies the supported JSON keys; `applySettings()` requires stopped outputs. The first stage intentionally uses H.264/AAC stereo, a shared encoder pair, and no B-frames.

## GUI wiring

- Initialize, then `loadConfiguration(absolutePath)`. Create a default scene when no configuration exists. Configuration saves are atomic with QSaveFile. Keys are separately protected by Windows DPAPI (`storeStreamKey`); never put a key into the endpoint/settings JSON or logs. The endpoint is the base server/application URL; the engine appends the protected key at start.
- Scene methods and `addSource(scene,type,name,properties)` operate real OBS sources. Types: `desktop`, `window`, `game`, `color`, `microphone`, `desktop_audio`, `application_audio`, `test_tone`. Use `sourceOptions(type)` for window/device/monitor choices; each entry identifies the property key and value to pass. No device capture is enabled automatically. Test tone is an explicit diagnostic source (`frequency_hz`, default 440).
- `sourceAudioAvailable(type)` checks native application-audio registration and window/game property support. `capture_audio: true` enables native window/game audio, tied to `window`; a game with a supplied window defaults to `capture_mode: "window"`. For display capture, provide `capture_audio: true` and `audio_window` using a `sourceOptions("application_audio")` window value. This creates a private audio companion scene item; meter, volume/mute, visibility and removal are owned by the display source. `application_audio` directly accepts `window`. No unavailable-target fallback captures desktop audio.
- `audioSources()` lists configured audio sources across scenes with the name, scene, live OBS active state and canonical properties. `audioProperties(name)` returns the source's saved settings. Use `setAudioProperties(name, patch)` on the controller thread. A rejected patch leaves the source and configuration unchanged. The display source controls its private application-audio companion.
- `properties.audio` accepts linear `volume` (0–2), `muted`, `mono`, stereo `balance` (0 left, 0.5 center, 1 right), integer `sync_offset_ms` (−950 to 20000), `monitoring` (`off`, `monitor_only`, `monitor_and_output`) and integer `tracks` (0–63). Bit 0 is track 1, used by all three outputs. Recording creates a stereo AAC track for each mixer through the highest assigned track, preserving gaps. Multiple sources may share a mixer. Monitor Only saves the chosen tracks and suppresses all output mixers until the mode changes. Monitoring uses the default Windows playback device.
- The `separate_audio_track` creation shortcut retains track 1 and allocates a free mixer from tracks 2–6. Optional `recording_track` selects an available mixer explicitly. Configuration import reserves manual masks and explicit legacy assignments first, then assigns unnumbered shortcuts. Existing gain/mute values from the optional `legacySourceAudio` argument apply only to legacy sources without an audio object. Saved configurations use canonical audio properties.
- Stop recording to change track assignments or to add/remove sources on tracks 2–6. Other audio controls remain live. Audio settings are applied before the source enters its scene. Streaming and replay each use track 1.
- `isMicrophone(name)`, `microphoneFilterTypes()`, `microphoneFilters(name)` and `setMicrophoneFilters(name, chain)` expose opt-in microphone-only filters on the Qt controller thread. The runtime catalog includes availability, native IDs, numeric ranges/defaults and supported suppression methods. Each `properties.filters` row has a stable unique `id` (1–64 ASCII letters/digits/underscore/hyphen), display `name`, `type` (`compressor`, `noise_gate`, `noise_suppression`), Boolean `enabled`, and a `settings` object. Missing numeric settings receive defaults; unknown settings, malformed rows, duplicate IDs, out-of-range values and reversed gate thresholds reject the entire edit/import. Maximum 32 filters per microphone. Existing configurations without this key remain unchanged.
- Chain order is audible processing order, matching `obs_source_enum_filters`; libobs's internal indices run in reverse. New private filters are staged before live mutation, and existing filter instances survive parameter, bypass and order edits. New suppression uses Speex −30 dB if advertised; v2 registration is preferred. No source, monitor or output activation is performed by filter edits. During import, structurally valid unavailable module/method rows remain dormant and round-trip unchanged; live edits cannot add unavailable filters or switch to unsupported methods. Removing, moving or bypassing an existing dormant row remains possible.
- Give `attachPreview` a native HWND from a QWidget with `WA_NativeWindow`; resize through `resizePreview`. Call `attachPreview(0,0,0)` before destroying that HWND. `setPreviewEnabled` only affects display drawing.
- Start/stop streaming, recording and replay independently. Poll `statistics()` at a modest interval and also react to `stateChanged`, `errorOccurred` and `replaySaved`. Output start is asynchronous; `relayConnected` shows actual transport connection. `saveReplay()` requests asynchronous MKV creation.
- Set delay through `setDelayMs`. Target range is 0–300000 ms; effective delay is approximate to keyframe granularity. Increasing holds output; decreasing cuts content at a keyframe. `delayDroppedPackets` includes intentional audio/video cuts, not solely bandwidth loss.
- `automationSnapshot()` supplies running processes, foreground/title and output state. Feed it to the portable `AutomationEngine`; dispatch returned actions to this API on the controller thread. Audio peaks come from native meters in `statistics().audioPeaksDb`; CPU comes from libobs process sampling. Global hotkey collection belongs to the application. Do not render fictional GPU counters.
- `loadPlugin(absoluteDll)` validates ABI v1 and exposes actions via `pluginActions`. `invokePluginAction` dispatches them. Plugins run trusted in-process, and must be explicitly selected by the user. The example DLL action requests 90 seconds of delay.

## Runtime layout

Development uses the official OBS root (`bin/64bit`, `core`, `data/libobs`). Portable builds may put executable/DLLs/helpers at package root, while retaining `core`, `data/libobs`, and an existing `bin/64bit` compatibility directory. Engine startup sets process cwd to runtime `bin/64bit` because official win-capture initializes hooks through `../../core`. It restores the previous directory at shutdown. Always use absolute application/config/output paths.

The helper executables `obs-ffmpeg-mux.exe`, `obs-nvenc-test.exe` and `obs-qsv-test.exe` must sit beside the frontend executable. Root CMake copies these beside built executables. Qt MinGW DLLs must be used instead of OBS's MSVC Qt DLLs. Only the explicitly loaded non-frontend module list is supported.

Shutdown disconnects signal handlers, stops/releases outputs and encoders, removes scene sources from OBS 33's program canvas (which holds strong references), releases the application's scene references, and drains libobs's deferred destruction queue until idle before `obs_shutdown`. Both canvas removal and queue draining are required. A single queue drain without canvas removal left a nondeterministic double-destroy error; strengthened smoke testing caught and rejected that intermediate build.

## Verified backend milestone

Native Debug build succeeded with Qt 6.8.3, MinGW 13.1 and pinned OBS 33.0.0. Tests initialized D3D11 on RTX 4070, used a real color scene and generated 440 Hz audio, and simultaneously produced MKV recording, MKV replay and dynamic-delay relay output. Software x264 and hardware NVENC both passed. The delay changed 500 → 1800 → 0 ms while live.

The final NVENC test sent RTMP to a local FFmpeg receiver (`127.0.0.1:19350`), then stopped cleanly. It reported relayError=0, effective delay about 811 ms at target zero with a one-second keyframe interval, and no unexpected OBS error-level messages. The receiver's EOF I/O diagnostic after publisher disconnect did not prevent a valid finalized MKV.

All nine generated software/NVENC/RTMP recording/replay/relay artifacts passed full FFmpeg decode, strict per-stream DTS monotonicity, video-first-keyframe and A/V end-time checks through `scripts/validate_media.py`. Detailed local reports are `work/engine-smoke3/validation.json`, `work/engine-nvenc/validation.json`, and `work/engine-rtmp/validation.json`. After the final canvas-reference fix, `work/engine-final2` passed the strengthened software smoke with no remaining sources or error-level messages, and all three media artifacts passed the same validation. These are test artifacts, not redistributable product outputs. Smoke now fails on unexpected libobs error-level messages as well as media/state failures.

## Explicit remaining limits

No external Twitch/YouTube session, network-reconnect fault test, long-duration soak, visual desktop/window/game capture validation, perceptual lip-sync measurement or OBS performance comparison has been completed in this stage. Native preview implementation awaits GUI validation. Recording supports six assignable stereo AAC tracks. Replay uses track 1. Output resolution/FPS and CBR video quality remain shared. Actual Windows application audio isolation is unverified. MP4 remux, splitting, disk-pressure monitoring after start, audio meters, global hotkeys, disk delay spill, richer plugin extension types and plugin unload controls remain work. The existing replay buffer has time/size settings but is OBS's implementation, not the core queue's exact memory accounting. Core delay caps bound queue payload capacity and packet count, not total process RSS.


## Workspace API (19 November 2026)

All workspace calls belong on the Engine Qt thread. Names are globally unique public
OBS source names (1–128 characters); scenes contain at most 64 public items and a
collection contains at most 128 scenes. The Engine owns scene references. Scene
items own sources; companion and audio-owner maps borrow pointers from those items.
Meters detach before source destruction. Both displays are detached and the preview
view/global scene/transition references released before draining OBS destruction.

- `renameScene`, `duplicateScene`, `moveScene(name, position)` manage ordered scenes.
  Duplication creates independent sources, filters and audio controls; copied source
  names use `new scene / old source`, with a numeric suffix on collisions.
  Already-global audio-only rows are omitted from the copy because the original
  global input covers both scenes; a desktop with global companion copies only its
  video capture. This avoids double capture/mixing. The original scene still owns
  that global input; removing its owner removes the shared global audio.
- `sources(scene)` returns authoritative rows with `name`, `type`, `properties`,
  `parent` (empty for root), `order` (zero-based sibling order, bottom to top),
  `visible`, `locked`, and `transform`. Build a tree using `parent`; reverse sibling
  presentation for the OBS-style topmost-first list. Do not override these values
  from an old UI visibility map. The array itself also preserves sibling order.
- `addGroup` creates a real libobs group. `moveSource(scene,name,parent,position)`
  moves and reorders items, preserving native group transforms and moving private
  display-audio companions with their owners. Positions beyond the sibling count
  append. Cyclic groups are rejected. Removing a group recursively removes children;
  it rejects locked descendants or track changes during recording before mutation.
- `renameSource` migrates audio owner, meter, global-audio and companion keys, and
  child parent references. Filter IDs/settings stay with the source. The UI should
  update its selected name and references after success.
- A source or ancestor group lock rejects removal, rename, reparent/reorder,
  transforms, capture-property and filter edits. Unlocking and visibility toggles
  remain usable. Audio mixer controls remain live. Scene deletion is independent
  of item locks.
- `sourcePropertySchema(scene,name)` returns supported native Boolean, integer,
  float, color and enumerated-list fields (`key`, `label`, `type`, numeric
  `min/max/step`, or `options:[{label,value}]`). `sourceProperties` returns current
  merged values. `setSourceProperties` accepts a patch only after checking the real
  native schema and enumerated options. It updates the existing native source.
  Capture-audio topology is chosen at creation; use the audio APIs for routing.
  Desktop companions expose an additional enumerated `audio_window` target.
- `sourceTransform` / `setSourceTransform` expose `x`, `y`, `rotation`, `scaleX`,
  `scaleY`, `boundsType`, `boundsWidth`, `boundsHeight`, `alignment`,
  `boundsAlignment`, `cropLeft`, `cropTop`, `cropRight`, `cropBottom`. Numeric ranges
  are validated, nonfinite values and unknown keys rejected. Bounds/alignment use
  the native OBS enums/bits. Changes are applied in a deferred native item update.
  Positions are parent-local. OBS automatically rebases group bounds: a child at
  local X=37 may become local X≈0 while its group moves to X=37, retaining canvas
  placement. Editors must refresh controls and their displayed baselines together;
  unchanged Apply must not resubmit an old coordinate after rebasing.
  `sourceWorldTransform(scene,name)` returns the composed native draw matrix as
  `m11,m12,m21,m22,dx,dy`: X=m11*x+m21*y+dx; Y=m12*x+m22*y+dy. It includes all
  ancestor groups. Deferred group updates settle on the next render tick, so
  immediate readback is not a promise that a pending edit has rendered.
  Native layout reads/writes and each scene's persisted parent/child snapshot use
  `obs_scene_atomic_update` to exclude concurrent group rebasing. New empty scenes
  initialize their native canvas dimensions on a private 1×1 render target before
  items are added; this prevents OBS33's first-render resize path from discarding
  saved child offsets. It neither activates capture/audio nor changes program.
  Nested group edits also update the native group draw matrix before child bounds
  can rebase it; ancestor bounds are then scheduled for recalculation. Duplication
  builds the complete hierarchy before restoring layout, since adding a child can
  resize previously created groups. Local group origins can therefore normalize
  after copy/load; rendered leaf placement is the persistence contract.
- `videoFilterTypes`, `videoFilters`, `setVideoFilters` expose native Color
  Correction, Chroma Key and Crop/Pad when registered. Chains have at most 16 rows
  `{id,name,type,enabled,settings}`; type IDs are `color_filter`,
  `chroma_key_filter`, `crop_filter`. The catalog supplies native fields/defaults.
  Rows are in processing order, like microphone filters. New native instances are
  staged before replacing the old chain. Unsupported types/settings reject the
  change/import; video filters are saved under `properties.video_filters`.

`setStudioMode(true)` makes `selectScene` edit preview without changing program.
`programScene()` identifies the current take target; `takePreview()` uses the
configured transition, or accepts `"cut"`/`"fade"` explicitly. `setTransition(type,
ms)` accepts 50–20000 ms. A second fade is rejected while a fade is active; Cut can
interrupt it. Disabling Studio cuts the selected preview to program. Outside Studio,
selecting a scene takes it with the configured transition. `statistics().transitionActive`
reports actual native transition state. The preview-only view is never registered
as an audio/video output mix, so scene-scoped preview audio is not encoded.

Use `attachPreview`/`resizePreview` and `attachProgram`/`resizeProgram` with separate
native HWNDs; pass zero to detach before destroying either window. Normal preview
renders program, Studio preview renders the edit scene, and the program display
always renders output. `setPreviewEnabled` disables only preview drawing; the UI
owns its disabled-preview overlay. Program and outputs continue.

Audio properties also accept `scope` (`scene`, default, or `global`) and
`mixer_hidden` (Boolean UI preference). Global routes the same audio source through
a private audio-only scene on output channel 1; OBS deduplicates sources also present
in program. It remains audible off-scene and independent of scene visibility (mute
it to silence). Supported for microphone, desktop audio, application audio, test tone
and a desktop source's private audio companion. Window/game video sources reject
Global; add an Application Audio source for that use. Changing scope updates native
activation. Removing the owner removes global routing. `mixer_hidden` does not mute.

`statistics()` adds measured `cpuPercent`, `streamDurationMs`, `recordingDurationMs`,
`replayDurationMs`, `programScene`, `studioMode`, and `transitionActive`. Durations
begin on genuine OBS output start signals and are zero while stopped; reconnect
signals do not reset them. Existing actual FPS, frame-time, byte and audio-meter
fields remain available. CPU is sampled at most once per 500 ms and finite samples
are cached for all readers; rapid mixer/status polling does not query the native
CPU sampler back-to-back. Zero is the initial value before the first sample.

### Persisted collection

Version 1 remains backward compatible. Optional top-level `programScene` defaults to
`selectedScene`. Settings add `studioMode`, `transition`, `transitionDurationMs`.
The ordered `scenes` array contains `{name,sources}`. Source rows retain their existing
`name/type/properties` and add `parent`, `order`, `visible`, `locked`, `transform`.
Groups use `type:"group"`; all rows remain in the scene's flat source array with
parent names describing hierarchy. Missing layout fields retain prior defaults.
The array order is authoritative; `order` is a derived sibling index. Audio and filter
state remains inside `properties` as described above.

Saving uses QSaveFile. Import first validates names, groups, cycles, layout, filters
and audio, then constructs a candidate while retaining the original graph and its
meters/output references. A native construction failure destroys the candidate and
restores the previous graph/settings/names. Only complete success replaces the
collection. Outputs must be stopped before import/video settings changes. Stream
keys remain outside this JSON.
