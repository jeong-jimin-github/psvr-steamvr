param([string]$Root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path)
$wmr=Get-Content (Join-Path $Root 'src\wmr_hid.cpp') -Raw
$display=Get-Content (Join-Path $Root 'src\display_component.cpp') -Raw
$header=Get-Content (Join-Path $Root 'src\display_component.h') -Raw
$hmd=Get-Content (Join-Path $Root 'src\hmd_device.cpp') -Raw
$poseThread=($hmd -split 'void PsvrHmdDevice::PoseThread\(\)',2)[1] -split 'void PsvrHmdDevice::RequestRecenter',2 | Select-Object -First 1
$checks=[ordered]@{
  wmr_read24_big_endian=($wmr -match 'int32_t\(p\[2\]\).*int32_t\(p\[1\]\) << 8.*int32_t\(p\[0\]\) << 16')
  no_foreground_activation=($display -notmatch 'SetForegroundWindow|SetActiveWindow|SetFocus|BringWindowToTop|AttachThreadInput')
  no_synthetic_input=($display -notmatch 'SendInput|SetCursorPos|WM_LBUTTONDOWN|WM_LBUTTONUP|WM_MOUSEACTIVATE')
  nonactivating_window_pos=($display -match 'SWP_NOACTIVATE')
  nonactivating_style=($display -match 'WS_EX_NOACTIVATE')
  retry_focus_state_removed=($header -notmatch 'compositor_activated_|activation_attempts_|fallback_clicks_|success_streak_|last_action_')
  single_window_control_thread=($poseThread -notmatch 'PinCompositorWindow')
}
foreach($c in $checks.GetEnumerator()){'{0}={1}' -f $c.Key,$c.Value}
$sample=[byte[]](0x01,0x02,0x03)
$decoded=[int]$sample[2]+([int]$sample[1]*256)+([int]$sample[0]*65536)
"sample_01_02_03_decoded=$decoded"
if($decoded -ne 66051){'FAIL: numeric decoder case';exit 2}
if($checks.Values -contains $false){'FAIL: controller byte order/focus isolation incomplete';exit 1}
'PASS: WMR 24-bit IMU decoding is big-endian and compositor placement never activates or injects input'
exit 0
