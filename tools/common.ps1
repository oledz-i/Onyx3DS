# SPDX-License-Identifier: GPL-3.0-or-later
# Shared helpers for the ONYX 3DS Windows build scripts.

$ErrorActionPreference = "Stop"
$Script:Root = (Resolve-Path "$PSScriptRoot\..").Path
$Script:Deps = Join-Path $Root "deps"
$Script:DepsLib = Join-Path $Deps "lib"
$Script:DepsBin = Join-Path $Deps "bin"
$Script:DepsBuild = Join-Path $Deps "build"
New-Item -ItemType Directory -Force -Path $Deps, $DepsLib, $DepsBin, $DepsBuild | Out-Null

# Pinned sources. The patches in patches/ were made against these commits.
$Script:AzaharRepo = "https://github.com/azahar-emu/azahar.git"
$Script:AzaharCommit = (Get-Content "$Root\patches\azahar\BASE_COMMIT" -Raw).Trim()
$Script:MesaRepos = @("https://gitlab.freedesktop.org/mesa/mesa.git",
                      "https://github.com/chaotic-cx/mesa-mirror.git")
$Script:MesaCommit = (Get-Content "$Root\patches\mesa\BASE_COMMIT" -Raw).Trim()
# DXIL.dll comes from the latest DirectX Shader Compiler release on GitHub.
$Script:DxcReleaseApi = "https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases/latest"

function Write-Step($text) {
    Write-Host ""
    Write-Host "==> $text" -ForegroundColor Cyan
}

function Invoke-Checked {
    param([string]$Exe, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

# Puts cl.exe, lib.exe, msbuild and the Windows SDK on PATH (x64).
function Use-VsDevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path $vswhere)) { throw "Visual Studio 2022 with the C++ UWP workload is required." }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw "Visual Studio C++ tools not found." }
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

# Applies a patch series once (a stamp file records it).
function Apply-Patches($repoDir, $patchDir) {
    $stamp = Join-Path $repoDir ".onyx-patched"
    if (Test-Path $stamp) { return }
    foreach ($p in Get-ChildItem $patchDir -Filter *.patch | Sort-Object Name) {
        Write-Host "   applying $($p.Name)"
        Invoke-Checked git -C $repoDir apply --whitespace=nowarn $p.FullName
    }
    Set-Content $stamp "patched"
}
