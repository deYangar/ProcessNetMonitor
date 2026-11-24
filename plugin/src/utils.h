#pragma once
#include <cstdio>
#include <atomic>
#include <windows.h>
#include <shellscalingapi.h>

// DLL's HINSTANCE (captured in tooltip_popup.cpp DllMain)
extern HINSTANCE s_dll_hinst;

// 进程退出且其它线程已被系统结束。Win8 起的 ExitProcess 先结束其它线程、再按初始化的
// 反序调 DllMain，走到我们的 DllMain 时，晚于本 DLL 初始化的模块都已卸载完。
// FreeLibrary 卸载、Win7 的进程退出（先调 DllMain 再结束线程）都是 false。
// 置位后退出路径的约定：不等待、不加锁、不调别的 DLL 的功能（只留落盘）。自建会话不停，
// 留给下次启动接管；兜底的 NT Kernel Logger 是例外，见 EtwCapture::Stop()。
extern bool g_pnm_exit_threads_dead;

// 「退出前先停」已装好：PNM 已经盯上 TM 主窗口的 WM_CLOSE，退出时能赶在 TM 收拾东西之前
// 把 ETW 会话干净停掉。定义在 plugin_main.cpp。EtwCapture::ConsumerLoop 读它：Win8+ 上
// 进程退出时我们不能停兜底的 NT Kernel Logger，所以只在它为真时才启用兜底。
extern std::atomic<bool> g_pnm_exit_watch_armed;

// Debug output directory: <dll dir>\ProcessNetMonitor\debug\ - sits next to
// the plugin's config folder (history.dat / settings.json live in
// <dll dir>\ProcessNetMonitor\) so everything plugin-related stays in one
// place (user requirement 2026-08-05: nothing in %APPDATA%, all local).
// ALL diagnostic outputs go here: crash.log, etw_capture.log, crash.dmp,
// werdumps/, capture.log.
// Pure Win32 + manual loop: safe to call from the zero-CRT invalid-parameter
// handler.
inline bool PNM_GetDebugDir(wchar_t* buf, size_t cap) {
    if (!buf || cap < 64) return false;
    buf[0] = 0;
    HINSTANCE h = s_dll_hinst;
    DWORD n = h ? GetModuleFileNameW(h, buf, (DWORD)cap) : 0;
    if (n == 0 || n >= cap) return false;
    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; i++) if (buf[i] == L'\\') last = i;
    if (last == (size_t)-1) return false;
    // 1) <dll dir>\ProcessNetMonitor (config folder - may not exist yet)
    static const wchar_t sub1[] = L"\\ProcessNetMonitor";
    size_t sub1len = (sizeof(sub1) / sizeof(wchar_t)) - 1;
    if (last + sub1len >= cap) return false;
    memcpy(buf + last, sub1, (sub1len + 1) * sizeof(wchar_t));
    CreateDirectoryW(buf, nullptr);
    // 2) <dll dir>\ProcessNetMonitor\debug
    static const wchar_t sub2[] = L"\\debug";
    size_t sub2len = (sizeof(sub2) / sizeof(wchar_t)) - 1;
    size_t cur = last + sub1len;
    if (cur + sub2len >= cap) return false;
    memcpy(buf + cur, sub2, (sub2len + 1) * sizeof(wchar_t));
    CreateDirectoryW(buf, nullptr);
    return true;
}

// Posted to popup/detail windows from the refresh timer thread: the window
// then pulls the latest snapshot on ITS OWN (UI) thread - never touch window
// members (std::vector etc.) from another thread (heap corruption risk).
#define WM_PNM_REFRESH (WM_APP + 1)
#define WM_PNM_LANG_CHANGED (WM_APP + 6)

// 用指定字体测量文本宽度（像素），实测即真实像素，天然适配 DPI。
// hdc 必须有效；hFont 为 nullptr 时用 DC 当前字体。
// 语言切换自动适配布局的统一测量入口（详情窗口/选项对话框共用）。
inline int PNM_MeasureText(HDC hdc, HFONT hFont, const wchar_t* text) {
    if (!text || !text[0]) return 0;
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : nullptr;
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    if (hOld) SelectObject(hdc, hOld);
    return sz.cx;
}

// Shared speed/byte formatting utilities
inline void FormatSpeed(double bps, wchar_t* buf, int n) {
    if (bps < 0.01) wcsncpy_s(buf, n, L"0 B/s", _TRUNCATE);
    else if (bps < 1024) swprintf_s(buf, n, L"%.0f B/s", bps);
    else if (bps < 1048576) swprintf_s(buf, n, L"%.1f KB/s", bps / 1024.0);
    else swprintf_s(buf, n, L"%.2f MB/s", bps / 1048576.0);
}

inline void FormatBytes(uint64_t bytes, wchar_t* buf, int n) {
    if (bytes == 0) wcsncpy_s(buf, n, L"0 B", _TRUNCATE);
    else if (bytes < 1024ULL) swprintf_s(buf, n, L"%llu B", bytes);
    else if (bytes < 1048576ULL) swprintf_s(buf, n, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824ULL) swprintf_s(buf, n, L"%.2f MB", bytes / 1048576.0);
    else swprintf_s(buf, n, L"%.2f GB", bytes / 1073741824.0);
}

// ============================================================
// ============================================================
// WER LocalDumps 登记（crash.dmp 之外的另一半转储来源；issue #20 起受
// settings.json "crash_dump" 开关控制）。root / key_path 可注入：生产环境
// 传 HKLM + PNM_WerLocalDumpsKey()，自动化验证传 HKCU 临时键，不碰真实登记。
// 登记与移除固定走 64 位注册表视图（KEY_WOW64_64KEY）：WER 只读 64 位视图，
// 崩溃的哪怕是 32 位进程也一样（2026-11-24 在 Windows 11 build 26300 上实测，
// 更老的系统没测：32 位进程崩溃时系统派的是 SysWOW64\WerFault.exe，但
// WOW6432Node 视图里的登记不生效，转储退回 %LOCALAPPDATA%\CrashDumps；
// 同一份登记写到 64 位视图才落进 DumpFolder）。
// 此前两边都不带标志，x86 版的登记一直落在 WOW6432Node 视图，等于没登记。
// 32 位 Windows 上这个标志被系统忽略，行为不变。
// ============================================================

inline const wchar_t* PNM_WerLocalDumpsKey() {
    return L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\TrafficMonitor.exe";
}

// DumpFolder 指向本插件的 debug\werdumps 才算「我们自己写的登记」——
// PTM 登记的是同一个键（后加载者覆盖 DumpFolder），不是我们的一律不动。
// 按路径后缀 + 忽略大小写匹配：portable 目录被整体搬走后旧登记仍认得出。
inline bool PNM_WerFolderIsOurs(const wchar_t* folder) {
    if (!folder) return false;
    size_t len = wcslen(folder);
    while (len > 0 && folder[len - 1] == L'\\') len--;
    static const wchar_t kSuffix[] = L"\\plugins\\ProcessNetMonitor\\debug\\werdumps";
    const size_t slen = (sizeof(kSuffix) / sizeof(kSuffix[0])) - 1;
    if (len < slen) return false;
    return CompareStringOrdinal(folder + len - slen, (int)slen, kSuffix, (int)slen, TRUE) == CSTR_EQUAL;
}

// 写 DumpFolder / DumpType=1（MiniDump）/ DumpCount=3。需要管理员权限，
// 非提权进程写不进 HKLM 时返回 false（WER 那一半转储不可用，插件自己
// 写的 crash.log / crash.dmp 不受影响）。
inline bool PNM_WerRegister(HKEY root, const wchar_t* key_path, const wchar_t* dump_dir) {
    HKEY hk = nullptr;
    REGSAM sam = KEY_SET_VALUE | KEY_WOW64_64KEY;
    if (RegCreateKeyExW(root, key_path, 0, nullptr, 0, sam, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return false;
    DWORD type = 1;   // MiniDump（曾为全量 2：单份 300-400MB，上报机器累计 1.4GB）
    DWORD count = 3;  // 最多保留 3 份
    bool ok = RegSetValueExW(hk, L"DumpFolder", 0, REG_SZ, (const BYTE*)dump_dir,
                             (DWORD)((wcslen(dump_dir) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS
        && RegSetValueExW(hk, L"DumpType", 0, REG_DWORD, (const BYTE*)&type, sizeof(type)) == ERROR_SUCCESS
        && RegSetValueExW(hk, L"DumpCount", 0, REG_DWORD, (const BYTE*)&count, sizeof(count)) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return ok;
}

enum PnmWerUnreg {
    PNM_WER_REMOVED = 0,     // 登记已移除
    PNM_WER_NOT_FOUND,       // 本来就没有登记
    PNM_WER_NOT_OURS,        // 登记指向别的目录（如 PTM 的），不动
    PNM_WER_REMOVE_FAILED,   // 是我们的登记但删不掉（非提权写不了 HKLM）
};

inline PnmWerUnreg PNM_WerUnregister(HKEY root, const wchar_t* key_path) {
    HKEY hk = nullptr;
    REGSAM sam = KEY_QUERY_VALUE | KEY_WOW64_64KEY;
    LSTATUS st = RegOpenKeyExW(root, key_path, 0, sam, &hk);
    if (st == ERROR_FILE_NOT_FOUND) return PNM_WER_NOT_FOUND;
    // 打不开就读不到 DumpFolder，无法判断归属：按「没关干净」上报，不装作已移除
    if (st != ERROR_SUCCESS) return PNM_WER_REMOVE_FAILED;
    wchar_t folder[MAX_PATH] = {};
    DWORD bytes = sizeof(folder);
    bool have = RegQueryValueExW(hk, L"DumpFolder", nullptr, nullptr, (BYTE*)folder, &bytes) == ERROR_SUCCESS;
    RegCloseKey(hk);
    if (!have || !PNM_WerFolderIsOurs(folder)) return PNM_WER_NOT_OURS;
    return RegDeleteKeyExW(root, key_path, KEY_WOW64_64KEY, 0) == ERROR_SUCCESS ? PNM_WER_REMOVED : PNM_WER_REMOVE_FAILED;
}

// ============================================================
// OS version gate
// RtlGetVersion via GetProcAddress: the truth-telling API (no
// VerifyVersionInfo compatibility lie), and never enters the import
// table so the DLL keeps loading on any Windows.
// ============================================================

// OS build number (15063 = Win10 1703, 19041 = 2004, ...). False only if
// RtlGetVersion itself is unavailable (pre-2000 - not a real scenario).
inline bool PNM_GetOSBuild(DWORD& build) {
    typedef LONG(WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    static DWORD cached = 0;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        auto fn = (PFN_RtlGetVersion)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        RTL_OSVERSIONINFOW vi = { sizeof(vi) };
        if (fn && fn(&vi) == 0) cached = vi.dwBuildNumber;
    }
    build = cached;
    return cached != 0;
}

// SWCA accent blur/acrylic (ACCENT_ENABLE_BLURBEHIND/ACRYLICBLURBEHIND)
// actually renders starting with Win10 1703 (build 15063). Below that the
// popup's acrylic/blur backdrops silently degrade to plain transparency.
inline bool PNM_IsAccentBlurSupported() {
    DWORD build = 0;
    return PNM_GetOSBuild(build) && build >= 15063;
}

// ============================================================
// DPI / multi-monitor helpers
// Same approach as TrafficMonitor's own DPIFromRect:
// MonitorFromRect + shcore!GetDpiForMonitor(MDT_EFFECTIVE_DPI).
// Works per-monitor regardless of host process DPI awareness.
// ============================================================

typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

inline PFN_GetDpiForMonitor PNM_GetDpiForMonitorProc() {
    static PFN_GetDpiForMonitor pfn = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore)
            pfn = (PFN_GetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    return pfn;
}

// DPI scale (1.0 = 100%) for a given monitor
inline float PNM_GetDpiScaleForMonitor(HMONITOR mon) {
    if (!mon) return 1.0f;
    auto pfn = PNM_GetDpiForMonitorProc();
    if (pfn) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(pfn(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dy > 0)
            return (float)dy / 96.0f;
    }
    return 1.0f;
}

// DPI scale for the monitor that contains (or is nearest to) the given rect
inline float PNM_GetDpiScaleForRect(const RECT& rc) {
    return PNM_GetDpiScaleForMonitor(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST));
}

// Work area of the monitor that contains (or is nearest to) the given rect
inline bool PNM_GetWorkAreaForRect(const RECT& rc, RECT& out_work, RECT& out_monitor) {
    HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &out_work, 0);
        out_monitor = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        return false;
    }
    out_work = mi.rcWork;
    out_monitor = mi.rcMonitor;
    return true;
}
