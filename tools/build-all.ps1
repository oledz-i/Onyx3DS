# SPDX-License-Identifier: GPL-3.0-or-later
# One command from a fresh clone to a sideloadable ONYX 3DS package.
#   powershell -ExecutionPolicy Bypass -File tools\build-all.ps1
param(
    [string]$Configuration = "Release",
    [switch]$SkipAzahar,
    [switch]$SkipDozen
)
. "$PSScriptRoot\common.ps1"
$sw = [Diagnostics.Stopwatch]::StartNew()

& "$PSScriptRoot\fetch-deps.ps1"
& "$PSScriptRoot\build-shell.ps1" -Configuration $Configuration
if (!$SkipAzahar) { & "$PSScriptRoot\build-azahar.ps1" -Configuration $Configuration }
if (!$SkipDozen) { & "$PSScriptRoot\build-dozen.ps1" }
& "$PSScriptRoot\build-app.ps1" -Configuration $Configuration

Write-Host ("Done in {0:N0} minutes" -f $sw.Elapsed.TotalMinutes) -ForegroundColor Green
