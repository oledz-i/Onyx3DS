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
