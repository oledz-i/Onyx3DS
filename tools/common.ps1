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

# Runs a native command and stops the build if it fails. Deliberately a plain
# function (no param block) so PowerShell passes flags like -S / -B / -G
# straight through instead of trying to bind them as parameters.
function Invoke-Checked {
    $exe = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    & $exe @rest
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
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

# Applies a patch series. The stamp records which patches are applied; when the
# series changes (a cached tree from an older run), only the files git sees as
# modified are reset before re-applying, so incremental builds stay fast.
function Apply-Patches($repoDir, $patchDir) {
    $patches = @(Get-ChildItem $patchDir -Filter *.patch | Sort-Object Name)
    $sig = ($patches | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ","
    $stamp = Join-Path $repoDir ".onyx-patched"
    if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $sig)) { return }
    if (Test-Path $stamp) {
        Write-Host "   patch series changed, resetting modified files in $repoDir"
        Invoke-Checked git -C $repoDir checkout -- .
    }
    foreach ($p in $patches) {
        Write-Host "   applying $($p.Name)"
        Invoke-Checked git -C $repoDir apply --whitespace=nowarn $p.FullName
    }
    Set-Content $stamp $sig
}
