# SPDX-License-Identifier: GPL-3.0-or-later
# Builds and signs the ONYX 3DS .msix for sideloading through the Xbox Device
# Portal. Output: out\AppPackages\ONYX3DS_<version>_x64_Test\
param([string]$Configuration = "Release")
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$proj = Join-Path $Root "app\ONYX3DS\ONYX3DS.vcxproj"
$pfx = Join-Path $Root "app\ONYX3DS\ONYX3DS_TemporaryKey.pfx"

# A self-signed certificate whose subject matches the manifest Publisher.
# Dev Mode accepts it; keep the .pfx so updates install over the old version.
New-Item -ItemType Directory -Force -Path (Join-Path $Root "out") | Out-Null
if (!(Test-Path $pfx)) {
    Write-Step "Creating a signing certificate (CN=ONYX3DS)"
    $cert = New-SelfSignedCertificate -Type Custom -Subject "CN=ONYX3DS" -KeyUsage DigitalSignature `
        -FriendlyName "ONYX 3DS sideload" -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    $pw = ConvertTo-SecureString -String "onyx3ds" -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pw | Out-Null
    Export-Certificate -Cert $cert -FilePath (Join-Path $Root "out\ONYX3DS.cer") -Force | Out-Null
}

# Stable signing key (committed, sideload-only): every build is signed by the same
# publisher, and the package version rises with each CI run, so a new build installs
# over the old one and the app's saved data survives.
Copy-Item (Join-Path $Root "app\ONYX3DS\ONYX3DS.cer") (Join-Path $Root "out\ONYX3DS.cer") -Force
if ($env:GITHUB_RUN_NUMBER) {
    $mf = Join-Path $Root "app\ONYX3DS\Package.appxmanifest"
    $n = [int]$env:GITHUB_RUN_NUMBER % 65535
    (Get-Content $mf -Raw) -replace '(<Identity[^>]*Version=")(\d+\.\d+\.\d+)\.\d+(")', "`${1}`$2.$n`$3" | Set-Content $mf -NoNewline
}

# Drivers ship at the package root (LoadPackagedLibrary looks there).
foreach ($d in "vulkan_dzn.dll", "dxil.dll") {
    $src = Join-Path $Root "deps\bin\$d"
    if (Test-Path $src) { Copy-Item $src (Join-Path $Root "app\ONYX3DS\$d") -Force }
    else { Write-Warning "$d not found in deps\bin; graphics will not work in this package" }
}

# OpenGL on D3D12 (Mesa, UWP build) for the OpenGL hardware renderer: see vendor\mesa-gl.
$glZip = Join-Path $Root "vendor\mesa-gl\mesa-uwp-26.1.3.zip"
$glTmp = Join-Path $Root "out\mesa-gl"
if (Test-Path $glZip) {
    $expected = "AA206F0547291254AE986C723E62A68B1CAC2CBDAD08C94DEC593A3640A0BF43"
    $actual = (Get-FileHash $glZip -Algorithm SHA256).Hash
    if ($actual -ne $expected) { throw "mesa-gl archive hash mismatch: $actual" }
    if (Test-Path $glTmp) { Remove-Item $glTmp -Recurse -Force }
    Expand-Archive -Path $glZip -DestinationPath $glTmp -Force
    foreach ($d in "opengl32.dll", "libgallium_wgl.dll", "z-1.dll") {
        Copy-Item (Join-Path $glTmp "26.1.3\bin\$d") (Join-Path $Root "app\ONYX3DS\$d") -Force
    }
} else {
    Write-Warning "vendor\mesa-gl archive missing; the OpenGL renderer will not be available"
}

Write-Step "Building ONYX 3DS ($Configuration|x64)"
New-Item -ItemType Directory -Force -Path (Join-Path $Root "out") | Out-Null
Invoke-Checked msbuild $proj /m /restore /p:Configuration=$Configuration /p:Platform=x64 `
    /p:AppxBundle=Never /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxPackageSigningEnabled=true `
    /p:PackageCertificateKeyFile=$pfx /p:PackageCertificatePassword=onyx3ds `
    /p:GenerateAppxPackageOnBuild=true

$pkg = Get-ChildItem (Join-Path $Root "out\AppPackages") -Recurse -Include *.msix, *.appx | Select-Object -First 1
if (!$pkg) { throw "No package produced" }
Write-Host ""
Write-Host "Package: $($pkg.FullName)" -ForegroundColor Green
Write-Host "Install it from the Xbox Device Portal (Add > choose the .msix, then add the files in"
Write-Host "the Dependencies\x64 folder next to it). See docs\INSTALL.md." -ForegroundColor Green
