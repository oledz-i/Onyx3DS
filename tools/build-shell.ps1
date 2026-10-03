# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the portable shell (onyx_shell.lib + rcheevos.lib) for UWP.
param([string]$Configuration = "Release")
. "$PSScriptRoot\common.ps1"
Use-VsDevShell

$build = Join-Path $DepsBuild "shell"
Write-Step "Building the ONYX shell for WindowsStore"
Invoke-Checked cmake -S (Join-Path $Root "shell") -B $build -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DONYX_SHELL_TESTS=OFF
Invoke-Checked cmake --build $build --config $Configuration --parallel
foreach ($lib in "onyx_shell.lib", "rcheevos.lib") {
    Copy-Item (Get-ChildItem $build -Recurse -Filter $lib | Where-Object { $_.FullName -match "\\$Configuration\\" } |
        Select-Object -First 1).FullName (Join-Path $DepsLib $lib) -Force
}
Write-Host "Shell libraries in $DepsLib" -ForegroundColor Green
