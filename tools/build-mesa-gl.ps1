# SPDX-License-Identifier: GPL-3.0-or-later
# Builds ONYX's own OpenGL (onyx_gl.dll): Mesa's OpenGL-on-D3D12 gallium driver
# with a small window-less front end (patches\mesa, src\gallium\targets\onyx_gl),
# for UWP. Built from upstream Mesa at the pinned commit plus our patches.
# Needs the same tools as build-dozen.ps1 (meson, ninja, mako, win_flex_bison).
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$src = Join-Path $Deps "mesa"
$build = Join-Path $DepsBuild "mesa-gl"
$uwp = "-DMESA_UWP=1 -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WIN32_WINNT=0x0A00 /wd4189"
$store = Join-Path $env:VCToolsInstallDir "lib\x64\store"
if (!(Test-Path (Join-Path $store "msvcrt.lib"))) { throw "Store CRT libs not found in $store" }

# The UWP link line (app CRT, /APPCONTAINER) lives on the onyx_gl target only, so
# Meson's configure-time test programs still link with the desktop libraries and
# run on this machine. ONYX_STORE_LIBS tells the target where the app CRT is.
$env:ONYX_STORE_LIBS = $store
Write-Step "Configuring Mesa (OpenGL on D3D12, UWP)"
$env:CC = "cl"
$env:CXX = "cl"
$mesonArgs = @(
    "setup", $build, $src, "--backend=ninja", "--buildtype=release", "-Db_vscrt=md", "-Db_ndebug=true",
    "-Dplatforms=windows", "-Dmin-windows-version=10",
    "-Dvulkan-drivers=", "-Dgallium-drivers=d3d12", "-Donyx-gl=true",
    "-Dopengl=true", "-Dgles1=disabled", "-Dgles2=disabled", "-Degl=disabled", "-Dglx=disabled",
    "-Dgbm=disabled", "-Dllvm=disabled", "-Dshader-cache=disabled", "-Dbuild-tests=false",
    "-Dmicrosoft-clc=disabled", "-Dspirv-to-dxil=false", "-Dvideo-codecs=",
    "-Dgallium-d3d12-video=disabled", "-Dgallium-va=disabled", "-Dgallium-mediafoundation=disabled",
    "-Dzstd=disabled", "-Dxmlconfig=disabled", "-Dvalgrind=disabled", "-Dlibunwind=disabled",
    "-Dmesa-clc=auto", "-Dprecomp-compiler=auto",
    "-Dc_args=$uwp", "-Dcpp_args=$uwp", "-Dc_link_args=", "-Dcpp_link_args="
)
$reconfigure = @()
if (Test-Path (Join-Path $build "build.ninja")) { $reconfigure = @("--reconfigure") }
Invoke-Checked meson @mesonArgs @reconfigure

Write-Step "Building onyx_gl.dll"
$out = Join-Path $build "src\gallium\targets\onyx_gl\onyx_gl.dll"
Remove-Item $out -ErrorAction SilentlyContinue
Invoke-Checked ninja -k 0 -C $build
if (!(Test-Path $out)) { throw "onyx_gl.dll was not produced" }

# The console refuses to start an app whose DLLs import anything outside the
# UWP API surface. Check before the DLL can reach the package.
$sdkLib = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib\*\um\x64\WindowsApp.lib" |
    Sort-Object FullName -Descending | Select-Object -First 1
$allowed = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($l in (& dumpbin /headers $sdkLib.FullName)) {
    if ($l -match '^\s+Symbol name\s*:\s*(\S+)') {
        [void]$allowed.Add((($Matches[1] -replace '^__imp_', '' -replace '^_', '') -replace '@\d+$', ''))
    }
}
$bad = @(); $cur = $null
foreach ($l in (& dumpbin /imports $out)) {
    if ($l -match '^\s+Summary\s*$') { break }
    if ($l -match '^\s{4}(\S+\.dll)\s*$') { $cur = $Matches[1]; continue }
    if (!$cur -or $cur -match '^api-ms-win-crt-' -or $cur -match '^(VCRUNTIME140(_1)?_APP|MSVCP140_APP)\.dll$') { continue }
    if ($l -match '^\s+[0-9A-F]+\s+(\S+)\s*$') {
        if (!$allowed.Contains($Matches[1])) { $bad += "$cur!$($Matches[1])" }
    } elseif ($l -match 'Ordinal\s+(\d+)' -and $cur -notmatch '^(d3d12|dxgi)\.dll$') {
        $bad += "$cur!#$($Matches[1])"
    }
}
if ($bad) {
    $bad | Sort-Object -Unique | ForEach-Object { Write-Host "NOT AVAILABLE ON XBOX: $_" -ForegroundColor Red }
    throw "onyx_gl.dll imports functions Xbox does not have ($($bad.Count)); not shipping it"
}
$desktop = (& dumpbin /dependents $out) -match '^\s+VCRUNTIME140(_1)?\.dll\s*$'
if ($desktop) { throw "onyx_gl.dll still links the desktop C++ runtime" }
Copy-Item $out (Join-Path $DepsBin "onyx_gl.dll") -Force
Write-Host "Wrote $DepsBin\onyx_gl.dll" -ForegroundColor Green
