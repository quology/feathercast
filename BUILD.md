# Building FeatherCast

Target: Windows 11 x64. Python 3.12+, Git and network access are required for the
first build. Dependencies install inside `work/`; no administrator access or
separately installed OBS instance is needed.

```powershell
python scripts/bootstrap.py
./scripts/build.ps1 -SkipBootstrap -Configuration Release
./scripts/build.ps1 -SkipBootstrap -Configuration Debug
work/toolenv/Scripts/python.exe scripts/package.py --version 0.1.0-dev
```

Use `-Python <absolute-path-to-python.exe>` when Python is not on PATH. A Codex
bundled Python interpreter works too. `work/deps/manifest.json` records resolved
paths. Bootstrap pins Qt 6.8.3, MinGW 13.1, OBS 33.0.0 and the public FFmpeg 8.x ABI
headers; OBS and FFmpeg source/runtime downloads are SHA-256 checked. Qt archives
are installed through pinned aqtinstall using the Qt repository's metadata checks.
The OBS binary runtime currently reports FFmpeg n8.1.2; public structure ABI majors
62/62/60 are compatible with the pinned 8.0 headers.

The app uses the libobs C ABI from MinGW and never shares C++ objects with the
MSVC-built OBS modules. General OBS frontend/Qt plugin compatibility is not promised.
The package excludes the OBS frontend, browser runtime and its MSVC Qt DLLs.

For a portable core-only build on a system with CMake and a C++20 compiler:

```sh
cmake -S . -B build/core -DFEATHERCAST_BUILD_APP=OFF -DFEATHERCAST_BUILD_ENGINE=OFF -DBUILD_TESTING=ON
cmake --build build/core
ctest --test-dir build/core --output-on-failure
```

Native media integration tests require a Windows graphics device. A hosted runner
can compile the application and run portable core tests, but passing compilation
does not establish real capture or hardware-encoder support. Record those results
separately in `TEST_RESULTS.md`. Development artifacts are not stable releases.

For source-audio and multitrack integration checks, run `feathercast_source_audio_smoke.exe OBS_RUNTIME OUTPUT_DIRECTORY` from the build directory with the Qt/compiler/OBS runtime DLL directories on PATH. It uses nonexistent app targets for capture ownership checks and synthetic tones for recordings; it does not capture a real user's application audio. Then run `python scripts/validate_source_audio.py OUTPUT_DIRECTORY/isolated.mkv --report OUTPUT_DIRECTORY/isolation.json` with FFmpeg/FFprobe on PATH to verify that the isolated track excludes the other test tone. `scripts/validate_media.py` checks and decodes every audio/video track.

The same native fixture also writes `routing.mkv` with 440 Hz on tracks 1 and 3 and 880 Hz on tracks 2 and 3. To exercise the production dialog on a Windows desktop, run `feathercast_advanced_audio_ui_smoke.exe OBS_RUNTIME OUTPUT_DIRECTORY`. It checks live edits, unit conversion, monitoring controls, each routing checkbox, active filtering, focus retention, recording locks and the Close button. It saves `advanced-audio.png` using the application's dark theme. Monitoring checks use muted synthetic sources.

Verify the independent and shared tracks with `python scripts/validate_source_audio.py OUTPUT_DIRECTORY/routing.mkv --routing --report OUTPUT_DIRECTORY/routing-validation.json`.
