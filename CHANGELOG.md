# Changelog

## Unreleased

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
