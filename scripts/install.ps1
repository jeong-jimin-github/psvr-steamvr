$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root 'build\dist\psvr'
if (-not (Test-Path (Join-Path $Dist 'bin\win64\driver_psvr.dll'))) {
  throw "Driver not built. Run scripts\build.ps1 first. Missing: $Dist\bin\win64\driver_psvr.dll"
}

$VrPathReg = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe'
if (-not (Test-Path $VrPathReg)) { throw "vrpathreg not found: $VrPathReg" }

# Drop stale paths from earlier driver attempts.
& $VrPathReg removedriver 'C:\Users\jm\Documents\psvr\build-msvc\dist\psvr_bridge' 2>$null
& $VrPathReg removedriver 'C:\Users\jm\Documents\psvr\build-msvc\dist\steamvr_psvr\steamvr_psvr' 2>$null
& $VrPathReg removedriver 'C:\Users\jm\source\repos\psvr-steamvr\build\dist\psvr' 2>$null
& $VrPathReg removedriver $Dist 2>$null
& $VrPathReg adddriver $Dist
Write-Host "Registered driver: $Dist"
& $VrPathReg show

$Settings = 'C:\Program Files (x86)\Steam\config\steamvr.vrsettings'
if (Test-Path $Settings) {
  python -c @"
import json, pathlib
p = pathlib.Path(r'$Settings')
data = json.loads(p.read_text(encoding='utf-8'))
sv = data.setdefault('steamvr', {})
sv['activateMultipleDrivers'] = True
sv['requireHmd'] = True
sv['showMirrorView'] = False
drv = data.setdefault('driver_psvr', {})
drv['enable'] = True
drv['blocked_by_safe_mode'] = False
p.write_text(json.dumps(data, indent=3), encoding='utf-8')
print('Updated', p)
"@
}

Write-Host @"

Installed. Next:
  1. Confirm HDMI is extended to 'SIE HMD' at 1920x1080.
  2. Optional USB check:  $Dist\bin\win64\psvr_ctl.exe vr
  3. Start SteamVR.
  4. Logs: %TEMP%\psvr_driver.log  and  Steam\logs\vrserver.txt
Mute button on the headset recenters yaw.

"@
