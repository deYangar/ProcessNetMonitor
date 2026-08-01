#pragma once
#include <cstdio>
#include <windows.h>
#include <shellscalingapi.h>

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
