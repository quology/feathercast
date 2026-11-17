param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$Python = 'python',
    [switch]$SkipBootstrap,
    [switch]$CoreOnly,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    if (-not $SkipBootstrap) {
        & $Python scripts/bootstrap.py
        if ($LASTEXITCODE -ne 0) { throw 'Dependency bootstrap failed.' }
    }
    $manifest = Get-Content -Raw -LiteralPath work/deps/manifest.json | ConvertFrom-Json
    $toolScripts = Join-Path $projectRoot 'work/toolenv/Scripts'
    $env:PATH = $manifest.qt_prefix + '/bin;' + $manifest.compiler_bin + ';' + $manifest.obs_runtime + '/bin/64bit;' + $env:PATH
    $buildDirectory = Join-Path $projectRoot "build/$Configuration"
    $appEnabled = if ($CoreOnly) { 'OFF' } else { 'ON' }
    $arguments = @('-S', $projectRoot, '-B', $buildDirectory, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_PREFIX_PATH=$($manifest.qt_prefix)",
        "-DCMAKE_CXX_COMPILER=$($manifest.compiler_bin)/g++.exe",
        "-DCMAKE_C_COMPILER=$($manifest.compiler_bin)/gcc.exe",
        "-DCMAKE_MAKE_PROGRAM=$toolScripts/ninja.exe",
        "-DFEATHERCAST_BUILD_APP=$appEnabled", "-DFEATHERCAST_BUILD_ENGINE=$appEnabled",
        '-DBUILD_TESTING=ON')
    & "$toolScripts/cmake.exe" @arguments
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & "$toolScripts/cmake.exe" --build $buildDirectory --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
    if (-not $SkipTests) {
        & "$toolScripts/ctest.exe" --test-dir $buildDirectory --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'CTest failed.' }
    }
}
finally { Pop-Location }
