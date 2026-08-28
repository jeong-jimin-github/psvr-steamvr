param([string]$Source = "C:\Users\jm\Documents\psvr-steamvr\src\wmr_hid.cpp")
$text=Get-Content -LiteralPath $Source -Raw
$bounds=$text -match 'amag >= 2\.0f && amag <= 40\.0f && gmag <= 20\.0f'
$idleHold=$text -match '(?s)idle_imu_report.*\+\+idle_imu_samples_.*if \(valid_imu\).*ahrs_\.Update'
$observedAmag=10.044; $observedGmag=0.050
$observedAccepted=$bounds -and $observedAmag -ge 2.0 -and $observedAmag -le 40.0 -and $observedGmag -le 20.0
$idleAmag=0.0; $idleGmag=0.0
$idleDetected=$idleHold -and $idleAmag -lt 0.0001 -and $idleGmag -lt 0.0001
"observed_input=amag=10.044,gmag=0.050"
"observed_result=ACCEPTED:$observedAccepted"
"idle_input=amag=0.000,gmag=0.000"
"idle_result=HOLD_ORIENTATION:$idleDetected"
if($observedAccepted -and $idleDetected){'PASS: observed motion sample is fused and observed idle sample holds orientation'; exit 0}
'FAIL: observed runtime states are not handled'; exit 1
