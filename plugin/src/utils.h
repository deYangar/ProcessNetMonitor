#pragma once
#include <cstdio>
#include <windows.h>
#include <shellscalingapi.h>

// DLL's HINSTANCE (captured in tooltip_popup.cpp DllMain)
extern HINSTANCE s_dll_hinst;

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
