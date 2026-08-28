param([Parameter(Mandatory=$true)][string]$Source)
$text = Get-Content -LiteralPath $Source -Raw
$little = $text.Contains('int32_t v = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);')
function Read-S24([byte[]]$b){
  if($little){ $v=[int]$b[0] -bor ([int]$b[1] -shl 8) -bor ([int]$b[2] -shl 16) }
  else { $v=[int]$b[2] -bor ([int]$b[1] -shl 8) -bor ([int]$b[0] -shl 16) }
  if($v -band 0x800000){ $v -= 0x1000000 }
  return $v
}
$accZ=Read-S24 ([byte[]](0x10,0x7A,0x07))
$gx=Read-S24 ([byte[]](0x64,0x00,0x00))
$gy=Read-S24 ([byte[]](0x88,0xFF,0xFF))
$gz=Read-S24 ([byte[]](0x50,0x00,0x00))
$amag=[math]::Abs($accZ / 49000.0)
$gmag=[math]::Sqrt([math]::Pow($gx*0.00001,2)+[math]::Pow($gy*0.00001,2)+[math]::Pow($gz*0.00001,2))
"decoder={0}" -f $(if($little){'LITTLE_ENDIAN'}else{'BIG_ENDIAN'})
"input_accel_z=10-7A-07 input_gyro=64-00-00,88-FF-FF,50-00-00"
"decoded_accel_z={0} amag={1:F6}" -f $accZ,$amag
"decoded_gyro={0},{1},{2} gmag={3:F6}" -f $gx,$gy,$gz,$gmag
if($little -and [math]::Abs($amag-10.0) -lt 0.000001 -and $gmag -lt 0.015){ 'PASS: stationary frame remains stationary'; exit 0 }
'FAIL: stationary frame becomes implausible motion'; exit 1
