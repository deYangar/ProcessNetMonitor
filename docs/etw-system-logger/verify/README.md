# ETW System Logger 切换 + PID 复用修复——施工验证留档（2026-11-06）

环境：Win11 26300 x64，TM 1.86（green 位 `C:\green\TrafficMonitor\`）提权运行，插件 v1.19.0-dev（本次施工产物，x64 DLL 700928 B / x86 581120 B，`/MT /OPT:REF`）。

## 验证手册 V1-V8 结论

| # | 场景 | 结论 | 证据 |
|---|------|------|------|
| V1 | 管理员正常路径 | **PASS（实测）** | `logman query -ets` 列表出现 `PnmNetTrace`（正在运行）；etw log `StartTrace('PnmNetTrace', system logger) OK (own session, maxbuf=120)`；周期行 10:06:23 `total=38` → 10:18:22 `total=375923348` 持续增长（`etw_log_digest.txt`） |
| V2 | 占用场景（核心） | **PASS（等效实测）** | ProcMon 未安装，占用者为系统存留的 NT Kernel Logger 会话（旧版插件被杀后的孤儿会话，等效「他人占用保留名」）。证据链：etw log `NT Kernel Logger busy (183), owner(s): (unknown) - not touching it`；TM 重启前后 `logman` 快照 NTKL 均在运行（未被 stop）；TM 侧数据正常增长 |
| V3 | 反向互不干扰 | **PASS（等效实测）** | 终态快照（`v3_final.txt`）：TM 走 L1 `PnmNetTrace` 正在运行 + `NT Kernel Logger` **不在会话列表**（保留槽空闲，其他工具随时可用）。ProcMon 未安装，无法实测「对方正常抓取」半边，如实标注 |
| V4 | 非管理员 | **无法构造（环境限制，如实标注）** | 本机所有启动路径均提权：TM.exe manifest 请求管理员 + UAC 静默提升，非提权 shell `Start-Process` 起的 TM 实测 `token elevated = True`（GetTokenInformation 实证）。`status=5` 路径转代码审查（与 V7 同等）。附：ctypes 裸调探针（`probe_starttrace.py`，已删）结构手工拼装不可靠，rc=161 不采信 |
| V5 | 崩溃残留接管 | **PASS（实测 ×2）** | 10:06:16 与 10:10:02 两轮：`session 'PnmNetTrace' leftover - stop rc=234` →（5s 重试）→ `StartTrace('PnmNetTrace', system logger) OK` → 数据恢复。**实测偏差**：stop 返回 234（ERROR_MORE_DATA）而非方案预期的 0——stop 实际生效（次轮 StartTrace 直接成功），234 疑为 props 缓冲小于系统想写回的数据；另有一轮（10:19:42）234 后当轮落 L2 空闲兜底成功（设计内降级路径，见「实测发现」） |
| V6 | EnableFlags 生效判据 | **PASS（实测）** | 12+ 分钟周期行 `net=`/`kept=`/`tcpU/tcpD/udpU/udpD` 全部持续增长（digest 首中尾五拍）；§5.6 备选补启用分支**未触发、不需要** |
| V7 | Win7 | 代码审查（无环境，如实标注） | `PNM_GetOSBuild < 9200` 跳过 L1 直接 L2；L2 行为=现状减去抢占/attach；极端场景（NTKL 被占）落 L3 legacy 有数据。瀑布保底逻辑与 §5.4 代码块一致 |
| V8 | 长跑 1 天+ | **待日常观察** | 施工会话内事件流稳定（含 PID 复用 burner 高压段 60 进程/秒 × 15 分钟，无 `ProcessTrace returned` 反复重连刷屏）；内存/句柄稳定性待咩咩日常使用观察 |

## 实测发现（与方案的偏差）

1. **残留 stop 返回码 234 而非 0**（`ERROR_MORE_DATA`）：**已修（2026-11-06 咩咩拍板 A+D，commit 79aaa0b）**。根因=stop 的 props 缓冲没留会话名空间，系统写回名字放不下返回 234（stop 本身生效）。修法 A：stop 缓冲补名字空间（返回码如实变 0）；修法 D：StartTrace 做残留是否清除的唯一事实源（只对 5/拒绝访问提前降级）。修复后实测（10:48:45）：`stop rc=0` + **同一秒** `StartTrace('PnmNetTrace') OK` + 2 秒后数据恢复——强杀重启首轮即接管 L1，不再借道 NTKL。连带消除发现 2。
2. ~~L2 会话在 TM 被强杀时同样残留~~：发生前提是「先经历过 L2 兜底轮」，发现 1 修复后 L2 兜底轮几乎不再出现（仅 Win7 走 L2），此场景随之消失。

## PID 复用修复验证（E3/E4）

- **自动脚本 PASS**：`pid_reuse_run.txt`——30 秒命中 **58 次** `PID-REUSE reset`，全部 `src=procstart`（检测点 A：ETW ProcessStart 钩子秒级重置；周期核对 B/C 首见记录静默维护，未到 10s 节流窗口无需报警）。样本：`PID-REUSE reset pid=29048 old=chrome.exe`（chrome 退出后 PID 被 burner 复用，瞬间重置）
- 脚本三处实测修正（vs 方案 E3 原稿）：
  1. 日志路径 `--log-dir` 默认值：方案写 `x64\plugins`，green 位实际为 `plugins\ProcessNetMonitor\debug`
  2. **loopback 打流量无效**：插件对 127.x 事件一律过滤（etw_capture.cpp SkipByLocalAddr 实测 filtAddrs=[127.0.0.1]），改连物理网卡 IP（自动探测默认路由出口 192.168.0.171）
  3. **日志为无 BOM UTF-16LE**（fwprintf 写出），utf-8 直读 NUL 混入导致子串漏检，read_log_text 编码探测修正
  4. 纯流量生灭推不动 PID 轮转（Windows PID 递增到上限才回绕），新增 burner（60 空进程/秒）烧穿计数器
- **green 日常实测（主验证）**：待咩咩日常观察——详情窗 node 组收缩为真 node、换主后 ≤2s 归位、速度无伪尖峰
- 回归：事件流稳定（周期行持续、无重连风暴）；历史 tab/悬浮窗待日常观察

## 文件清单

- `tmp_logman.txt`——V2 快照（提权 logman，注意脚本第二次执行覆盖了第一次，第一次内容见上表 V2 行）
- `v4_unelevated.ps1` / `v4_out.txt`——V4 尝试记录（token elevated=True 实证在此）
- `v3_ntkl_free.ps1` / `v3_ntkl_free.txt`——清孤儿 NTKL → 重启 TM → L2 兜底轮记录
- `v3_final.ps1` / `v3_final.txt`——V3 终态（PnmNetTrace 在跑 + NTKL 空闲）
- `etw_log_digest.txt`——etw_capture.log 关键行 + 周期行增长五拍摘录
- `pid_reuse_run.txt`——PID 复用自动验证输出（58 命中 PASS）
- `run_logman.ps1`——提权 logman 快照辅助
- 脚本副本：`tools/verify_pid_reuse/run_verify.py`（正式版）、`tools/deploy_green.ps1`（green 位部署）
