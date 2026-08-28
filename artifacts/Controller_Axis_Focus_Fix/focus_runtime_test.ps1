Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class Fg2 { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow(); [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h,StringBuilder s,int n); [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p); }
"@
function Fg { $h=[Fg2]::GetForegroundWindow();$s=New-Object Text.StringBuilder 256;[void][Fg2]::GetWindowText($h,$s,256);$p=0;[void][Fg2]::GetWindowThreadProcessId($h,[ref]$p);$proc=Get-Process -Id $p -ErrorAction SilentlyContinue;[pscustomobject]@{Pid=$p;Process=$proc.ProcessName;Title=$s.ToString()} }
Start-Process 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrmonitor.exe' -WindowStyle Hidden
Start-Sleep -Seconds 7
$start=Fg;$vrcompositorFocus=0;$samples=80
for($i=0;$i -lt $samples;$i++){ $f=Fg;if($f.Process -eq 'vrcompositor'){$vrcompositorFocus++};Start-Sleep -Milliseconds 250 }
"POST_START_FOCUS=pid=$($start.Pid) process=$($start.Process) title=$($start.Title)"
"SAMPLES=$samples"
"VRCOMPOSITOR_FOREGROUND_SAMPLES=$vrcompositorFocus"
"HEADSET_FOCUS_STEAL_FREE=$($vrcompositorFocus -eq 0)"
Get-Process vrserver,vrmonitor,vrcompositor -ErrorAction SilentlyContinue|Sort-Object Name|ForEach-Object{"PROCESS name=$($_.Name) responding=$($_.Responding)"}
