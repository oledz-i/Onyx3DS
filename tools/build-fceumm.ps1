# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the FCEUmm libretro core (NES) as fceumm_libretro.dll for UWP x64 and
# copies it to deps\bin. The NES core is optional: CI runs this step with
# continue-on-error, and when the DLL is missing the app hides NES.
#   source   github.com/libretro/libretro-fceumm at the commit in vendor\fceumm\BASE_COMMIT
#   build    tools\fceumm\CMakeLists.txt (source list from the core's Makefile.common)
param([string]$Configuration = "Release")
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$commit = (Get-Content (Join-Path $Root "vendor\fceumm\BASE_COMMIT") -Raw).Trim()
$src = Join-Path $Deps "fceumm"
$build = Join-Path $DepsBuild "fceumm"
if (!(Test-Path "$src\.git")) {
    Write-Step "Fetching FCEUmm $commit"
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    Invoke-Checked git -C $src init --quiet
    Invoke-Checked git -C $src fetch --depth 1 https://github.com/libretro/libretro-fceumm.git $commit
    Invoke-Checked git -C $src checkout --quiet --force FETCH_HEAD
}
$head = (git -C $src rev-parse HEAD).Trim()
if ($head -ne $commit) { throw "FCEUmm checkout is at $head, expected $commit" }

Write-Step "Configuring FCEUmm for WindowsStore (UWP)"
$cmakeArgs = @(
    "-S", (Join-Path $Root "tools\fceumm"), "-B", $build,
    "-G", "Visual Studio 17 2022", "-A", "x64",
    "-DCMAKE_SYSTEM_NAME=WindowsStore", "-DCMAKE_SYSTEM_PROCESSOR=AMD64", "-DCMAKE_SYSTEM_VERSION=10.0",
    "-DFCEUMM_SRC=$($src -replace '\\','/')",
    "-DCMAKE_PROJECT_INCLUDE=$((Join-Path $Root 'tools\uwp-flags.cmake') -replace '\\','/')"
)
Invoke-Checked cmake @cmakeArgs

Write-Step "Building FCEUmm ($Configuration)"
Invoke-Checked cmake --build $build --config $Configuration --parallel

$dll = Get-ChildItem $build -Recurse -Filter fceumm_libretro.dll | Where-Object { $_.FullName -match "\\$Configuration\\" } |
    Select-Object -First 1
if (!$dll) { throw "fceumm_libretro.dll was not produced" }
Copy-Item $dll.FullName (Join-Path $DepsBin "fceumm_libretro.dll") -Force
Write-Host "Wrote $(Join-Path $DepsBin 'fceumm_libretro.dll')" -ForegroundColor Green
