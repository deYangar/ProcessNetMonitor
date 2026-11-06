$out = "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\docs\etw-system-logger\verify\v4_out.txt"
# confirm this shell is UNELEVATED (otherwise V4 is meaningless)
$id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object System.Security.Principal.WindowsPrincipal($id)
$elev = $pr.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
"shell elevated = $elev" | Out-File $out -Encoding utf8
if ($elev) { "ABORT: run from unelevated shell" | Out-File $out -Append -Encoding utf8; exit 1 }
# kill elevated TM
$procs = Get-CimInstance Win32_Process -Filter "Name='TrafficMonitor.exe'"
foreach ($p in $procs) { Invoke-CimMethod -InputObject $p -MethodName Terminate | Out-Null }
Start-Sleep -Milliseconds 800
# start TM at NORMAL privilege (inherits this unelevated context)
Start-Process "C:\green\TrafficMonitor\TrafficMonitor.exe"
Start-Sleep -Seconds 20
$tm = Get-Process TrafficMonitor -ErrorAction SilentlyContinue
if ($tm) { "TM running PID=$($tm.Id)" | Out-File $out -Append -Encoding utf8 } else { "TM NOT running" | Out-File $out -Append -Encoding utf8 }
$etw = "C:\green\TrafficMonitor\plugins\ProcessNetMonitor\debug\etw_capture.log"
$cap = "C:\green\TrafficMonitor\plugins\ProcessNetMonitor\debug\capture.log"
"=== etw tail ===" | Out-File $out -Append -Encoding utf8
if (Test-Path $etw) { Get-Content $etw -Tail 12 | Out-File $out -Append -Encoding utf8 }
"=== capture tail ===" | Out-File $out -Append -Encoding utf8
if (Test-Path $cap) { Get-Content $cap -Tail 12 | Out-File $out -Append -Encoding utf8 }
"DONE" | Out-File $out -Append -Encoding utf8
