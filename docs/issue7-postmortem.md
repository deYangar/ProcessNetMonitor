# Issue #7 事件复盘（2026-08-14）

> 用户报告：加载 ProcessNetMonitor 插件后持续弹出「遇到不适当的参数」报错弹窗，且退出时有崩溃（crash.dmp）。
> 涉及：ProcessNetMonitor v1.10.0/v1.11.0 → v1.12.0（修复版）、TrafficMonitor 1.86 x64、Win11 25H2、天气插件 1.03（Weather.dll）、搜狗输入法。

---

## 一、事件概述

报告者（hgdjnn）环境：TrafficMonitor 1.86 x64 + ProcessNetMonitor 插件 + 天气插件 1.03 + PowerMonPlugin + HardwareMonitor + 搜狗输入法 + Windhawk。

两个症状：
1. **弹窗**：加载插件后持续弹出「遇到不适当的参数。」（标题 TrafficMonitor）
2. **崩溃**：crash.log/crash.dmp 记录多次 0xc0000005（访问违例，读地址 0x10）

## 二、诊断结论（两个问题，两个根因）

### 问题 1：退出/卸载崩溃 —— 插件侧缺陷，已修复（v1.12.0）

**根因**：DLL 卸载路径（CRT 静态析构）中 `~CTooltipPopup` 调用 `DestroyWindow`，销毁触发同步用户态回调（用户32 → MFC → **搜狗输入法 SogouTSF.ime**），IME 回调内空指针崩溃。

**证据**：用 v1.11.0 标签重建带符号 DLL（导出表 RVA 与 release 完全一致），符号化用户 crash.dmp 调用栈，确认 `_execute_onexit_table`（CRT 静态析构）→ `~CTooltipPopup` → `win32u` → `KiUserCallbackDispatcher` → user32/MFC → SogouTSF.ime → ntdll AV。

**修复**（v1.12.0）：
- `DllMain(DLL_PROCESS_DETACH)` 置 `g_shutting_down` 标志
- 卸载路径析构跳过 `DestroyWindow`（窗口随进程销毁）
- 非卸载路径走 `SafeDestroyWindow`：先 `ImmAssociateContext(NULL)` 摘除 IME 关联，再 SEH 包裹 `DestroyWindow` 兜底
- SEH 放在独立函数（C2712：析构函数含对象展开不能直接用 `__try`）

**验证**：报告者 v1.12.0 下不再产生 crash.log（卸载崩溃消除）。✅

### 问题 2：「遇到不适当的参数」弹窗 —— 天气插件线程安全缺陷，非本插件问题

**弹窗本质**：MFC 资源串 `AFX_IDS_INVALID_ARG_EXCEPTION`（0xF025）"Encountered an improper argument."，由 MFC 顶层异常处理（`CWinApp::ProcessWndProcException` → `ReportError`）显示——即 TM 进程内某 MFC 代码抛出了未捕获的 `CInvalidArgException`。本插件纯 Win32、零 MFC 依赖（dumpbin 验证），不可能抛出该异常。

**根因**：天气插件（Weather.dll 1.03，官方仓库 zhongyang219/TrafficMonitorPlugins 的 MFC 插件）**跨线程无锁读写 `std::wstring`**：

```cpp
// Weather.h:44 —— 无锁共享的提示文本
std::wstring m_tooltop_info;

// Weather.cpp:400 —— 每次刷新起后台线程
void CWeather::SendWetherInfoQequest()
{
    if (!m_is_thread_runing)    //确保线程已退出
        AfxBeginThread(ThreadCallback, nullptr);   // ← 后台线程
}

// Weather.cpp:27 —— 后台线程：网络请求 + 解析 + 写入
UINT CWeather::ThreadCallback(LPVOID dwUser)
{
    ...
    if (CCommon::GetURL(url, weather_data, ...))   // 网络请求
    {
        if (m_instance.ParseJsonData(weather_data)) // ← 内部执行 m_tooltop_info = wss.str()
        ...
    }
}

// Weather.cpp:300 —— TM 主线程每秒读取同一字符串
const wchar_t* CWeather::GetTooltipInfo()
{
    if (g_data.m_setting_data.m_show_weather_in_tooltips)
        return m_tooltop_info.c_str();   // ← 无锁并发读
    else
        return L"";
}
```

后台线程（`AfxBeginThread`）每 30 分钟写一次 `m_tooltop_info`；TM 主线程每秒调 `GetTooltipInfo()` 读取并拼接进鼠标提示。无锁并发读写 `std::wstring` 为 C++ 未定义行为：写入触发 realloc 的间隙（释放旧缓冲 → 更新指针），读取可能拿到悬垂指针/指针长度不同步 → 拷贝出畸形/超长文本 → TM 的 MFC 代码处理 tooltip 时抛 `CInvalidArgException` → 弹窗。

**为什么单独用都正常、共存才弹**（报告者实测）：
- 单独天气插件：30 分钟写一次，撞上 TM 每秒读取的概率极低
- 单独本插件：提示文本为定长数组、任何时刻都合法终止，结构上不会产生垃圾
- 两者共存：本插件提示每秒更新，tooltip 链路持续高负载；天气插件写入撞上读取的概率被放大，撞上一次即持续弹窗（tooltip 控件被喂过一次巨长/畸形文本后状态损坏）

**上游佐证**：zhongyang219/TrafficMonitor#1219（2022 年报告，WeatherPro 插件触发，同架构同问题，至今未修）。

**报告者验证结果**：禁用 ProcessNetMonitor 或天气插件任一，均正常；同时启用即弹窗。与上述机制完全吻合。

## 三、处理记录

| 时间 | 动作 |
|---|---|
| 08-14 上午 | 收到 issue，下载附件（crash.dmp、日志、plugins.zip）|
| 08-14 | 重建 v1.11.0 符号 → 符号化 dump → 定位卸载崩溃根因 |
| 08-14 | 定位弹窗为 MFC `CInvalidArgException`（0xF025），排除本插件直接抛出可能 |
| 08-14 | 修复卸载崩溃 + 加载初期显示项 ID 重复（构造时初始化）|
| 08-14 | 发布 v1.12.0（正式版），回复 issue |
| 08-14 | 报告者实测：崩溃修复生效，弹窗依旧；提供 plugins.zip |
| 08-14 | 下载天气插件源码（官方仓库）→ 锁定线程安全缺陷 → 回复附源码证据 |
| 08-14 | 本地复现尝试：装入报告者全部插件、开启 tooltip、多次重启触发天气刷新、VEH 诊断版记录 C++ 异常——均未复现（依赖天气数据/时序状态）|

## 四、遗留事项

- [ ] 天气插件线程安全问题 → 已建议报告者向官方仓库反馈（TrafficMonitorPlugins Issues）；临时规避：关闭天气插件「在鼠标提示中显示天气」或 TM「显示鼠标提示」
- [ ] 本插件本地复现未成功：可提供带 C++ 异常调用栈记录的诊断版给报告者，若复现可抓取 100% 证据
- [ ] 报告者验证关闭 tooltip 后弹窗是否消失

## 五、经验教训

1. **第三方插件共存问题，先怀疑插件间共享数据路径**：TM 的 tooltip 链路（`GetPlauginTooltipInfo` 遍历所有插件）是所有插件共享的通道，任何插件的无锁数据共享都会在此爆发。
2. **MFC 弹窗 ≠ 调用方直接抛出**：弹窗文本来自 MFC 资源串，需区分「谁抛的异常」与「谁显示的弹窗」。VEH 记录 first-chance C++ 异常调用栈是最快定位手段。
3. **符号化依赖版本匹配**：v1.11.0 标签重建 DLL 与 release 的 .text 布局一致（仅 .rdata 0x10 偏移差异），导出表 RVA 完全一致，可作符号化基准。
4. **卸载路径销毁窗口是高危操作**：DestroyWindow 触发同步回调（IME/TSF 钩子），必须在 DllMain DETACH 前置关闭标志 + IME 摘除 + SEH 兜底。
5. **固定数组 vs std::wstring 的无锁共享安全性差异**：定长数组逐字符覆盖永远合法终止（读安全，仅内容混合）；std::wstring realloc 会制造悬垂窗口（读不安全）。
