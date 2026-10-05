# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads and patches everything the Xbox build needs:
#   deps\azahar   Azahar at the pinned commit + patches\azahar + patches\dynarmic
#   deps\mesa     Mesa (for Dozen) at the pinned commit + patches\mesa
#   deps\bin\dxil.dll   DXIL signer from the DirectX Shader Compiler release
#   app\packages  C++/WinRT NuGet package
# -Only lets CI jobs fetch just what they need: azahar, mesa, dxil, nuget.
param([string[]]$Only = @("azahar", "mesa", "dxil", "nuget", "headers"))
. "$PSScriptRoot\common.ps1"

# --- Azahar -------------------------------------------------------------------
if ($Only -contains "azahar") {
$azahar = Join-Path $Deps "azahar"
if (!(Test-Path "$azahar\.git")) {
    Write-Step "Cloning Azahar $AzaharCommit"
    Invoke-Checked git clone $AzaharRepo $azahar
}
# A cached tree can be incomplete (a cancelled run saved it half-fetched): missing
# sources or empty submodules. Check for that and repair, or start over.
function Test-AzaharTree($dir) {
    foreach ($f in @("src\CMakeLists.txt", "externals\dynarmic\CMakeLists.txt",
                     "externals\xxHash\xxhash.h", "externals\glslang\CMakeLists.txt",
                     "externals\sirit\sirit\CMakeLists.txt", "externals\boost\.git",
                     "externals\fmt\CMakeLists.txt", "externals\cryptopp\.git")) {
        if (!(Test-Path (Join-Path $dir $f))) { Write-Host "   missing $f"; return $false }
    }
    return $true
}
Push-Location $azahar
try {
    if (!(Test-Path "externals\dynarmic\.git") -or !(Test-AzaharTree $azahar)) {
        Write-Step "Fetching Azahar sources and submodules (this takes a while the first time)"
        Remove-Item -Force -ErrorAction SilentlyContinue ".onyx-patched"
        Invoke-Checked git checkout --quiet --force $AzaharCommit
        Invoke-Checked git checkout --quiet -- .
        Invoke-Checked git submodule update --init --recursive --force --depth 1 --jobs 8
        foreach ($sub in @("dynarmic", "boost", "cryptopp")) {
            Remove-Item -Force -ErrorAction SilentlyContinue "externals\$sub\.onyx-patched"
        }
    }
} finally { Pop-Location }
if (!(Test-AzaharTree $azahar)) {
    Write-Step "Cached Azahar tree is broken; cloning it again"
    Remove-Item -Recurse -Force $azahar
    Invoke-Checked git clone $AzaharRepo $azahar
    Push-Location $azahar
    try {
        Invoke-Checked git checkout --quiet $AzaharCommit
        Invoke-Checked git submodule update --init --recursive --depth 1 --jobs 8
    } finally { Pop-Location }
}
Write-Step "Patching Azahar"
Apply-Patches $azahar (Join-Path $Root "patches\azahar")
Apply-Patches (Join-Path $azahar "externals\dynarmic") (Join-Path $Root "patches\dynarmic")
Apply-Patches (Join-Path $azahar "externals\boost") (Join-Path $Root "patches\boost")
Apply-Patches (Join-Path $azahar "externals\cryptopp") (Join-Path $Root "patches\cryptopp")

}
# --- Mesa (Dozen) ---------------------------------------------------------------
if ($Only -contains "mesa") {
$mesa = Join-Path $Deps "mesa"
if (!(Test-Path "$mesa\.git")) {
    Write-Step "Fetching Mesa $MesaCommit"
    New-Item -ItemType Directory -Force -Path $mesa | Out-Null
    Invoke-Checked git -C $mesa init --quiet
    $ok = $false
    foreach ($repo in $MesaRepos) {
        git -C $mesa fetch --depth 1 $repo $MesaCommit
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
        Write-Host "   $repo did not have the commit, trying the next mirror"
    }
    if (!$ok) { throw "Could not fetch Mesa $MesaCommit" }
    Invoke-Checked git -C $mesa checkout --quiet FETCH_HEAD
}
Write-Step "Patching Mesa"
Apply-Patches $mesa (Join-Path $Root "patches\mesa")

}
# --- DXIL.dll -------------------------------------------------------------------
if ($Only -contains "dxil") {
$dxil = Join-Path $DepsBin "dxil.dll"
if (!(Test-Path $dxil)) {
    Write-Step "Downloading DXIL.dll (DirectX Shader Compiler release)"
    $zip = Join-Path $Deps "dxc.zip"
    Invoke-WebRequest -Uri $DxcZipUrl -OutFile $zip
    $out = Join-Path $Deps "dxc"
    Expand-Archive -Force $zip $out
    Copy-Item (Join-Path $out "bin\x64\dxil.dll") $dxil
}

}
# --- NuGet / C++/WinRT ------------------------------------------------------------
if ($Only -contains "nuget") {
$nuget = Join-Path $Deps "nuget.exe"
if (!(Test-Path $nuget)) {
    Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $nuget
}
Write-Step "Restoring C++/WinRT"
Invoke-Checked $nuget restore (Join-Path $Root "app\ONYX3DS\packages.config") -PackagesDirectory (Join-Path $Root "app\packages")

}
# --- Headers for the app (no full Azahar checkout needed) ---------------------
if ($Only -contains "headers") {
    $h = Join-Path $Deps "headers"
    if (!(Test-Path "$h\Vulkan-Headers")) {
        Write-Step "Fetching Vulkan and libretro headers"
        Invoke-Checked git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "$h\Vulkan-Headers"
        Invoke-Checked git clone --depth 1 https://github.com/libretro/libretro-common.git "$h\libretro-common"
    }
}

Write-Host "Dependencies ready in $Deps" -ForegroundColor Green
