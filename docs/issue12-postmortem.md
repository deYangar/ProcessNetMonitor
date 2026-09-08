# Issue #12 事件复盘（2026-09-08）

> 用户报告：加载 ProcessNetMonitor 插件后 TrafficMonitor 启动即崩溃（「错误信息」弹窗 + dmp），删除插件可正常启动。
> 涉及：ProcessNetMonitor v1.15.0、TrafficMonitor 1.85.1 x64、Win10 build 19045 / Win11 26100。
> Issue：https://github.com/deYangar/ProcessNetMonitor/issues/12 （报告者 XX4free25XX，2026-09-06）

---

## 一、结论（TL;DR）

1. **崩溃与 lite/全量无关**：TM 1.85.1 的 lite 和全量**都崩**，1.86 的 lite 和全量**都不崩**。真正的变量是 **TM 主程序版本**。
2. 报告者截图弹窗为 `Version: 1.85.1 x64`、`Last compiled date: 2025/02/10 16:24:52`——即 **1.85.1 全量版**（与本地实测 1.85.1 全量弹窗逐字一致；1.85.1 lite 弹窗带 ` (Lite)` 后缀且编译时间 16:19:32）。
3. 之前「lite 崩、全量不崩」的观察是**混淆变量**：测试的 lite 是 1.85.1，全量是日常使用的 1.86。
4. 根因在**插件侧**：插件 v1.15.0 把 `InitializeCriticalSection(&m_data_lock)` 和 `StartRefreshTimer()` 全部放在 `OnInitialize()` 中，而 **TM 1.85.x 的 LoadPlugins 从不调用 OnInitialize**（该调用是 TM 1.86 新增），导致锁永远处于全零状态，首次进入即崩溃。

## 二、崩溃机制（完整链条）

1. **接口兼容性**：ITMPlugin vtable 前 28 个槽位（GetItemName…IsCommandChecked）在 1.85.1 与 1.86 完全一致——旧方法调用零错位（即此前「插件接口一样」的调查结论，该结论本身正确但不完整）。1.86 在 vtable **末尾追加** `OnInitialize`，并只在 1.86 的 `CPluginManager::LoadPlugins` 里有：
   ```cpp
   if (version >= 7)
       plugin_info.plugin->OnInitialize(&theApp);   // 1.86 才有；1.85.1 无此调用
   ```
2. **锁未初始化**：1.85.1 下 `m_data_lock`（CRITICAL_SECTION，`s_instance` 成员）保持全零。
   - 正常 `InitializeCriticalSection` 后 `DebugInfo = -1`（0xFFFF…FF，ntdll 有 `cmp rax,-1` 防护）；
   - 全零锁 `DebugInfo = NULL`，无防护。
3. **触发路径**（无需定时器、无需争抢线程）：
   - TM 每秒调 `DataRequired()`：首次 `m_capture.Start()` 成功后 return；
   - 第二次起走慢路径（`m_refresh_timer_ok == false`）→ `BuildTooltip()`（plugin_main.cpp:598）→ `EnterCriticalSection(&m_data_lock)`；
   - 全零锁 `LockCount = 0`（正常为 -1），`lock inc` 后非零 → ntdll 判定「已被持有」走 `RtlpEnterCriticalSectionContended`；
   - `rax = DebugInfo = NULL` → `inc dword ptr [rax+0x24]`（RTL_CRITICAL_SECTION_DEBUG.ContentionCount，偏移 0x24）→ **写 NULL+0x24 → 0xC0000005**。
4. **dmp 硬证据**（20260908151015_TrafficMonitor.exe.dmp，1.85.1 lite 复现）：
   - 异常：0xC0000005 写违例 @ 0x24；RIP = ntdll.dll+0xfa7d（Win11 26100，`RtlpEnterCriticalSectionContended` 内，函数起始 RVA 0xf9c0）；
   - rcx = ProcessNetMonitor.dll+0x1c3a38 = `.data:0x2a38`（即 `s_instance`（`.data:0x550`，MAP 文件符号）+ 成员偏移 0x24E8 = `m_data_lock`）；
   - 栈上活动帧：`RtlEnterCriticalSection+0xf2`（0x128e2）→ Contended → 崩溃点；较老位置残留 `ComputeWindowSpeeds+0x55b`、`dllmain_crt_dispatch`（LoadPlugins/加载期痕迹）。
5. **为什么必须管理员权限才崩**：非提权时 `m_capture.Start()`（和 ETW）失败 → `m_started=false` → `DataRequired` 每次提前 return（显示 ERR）→ **永远走不到 BuildTooltip** → 不崩。提权时 Start 成功，第二次 DataRequired（启动后约 1–2 秒）即崩。与实测时序吻合。

## 三、实测矩阵（2026-09-08 本地，管理员权限，插件 v1.15.0）

| TM 版本 | 形态 | 结果 |
|---|---|---|
| 1.85.1 | Lite  | 💌 崩溃（~12 秒内出 dmp，弹窗 `1.85.1 x64 (Lite)`，编译时间 16:19:32） |
| 1.85.1 | 全量  | 💌 崩溃（弹窗 `1.85.1 x64`，编译时间 16:24:52，与报告者截图一字不差） |
| 1.86   | Lite  | ✅ 稳定（提权运行 6+ 分钟） |
| 1.86   | 全量  | ✅ 稳定（日常环境） |

非提权 1.86 lite：插件功能不完整（ETW/抓包起不来），但不崩。

## 四、修复方案（插件侧，待实施）

### 方案 1：止血（必做）
把临界区初始化挪到 `CProcessNetPlugin` 构造函数（静态对象构造在 DLL 加载期执行，1.85.x/1.86 全覆盖）：

```cpp
CProcessNetPlugin::CProcessNetPlugin() {
    m_items[0].Init(CProcessNetItem::DIR_UPLOAD);
    m_items[1].Init(CProcessNetItem::DIR_DOWNLOAD);
    m_items[2].Init(CProcessNetItem::DIR_TRANSPARENT);
    InitializeCriticalSection(&m_data_lock);
    m_lock_inited = true;
}
```

`OnInitialize` 中的原初始化保留（幂等，`m_lock_inited` 守卫）。析构中已有对应的 `DeleteCriticalSection`，无需改。

### 方案 2：兼容增强（建议同版本一起做）
`OnInitialize` 在 1.85.x 下永不执行，其中的功能初始化需要兜底。做法：首次 `DataRequired()` 时执行一次性 `EnsureInitialized()`：
- 配置目录兜底：`OnInitialize` 依赖 `p->GetPluginConfigDir()`（ITrafficMonitor 新接口，1.85.1 vtable 里不存在，**不可调用**）；兜底改用 DLL 同目录（`GetModuleFileNameW(s_dll_hinst)` → `plugins\ProcessNetMonitor\`）
- i18n：扫描 `<dll dir>\ProcessNetMonitor\lang`；`GetStringRes` 不可调用（1.86 才有），auto 模式固定 `zh-CN`
- ETW/抓包启动、`StartRefreshTimer()`、IP 库、详情窗/悬浮窗初始化照搬
- 现有 `DataRequired` 慢路径与新定时器并存的逻辑已由 `m_refresh_timer_ok` 区分，无需重构

### 明确不可为（接口红线）
1.85.x 下**禁止调用** ITrafficMonitor 的任何新方法（GetPluginConfigDir / GetStringRes / GetDPI / GetThemeColor / ShowNotifyMessage 等，均为 1.86 接口，vtable 不存在，调用即野指针）。插件代码中所有 `m_app->` 调用点需要逐一标注可用性（`m_app` 仅在 OnInitialize 被 1.86 调用后才非空，本身就是天然判据；1.85.x 下 m_app == nullptr）。

### 回归验证清单
- [ ] 1.85.1 lite + 全量，提权：启动不再崩，任务栏显示 Up/Down/透明区，右键菜单/悬浮提示可用
- [ ] 1.85.1 非提权：ERR 提示正常，无崩溃
- [ ] 1.86 lite + 全量，提权：全功能回归（定时器、ETW、i18n、详情窗、设置持久化）
- [ ] 设置保存路径在 1.85.x 下降级到 DLL 同目录（需在 README/release note 说明）

## 五、调查产物（本目录）

- `parse_dmp.py` / `walk_stack.py`：minidump 解析脚本（异常上下文、模块映射、栈回溯，无需 WinDbg）
- `ntdll_win.bin`：ntdll 崩溃点附近反汇编窗口（capstone）
- `tm1851_src/`：TM 1.85.1 官方源码（对比 LoadPlugins / PluginInterface.h 用）
- `repro/`：四个隔离测试环境（portable 模式），修复后可直接回归
  - `lite/`（1.86 lite）、`full/`（1.86 全量）、`lite1851/`、`full1851/`
  - 插件更新后覆盖 `repro/*/TrafficMonitor/plugins/` 里的 DLL 即可重测
- `img1.png` / `img2.png`：报告者截图

## 六、经验教训

1. **「接口一样」要区分调用方视角和数据方视角**：vtable 布局一致 ≠ 生命周期钩子会被调用。OnInitialize 是 vtable 末尾追加 + 上层可选调用，旧宿主「看不见」它。
2. **插件初始化不能只依赖 OnInitialize**：应假设宿主可能从不调用（老版本），关键资源（锁！）必须在构造函数/静态初始化中就绪。
3. **全零 CRITICAL_SECTION 是隐形炸弹**：无争抢时看起来能用（本例中连第一次 Enter 都会走 Contended），必须靠初始化保证，不能靠运气。可选的更稳做法：改用 SRWLOCK（SRWLOCK_INIT 即全零，零初始化就是合法状态）。
4. **ITrafficMonitor 新接口（1.86）调用点要集中管理**：所有 `m_app->` 调用集中在 OnInitialize 及其后，用 `m_app == nullptr` 做版本判据，避免 1.85.x 下野指针。
5. **混淆变量**：lite/全量对比实验中两个变量（版本、形态）未分离，导致第一轮误判。做 A/B 前先确认「两组唯一差异」。

## 七、修复实施记录（2026-09-08，v1.15.1）

### 已落地（plugin_main.cpp / .h，备份 *.backup_20260908）

1. **临界区初始化挪进构造函数**（方案 1）：DLL 加载期静态构造执行，覆盖所有宿主版本；`OnInitialize` 原位置保留幂等守卫。
2. **`InitOnce(cfg_base)` 重构**（方案 2）：`OnInitialize` 主体抽出参数化；配置目录兜底链 = `GetPluginConfigDir()`（1.86）→ `EI_CONFIG_DIR`（1.85.x，实测两者等价，均为 `<config>\plugins\`，配置/历史无缝共享）→ DLL 同目录推导。
3. **兜底入口挂在 `OnExtenedInfo(EI_CONFIG_DIR)`**（修正案）：两版 TM 都在 **LoadPlugins 主线程**同步调用它，且早于监控线程启动。首版挂在首次 `DataRequired()` 上是错的——**TM 1.85.1 的 DataRequired 由 `AfxBeginThread` 的监控工作线程调用**（MonitorThreadCallback→DoMonitorAcquisition），在工作线程创建 popup/detail 窗口 = 无消息泵 = WM_TIMER 永不分发 = hover 悬浮窗永不出现（咩咩实测反馈）。DataRequired 里的兜底降级为最后防线（宿主不传 EI_CONFIG_DIR 的极端情形）。
4. `m_app->` 全部调用点复查：仅 3 处且均有空指针守卫，1.85.x 下 m_app==nullptr 安全；auto 语言固定 zh-CN。
5. 版本号 → 1.15.1；`build_release.bat` 旧 openclaw 路径已修正。

### 回归结果（2026-09-08 本地，MSVC 14.44.35207 + SDK 26100）

| 环境 | 提权 | 结果 |
|---|---|---|
| 1.85.1 lite | ✔ | ✅ 稳定 25s+（v1.15.0 同环境 12s 内崩）；`debug\werdumps\` 创建证明 EnsureInitialized 兜底跑通；悬浮窗 "Process Net Monitor (ETW) Total: U/D + 进程列表" 正常（BuildTooltip 原崩溃点产出） |
| 1.85.1 full | ✔ | ✅ 稳定；**hover 主悬浮窗 popup 正常弹出/移开隐藏**（窗口枚举 PNMTooltip VIS↔hid 实证）；详情窗可见 |
| 1.86 lite | ✔ | ✅ 稳定 20s+，PNM 窗口全部创建（OnInitialize→InitOnce 主线程路径正常）；hover 交互受首启对话框遮挡未验，由日常环境覆盖 |
| 1.86 full | — | 咩咩日常环境自行验证（代码路径与 v1.15.0 逐行等价） |

全程零 dmp / crash.log。

## 八、遗留事项

- [x] 按方案 1+2 出修复版 v1.15.1（含线程修正案），1.85.1 双形态 + 1.86 lite 回归通过
- [ ] 回复 issue #12：说明根因（1.85.x 不调 OnInitialize）+ v1.15.1 兼容修复，建议升级插件
- [ ] 检查 v1.10–v1.14 各版本是否同样依赖 OnInitialize（若是，1.85.x + 旧版插件同样存在此雷，发版说明里提示升级）
- [ ] 发版：release note 说明 1.85.x 下配置目录走 EI_CONFIG_DIR（与 1.86 同路径）
