$out = "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\docs\etw-system-logger\verify\tmp_logman.txt"
logman query -ets 2>&1 | Out-File -FilePath $out -Encoding utf8
"DONE" | Out-File -FilePath $out -Append -Encoding utf8
