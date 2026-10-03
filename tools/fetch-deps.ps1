# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads and patches everything the Xbox build needs:
#   deps\azahar   Azahar at the pinned commit + patches\azahar + patches\dynarmic
#   deps\mesa     Mesa (for Dozen) at the pinned commit + patches\mesa
#   deps\bin\dxil.dll   DXIL signer from the DirectX Shader Compiler release
#   app\packages  C++/WinRT NuGet package
. "$PSScriptRoot\common.ps1"

# --- Azahar -------------------------------------------------------------------
$azahar = Join-Path $Deps "azahar"
if (!(Test-Path "$azahar\.git")) {
    Write-Step "Cloning Azahar $AzaharCommit"
    Invoke-Checked git clone $AzaharRepo $azahar
}
Push-Location $azahar
try {
    if (!(Test-Path ".onyx-patched")) {
        Invoke-Checked git checkout --quiet $AzaharCommit
        Write-Step "Fetching Azahar submodules (this takes a while the first time)"
        Invoke-Checked git submodule update --init --recursive --depth 1 --jobs 8
    }
} finally { Pop-Location }
Write-Step "Patching Azahar"
Apply-Patches $azahar (Join-Path $Root "patches\azahar")
Apply-Patches (Join-Path $azahar "externals\dynarmic") (Join-Path $Root "patches\dynarmic")

# --- Mesa (Dozen) ---------------------------------------------------------------
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

# --- DXIL.dll -------------------------------------------------------------------
$dxil = Join-Path $DepsBin "dxil.dll"
if (!(Test-Path $dxil)) {
    Write-Step "Downloading DXIL.dll (DirectX Shader Compiler release)"
    $zip = Join-Path $Deps "dxc.zip"
    $release = Invoke-RestMethod -Uri $DxcReleaseApi -Headers @{ "User-Agent" = "ONYX3DS-build" }
    $asset = $release.assets | Where-Object { $_.name -match '^dxc_.*\.zip$' } | Select-Object -First 1
    if (!$asset) { throw "No Windows dxc_*.zip asset in the latest DirectXShaderCompiler release" }
    Write-Host "   $($release.tag_name): $($asset.name)"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $zip
    $out = Join-Path $Deps "dxc"
    Expand-Archive -Force $zip $out
    Copy-Item (Join-Path $out "bin\x64\dxil.dll") $dxil
}

# --- NuGet / C++/WinRT ------------------------------------------------------------
$nuget = Join-Path $Deps "nuget.exe"
if (!(Test-Path $nuget)) {
    Invoke-WebRequest -Uri "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe" -OutFile $nuget
}
Write-Step "Restoring C++/WinRT"
Invoke-Checked $nuget restore (Join-Path $Root "app\ONYX3DS\packages.config") -PackagesDirectory (Join-Path $Root "app\packages")

Write-Host "Dependencies ready in $Deps" -ForegroundColor Green
