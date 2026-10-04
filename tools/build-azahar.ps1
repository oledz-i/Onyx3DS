# SPDX-License-Identifier: GPL-3.0-or-later
# Builds Azahar's libretro core as a static library for UWP (x64) and merges
# every library it needs into deps\lib\azahar_all.lib.
param([string]$Configuration = "Release")
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$src = Join-Path $Deps "azahar"
$build = Join-Path $DepsBuild "azahar"

Write-Step "Configuring Azahar for WindowsStore (UWP)"
$cmakeArgs = @(
    "-S", $src, "-B", $build,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_SYSTEM_NAME=WindowsStore", "-DCMAKE_SYSTEM_VERSION=10.0",
    "-DENABLE_LIBRETRO=ON", "-DLIBRETRO_STATIC_CORE=ON",
    "-DENABLE_QT=OFF", "-DENABLE_SDL2=OFF", "-DENABLE_WEB_SERVICE=OFF",
    "-DENABLE_SCRIPTING=OFF", "-DENABLE_GDBSTUB=OFF", "-DENABLE_CUBEB=OFF",
    "-DENABLE_OPENAL=OFF", "-DENABLE_LIBUSB=OFF", "-DENABLE_ROOM=OFF",
    "-DENABLE_ROOM_STANDALONE=OFF", "-DENABLE_TESTS=OFF",
    "-DENABLE_OPENGL=OFF", "-DENABLE_VULKAN=ON", "-DENABLE_SOFTWARE_RENDERER=ON",
    "-DENABLE_DISCORD_RPC=OFF", "-DENABLE_LTO=OFF",
    "-DCITRA_WARNINGS_AS_ERRORS=OFF", "-DCITRA_USE_PRECOMPILED_HEADERS=ON",
    # UWP only hands out W^X JIT memory (patches\dynarmic)
    "-DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON",
    "-DCMAKE_PROJECT_INCLUDE=$((Join-Path $Root 'tools\uwp-flags.cmake') -replace '\\','/')"
)
Invoke-Checked cmake @cmakeArgs

Write-Step "Building Azahar ($Configuration)"
Invoke-Checked cmake --build $build --config $Configuration --target citra_libretro --parallel

Write-Step "Merging static libraries"
$libs = Get-ChildItem $build -Recurse -Filter *.lib |
    Where-Object { $_.FullName -match "\\$Configuration\\" -and $_.Name -notmatch "test" } |
    Select-Object -ExpandProperty FullName
if ($libs.Count -eq 0) { throw "No libraries found under $build" }
$out = Join-Path $DepsLib "azahar_all.lib"
$rsp = Join-Path $build "merge.rsp"
Set-Content $rsp (($libs | ForEach-Object { "`"$_`"" }) -join "`n")
Invoke-Checked lib.exe /NOLOGO /IGNORE:4006 /IGNORE:4221 "/OUT:$out" "@$rsp"
Write-Host "Wrote $out from $($libs.Count) libraries" -ForegroundColor Green
