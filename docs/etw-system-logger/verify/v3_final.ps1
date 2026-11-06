$out = "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\docs\etw-system-logger\verify\v3_final.txt"
# 1) 杀 TM（当前走 L2/NTKL）
$procs = Get-CimInstance Win32_Process -Filter "Name='TrafficMonitor.exe'"
foreach ($p in $procs) { Invoke-CimMethod -InputObject $p -MethodName Terminate | Out-Null }
Start-Sleep -Milliseconds 800
# 2) 重启 TM（RunAs）
Start-Process -Verb RunAs "C:\green\TrafficMonitor\TrafficMonitor.exe"
Start-Sleep -Seconds 15
# 3) 清掉 TM L2 死后残留的 NT Kernel Logger 会话（v1.18 时代老问题同款；
#    新版 TM L1 成功后不再碰 NTKL，此残留是上一轮 L2 兜底的遗留）
"=== stop leftover NTKL (from L2 fallback round) ===" | Out-File $out -Encoding utf8
logman stop "NT Kernel Logger" -ets 2>&1 | Out-File $out -Append -Encoding utf8
Start-Sleep -Seconds 3
# 4) 终态快照：PnmNetTrace 在跑 + NT Kernel Logger 空闲（保留槽让给其他工具）
"=== final session snapshot ===" | Out-File $out -Append -Encoding utf8
logman query -ets 2>&1 | Out-File $out -Append -Encoding utf8
"DONE" | Out-File $out -Append -Encoding utf8
