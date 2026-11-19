# Third-party components and redistribution

VELA's own source is GPL-3.0-or-later. The development package dynamically
links the selected components below. Preserve each component's original notices;
this file does not replace their licenses or complete corresponding-source duties.

| Component | Version/build | License and source |
|---|---|---|
| OBS/libobs and selected capture/encoder/output modules | 33.0.0, source commit `02acc6a4cf08eba4ce2eab9342c5dd305b41e9e5` | GPL-2.0-or-later; [source archive](https://github.com/obsproject/obs-studio/releases/download/33.0.0/OBS-Studio-33.0.0-Sources.tar.gz), [build sources](https://github.com/obsproject/obs-studio/tree/33.0.0), [OBS dependency build scripts](https://github.com/obsproject/obs-deps) |
| Qt Core, Gui, Widgets and runtime platform plugins | 6.8.3, MinGW x64 | Open-source LGPLv3/GPLv3 options and component notices; [Qt licensing](https://doc.qt.io/qt-6/licensing.html), [Qt Base source](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz) |
| Qt Network and Qt WebSockets (Twitch integration) | 6.8.3, MinGW x64 | [Qt Base source](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz), [Qt WebSockets source and notices](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtwebsockets-everywhere-src-6.8.3.tar.xz) |
| FFmpeg shared libraries bundled by OBS | Runtime reports `n8.1.2`, avformat/avcodec major 62, avutil major 60 | Runtime `avutil_license()` reports **GPL version 3 or later**; [FFmpeg sources](https://ffmpeg.org/releases/), [OBS FFmpeg build configuration](https://github.com/obsproject/obs-deps) |
| FFmpeg public headers used for compilation | 8.0 | Compatible 8.x public ABI majors; source archive SHA-256 pinned in bootstrap; source license files copied to package |
| MinGW GCC runtime | 13.1.0 | GNU runtime libraries and applicable GCC Runtime Library Exception; [GCC licensing](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html), [MinGW toolchain](https://github.com/niXman/mingw-builds-binaries) |
| Other OBS binary dependencies | Original versions from verified OBS archive | Includes x264, zlib, pthreads, SRT/RIST and other linked components. Preserve their applicable notices and corresponding source/build material from OBS dependency releases. |

The runtime FFmpeg configuration was queried directly on 17 November 2026. It
includes `--enable-version3 --enable-gpl --enable-libx264 --enable-shared`; it must
not be described as an LGPL-only FFmpeg build.

The Qt SDK supplies SPDX files under `sbom/`; packaging includes available Qt SPDX
and notices. Dynamic replacement of the Qt DLLs remains possible. The package
does not include OBS's Qt frontend, Chromium browser module, Scene Switcher code,
or InstantDelay proprietary source/assets.

The separately downloaded FFmpeg/ffprobe command-line tools are test dependencies
only and are not included in the application package. They are Gyan's 8.1.2
essentials build, verified against its published SHA-256.

Before a public binary release, finish an inventory of every shipped DLL, copy
all dependency-specific copyright/license notices, and provide complete matching
corresponding source and build material using a compliant distribution method.
The private development archive is not advertised as having completed that public
redistribution audit. Do not publish it as a stable public release until reviewed.
