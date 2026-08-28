$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $Vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsPath) { throw 'Visual Studio C++ tools not found' }

$DevCmd = Join-Path $VsPath 'Common7\Tools\Launch-VsDevShell.ps1'
& $DevCmd -Arch amd64 -SkipAutomaticLocation | Out-Null

$Build = Join-Path $Root 'build'
New-Item -ItemType Directory -Force -Path $Build | Out-Null
Set-Location $Build

cmake -G Ninja -DCMAKE_BUILD_TYPE=Release $Root
if ($LASTEXITCODE -ne 0) {
  cmake -G "Visual Studio 18 2026" -A x64 $Root
  if ($LASTEXITCODE -ne 0) {
    cmake -G "Visual Studio 17 2022" -A x64 $Root
  }
}
cmake --build . --config Release
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$Dist = Join-Path $Build 'dist\psvr'
Write-Host "Driver assembled at $Dist"
