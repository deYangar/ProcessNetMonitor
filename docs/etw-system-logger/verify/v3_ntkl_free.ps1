$out = "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\docs\etw-system-logger\verify\v3_ntkl_free.txt"
# 1) 提权上下文里清理旧版 TM 的孤儿 NT Kernel Logger 会话（占着保留槽）
"=== stop orphan NT Kernel Logger ===" | Out-File $out -Encoding utf8
logman stop "NT Kernel Logger" -ets 2>&1 | Out-File $out -Append -Encoding utf8
# 2) 重启 TM
$procs = Get-CimInstance Win32_Process -Filter "Name='TrafficMonitor.exe'"
foreach ($p in $procs) { Invoke-CimMethod -InputObject $p -MethodName Terminate | Out-Null }
Start-Sleep -Milliseconds 800
Start-Process -Verb RunAs "C:\green\TrafficMonitor\TrafficMonitor.exe"
Start-Sleep -Seconds 15
# 3) 快照：NT Kernel Logger 应保持空闲（TM 不占它），PnmNetTrace 应在跑
"=== session snapshot after TM restart ===" | Out-File $out -Append -Encoding utf8
logman query -ets 2>&1 | Out-File $out -Append -Encoding utf8
"DONE" | Out-File $out -Append -Encoding utf8
