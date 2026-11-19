"""Create a standalone development portable archive from a validated Qt build."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--configuration", default="Release", choices=["Debug", "Release"])
    parser.add_argument("--version", default="0.1.0-dev")
    args = parser.parse_args()
    manifest = json.loads((ROOT / "work/deps/manifest.json").read_text())
    build = ROOT / "build" / args.configuration
    executables = list(build.rglob("VELA.exe"))
    if len(executables) != 1:
        raise SystemExit("Build exactly one VELA.exe before packaging.")
    name = f"VELA-{args.version}-win64"
    stage = ROOT / "dist" / name
    if stage.exists():
        raise SystemExit(f"Refusing to merge with an existing stage: {stage}. Use a new version or remove that named stage explicitly.")
    stage.mkdir(parents=True)
    # OBS win-capture resolves its signed hook resources from ../../core relative
    # to the process working directory; the engine selects this directory.
    (stage / "bin/64bit").mkdir(parents=True)
    (stage / "bin/64bit/.keep").write_text("")
    shutil.copy2(executables[0], stage / "VELA.exe")
    remuxers = list(build.rglob("feathercast_remux.exe"))
    if len(remuxers) != 1:
        raise SystemExit("Build the feathercast_remux helper before packaging.")
    shutil.copy2(remuxers[0], stage / "feathercast_remux.exe")
    runtime = Path(manifest["obs_runtime"])
    # libobs and codec/graphics dependencies; never package the OBS frontend or
    # its incompatible MSVC Qt libraries into the MinGW Qt application.
    for binary in (runtime / "bin/64bit").iterdir():
        if binary.is_file() and binary.suffix.lower() in (".dll", ".exe") and not binary.name.startswith("Qt") and binary.name not in ("obs64.exe", "obs-frontend-api.dll", "obs-scripting.dll"):
            shutil.copy2(binary, stage / binary.name)
    modules = ["win-capture", "win-wasapi", "image-source", "obs-transitions", "obs-filters", "obs-outputs", "obs-ffmpeg", "obs-x264", "obs-nvenc", "obs-qsv11", "rtmp-services"]
    for module in modules:
        source = runtime / "core" / module
        if source.is_dir():
            shutil.copytree(source, stage / "core" / module, ignore=shutil.ignore_patterns("*.pdb"))
    shutil.copytree(runtime / "data/libobs", stage / "data/libobs")
    plugins = list(build.rglob("feathercast_example_plugin.dll"))
    if plugins:
        (stage / "plugins").mkdir()
        shutil.copy2(plugins[0], stage / "plugins" / plugins[0].name)
    deploy = Path(manifest["qt_prefix"]) / "bin/windeployqt.exe"
    deploy_env = dict(os.environ, PATH=str(deploy.parent) + os.pathsep + manifest["compiler_bin"] + os.pathsep + os.environ.get("PATH", ""))
    subprocess.run([str(deploy), "--release" if args.configuration == "Release" else "--debug", "--compiler-runtime", "--no-translations", "--no-opengl-sw", str(stage / "VELA.exe")], check=True, env=deploy_env)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    twitch_option = next((line.partition("=")[2] for line in cache.splitlines()
                          if line.startswith("FEATHERCAST_TWITCH:BOOL=")), "")
    twitch_enabled = twitch_option.upper() in {"1", "ON", "YES", "TRUE", "Y"}
    if twitch_enabled:
        debug_suffix = "d" if args.configuration == "Debug" else ""
        for dependency in (f"Qt6Network{debug_suffix}.dll", f"Qt6WebSockets{debug_suffix}.dll", f"tls/qschannelbackend{debug_suffix}.dll"):
            if not (stage / dependency).is_file():
                raise SystemExit(f"Twitch runtime dependency missing from staged package: {dependency}")
    # windeployqt's toolchain detection is not guaranteed outside Qt Creator.
    for name_ in ("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"):
        shutil.copy2(Path(manifest["compiler_bin"]) / name_, stage / name_)
    for doc in ("LICENSE", "README.md", "BUILD.md", "TEST_RESULTS.md", "ARCHITECTURE.md"):
        if (ROOT / doc).exists():
            shutil.copy2(ROOT / doc, stage / doc)
    shutil.copytree(ROOT / "docs", stage / "docs")
    (stage / "DEPENDENCIES.json").write_text(json.dumps({k: v for k, v in manifest.items() if not k.endswith(("prefix", "bin", "source", "runtime", "sdk", "path"))}, indent=2) + "\n")
    notices = stage / "licenses"
    notices.mkdir()
    source_root = Path(manifest["obs_source"])
    shutil.copy2(source_root / "COPYING", notices / "OBS-COPYING.txt")
    ffmpeg_root = Path(manifest["ffmpeg_headers_path"])
    for text in ffmpeg_root.glob("COPYING*"):
        shutil.copy2(text, notices / f"FFmpeg-{text.name}.txt")
    qt_licenses = Path(manifest["qt_prefix"]) / "LICENSES"
    if qt_licenses.is_dir():
        shutil.copytree(qt_licenses, notices / "Qt")
    source_licenses = ROOT / f'work/deps/qt-source-licenses/qtbase-everywhere-src-{manifest["qt"]}/LICENSES'
    if not source_licenses.is_dir():
        raise SystemExit("Qt license inventory missing: rerun scripts/bootstrap.py before packaging.")
    shutil.copytree(source_licenses, notices / "Qt-source-licenses", dirs_exist_ok=True)
    if twitch_enabled:
        websockets_licenses = ROOT / f'work/deps/qt-source-licenses/qtwebsockets-everywhere-src-{manifest["qt"]}/LICENSES'
        if not websockets_licenses.is_dir():
            raise SystemExit("Qt WebSockets license inventory missing: rerun scripts/bootstrap.py before packaging.")
        shutil.copytree(websockets_licenses, notices / "Qt-WebSockets-source-licenses")
    shutil.copytree(Path(manifest["qt_prefix"]) / "sbom", notices / "Qt-SPDX")
    archive_path = ROOT / "outputs" / f"{name}.zip"
    archive_path.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in stage.rglob("*"):
            if file.is_file():
                archive.write(file, file.relative_to(stage.parent))
    with zipfile.ZipFile(archive_path) as archive:
        assert archive.testzip() is None
    with archive_path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    archive_path.with_suffix(".zip.sha256").write_text(f"{digest}  {archive_path.name}\n")
    print(archive_path)
    print("SHA-256:", digest)


if __name__ == "__main__":
    main()
