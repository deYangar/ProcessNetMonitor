# PID 复用身份错乱修复——施工图（照做级）

状态：**已施工完成（2026-11-06）**。P1-P7 全部落地（P6 有一处编译级修正：`std::string + const wchar_t*` 不合法，src 窄化转换后拼接）；编译零 error 零 warning，已部署 green 位。
验证：自动脚本 30 秒命中 58 次 `PID-REUSE reset`（全部 src=procstart，检测点 A 实测工作），PASS；周期核对 B/C 首见记录静默维护。green 日常实测（node 组收缩/图标归位）待日常观察。
脚本实测修正 4 处见 `etw-system-logger/verify/README.md`（日志路径、loopback 被过滤、UTF-16LE 编码、PID 烧穿 burner）。
原则：每个 Patch 给锚点原文 + 完整可粘贴代码，改完逐 Patch 可编译。行号基于当前 master（锚点代码才是真正定位，行号漂移以锚点为准）。

根因一句话：PID 被系统复用后，累计条目名字冻结在旧主人（node）、路径实查拿到新主人（postgres），身份 = PID + 创建时间即可根治。

---

## Patch 总览（顺序施工，8 个 Patch / 4 文件）

| # | 文件 | 内容 |
|---|------|------|
| P1 | etw_capture.h | `Cum` 加 2 字段 + 3 常量 + 1 声明 |
| P2 | etw_capture.cpp | 辅助函数：`QueryOwnerStamp`（static）+ `ResetCumLocked` |
| P3 | etw_capture.cpp | OnEvent ProcessStart 段插检测点 A（换主秒级重置） |
| P4 | etw_capture.cpp | `GetStats` 整函数替换为三段式（检测点 B/C） |
| P5 | capture.h | 侧表 `m_owner_stamps` + 3 常量 + 2 声明 |
| P6 | capture.cpp | 辅助：`QueryOwnerStamp`（static）+ `ResetStatsLocked` |
| P7 | capture.cpp | `PacketCapture::GetStats` 三段式改造（检测点 B/C，降级路线） |
| P8 | — | 编译 + 部署 + 验证（命令与脚本见 §E） |

---

## P1 — etw_capture.h：Cum 加字段

**锚点 1**（etw_capture.h:84-93，`struct Cum`）：

```cpp
    struct Cum {
        uint64_t sent = 0;
        uint64_t recv = 0;
        uint64_t prev_sent = 0;
        uint64_t prev_recv = 0;
        ULONGLONG last_seen = 0;
        int idle_rounds = 0;
        bool seen = false;      // first GetStats snapshot only sets the baseline
        std::wstring name;      // resolved process name - kept after process exits
    };
```

**替换为**：

```cpp
    struct Cum {
        uint64_t sent = 0;
        uint64_t recv = 0;
        uint64_t prev_sent = 0;
        uint64_t prev_recv = 0;
        ULONGLONG last_seen = 0;
        int idle_rounds = 0;
        bool seen = false;      // first GetStats snapshot only sets the baseline
        std::wstring name;      // resolved process name - kept after process exits
        // PID 复用防护（2026-11-06）：进程身份 = PID + 创建时间。
        // owner_ct = 当前主人的进程创建时间（FILETIME 转 ULONGLONG，0=未知，
        // 首次核对时记录）。GetStats 周期比对：不一致 = PID 已换主，条目重置。
        ULONGLONG owner_ct = 0;
        ULONGLONG ct_check_tick = 0;  // 上次 CreationTime 核对时间（节流用）
    };
```

**锚点 2**（etw_capture.h:135-138，kCumMax 常量区）：

```cpp
    static const size_t kCumMax = 4096;
    static const size_t kCumLow = 3500;
```

**替换为**：

```cpp
    static const size_t kCumMax = 4096;
    static const size_t kCumLow = 3500;
    // PID 复用核对参数（两路线一致）：活跃窗 60s（死条目不占核对名额）、
    // 核对节流 10s、每轮核对上限 32 条（放锁段执行，防大批量卡顿）。
    static const ULONGLONG kCtActiveWindowMs = 60000;
    static const ULONGLONG kCtCheckIntervalMs = 10000;
    static const size_t kCtCheckBatch = 32;
```

**锚点 3**（etw_capture.h:170，`void LogLine` 声明行前）插入：

```cpp
    // PID 换主时重置累计条目（caller 持 m_mutex）。new_path 非空时顺带刷新
    // 名字/路径缓存（权威值来自核对句柄）。src 仅用于日志（procstart/periodic）。
    void ResetCumLocked(DWORD pid, ULONGLONG new_ct, const wchar_t* src, const std::wstring& new_path);
```

---

## P2 — etw_capture.cpp：两个辅助函数

**插入位置**：`EtwCapture::GetStats` 函数定义之前（锚点：`std::vector<ProcTraffic> EtwCapture::GetStats(double interval_sec) {` 行的正上方）。

```cpp
// ---- PID 复用防护辅助 ----
// FILETIME → 64 位比较值（进程创建时间是其固有属性，不受运行期时钟调整影响）
static ULONGLONG FtToUll(const FILETIME& ft) {
    return (ULONGLONG)ft.dwLowDateTime | ((ULONGLONG)ft.dwHighDateTime << 32);
}

// 打开 pid 取创建时间 + 权威全路径。失败返回 false（进程退出/受保护）。
// 纯内核快速调用，必须只在放锁段调用（持锁不做重活）。
static bool EtwQueryOwnerStamp(DWORD pid, ULONGLONG& ct, std::wstring& path) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME crea, exit_t, kern, user;
    bool ok = GetProcessTimes(h, &crea, &exit_t, &kern, &user) != FALSE;
    if (ok) {
        ct = FtToUll(crea);
        wchar_t buf[512]; DWORD n = 512;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) path = buf;
    }
    CloseHandle(h);
    return ok;
}

void EtwCapture::ResetCumLocked(DWORD pid, ULONGLONG new_ct, const wchar_t* src,
                                const std::wstring& new_path) {
    auto it = m_cum.find(pid);
    if (it == m_cum.end()) return;
    Cum& c = it->second;
    LogLine(L"PID-REUSE reset pid=%lu old=%ls src=%ls", pid, c.name.c_str(), src);
    c = Cum{};            // 计数/基线/seen/name 全清；seen=false 下拍只立基线，无伪速度
    c.owner_ct = new_ct;  // 0 = 未知（procstart 钩子拿不到 ct），留待周期核对首见记录
    m_name_retry_ts.erase(pid);
    if (!new_path.empty()) {
        std::lock_guard<std::mutex> nlk(m_name_mutex);  // 锁序 m_mutex→m_name_mutex，与 GetStats/ProcName 同向
        m_path_cache[pid] = new_path;
        size_t p = new_path.find_last_of(L"\\/");
        if (p != std::wstring::npos) m_name_cache[pid] = new_path.substr(p + 1);
    }
}
```

---

## P3 — etw_capture.cpp：OnEvent ProcessStart 段插检测点 A

**插入位置**：OnEvent 的 ProcessStart 处理块内，`if (!base.empty()) {` 整块（缓存刷新 + PS path 兜底）的闭合 `}` 之后、外层布局校验 if（条件含 `ud[53] >= 1 && ud[53] <= 15`）的闭合 `}` 之前。

**锚点原文**（该块尾部，约 624-630 行）：

```cpp
                            // file IO outside the lock (log writes are slow)
                            LogLine(L"PS path: pid=%lu path=%ls", pid, buf);
                        }
                        CloseHandle(h);
                    }
                }
            }
        }
    }
```

**在其后追加**（保持与 `if (!base.empty())` 同级的缩进，即 8 空格）：

```cpp
        // PID 复用检测点 A（2026-11-06）：realtime ProcessStart（opcode 1）=
        // 内核此刻在该 PID 上创建新进程。PID 不可能复用给活着的进程，故
        // 事件命中现存累计条目即为换主铁证（无需名字比对，规避 issue#15
        // 内核截断名的前缀误判；同 exe 重启复用 PID 同样命中）。replay
        // (opcode 3) 不参与：session 启动/接管时 m_cum 为空或属 Start 流程。
        // 顺序在缓存刷新之后：当拍并发 GetStats 至少读到新主人名字。
        // 锁序：先刷缓存(m_name_mutex,已放)再重置(m_mutex)，不嵌套。
        if (rec->EventHeader.EventDescriptor.Opcode == 1) {
            std::lock_guard<std::mutex> lk(m_mutex);
            ResetCumLocked(pid, 0, L"procstart", L"");
        }
```

> 注：若 `base` 解析失败（系统进程无 CommandLine），本钩子依然生效（在 `if (!base.empty())` 之外），缓存留给周期核对刷新。

---

## P4 — etw_capture.cpp：GetStats 整函数替换（三段式）

**锚点**：`std::vector<ProcTraffic> EtwCapture::GetStats(double interval_sec) {` 至其函数尾（原 759-814 行），整函数替换为：

```cpp
std::vector<ProcTraffic> EtwCapture::GetStats(double interval_sec) {
    std::vector<ProcTraffic> out;
    if (interval_sec < 0.05) interval_sec = 0.05;

    // 三段式（持锁不做重活）：段1 持锁产出+收集待核对清单 → 段2 放锁
    // OpenProcess/GetProcessTimes → 段3 再持锁判定换主并重置。
    // 段2 期间条目可能被消费线程新增/驱逐（kCumMax），段3 一律 find 不假设存在。
    std::vector<DWORD> stamp_reqs;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ULONGLONG now = GetTickCount64();
        for (auto& [pid, c] : m_cum) {
            c.idle_rounds++;

            ProcTraffic pt;
            pt.pid = pid;
            pt.bytes_sent = c.sent;
            pt.bytes_recv = c.recv;
            uint64_t ds = c.sent - c.prev_sent;
            uint64_t dr = c.recv - c.prev_recv;
            c.prev_sent = c.sent;
            c.prev_recv = c.recv;
            // First snapshot: only set the baseline - never report the cumulative
            // bytes as a delta (that produced a huge bogus speed on plugin start).
            if (!c.seen) {
                c.seen = true;
                pt.speed_up = 0;
                pt.speed_down = 0;
            } else {
                pt.speed_up = (double)ds / interval_sec;
                pt.speed_down = (double)dr / interval_sec;
            }
            pt.conn_count = 0;
            // Re-resolve when empty OR when the previous lookup failed ("<pid>"):
            // the failed placeholder must never become durable, otherwise an
            // exiting/protected process shows "<pid>" forever. Once a real name
            // is resolved it stays (survives process exit).
            // Backoff: a dead process can never resolve - don't burn a Toolhelp
            // snapshot every tick for it; retry at most every 30s.
            if (c.name.empty() || c.name[0] == L'<') {
                auto rit = m_name_retry_ts.find(pid);
                if (rit == m_name_retry_ts.end() || now - rit->second > 30000) {
                    std::wstring try_name = ProcName(pid);
                    if (try_name.empty() || try_name[0] == L'<') {
                        m_name_retry_ts[pid] = now;   // failed: back off 30s
                        if (c.name.empty()) c.name = try_name;  // show "<pid>" for now
                    } else {
                        c.name = try_name;            // resolved - durable
                    }
                }
            }
            pt.name = c.name;
            pt.exe_path = ProcPath(pid);
            // Keep every process that has ever had traffic - never evict
            // (user request 2026-08-04: drop the idle-removal entirely)
            out.push_back(std::move(pt));

            // PID 复用检测点 B（周期核对清单收集）：
            // - 活跃窗内（60s 内有过流量）才核对，死条目不占名额
            // - owner_ct==0（首见，需记录）或距上次核对 ≥10s
            // - 每轮上限 32 条；先记账 ct_check_tick 防重入（查询失败也等下个周期）
            if (stamp_reqs.size() < kCtCheckBatch &&
                now - c.last_seen < kCtActiveWindowMs &&
                (c.owner_ct == 0 || now - c.ct_check_tick >= kCtCheckIntervalMs)) {
                stamp_reqs.push_back(pid);
                c.ct_check_tick = now;
            }
        }
        LogPeriodicLocked();
        (void)now;
    }

    // 段2：放锁查询（OpenProcess/GetProcessTimes/QueryFullProcessImageName 均为
    // 纯内核快速调用，但遵守持锁不做重活铁律放锁执行）
    struct StampRsp { DWORD pid; bool ok; ULONGLONG ct; std::wstring path; };
    std::vector<StampRsp> stamp_rsps;
    for (DWORD pid : stamp_reqs) {
        StampRsp r{ pid, false, 0, L"" };
        r.ok = EtwQueryOwnerStamp(pid, r.ct, r.path);
        stamp_rsps.push_back(r);
    }

    // 段3：再持锁判定（检测点 C 首见记录在此合并：owner_ct==0 即记录）
    if (!stamp_rsps.empty()) {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto& r : stamp_rsps) {
            auto it = m_cum.find(r.pid);
            if (it == m_cum.end() || !r.ok) continue;  // 已被驱逐/查询失败：下轮再试
            Cum& c = it->second;
            if (c.owner_ct == 0) {
                c.owner_ct = r.ct;      // 首见记录：ct 即真主人（查询时主人活着）
            } else if (c.owner_ct != r.ct) {
                // 换主：重置累计并刷新缓存（new_path 即新主人权威路径）
                ResetCumLocked(r.pid, r.ct, L"periodic", r.path);
            }
            // owner_ct == r.ct：同一主人，无事
        }
    }
    return out;
}
```

> 当拍语义：重置发生在 `out` 产出之后，本拍仍显示旧主人名/速度（数据本就属于旧主人），下一拍（1s）归位。历史 tab 最后一拍按旧名落账，不改姓。

---

## P5 — capture.h：侧表 + 常量 + 声明

**锚点 1**（capture.h:131-132，名字/路径缓存声明后）追加：

```cpp
    std::map<DWORD, std::wstring> m_name_cache;
    std::map<DWORD, std::wstring> m_path_cache;
    // PID 复用防护（2026-11-06）：进程身份 = PID + 创建时间。侧表记录各 pid
    // 当前主人的创建时间与上次核对时间，GetStats 周期比对，不一致即重置
    // m_stats 条目（不污染 ProcTraffic wire 结构）。由 m_mutex 保护。
    struct OwnerStamp {
        ULONGLONG ct = 0;          // 主人创建时间（FILETIME→ULONGLONG，0=未知）
        ULONGLONG check_tick = 0;  // 上次核对时间（节流）
    };
    std::map<DWORD, OwnerStamp> m_owner_stamps;
    static const ULONGLONG kCtActiveWindowMs = 60000;
    static const ULONGLONG kCtCheckIntervalMs = 10000;
    static const size_t kCtCheckBatch = 32;
```

**锚点 2**（capture.h:168，`void LogStartupFailure();` 声明行前）插入：

```cpp
    // PID 换主时重置 m_stats 条目与关联累计表（caller 持 m_mutex）
    void ResetStatsLocked(DWORD pid, ULONGLONG new_ct, const wchar_t* src,
                          const std::wstring& new_name, const std::wstring& new_path);
```

---

## P6 — capture.cpp：辅助函数

**插入位置**：`PacketCapture::GetProcessPath` 函数结束后（锚点：GetProcessPath 的 `return path;\n}` 之后、`bool PacketCapture::Start` 之前）。

```cpp
// ---- PID 复用防护辅助（降级路线，与 EtwCapture 同语义）----
static ULONGLONG FtToUll(const FILETIME& ft) {
    return (ULONGLONG)ft.dwLowDateTime | ((ULONGLONG)ft.dwHighDateTime << 32);
}

static bool LegacyQueryOwnerStamp(DWORD pid, ULONGLONG& ct, std::wstring& path) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME crea, exit_t, kern, user;
    bool ok = GetProcessTimes(h, &crea, &exit_t, &kern, &user) != FALSE;
    if (ok) {
        ct = FtToUll(crea);
        wchar_t buf[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) path = buf;
    }
    CloseHandle(h);
    return ok;
}

void PacketCapture::ResetStatsLocked(DWORD pid, ULONGLONG new_ct, const wchar_t* src,
                                     const std::wstring& new_name, const std::wstring& new_path) {
    auto it = m_stats.find(pid);
    if (it == m_stats.end()) return;
    // 日志不带旧名（WriteLog 走窄字符，中文名转码无谓踩坑），pid+src 足够排查
    WriteLog("PID-REUSE reset pid=" + std::to_string(pid) + " src=" + src);
    ProcTraffic fresh;   // 计数全零；name/path 直接用核对拿到的新主人权威值
    fresh.pid = pid;
    fresh.name = new_name;
    fresh.exe_path = new_path;
    m_stats[pid] = fresh;
    // 关联累计表一并清（m_tcp_cum 每轮 QueryTcpStats 全量重建，无需清）：
    // prev 表不清的话新条目 bytes=0 对旧基线算出大负 delta（虽被 d_in>0 滤掉，
    // 清掉语义更干净）；udp_cum 是累计计数，旧主人字节不能留给新主人
    m_udp_cum.erase(pid);
    m_tcp_prev.erase(pid);
    m_udp_prev.erase(pid);
    // 缓存覆盖（legacy 侧两张缓存由 m_mutex 保护，此处本就持锁）
    if (!new_name.empty()) m_name_cache[pid] = new_name;
    if (!new_path.empty()) m_path_cache[pid] = new_path;
    m_owner_stamps[pid].ct = new_ct;
}
```

> P5/P6 的 `FtToUll` 与 P2 的同名 static 函数分属两个编译单元（TU 内 static，无链接冲突），刻意不共享——两路线本就独立演进。

---

## P7 — capture.cpp：PacketCapture::GetStats 三段式

**改造方式**：三处精准修改，不整函数替换（该函数长，整替 diff 噪音大）。

**改 1**：锁改可释放。锚点（capture.cpp:836）：

```cpp
    std::vector<ProcTraffic> result;
    std::lock_guard<std::mutex> lk(m_mutex);
```

替换为：

```cpp
    std::vector<ProcTraffic> result;
    std::unique_lock<std::mutex> lk(m_mutex);
```

**改 2**：主循环收集核对清单。锚点（capture.cpp:906-909，主循环尾部）：

```cpp
        auto cit = conn_counts.find(pid);
        st.conn_count = (cit != conn_counts.end()) ? cit->second : 0;
        result.push_back(st);
    }
```

替换为：

```cpp
        auto cit = conn_counts.find(pid);
        st.conn_count = (cit != conn_counts.end()) ? cit->second : 0;
        result.push_back(st);

        // PID 复用检测（降级路线，无 ETW 事件只有周期核对）：legacy 条目量级
        // 小（byte capture 关时仅连接表驱动），不做活跃窗过滤，10s 节流+每轮
        // 32 条上限足够；owner_ct==0 视为首见，优先核对
        ULONGLONG now_ms = GetTickCount64();
        if (stamp_reqs.size() < kCtCheckBatch) {
            auto sit = m_owner_stamps.find(pid);
            ULONGLONG last_chk = (sit != m_owner_stamps.end()) ? sit->second.check_tick : 0;
            ULONGLONG owner_ct = (sit != m_owner_stamps.end()) ? sit->second.ct : 0;
            if (owner_ct == 0 || now_ms - last_chk >= kCtCheckIntervalMs) {
                stamp_reqs.push_back(pid);
                m_owner_stamps[pid].check_tick = now_ms;
            }
        }
    }
```

**改 3**：函数尾部追加段 2/段 3。锚点（capture.cpp:917-920，函数尾）：

```cpp
    std::sort(result.begin(), result.end(), [](auto& a, auto& b) { return (a.speed_up + a.speed_down) > (b.speed_up + b.speed_down); });
    // DO NOT reset byte counters - just let them accumulate
    // prev_sent/prev_recv track the last snapshot for delta calculation
    return result;
}
```

替换为：

```cpp
    // ---- 段2：放锁查询主人创建时间（OpenProcess/GetProcessTimes 纯内核
    // 快速调用，遵守持锁不做重活铁律）----
    struct StampRsp { DWORD pid; bool ok; ULONGLONG ct; std::wstring path; };
    std::vector<StampRsp> stamp_rsps;
    for (DWORD pid : stamp_reqs) {
        StampRsp r{ pid, false, 0, L"" };
        r.ok = LegacyQueryOwnerStamp(pid, r.ct, r.path);
        stamp_rsps.push_back(r);
    }

    // ---- 段3：再持锁判定换主 ----
    if (!stamp_rsps.empty()) {
        lk.lock();
        for (auto& r : stamp_rsps) {
            if (!r.ok || m_stats.find(r.pid) == m_stats.end()) continue;
            std::wstring new_name;
            if (!r.path.empty()) {
                size_t p = r.path.find_last_of(L"\\/");
                if (p != std::wstring::npos) new_name = r.path.substr(p + 1);
            }
            auto sit = m_owner_stamps.find(r.pid);
            if (sit == m_owner_stamps.end() || sit->second.ct == 0) {
                m_owner_stamps[r.pid].ct = r.ct;   // 首见记录
            } else if (sit->second.ct != r.ct) {
                ResetStatsLocked(r.pid, r.ct, L"periodic", new_name, r.path);
            }
        }
        lk.unlock();
    }

    std::sort(result.begin(), result.end(), [](auto& a, auto& b) { return (a.speed_up + a.speed_down) > (b.speed_up + b.speed_down); });
    // DO NOT reset byte counters - just let them accumulate
    // prev_sent/prev_recv track the last snapshot for delta calculation
    return result;
}
```

**改 4**：核对清单声明。锚点（改 1 处的 `std::unique_lock` 行后）插入：

```cpp
    std::vector<DWORD> stamp_reqs;   // 待核对 CreationTime 的 pid 清单（三段式段1产物）
```

> 注意：`m_on_stats(result)` 回调（capture.cpp:912）保持在段 1 锁内原位不动——本拍重置发生在回调之后，语义与 ETW 侧一致（当拍按旧名落账，下拍归位）。

---

## P8 — 编译、部署、验证

### E1 编译

```bash
cd "C:/Users/Yang/.zcode/workspace/default/projects/ProcessNetMonitor/plugin" && cmd //c build_release.bat
```

验收：零 error 零新增 warning；产出 `ProcessNetMonitor.dll`（x64）+ `ProcessNetMonitor_x86.dll`。构建脚本已带 `/OPT:REF`（构建铁律），无需改。

### E2 部署 green 位（提权）

```powershell
# 1. 杀提权 TM（taskkill /f 会拒访问）
Get-CimInstance Win32_Process -Filter "name='TrafficMonitor.exe'" | Invoke-CimMethod -MethodName Terminate
# 2. 覆盖 DLL（x64）
Copy-Item "C:\Users\Yang\.zcode\workspace\default\projects\ProcessNetMonitor\plugin\ProcessNetMonitor.dll" "C:\green\TrafficMonitor\x64\plugins\ProcessNetMonitor.dll" -Force
# 3. RunAs 提权重启（非提权起不了 Highest 计划任务）
Start-Process -Verb RunAs "C:\green\TrafficMonitor\TrafficMonitor.exe"
```

> 以上 PowerShell 一律落 `.ps1` 文件执行（铁律：禁止 bash 内联）。

### E3 自动验证脚本

新建 `tools/verify_pid_reuse/run_verify.py`（完整代码）：

```python
#!/usr/bin/env python3
"""PID 复用修复自动验证。

原理：持续起短命子进程打 loopback 流量（插件 ETW 不过滤 loopback），进程快速
生灭制造 PID 复用；换主发生时插件写 "PID-REUSE reset" 日志行，本脚本轮询命中。

用法（建议管理员，读提权 TM 写的日志）：
  python run_verify.py --minutes 15 --log-dir "C:\\green\\TrafficMonitor\\x64\\plugins"

输出：每 30s 打印进度（已起 N 个子进程 / 日志命中 M 次）；命中>=1 次退出码 0。
"""
import argparse, glob, os, random, socket, subprocess, sys, threading, time

LISTEN_PORT = 0  # 随机端口，启动时确定

def start_listener():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(8)
    port = srv.getsockname()[1]
    def serve():
        while True:
            try:
                conn, _ = srv.accept()
                threading.Thread(target=echo, args=(conn,), daemon=True).start()
            except OSError:
                return
    def echo(conn):
        try:
            while True:
                d = conn.recv(65536)
                if not d: break
                conn.sendall(d)
        except OSError:
            pass
        finally:
            conn.close()
    threading.Thread(target=serve, daemon=True).start()
    return port

CHILD = r"""
import socket, sys
port = int(sys.argv[1]); dur = float(sys.argv[2])
s = socket.create_connection(("127.0.0.1", port), timeout=2)
t0 = __import__("time").time()
payload = b"x" * 65536
while __import__("time").time() - t0 < dur:
    s.sendall(payload)
    got = 0
    while got < len(payload):
        d = s.recv(65536)
        if not d: sys.exit(0)
        got += len(d)
"""

def scan_logs(log_dir):
    hits = []
    for f in glob.glob(os.path.join(log_dir, "*.log")) + glob.glob(os.path.join(log_dir, "*.log.bak")):
        try:
            with open(f, "r", encoding="utf-8", errors="replace") as fh:
                hits += [l.strip() for l in fh if "PID-REUSE reset" in l]
        except OSError:
            pass
    return hits

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=15)
    ap.add_argument("--log-dir", default=r"C:\green\TrafficMonitor\x64\plugins")
    args = ap.parse_args()

    port = start_listener()
    print(f"[verify] listener 127.0.0.1:{port}, log-dir={args.log_dir}", flush=True)

    deadline = time.time() + args.minutes * 60
    spawned, last_report = 0, time.time()
    while time.time() < deadline:
        # 短命流量进程：活 2-4s 自然退出，快速生灭推动 PID 轮转
        dur = random.uniform(2.0, 4.0)
        p = subprocess.Popen([sys.executable, "-c", CHILD, str(port), str(dur)])
        spawned += 1
        time.sleep(0.5)

        if time.time() - last_report >= 30:
            hits = scan_logs(args.log_dir)
            print(f"[verify] 进度: 已起 {spawned} 个子进程, PID-REUSE 命中 {len(hits)} 次", flush=True)
            for h in hits[-3:]:
                print(f"[verify]   {h}", flush=True)
            last_report = time.time()
            if hits:
                print("[verify] 判定: PASS（检测到换主重置）", flush=True)
                return 0

    hits = scan_logs(args.log_dir)
    print(f"[verify] 结束: 子进程 {spawned} 个, 命中 {len(hits)} 次 -> {'PASS' if hits else 'FAIL（未见换主记录，人工核查）'}", flush=True)
    return 0 if hits else 1

if __name__ == "__main__":
    sys.exit(main())
```

执行（后台挂起 + 完成通知，不阻塞等）：

```bash
python "C:/Users/Yang/.zcode/workspace/default/projects/ProcessNetMonitor/tools/verify_pid_reuse/run_verify.py" --minutes 15
```

### E4 验收标准

| 项 | 标准 |
|---|------|
| 编译 | 零 error，无新增 warning |
| 自动脚本 | 15 分钟内 capture 日志出现 ≥1 条 `PID-REUSE reset`，且行为符合（重置后同名/图标归位） |
| green 日常实测（主验证） | 详情窗 node 组收缩为真 node；postgres/cmd/ECAgent/bash 各归各组，图标=名字；换主后 ≤2s 归位；速度无伪尖峰 |
| 回归 | 历史 tab 旧账不变；悬浮窗速度平滑；ETW→legacy 降级后同样归位；连接数列正常 |

---

## 提交与发版

1. 全部 P1-P7 + 验证产物 → 单个自洽 commit（中文 message，例：`fix: PID 复用导致进程名/图标错乱——身份改 PID+创建时间双保险检测（etw 钩子+周期核对）`）。
2. 实测通过后拟 **v1.18.1**：CHANGELOG 条目 + release note 覆盖未发布版本惯例。
3. push / Release / issue 评论等一切上线动作：文案逐字贴出，等咩咩终审。

## 明确不做（防跑偏）

- 不动：UI 绘制（detail_window/tooltip_popup/plugin_main）、ETW 归因主链路（shape 解析/虚拟网卡过滤/字节统计）、签名分类、历史记录结构。
- 不给死条目做「查无此进程就清除」——保留历史累计是 2026-08-04 拍板，换主重置≠驱逐。
- 不做名字比对判换主——创建时间是唯一依据。
