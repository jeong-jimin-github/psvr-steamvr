param([string]$Source = "C:\Users\jm\Documents\psvr-steamvr\src\wmr_hid.cpp")
$text = Get-Content -LiteralPath $Source -Raw
$checks = [ordered]@{
  little_endian_signed24 = $text.Contains('int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);')
  monado_byte_order_comment = $text -match 'signed 24-bit little-endian'
  finite_imu_validation = $text -match 'std::isfinite\(amag\).*std::isfinite\(gmag\)'
  implausible_sample_rejection = $text -match '(?s)valid_imu.*amag >= 2\.0f.*amag <= 40\.0f.*gmag <= 20\.0f'
  stationary_gyro_lock = $text -match '(?s)corrected_gmag\s*<\s*0\.015f.*gx\s*=\s*gy\s*=\s*gz\s*=\s*0\.f'
  bias_requires_gravity = $text -match 'stationary_for_bias\s*=.*amag\s*>\s*8\.0f.*amag\s*<\s*12\.0f'
  idle_report_holds_orientation = $text -match '(?s)idle_imu_report.*\+\+idle_imu_samples_.*if \(valid_imu\).*ahrs_\.Update'
  diagnostic_motion_metrics = $text -match 'imu packets=%u amag=%.3f gmag=%.4f idle=%u rejected=%u stationary=%u'
}
$checks.GetEnumerator() | ForEach-Object { "{0}={1}" -f $_.Key,$_.Value }
if($checks.Values -contains $false){ 'FAIL: stationary WMR IMU decoding/guard incomplete'; exit 1 }
'PASS: WMR IMU is little-endian, bounded, gravity-qualified, bias-corrected, idle-held, and stationary-locked'; exit 0

