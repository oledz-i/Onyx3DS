# SPDX-License-Identifier: GPL-3.0-or-later
# Builds Mesa's Dozen Vulkan driver (vulkan_dzn.dll) for UWP.
# Needs: Python 3 with meson, ninja and mako (pip install meson ninja mako),
#        and win_flex_bison on PATH (choco install winflexbison3).
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$src = Join-Path $Deps "mesa"
$build = Join-Path $DepsBuild "mesa"
$uwp = "-DMESA_UWP=1 -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WIN32_WINNT=0x0A00"
# Dozen links D3D12/DXGI directly on UWP (patches\mesa replaces LoadLibrary).
$link = "/APPCONTAINER WindowsApp.lib d3d12.lib dxgi.lib"

if (!(Test-Path (Join-Path $build "build.ninja"))) {
    Write-Step "Configuring Mesa (Dozen only, UWP)"
    $env:CC = "cl"
    $env:CXX = "cl"
    $mesonArgs = @(
        "setup", $build, $src, "--backend=ninja", "--buildtype=release", "-Db_vscrt=md",
        "-Dplatforms=windows", "-Dmin-windows-version=10",
        "-Dvulkan-drivers=microsoft-experimental", "-Dgallium-drivers=",
        "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled", "-Degl=disabled", "-Dglx=disabled",
        "-Dgbm=disabled", "-Dllvm=disabled", "-Dshader-cache=disabled", "-Dbuild-tests=false",
        "-Dmicrosoft-clc=disabled", "-Dspirv-to-dxil=false", "-Dvideo-codecs=",
        "-Dgallium-d3d12-video=disabled", "-Dgallium-va=disabled", "-Dgallium-mediafoundation=disabled",
        "-Dzstd=disabled", "-Dxmlconfig=disabled", "-Dvalgrind=disabled", "-Dlibunwind=disabled",
        "-Dmesa-clc=auto", "-Dprecomp-compiler=auto",
        "-Dc_args=$uwp", "-Dcpp_args=$uwp",
        "-Dc_link_args=$link", "-Dcpp_link_args=$link"
    )
    Invoke-Checked meson @mesonArgs
}

Write-Step "Building vulkan_dzn.dll"
Invoke-Checked ninja -C $build src/microsoft/vulkan/vulkan_dzn.dll
Copy-Item (Join-Path $build "src\microsoft\vulkan\vulkan_dzn.dll") (Join-Path $DepsBin "vulkan_dzn.dll") -Force
Write-Host "Wrote $DepsBin\vulkan_dzn.dll" -ForegroundColor Green
