"""Provision local, pinned Windows dependencies without administrator privileges."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / "work/deps"
QT_VERSION = "6.8.3"
QT_SOURCE_SHA256 = "56001b905601bb9023d399f3ba780d7fa940f3e4861e496a7c490331f49e0b80"
QT_WEBSOCKETS_SOURCE_SHA256 = "fc6310c1b12d6e22a31062727aa6574ed69239d036c8a4247b3b089902709b11"
OBS_VERSION = "33.0.0"
OBS_SOURCE_COMMIT = "02acc6a4cf08eba4ce2eab9342c5dd305b41e9e5"
OBS_ZIP_SHA256 = "3369a3ecd67b5669a8a08d77357f87cc786d1fba42fbbd1054d841a69f6271a7"
OBS_SOURCE_SHA256 = "c1ae3c98c1bb70ebdced33e894a526fcbbbda9bfd95806970a3fc31f74b08f6c"
FFMPEG_HEADERS_VERSION = "8.0"
FFMPEG_SHA256 = "b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e"


def run(args, **kwargs):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def download(url: str, destination: Path, expected: str):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or hashlib.file_digest(destination.open("rb"), "sha256").hexdigest() != expected:
        temporary = destination.with_suffix(destination.suffix + ".part")
        print(f"Downloading {url}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        with temporary.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            temporary.unlink()
            raise RuntimeError(f"Checksum mismatch for {destination.name}: {actual}")
        temporary.replace(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools-ready", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("The full application bootstrap targets Windows x64. Core-only CMake builds are portable.")
    if not args.tools_ready:
        python = ROOT / "work/toolenv/Scripts/python.exe"
        if not python.exists():
            run([sys.executable, "-m", "venv", ROOT / "work/toolenv"])
        run([python, "-m", "pip", "install", "--disable-pip-version-check", "-r", ROOT / "requirements-build.txt"])
        run([python, __file__, "--tools-ready"])
        return

    DEPS.mkdir(parents=True, exist_ok=True)
    qt = DEPS / f"Qt/{QT_VERSION}/mingw_64"
    compiler = DEPS / "Qt/Tools/mingw1310_64/bin"
    if not (qt / "lib/cmake/Qt6/Qt6Config.cmake").exists():
        run([sys.executable, "-m", "aqt", "install-qt", "windows", "desktop", QT_VERSION, "win64_mingw", "--archives", "qtbase", "qttools", "--outputdir", DEPS / "Qt"])
    if not (qt / "lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake").exists():
        run([sys.executable, "-m", "aqt", "install-qt", "windows", "desktop", QT_VERSION, "win64_mingw", "-m", "qtwebsockets", "--archives", "qtwebsockets", "--outputdir", DEPS / "Qt"])
    if not (compiler / "g++.exe").exists():
        run([sys.executable, "-m", "aqt", "install-tool", "windows", "desktop", "tools_mingw1310", "qt.tools.win64_mingw1310", "--outputdir", DEPS / "Qt"])
    qt_licenses = DEPS / "qt-source-licenses"
    for module, digest in (("qtbase", QT_SOURCE_SHA256), ("qtwebsockets", QT_WEBSOCKETS_SOURCE_SHA256)):
        source_name = f"{module}-everywhere-src-{QT_VERSION}"
        qt_source = DEPS / f"{source_name}.tar.xz"
        download(f"https://download.qt.io/archive/qt/6.8/{QT_VERSION}/submodules/{qt_source.name}", qt_source, digest)
        if not (qt_licenses / source_name / "LICENSES").exists():
            qt_licenses.mkdir(exist_ok=True)
            with tarfile.open(qt_source) as package:
                members = [entry for entry in package.getmembers() if "/LICENSES/" in entry.name]
                package.extractall(qt_licenses, members=members, filter="data")

    runtime = DEPS / "obs-runtime"
    archive = DEPS / "obs-runtime.zip"
    download(f"https://github.com/obsproject/obs-studio/releases/download/{OBS_VERSION}/OBS-Studio-{OBS_VERSION}-Windows-x64.zip", archive, OBS_ZIP_SHA256)
    if not (runtime / "bin/64bit/obs.dll").exists():
        with zipfile.ZipFile(archive) as package:
            package.extractall(runtime)
    source = DEPS / "obs-source"
    if not (source / "libobs/obs.h").exists():
        source_archive = DEPS / "obs-source.tar.gz"
        download(f"https://github.com/obsproject/obs-studio/releases/download/{OBS_VERSION}/OBS-Studio-{OBS_VERSION}-Sources.tar.gz", source_archive, OBS_SOURCE_SHA256)
        extracted = DEPS / "obs-source-unpack"
        extracted.mkdir(exist_ok=True)
        with tarfile.open(source_archive) as package:
            package.extractall(extracted, filter="data")
        candidates = [p.parent for p in extracted.rglob("libobs/obs.h")]
        if len(candidates) != 1:
            raise RuntimeError("Unexpected OBS source archive structure")
        shutil.copytree(candidates[0].parent, source, dirs_exist_ok=True)
    ffmpeg = DEPS / f"ffmpeg-{FFMPEG_HEADERS_VERSION}"
    ffmpeg_archive = DEPS / f"ffmpeg-{FFMPEG_HEADERS_VERSION}.tar.xz"
    download(f"https://ffmpeg.org/releases/ffmpeg-{FFMPEG_HEADERS_VERSION}.tar.xz", ffmpeg_archive, FFMPEG_SHA256)
    if not (ffmpeg / "libavformat/avformat.h").exists():
        with tarfile.open(ffmpeg_archive) as package:
            package.extractall(DEPS, filter="data")
    # FFmpeg keeps public structures ABI-compatible within these major versions.
    (ffmpeg / "libavutil/avconfig.h").write_text("#pragma once\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n", encoding="utf-8")
    sdk = DEPS / "obs-sdk"
    (sdk / "include").mkdir(parents=True, exist_ok=True)
    (sdk / "lib").mkdir(exist_ok=True)
    (sdk / "include/obsconfig.h").write_text('#pragma once\n#define OBS_DATA_PATH "data"\n#define OBS_PLUGIN_PATH "core"\n#define OBS_PLUGIN_DESTINATION "core"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n#define OBS_INSTALL_PREFIX "."\n', encoding="utf-8")
    environment = dict(os.environ, PATH=str(compiler) + os.pathsep + os.environ["PATH"])
    for dll, library in [("obs.dll", "obs"), ("avformat-62.dll", "avformat"), ("avcodec-62.dll", "avcodec"), ("avutil-60.dll", "avutil")]:
        definition = sdk / f"lib/{library}.def"
        with definition.open("w", encoding="utf-8") as output:
            run([compiler / "gendef.exe", "-", runtime / "bin/64bit" / dll], stdout=output, env=environment)
        run([compiler / "dlltool.exe", "-d", definition, "-l", sdk / f"lib/lib{library}.dll.a", "-D", dll], env=environment)
    manifest = {"qt": QT_VERSION, "compiler": "MinGW 13.1.0", "obs": OBS_VERSION, "obs_source_commit": OBS_SOURCE_COMMIT, "obs_runtime_sha256": OBS_ZIP_SHA256, "ffmpeg_headers": FFMPEG_HEADERS_VERSION, "ffmpeg_runtime_abi": {"avformat": 62, "avcodec": 62, "avutil": 60}, "qt_prefix": str(qt), "compiler_bin": str(compiler), "obs_source": str(source), "obs_runtime": str(runtime), "obs_sdk": str(sdk), "ffmpeg_headers_path": str(ffmpeg)}
    (DEPS / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Dependencies ready:", DEPS / "manifest.json", flush=True)


if __name__ == "__main__":
    main()
