# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the mGBA libretro core (GBA) as mgba_libretro.dll for UWP x64 and copies it to
# deps\bin. The GBA core is optional: CI runs this step with continue-on-error, and when the
# DLL is missing the app hides GBA.
#   source   github.com/libretro/mgba at the commit in vendor\mgba\BASE_COMMIT
#   build    mGBA's own CMake (BUILD_LIBRETRO), with every optional dependency switched off
param([string]$Configuration = "Release")
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$commit = (Get-Content (Join-Path $Root "vendor\mgba\BASE_COMMIT") -Raw).Trim()
$src = Join-Path $Deps "mgba"
$build = Join-Path $DepsBuild "mgba"
if (!(Test-Path "$src\.git")) {
    Write-Step "Fetching mGBA $commit"
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    Invoke-Checked git -C $src init --quiet
    Invoke-Checked git -C $src fetch --depth 1 https://github.com/libretro/mgba.git $commit
    Invoke-Checked git -C $src checkout --quiet --force FETCH_HEAD
}
$head = (git -C $src rev-parse HEAD).Trim()
if ($head -ne $commit) { throw "mGBA checkout is at $head, expected $commit" }

Write-Step "Configuring mGBA for WindowsStore (UWP)"
$off = "ENABLE_SCRIPTING","ENABLE_GDB_STUB","ENABLE_DEBUGGERS","USE_EDITLINE","USE_FFMPEG","USE_ZLIB",
       "USE_MINIZIP","USE_PNG","USE_LIBZIP","USE_SQLITE3","USE_ELF","USE_LUA","USE_JSON_C","USE_FREETYPE",
       "USE_LZMA","USE_DISCORD_RPC","USE_EPOXY","BUILD_QT","BUILD_SDL","BUILD_GL","BUILD_GLES2","BUILD_GLES3",
       "BUILD_STATIC","BUILD_TEST","BUILD_SUITE","BUILD_HEADLESS","BUILD_EXAMPLE","BUILD_PERF"
$cmakeArgs = @(
    "-S", ($src -replace '\\','/'), "-B", $build,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_SYSTEM_NAME=WindowsStore", "-DCMAKE_SYSTEM_PROCESSOR=AMD64", "-DCMAKE_SYSTEM_VERSION=10.0",
    "-DBUILD_LIBRETRO=ON", "-DSKIP_LIBRARY=ON", "-DBUILD_SHARED=ON", "-DM_CORE_GBA=ON", "-DM_CORE_GB=OFF",
    "-DDISABLE_DEPS=ON", "-DLIBMGBA_ONLY=OFF",
    "-DCMAKE_PROJECT_INCLUDE=$((Join-Path $Root 'tools\uwp-flags.cmake') -replace '\\','/')"
)
foreach ($o in $off) { $cmakeArgs += "-D$o=OFF" }
Invoke-Checked cmake @cmakeArgs

Write-Step "Building mGBA ($Configuration)"
Invoke-Checked cmake --build $build --config $Configuration --target mgba_libretro --parallel

$dll = Get-ChildItem $build -Recurse -Filter mgba_libretro.dll | Where-Object { $_.FullName -match "\\$Configuration\\" } |
    Select-Object -First 1
if (!$dll) { throw "mgba_libretro.dll was not produced" }

# The package check (tools\verify-package.ps1) fails the whole build when a shipped DLL imports
# anything outside the UWP surface. The GBA core is optional, so refuse it here instead.
Write-Step "Checking mgba_libretro.dll only imports what Xbox has"
$sdkLib = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib\*\um\x64\WindowsApp.lib" |
    Sort-Object FullName -Descending | Select-Object -First 1
$allowed = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($l in (& dumpbin /headers $sdkLib.FullName)) {
    if ($l -match '^\s+Symbol name\s*:\s*(\S+)') {
        [void]$allowed.Add((($Matches[1] -replace '^__imp_', '' -replace '^_', '') -replace '@\d+$', ''))
    }
}
$provided = "VCRUNTIME140_APP.dll","VCRUNTIME140_1_APP.dll","MSVCP140_APP.dll","MSVCP140_1_APP.dll","MSVCP140_2_APP.dll"
$bad = @(); $cur = $null
foreach ($l in (& dumpbin /imports $dll.FullName)) {
    if ($l -match '^\s+Summary\s*$') { break }
    if ($l -match '^\s{4}(\S+\.dll)\s*$') { $cur = $Matches[1]; continue }
    if (!$cur -or $provided -contains $cur -or $cur -match '^api-ms-win-crt-') { continue }
    if ($l -match '^\s+[0-9A-F]+\s+(\S+)\s*$') {
        if (!$allowed.Contains($Matches[1])) { $bad += "$cur!$($Matches[1])" }
    }
}
if ($bad) { throw "mgba_libretro.dll imports functions Xbox does not have: $(($bad | Sort-Object -Unique) -join ' ')" }

Copy-Item $dll.FullName (Join-Path $DepsBin "mgba_libretro.dll") -Force
Write-Host "Wrote $(Join-Path $DepsBin 'mgba_libretro.dll')" -ForegroundColor Green
