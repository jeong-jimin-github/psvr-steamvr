param([Parameter(Mandatory=$true)][string]$Image)
Add-Type -AssemblyName System.Drawing
$b=[Drawing.Bitmap]::FromFile((Resolve-Path $Image));$red=0;$n=0
for($y=0;$y-lt$b.Height;$y+=8){for($x=0;$x-lt$b.Width;$x+=8){$c=$b.GetPixel($x,$y);if($c.R-gt 80-and$c.G-lt 45-and$c.B-lt 45){$red++};$n++}}
$b.Dispose();$ratio=$red/[double]$n
"samples=$n";"red_samples=$red";"red_ratio=$($ratio.ToString('F6',[Globalization.CultureInfo]::InvariantCulture))"
if($ratio-gt 0.90){'RESULT=SOLID_RED';exit 1}else{'RESULT=VR_SCENE';exit 0}
