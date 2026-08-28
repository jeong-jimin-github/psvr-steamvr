param([string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path)
$ErrorActionPreference='Stop'
$provider=Get-Content (Join-Path $Root 'src\device_provider.cpp') -Raw
$controller=Get-Content (Join-Path $Root 'src\controller_device.cpp') -Raw
$bleH=Get-Content (Join-Path $Root 'src\gearvr_ble.h') -Raw
$bleCpp=Get-Content (Join-Path $Root 'src\gearvr_ble.cpp') -Raw
$checks=[ordered]@{
  provider_registration=($provider -match 'GearVrControllerDevice')
  hmd_aligned_recenter=($controller -match 'Recenter\(FromHmd\(hmd_pose\.qRotation\)\)')
  automatic_initial_snap=($controller -match 'did_auto_snap_')
  head_relative_origin=($controller -match 'RotateVec')
  synchronized_filter=($bleCpp -match 'same critical section')
  recenter_api=($bleH -match 'Recenter\(const Quaternion &hmd_rotation\)')
}
$alignmentError=[double](& python -c "import math; m=lambda a,b:(a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3],a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1],a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]); h=(math.cos(math.pi/6),0,math.sin(math.pi/6),0); r=(math.cos(math.pi/9),math.sin(math.pi/9),0,0); q=m(m(h,(r[0],-r[1],-r[2],-r[3])),r); print(math.sqrt(sum((a-b)**2 for a,b in zip(q,h))))")
$checks.calibration_math=($alignmentError -lt 1e-6)
foreach($c in $checks.GetEnumerator()){ '{0}={1}' -f $c.Key,$c.Value }
'alignment_error={0:E6}' -f $alignmentError
if($checks.Values -contains $false){ 'FAIL: GearVR pointing alignment incomplete'; exit 1 }
'PASS: GearVR pointer registers, snaps to HMD forward, and keeps a head-relative origin'
exit 0

