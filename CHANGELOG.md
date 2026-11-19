# Changelog

## 0.1.0-dev.4-workspace-r1 — development preview, 19 November 2026

- Fixed live output timers to latch first native activation, retain elapsed time through relay reconnects and return zero after stop.
- Added native scene ordering, duplication and renaming; source groups, ordering, visibility, locking, properties, transforms and video filters.
- Added an audio mixer using measured libobs peaks and actual gain, mute, monitoring and scene/global settings; added Studio Mode, transitions, disabled-preview recovery and measured status.
- Added a resettable dock layout with four Twitch docks and optional activity permissions. Virtual camera is not included.
- Fixed grouped-source repeated Apply and immediate-save/reload placement; native group origins may rebase while world placement remains fixed. CPU samples are cached for at least 500 ms.
- Release CTest 5/5, coordinate 9/9, workspace UI 14/14 and restart 4/4 passed; source checkpoint 4dcf30f [CI passed](https://github.com/quology/feathercast/actions/runs/44560702657).


## Unreleased

- Added a compact eight-menu workspace bar with dock toggles and existing scene, source, settings and tool actions.
- Rebranded the desktop app as VELA (Video Encoding and Livestreaming Application), with a new Windows icon and `VELA.exe` package entry point. Existing profiles and stream keys are retained.
- Added native Twitch channel and chat docks with public device authorization, Windows-protected tokens, editable channel metadata, live status, and text chat.

- Added opt-in microphone filters using native libobs Compressor, Noise Gate and Noise Suppression. The themed Filters dialog includes bypass, add/remove, processing order, defaults, Speex level and runtime-supported RNNoise. Filter settings and order persist; unavailable optional filters remain saved and visibly dormant.
- Added Advanced Audio Properties for sources across all scenes, with live status, dB/percent gain, mute, mono, stereo balance, signed sync offset, monitoring and six independent track assignments. Stream and replay use track 1. Existing isolated-track and UI gain settings migrate into the scene configuration.
- Added optional application audio to display/window/game capture, an Application audio source, and separate source recording tracks alongside the normal mix (up to five isolated tracks). Source controls, meters, scene lifecycle and saved configuration include capture audio; streaming/replay keep the mixed track.
- Refreshed the Qt workspace with neutral light/dark themes, monochrome primary actions, preview framing and compact tabbed monitoring docks.
- Added settings category navigation, clearer automation grouping and descriptive scene/source controls while preserving saved dock identities and existing actions.

## 0.1.0-dev — development preview, 18 November 2026

- Added standalone libobs/D3D11 engine, shared H.264/AAC outputs, MKV recording, replay and RTMP(S) relay.
- Added bounded live delay, keyframe cuts, timestamp rebasing, pacing and reconnect; 13 portable core scenarios passed.
- Added dockable Qt UI, device options, real audio peaks, settings/layout persistence, themes and module gates.
- Added visual priority automation, ordered actions, timing/override/fallback and transactional JSON import/export; disabled application examples.
- Added global hotkeys, DPAPI secrets and atomic settings; fixed native callback null-result handling and invalid configuration scene preservation.
- Added asynchronous non-reencoding MP4 remux with temporary output and preservation checks.
- Added native plugin action ABI and working 90-second delay example.
- Verified synthetic x264/NVENC/local RTMP, receiver failure/reconnect, Debug/Release CTest and Debug UI checks.

Baseline CI passed. Final-source package/CI sign-off is tracked in [TEST_RESULTS.md](TEST_RESULTS.md). External platforms, visual game capture, gaming performance, full-disk safety and public license/source audit remain unverified. No stable release has been published.
