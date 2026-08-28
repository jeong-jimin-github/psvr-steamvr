$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = & $Vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VsPath) { throw 'Visual Studio C++ tools not found' }

$DevCmd = Join-Path $VsPath 'Common7\Tools\Launch-VsDevShell.ps1'
& $DevCmd -Arch amd64 -SkipAutomaticLocation | Out-Null

# Do not pick up an unrelated MSYS/devkitPro cmake from PATH. Visual Studio
# ships a Windows-native CMake/Ninja pair that handles drive-letter paths.
$CMake = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$Ninja = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (-not (Test-Path $CMake)) { throw "Visual Studio CMake not found: $CMake" }
if (-not (Test-Path $Ninja)) { throw "Visual Studio Ninja not found: $Ninja" }

$Build = Join-Path $Root 'build'
New-Item -ItemType Directory -Force -Path $Build | Out-Null
& $CMake --fresh -S $Root -B $Build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& $CMake --build $Build --config Release
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

$Dist = Join-Path $Build 'dist\psvr'
Write-Host "Driver assembled at $Dist"
