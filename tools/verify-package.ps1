# SPDX-License-Identifier: GPL-3.0-or-later
# Post-build checks on the finished .msix, run in CI:
#  1. Import scan: every DLL function the app (and the DLLs it ships) imports
#     must be part of the UWP API surface (WindowsApp.lib). Xbox only has
#     that surface, so anything else makes the console refuse to start the
#     app, dropping you straight back to Dev Home.
#  2. Smoke launch: install the package on the runner, start it, and collect
#     its log plus any crash / activation events.
# Writes report-imports.txt and report-launch.txt; exits 1 if either fails.
param([string]$PackageRoot = "artifacts\ONYX3DS-xbox")
$ErrorActionPreference = "Continue"
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$msix = Get-ChildItem $PackageRoot -Recurse -Filter *.msix | Select-Object -First 1
if (!$msix) { throw "No .msix under $PackageRoot" }
$x = Join-Path $env:RUNNER_TEMP "msix"
Remove-Item -Recurse -Force $x -ErrorAction SilentlyContinue
Copy-Item $msix.FullName "$x.zip"
Expand-Archive "$x.zip" $x -Force
$failed = $false

# ---------------------------------------------------------------- imports --
$sdkLib = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib\*\um\x64\WindowsApp.lib" |
    Sort-Object FullName -Descending | Select-Object -First 1
$allowed = New-Object 'System.Collections.Generic.HashSet[string]'
$allowedDlls = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$dll = $null
foreach ($l in (& dumpbin /headers $sdkLib.FullName)) {
    if ($l -match '^\s+DLL name\s*:\s*(\S+)') { $dll = $Matches[1]; [void]$allowedDlls.Add($dll) }
    elseif ($l -match '^\s+Symbol name\s*:\s*(\S+)') {
        $s = $Matches[1] -replace '^__imp_', '' -replace '^_', ''
        [void]$allowed.Add(($s -replace '@\d+$', ''))
    }
}
# Shipped next to the exe or provided by the VCLibs framework package.
$shipped = @(Get-ChildItem $x -Filter *.dll | ForEach-Object Name) +
    @("VCRUNTIME140_APP.dll", "VCRUNTIME140_1_APP.dll", "MSVCP140_APP.dll", "VCCORLIB140_APP.dll",
      "CONCRT140_APP.dll", "MSVCP140_1_APP.dll", "MSVCP140_2_APP.dll")

$rep = @("WindowsApp.lib: $($sdkLib.FullName) ($($allowed.Count) symbols)")
foreach ($bin in Get-ChildItem $x -Include *.exe, *.dll -Recurse) {
    $bad = @(); $cur = $null
    foreach ($l in (& dumpbin /imports $bin.FullName)) {
        if ($l -match '^\s{4}(\S+\.dll)\s*$') { $cur = $Matches[1]; continue }
        if (!$cur -or $shipped -contains $cur) { continue }
        if ($l -match '^\s+[0-9A-F]+\s+(\S+)\s*$') {
            $fn = $Matches[1]
            if (!$allowed.Contains($fn)) { $bad += "$cur!$fn" }
        } elseif ($l -match 'Ordinal\s+(\d+)') { $bad += "$cur!#$($Matches[1])" }
    }
    $deps = (& dumpbin /dependents $bin.FullName) | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } |
        ForEach-Object { $_.Trim() }
    $rep += "== $($bin.Name): imports from $($deps -join ', ')"
    if ($bad) { $failed = $true; $rep += "   NOT AVAILABLE ON XBOX ($($bad.Count)): " + (($bad | Sort-Object -Unique) -join ' ') }
}
$rep | Set-Content report-imports.txt
$rep | Write-Host

# ----------------------------------------------------------------- launch --
$launch = @()
try {
    $cer = Get-ChildItem $PackageRoot -Recurse -Filter *.cer | Select-Object -First 1
    Import-Certificate -FilePath $cer.FullName -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
    Import-Certificate -FilePath $cer.FullName -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    $unlock = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock"
    New-Item $unlock -Force | Out-Null
    Set-ItemProperty $unlock AllowAllTrustedApps 1
    Set-ItemProperty $unlock AllowDevelopmentWithoutDevLicense 1
    $deps = Get-ChildItem (Join-Path $msix.Directory "Dependencies\x64") -Include *.appx, *.msix -Recurse
    Add-AppxPackage -Path $msix.FullName -DependencyPath ($deps | ForEach-Object FullName) -ErrorAction Stop
    $pkg = Get-AppxPackage ONYX3DS
    $launch += "Installed $($pkg.PackageFullName)"
    $start = Get-Date
    Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!App"
    Start-Sleep 25
    $p = Get-Process ONYX3DS -ErrorAction SilentlyContinue
    if ($p) { $launch += "RUNNING after 25s (pid $($p.Id), $([int]($p.WorkingSet64/1MB)) MB)" }
    else { $failed = $true; $launch += "NOT RUNNING after 25s (crashed or never started)" }

    $logDir = Join-Path $env:LOCALAPPDATA "Packages\$($pkg.PackageFamilyName)\LocalState"
    foreach ($f in "onyx.log", "onyx.prev.log") {
        $lp = Join-Path $logDir $f
        if (Test-Path $lp) { $launch += "--- $f (last 40 lines)"; $launch += Get-Content $lp -Tail 40 }
    }
    if (!(Test-Path (Join-Path $logDir "onyx.log"))) { $launch += "(no onyx.log: died before OnLaunched)" }
    $launch += "--- events"
    $ev = @()
    $ev += Get-WinEvent -FilterHashtable @{ LogName = "Application"; StartTime = $start } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match "ONYX3DS" }
    foreach ($ln in "Microsoft-Windows-AppModel-Runtime/Admin", "Microsoft-Windows-TWinUI/Operational",
                    "Microsoft-Windows-Immersive-Shell/Operational") {
        $ev += Get-WinEvent -FilterHashtable @{ LogName = $ln; StartTime = $start; Level = 1, 2, 3 } -ErrorAction SilentlyContinue
    }
    foreach ($e in $ev | Select-Object -First 12) {
        $launch += "[$($e.ProviderName) $($e.Id)] " + (($e.Message -split "`r?`n" | Where-Object { $_ } | Select-Object -First 8) -join ' | ')
    }
} catch {
    $failed = $true
    $launch += "Launch test error: $_"
}
$launch | Set-Content report-launch.txt
$launch | Write-Host
if ($failed) { exit 1 }
