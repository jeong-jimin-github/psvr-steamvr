param([string]$Root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path)
$d=Get-Content (Join-Path $Root 'src\display_component.cpp') -Raw
$h=Get-Content (Join-Path $Root 'src\display_component.h') -Raw
$hd=Get-Content (Join-Path $Root 'src\hmd_device.cpp') -Raw
$hw=Get-Content (Join-Path $Root 'src\psvr_hw.cpp') -Raw
$checks=[ordered]@{
  proximity_api=($h -match 'PinCompositorWindow\(bool headset_worn\)')
  focus_lease_state=($h -match 'previous_foreground_' -and $h -match 'headset_worn_')
  video_focus_when_worn=($d -match 'if \(headset_worn\)' -and $d -match 'SetForegroundWindow\(hwnd\)')
  desktop_focus_restore=($d -match 'SetForegroundWindow\(previous\)')
  no_global_input_injection=($d -notmatch 'SendInput|SetCursorPos')
  hmd_passes_proximity=($hd -match 'PinCompositorWindow\(hw\.worn\)')
  report_state_is_worn_source=($hw -match 'pose_\.worn = worn;' -and $hw -notmatch 'worn \|\| proximity')
}
foreach($c in $checks.GetEnumerator()){'{0}={1}' -f $c.Key,$c.Value}
if($checks.Values -contains $false){'FAIL: red-screen/focus lease incomplete';exit 1}
'PASS: compositor fullscreen is proximity-gated and desktop focus is restored after headset removal'
exit 0
