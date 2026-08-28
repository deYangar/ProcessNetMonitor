#include "detail_window.h"
#include "plugin_main.h"
#include "utils.h"
#include "signature.h"
#include "ip_geo.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <ctime>

#pragma comment(lib, "imm32.lib")

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

CDetailWindow* CDetailWindow::s_instance = nullptr;

static LRESULT CALLBACK DetailStaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (CDetailWindow::s_instance)
        return CDetailWindow::s_instance->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// Construction / Destruction
// ============================================================

static ULONGLONG WallClockMs() { return (ULONGLONG)time(NULL) * 1000; }

// Local midnight (00:00:00) of (today + day_offset) as a UTC Unix-ms timestamp.
// day_offset: 0 = today, -1 = yesterday, -2 = day before yesterday, ...
// WallClockMs() stores UTC Unix-ms; history tabs are aligned to the user's
// local calendar day (issue #9: "24h" rolls across two calendar days, which
// nobody reads naturally).
static ULONGLONG LocalDayStartUnixMs(int day_offset) {
    time_t now = time(NULL);
    struct tm local_tm;
    localtime_s(&local_tm, &now);
    local_tm.tm_hour = 0;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    // _mkgmtime treats the tm fields as UTC and returns a UTC time_t; since
    // the fields hold LOCAL wall-clock time, the result is the UTC instant
    // at which local midnight occurs.
    time_t midnight = _mkgmtime(&local_tm);
    return (ULONGLONG)(midnight + (time_t)day_offset * 86400) * 1000ULL;
}

CDetailWindow::CDetailWindow() {
    s_instance = this;
    m_dark_mode = IsDarkMode();
    m_dark_mode_tick = GetTickCount64();
    m_session_start_tick = GetTickCount64();
    m_history_start_tick = WallClockMs();
    m_last_save_tick = GetTickCount64();  // prevent immediate overwrite on first UpdateData
}

CDetailWindow::~CDetailWindow() {
    OutputDebugStringW(L"[PNM] ~CDetailWindow: calling SaveHistory");
    SaveSettings();
    SaveHistory();
    OutputDebugStringW(L"[PNM] ~CDetailWindow: SaveHistory done");
    if (m_hwnd) {
        if (g_shutting_down) {
            // DLL unload / process exit: skip DestroyWindow (issue #7 - see
            // CTooltipPopup::~CTooltipPopup).
            m_hwnd = nullptr;
        } else {
            SafeDestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }
    for (auto& [path, icon] : m_icon_cache) {
        if (icon) DestroyIcon(icon);
    }
    if (m_font_title) DeleteObject(m_font_title);
    if (m_font_row) DeleteObject(m_font_row);
    if (m_font_header) DeleteObject(m_font_header);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_font_time) DeleteObject(m_font_time);
    if (m_pen_border) DeleteObject(m_pen_border);
    if (m_pen_border_exp) DeleteObject(m_pen_border_exp);
    for (auto& b : m_br_row) { if (b) DeleteObject(b); }
    if (m_br_hover) DeleteObject(m_br_hover);
    if (m_br_child) DeleteObject(m_br_child);
    s_instance = nullptr;
}

bool CDetailWindow::IsDarkMode() {
    ULONGLONG now = GetTickCount64();
    if (now - m_dark_mode_tick < 5000) return m_dark_mode_cached;
    m_dark_mode_tick = now;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hKey);
        m_dark_mode_cached = (val == 0);
    }
    return m_dark_mode_cached;
}

HICON CDetailWindow::GetProcessIcon(const std::wstring& exe_path) {
    if (exe_path.empty()) return nullptr;
    auto it = m_icon_cache.find(exe_path);
    if (it != m_icon_cache.end()) return it->second;

    HICON hIcon = nullptr;
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(exe_path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        hIcon = sfi.hIcon;
    }
    if (!hIcon) {
        wchar_t path_buf[MAX_PATH] = {};
        wcsncpy_s(path_buf, MAX_PATH, exe_path.c_str(), _TRUNCATE);
        WORD idx = 0;
        hIcon = ExtractAssociatedIconW(m_hinst, path_buf, &idx);
    }
    m_icon_cache[exe_path] = hIcon;
    return hIcon;
}

// ============================================================
// Colors
// ============================================================

COLORREF CDetailWindow::GetBgColor() {
    return m_dark_mode ? RGB(32, 32, 36) : RGB(249, 249, 249);
}
COLORREF CDetailWindow::GetTitleBarColor() {
    return m_dark_mode ? RGB(28, 28, 32) : RGB(243, 243, 243);
}
COLORREF CDetailWindow::GetTextColor() {
    return m_dark_mode ? RGB(240, 240, 240) : RGB(30, 30, 30);
}
COLORREF CDetailWindow::GetSecondaryTextColor() {
    return m_dark_mode ? RGB(140, 140, 140) : RGB(130, 130, 130);
}
COLORREF CDetailWindow::GetAccentColor(bool is_upload) {
    if (is_upload) return m_dark_mode ? RGB(255, 165, 0) : RGB(230, 126, 34);
    return m_dark_mode ? RGB(80, 200, 120) : RGB(46, 160, 67);
}
COLORREF CDetailWindow::GetHeaderBgColor() {
    return m_dark_mode ? RGB(38, 38, 42) : RGB(240, 240, 240);
}
COLORREF CDetailWindow::GetRowBgColor(int row, bool hovered) {
    if (hovered) return m_dark_mode ? RGB(48, 48, 54) : RGB(232, 232, 232);
    return (row % 2 == 0) ? GetBgColor() : (m_dark_mode ? RGB(35, 35, 39) : RGB(245, 245, 245));
}
COLORREF CDetailWindow::GetBorderColor() {
    return m_dark_mode ? RGB(55, 55, 60) : RGB(220, 220, 220);
}

// FormatSpeed / FormatBytes delegated to shared utils.h

void CDetailWindow::InitColumns() {
    static const wchar_t* rt_titles[NUM_COLS] = {
        L"", TR(L"\u7A0B\u5E8F\u540D\u79F0"), TR(L"\u7A0B\u5E8F\u7C7B\u522B"),
        TR(L"\u4E0B\u8F7D\u901F\u5EA6"), TR(L"\u4E0A\u4F20\u901F\u5EA6"),
        TR(L"\u8FDE\u63A5\u6570"), TR(L"\u64CD\u4F5C")
    };
    for (int i = 0; i < NUM_COLS; i++) m_rt_cols[i].title = rt_titles[i];

    static const wchar_t* hist_titles[NUM_COLS] = {
        L"", TR(L"\u7A0B\u5E8F\u540D\u79F0"), TR(L"\u7A0B\u5E8F\u7C7B\u522B"),
        TR(L"\u603B\u4E0B\u8F7D"), TR(L"\u603B\u4E0A\u4F20"),
        TR(L"\u5E73\u5747\u4E0B\u8F7D"), TR(L"\u5E73\u5747\u4E0A\u4F20")
    };
    for (int i = 0; i < NUM_COLS; i++) m_hist_cols[i].title = hist_titles[i];

    static const wchar_t* conn_titles[NUM_CONN_COLS] = {
        TR(L"\u534F\u8BAE"), TR(L"\u672C\u5730\u5730\u5740"),
        TR(L"\u8FDC\u7A0B\u5730\u5740"), TR(L"\u5F52\u5C5E\u5730"), TR(L"\u72B6\u6001")
    };
    for (int i = 0; i < NUM_CONN_COLS; i++) m_conn_cols[i].title = conn_titles[i];
}

// ============================================================
// Window creation
// ============================================================

bool CDetailWindow::Initialize(HINSTANCE hInst) {
    m_hinst = hInst;
    InitColumns();

    // Declare per-monitor DPI awareness (v2 if available)
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        auto pfn = (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pfn) {
            pfn((HANDLE)-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }

    // Get DPI (use GetDpiForSystem as window doesn't exist yet)
    int dpi = 96;
    if (hUser32) {
        typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
        auto pfn = (PFN_GetDpiForSystem)GetProcAddress(hUser32, "GetDpiForSystem");
        if (pfn) dpi = pfn();
    }
    m_dpi_scale = dpi / 96.0f;
    UpdateDpiScale();  // will recalc after window creation

    wchar_t className[64];
    swprintf_s(className, 64, L"PNMDetail_%p", (void*)this);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = m_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(m_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTSIZE);
    wc.hIconSm = (HICON)LoadImageW(m_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTSIZE);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        className, L"ProcessNetMonitor",
        WS_POPUP | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, MIN_WIDTH + 40, MIN_HEIGHT + 80,
        NULL, NULL, m_hinst, NULL);

    if (!m_hwnd) return false;

    // Signature verification worker notifies this window on completion
    SignatureCache::Instance().SetNotifyHwnd(m_hwnd);

    // Now that window exists, get accurate DPI for this monitor
    UpdateDpiScale(m_hwnd);

    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)DetailStaticWndProc);

    int corner_pref = 2;
    DwmSetWindowAttribute(m_hwnd, 33, &corner_pref, sizeof(corner_pref));

    CreateFonts();
    m_pen_border = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
    m_pen_border_exp = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
    m_br_row[0] = CreateSolidBrush(GetBgColor());
    m_br_row[1] = CreateSolidBrush(m_dark_mode ? RGB(35, 35, 39) : RGB(245, 245, 245));
    m_br_hover = CreateSolidBrush(m_dark_mode ? RGB(48, 48, 54) : RGB(232, 232, 232));
    m_br_child = CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247));

    return true;
}

void CDetailWindow::ApplyLayoutScale() {
    TITLE_BAR_H      = (int)(BASE_TITLE_BAR_H * m_dpi_scale);
    TAB_BAR_H        = (int)(BASE_TAB_BAR_H * m_dpi_scale);
    TIME_RANGE_H     = (int)(BASE_TIME_RANGE_H * m_dpi_scale);
    SUMMARY_H        = (int)(BASE_SUMMARY_H * m_dpi_scale);
    TABLE_HEADER_H   = (int)(BASE_TABLE_HEADER_H * m_dpi_scale);
    ROW_H            = (int)(BASE_ROW_H * m_dpi_scale);
    CHILD_ROW_H      = (int)(BASE_CHILD_ROW_H * m_dpi_scale);
    PADDING          = (int)(BASE_PADDING * m_dpi_scale);
    CORNER_RADIUS    = (int)(BASE_CORNER_RADIUS * m_dpi_scale);
    ICON_SIZE        = (int)(BASE_ICON_SIZE * m_dpi_scale);
    SCROLL_W         = (int)(BASE_SCROLL_W * m_dpi_scale);
    MIN_WIDTH        = (int)(BASE_MIN_WIDTH * m_dpi_scale);
    MIN_HEIGHT       = (int)(BASE_MIN_HEIGHT * m_dpi_scale);
    CONN_HEADER_H    = (int)(BASE_CONN_HEADER_H * m_dpi_scale);
    CONN_ROW_H       = (int)(BASE_CONN_ROW_H * m_dpi_scale);
    CONN_TABLE_PADDING = (int)(BASE_CONN_TABLE_PADDING * m_dpi_scale);
    SUBPROC_HEADER_H = (int)(BASE_SUBPROC_HEADER_H * m_dpi_scale);
    SUBPROC_INDENT   = (int)(BASE_SUBPROC_INDENT * m_dpi_scale);
    
    static const int BASE_RT_WIDTHS[NUM_COLS] = {42, 180, 90, 100, 100, 60, 80};
    static const int BASE_HIST_WIDTHS[NUM_COLS] = {42, 140, 70, 90, 90, 85, 85};
    static const int BASE_CONN_WIDTHS[NUM_CONN_COLS] = {50, 150, 150, 150, 110};
    for (int i = 0; i < NUM_COLS; i++) {
        m_rt_cols[i].width = (int)(BASE_RT_WIDTHS[i] * m_dpi_scale);
        m_hist_cols[i].width = (int)(BASE_HIST_WIDTHS[i] * m_dpi_scale);
    }
    for (int i = 0; i < NUM_CONN_COLS; i++) {
        m_conn_cols[i].width = (int)(BASE_CONN_WIDTHS[i] * m_dpi_scale);
    }
}

void CDetailWindow::UpdateDpiScale(HWND hwnd) {
    if (hwnd == nullptr) hwnd = m_hwnd;
    int dpi = 96;
    // Prefer GetDpiForWindow (works even if process is not DPI-aware)
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32 && hwnd) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        auto pfn = (PFN_GetDpiForWindow)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pfn) dpi = pfn(hwnd);
    }
    if (dpi == 0) dpi = 96;
    m_dpi_scale = dpi / 96.0f;
    ApplyLayoutScale();
}

void CDetailWindow::UpdateDpiScaleForRect(const RECT& rc) {
    // Per-monitor DPI via shcore (same approach as TM's own DPIFromRect) -
    // reliable per target monitor, e.g. when the window opens on a secondary
    // monitor with different scaling.
    float s = PNM_GetDpiScaleForRect(rc);
    if (s < 0.5f) s = 1.0f;
    m_dpi_scale = s;
    ApplyLayoutScale();
}

void CDetailWindow::CreateFonts() {
    if (m_font_title) DeleteObject(m_font_title);
    if (m_font_row) DeleteObject(m_font_row);
    if (m_font_header) DeleteObject(m_font_header);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_font_time) DeleteObject(m_font_time);
    
    // NOTE: use float scaling here. The old code did `int s = (int)m_dpi_scale;`
    // which truncated 1.5 -> 1, leaving fonts unscaled at 125%/150%/175% DPI
    // while the layout grew -> tiny text (issue #4).
    float s = m_dpi_scale;
    m_font_title  = CreateFontW((int)(-14*s), 0,0,0, FW_SEMIBOLD, FALSE,FALSE,FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei");
    m_font_row    = CreateFontW((int)(-14*s), 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei");
    m_font_header = CreateFontW((int)(-13*s), 0,0,0, FW_SEMIBOLD, FALSE,FALSE,FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei");
    m_font_small  = CreateFontW((int)(-11*s), 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei");
    m_font_time   = CreateFontW((int)(-12*s), 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_SWISS, L"Microsoft YaHei");
}

void CDetailWindow::RecreateGdiObjects() {
    CreateFonts();
    if (m_pen_border) DeleteObject(m_pen_border);
    if (m_pen_border_exp) DeleteObject(m_pen_border_exp);
    if (m_br_row[0]) DeleteObject(m_br_row[0]);
    if (m_br_row[1]) DeleteObject(m_br_row[1]);
    if (m_br_hover) DeleteObject(m_br_hover);
    if (m_br_child) DeleteObject(m_br_child);
    m_pen_border = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42,42,46) : RGB(238,238,238));
    m_pen_border_exp = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42,42,46) : RGB(238,238,238));
    m_br_row[0] = CreateSolidBrush(GetBgColor());
    m_br_row[1] = CreateSolidBrush(m_dark_mode ? RGB(35,35,39) : RGB(245,245,245));
    m_br_hover = CreateSolidBrush(m_dark_mode ? RGB(48,48,54) : RGB(232,232,232));
    m_br_child = CreateSolidBrush(m_dark_mode ? RGB(36,36,40) : RGB(247,247,247));
}

void CDetailWindow::Show(HWND parent_wnd) {
    m_parent_wnd = parent_wnd;
    bool old_dark = m_dark_mode;
    m_dark_mode = IsDarkMode();
    if (m_dark_mode != old_dark) {
        if (m_pen_border) DeleteObject(m_pen_border);
        if (m_pen_border_exp) DeleteObject(m_pen_border_exp);
        for (auto& b : m_br_row) { if (b) DeleteObject(b); }
        if (m_br_hover) DeleteObject(m_br_hover);
        if (m_br_child) DeleteObject(m_br_child);
        m_pen_border = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
        m_pen_border_exp = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
        m_br_row[0] = CreateSolidBrush(GetBgColor());
        m_br_row[1] = CreateSolidBrush(m_dark_mode ? RGB(35, 35, 39) : RGB(245, 245, 245));
        m_br_hover = CreateSolidBrush(m_dark_mode ? RGB(48, 48, 54) : RGB(232, 232, 232));
        m_br_child = CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247));
    }

    // Center on the monitor that contains the parent window (multi-monitor fix:
    // previously always centered on the primary screen, so a detail window opened
    // from a popup on a secondary monitor landed on the primary one).
    RECT ref_rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (parent_wnd && IsWindow(parent_wnd))
        GetWindowRect(parent_wnd, &ref_rc);
    RECT work, mon;
    PNM_GetWorkAreaForRect(ref_rc, work, mon);

    // Adopt the target monitor's DPI before computing the window size
    UpdateDpiScaleForRect(ref_rc);
    RecreateGdiObjects();

    int aw = work.right - work.left;
    int ah = work.bottom - work.top;
    int w = min((int)(720 * m_dpi_scale), aw - 40);
    int h = min((int)(560 * m_dpi_scale), ah - 40);
    int x = work.left + (aw - w) / 2;
    int y = work.top + (ah - h) / 2;

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    m_visible = true;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::Hide() {
    if (m_visible) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
    }
}

// ============================================================
// History recording
// ============================================================

void CDetailWindow::RecordHistory(const std::vector<ProcTraffic>& stats) {
    ULONGLONG now = WallClockMs();
    bool added = false;
    
    // Aggregate bytes by process name (multiple PIDs may share same name)
    struct AggBytes { uint64_t recv = 0, sent = 0; std::wstring exe_path; };
    std::map<std::wstring, AggBytes> agg;
    for (auto& st : stats) {
        auto& a = agg[st.name];
        a.recv += st.bytes_recv;
        a.sent += st.bytes_sent;
        if (a.exe_path.empty() && !st.exe_path.empty()) a.exe_path = st.exe_path;
    }
    
    for (auto& [name, a] : agg) {
        auto& hist = m_history[name];
        if (hist.name.empty()) { hist.name = name; hist.exe_path = a.exe_path; added = true; }
        if (hist.exe_path.empty() && !a.exe_path.empty()) hist.exe_path = a.exe_path;

        // Store DELTA (bytes transferred since last snapshot), not cumulative.
        uint64_t delta_recv = 0, delta_sent = 0;
        auto it = m_last_cum.find(name);
        if (it != m_last_cum.end()) {
            if (a.recv >= it->second.recv)
                delta_recv = a.recv - it->second.recv;
            else
                delta_recv = 0;  // connection(s) closed, cumulative dropped - clamp to 0
            if (a.sent >= it->second.sent)
                delta_sent = a.sent - it->second.sent;
            else
                delta_sent = 0;
        } else {
            // First time seeing this process - just set baseline, don't count existing cumulative bytes as new traffic
            delta_recv = 0;
            delta_sent = 0;
        }
        m_last_cum[name] = { a.recv, a.sent };

        HistorySnapshot snap = { now, delta_recv, delta_sent, false };
        hist.raw.push_back(snap);
        while ((int)hist.raw.size() > MAX_RAW) hist.raw.pop_front();
        added = true;
    }
    if (added) m_history_dirty = true;
    static int compress_counter = 0;
    if (++compress_counter >= 60) { compress_counter = 0; CompressHistory(); }
}

void CDetailWindow::ClearHistory() {
    m_history.clear();
    m_last_cum.clear();
    m_rows.clear();
    m_hist_total_recv = 0;
    m_hist_total_sent = 0;
    m_history_start_tick = WallClockMs();
    m_history_dirty = true;

    // Delete history file
    if (!m_config_dir.empty()) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"%s\\history.dat", m_config_dir.c_str());
        DeleteFileW(path);
    }

    // Save empty history to reset file
    SaveHistory();

    if (m_visible) InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::CompressHistory() {
    ULONGLONG now = WallClockMs();
    for (auto& [name, h] : m_history) {
        // raw → min: downsample to 1 entry per minute
        // SUM deltas within each window (not last cumulative)
        ULONGLONG min_cutoff = now - 120000;
        while (!h.raw.empty() && h.raw.front().tick < min_cutoff) {
            ULONGLONG win_end = h.raw.front().tick + 60000;
            HistorySnapshot acc = { 0, 0, 0, false };
            while (!h.raw.empty() && h.raw.front().tick < win_end && h.raw.front().tick < min_cutoff) {
                acc.tick = h.raw.front().tick;
                acc.cum_recv += h.raw.front().cum_recv;
                acc.cum_sent += h.raw.front().cum_sent;
                h.raw.pop_front();
            }
            h.min.push_back(acc);
        }
        while ((int)h.min.size() > MAX_MIN) h.min.pop_front();

        // min → hour: SUM deltas within 1-hour windows
        ULONGLONG hour_cutoff = now - 7200000;
        while (!h.min.empty() && h.min.front().tick < hour_cutoff) {
            ULONGLONG win_end = h.min.front().tick + 3600000;
            HistorySnapshot acc = { 0, 0, 0, false };
            while (!h.min.empty() && h.min.front().tick < win_end && h.min.front().tick < hour_cutoff) {
                acc.tick = h.min.front().tick;
                acc.cum_recv += h.min.front().cum_recv;
                acc.cum_sent += h.min.front().cum_sent;
                h.min.pop_front();
            }
            h.hour.push_back(acc);
        }
        while ((int)h.hour.size() > MAX_HOUR) h.hour.pop_front();

        // hour → day: SUM deltas within 1-day windows
        ULONGLONG day_cutoff = now - 172800000;
        while (!h.hour.empty() && h.hour.front().tick < day_cutoff) {
            ULONGLONG win_end = h.hour.front().tick + 86400000;
            HistorySnapshot acc = { 0, 0, 0, false };
            while (!h.hour.empty() && h.hour.front().tick < win_end && h.hour.front().tick < day_cutoff) {
                acc.tick = h.hour.front().tick;
                acc.cum_recv += h.hour.front().cum_recv;
                acc.cum_sent += h.hour.front().cum_sent;
                h.hour.pop_front();
            }
            h.day.push_back(acc);
        }
        while ((int)h.day.size() > MAX_DAY) h.day.pop_front();
    }
}

void CDetailWindow::GetMergedSnapshots(const ProcessHistory& h,
    std::vector<const HistorySnapshot*>& out, ULONGLONG start_ms, ULONGLONG end_ms) const {
    // Binary search: deques are sorted by tick; collect entries in
    // [start_ms, end_ms). Each tier (day/hour/min/raw) covers a disjoint
    // time span after compression, so the same instant is never counted
    // twice.
    auto collect = [&](const std::deque<HistorySnapshot>& dq) {
        auto it = std::lower_bound(dq.begin(), dq.end(), start_ms,
            [](const HistorySnapshot& s, ULONGLONG c) { return s.tick < c; });
        for (; it != dq.end(); ++it) {
            if (it->tick >= end_ms) break;
            out.push_back(&(*it));
        }
    };
    collect(h.day);
    collect(h.hour);
    collect(h.min);
    collect(h.raw);
}

void CDetailWindow::BuildHistoryRows() {
    // Save expanded state before clearing
    std::set<std::wstring> expanded_names;
    for (auto& r : m_rows) {
        if (r.expanded) expanded_names.insert(r.name);
    }
    m_rows.clear();
    m_hist_total_recv = 0;
    m_hist_total_sent = 0;

    ULONGLONG now = WallClockMs();
    // Natural-day ranges, aligned to local midnight (00:00:00) - issue #9:
    //   今日    = today 00:00 .. now
    //   昨日    = yesterday 00:00 .. today 00:00 (exclusive)
    //   近3天   = day-before-yesterday 00:00 .. now (3 calendar days)
    //   近7天   = 6 days ago 00:00 .. now (7 calendar days)
    //   近30天  = 29 days ago 00:00 .. now (30 calendar days)
    ULONGLONG start_ms = 0, end_ms = now;
    double span_sec = 0;  // nominal span for average speed
    switch (m_time_range) {
    case TR_TODAY:
        start_ms = LocalDayStartUnixMs(0);
        end_ms = now;
        span_sec = (double)(now - start_ms) / 1000.0;
        break;
    case TR_YESTERDAY:
        start_ms = LocalDayStartUnixMs(-1);
        end_ms = LocalDayStartUnixMs(0);  // today 00:00, exclusive
        span_sec = 86400.0;               // a full calendar day
        break;
    case TR_3D:
        start_ms = LocalDayStartUnixMs(-2);
        end_ms = now;
        span_sec = (double)(now - start_ms) / 1000.0;
        break;
    case TR_7D:
        start_ms = LocalDayStartUnixMs(-6);
        end_ms = now;
        span_sec = (double)(now - start_ms) / 1000.0;
        break;
    case TR_30D:
        start_ms = LocalDayStartUnixMs(-29);
        end_ms = now;
        span_sec = (double)(now - start_ms) / 1000.0;
        break;
    }
    if (start_ms < m_history_start_tick) start_ms = m_history_start_tick;
    if (span_sec < 1.0) span_sec = 1.0;

    for (auto& [name, hist] : m_history) {
        std::vector<const HistorySnapshot*> snaps;
        GetMergedSnapshots(hist, snaps, start_ms, end_ms);
        if (snaps.empty()) continue;

        // Data is stored as deltas (bytes per interval). Sum all deltas in range.
        uint64_t total_recv = 0, total_sent = 0;
        for (auto* s : snaps) {
            total_recv += s->cum_recv;  // cum_recv is actually delta_recv
            total_sent += s->cum_sent;  // cum_sent is actually delta_sent
        }

        // Average speed over the whole calendar range (e.g. yesterday is
        // always /86400s), not just the span between first/last samples.
        double elapsed_sec = span_sec;

        m_hist_total_recv += total_recv;
        m_hist_total_sent += total_sent;

        DisplayRow row;
        row.name = hist.name;
        row.exe_path = hist.exe_path;
        row.hist_recv = total_recv;
        row.hist_sent = total_sent;
        row.hist_avg_down = (double)total_recv / elapsed_sec;
        row.hist_avg_up = (double)total_sent / elapsed_sec;

        std::wstring lower = hist.name;
        for (auto& c : lower) c = towlower(c);
        if (!hist.name.empty() && hist.name[0] == L'<') {
            row.category = TR(L"\u672A\u77E5");
        } else {
            // signature-based classification (background worker + cache)
            SigClass sc = SignatureCache::Instance().Check(row.exe_path);
            if (sc == SigClass::Unknown && row.exe_path.empty()) {
                // kernel pseudo-processes have no exe (System/Registry/Idle/
                // Memory Compression) - classify by well-known names
                if (lower.find(L"system") != std::wstring::npos ||
                    lower.find(L"registry") != std::wstring::npos ||
                    lower.find(L"idle") != std::wstring::npos ||
                    lower.find(L"memory compression") != std::wstring::npos)
                    row.category = TR(L"\u7CFB\u7EDF\u8FDB\u7A0B");
                else
                    row.category = TR(L"\u672A\u77E5");
            } else {
                switch (sc) {
                case SigClass::System:     row.category = TR(L"\u7CFB\u7EDF\u8FDB\u7A0B"); break;
                case SigClass::ThirdParty: row.category = TR(L"\u7B2C\u4E09\u65B9\u7A0B\u5E8F"); break;
                case SigClass::Unsigned:   row.category = TR(L"\u672A\u7B7E\u540D"); break;
                default:                   row.category = TR(L"\u672A\u77E5"); break;
                }
            }
        }

        if (expanded_names.count(name)) row.expanded = true;
        m_rows.push_back(row);
    }
}

// ============================================================
// Persistence
// ============================================================

static void WriteDQ(FILE* f, const std::deque<CDetailWindow::HistorySnapshot>& dq) {
    uint32_t n = (uint32_t)dq.size(); fwrite(&n, 4, 1, f);
    for (auto& s : dq) {
        fwrite(&s.tick, sizeof(ULONGLONG), 1, f);
        fwrite(&s.cum_recv, sizeof(uint64_t), 1, f);
        fwrite(&s.cum_sent, sizeof(uint64_t), 1, f);
    }
}
static bool ReadDQ(FILE* f, std::deque<CDetailWindow::HistorySnapshot>& dq, int max_n, ULONGLONG max_age, ULONGLONG now) {
    uint32_t n; if (fread(&n, 4, 1, f) != 1 || n > (uint32_t)max_n + 100) return false;
    for (uint32_t i = 0; i < n; i++) {
        CDetailWindow::HistorySnapshot s;
        s.loaded = true;  // data from disk (old session)
        if (fread(&s.tick, sizeof(ULONGLONG), 1, f) != 1) return false;
        if (fread(&s.cum_recv, sizeof(uint64_t), 1, f) != 1) return false;
        if (fread(&s.cum_sent, sizeof(uint64_t), 1, f) != 1) return false;
        if (max_age > 0 && now > s.tick && now - s.tick > max_age) continue;
        dq.push_back(s);
    }
    return true;
}

void CDetailWindow::SaveHistory() {
    if (m_config_dir.empty()) { OutputDebugStringW(L"[PNM] SaveHistory: config_dir empty!"); return; }
    CreateDirectoryW(m_config_dir.c_str(), NULL);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\history.dat", m_config_dir.c_str());
    FILE* f = _wfopen(path, L"wb");
    if (!f) { OutputDebugStringW(L"[PNM] SaveHistory: fopen failed!"); return; }

    uint32_t magic = 0x504E4D48, version = 4, count = (uint32_t)m_history.size();
    fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f); fwrite(&count, 4, 1, f);
    for (auto& [name, hist] : m_history) {
        uint32_t nl = (uint32_t)name.size(); fwrite(&nl, 4, 1, f); fwrite(name.c_str(), sizeof(wchar_t), nl, f);
        uint32_t pl = (uint32_t)hist.exe_path.size(); fwrite(&pl, 4, 1, f); fwrite(hist.exe_path.c_str(), sizeof(wchar_t), pl, f);
        WriteDQ(f, hist.raw); WriteDQ(f, hist.min); WriteDQ(f, hist.hour); WriteDQ(f, hist.day);
    }
    fclose(f);
}

void CDetailWindow::LoadHistory() {
    if (m_config_dir.empty()) { OutputDebugStringW(L"[PNM] LoadHistory: config_dir empty!"); return; }
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\history.dat", m_config_dir.c_str());
    FILE* f = _wfopen(path, L"rb");
    if (!f) { OutputDebugStringW(L"[PNM] LoadHistory: file not found"); return; }
    uint32_t magic, version, count;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x504E4D48) { fclose(f); return; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return; }
    if (fread(&count, 4, 1, f) != 1) { fclose(f); return; }
    ULONGLONG max_age = MS_PER_YEAR;
    // Version 2 stored GetTickCount64() ticks (uptime); version 3 stores wall-clock ms.
    // Convert old ticks to wall-clock using the offset at load time.
    ULONGLONG uptime_now = GetTickCount64();
    ULONGLONG wall_now = WallClockMs();
    bool needs_tick_convert = (version < 3);   // uptime ticks → wall-clock
    bool needs_delta_convert = (version < 4);  // cumulative → delta
    ULONGLONG readq_now = needs_tick_convert ? uptime_now : wall_now;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nl; if (fread(&nl, 4, 1, f) != 1 || nl > 512) break;
        std::wstring name(nl, L'\0'); if (fread(&name[0], sizeof(wchar_t), nl, f) != nl) break;
        uint32_t pl; if (fread(&pl, 4, 1, f) != 1 || pl > MAX_PATH) break;
        std::wstring exe_path(pl, L'\0'); if (fread(&exe_path[0], sizeof(wchar_t), pl, f) != pl) break;
        auto& hist = m_history[name]; hist.name = name; hist.exe_path = exe_path;
        if (version >= 2) {
            if (!ReadDQ(f, hist.raw, MAX_RAW, max_age, readq_now)) break;
            if (!ReadDQ(f, hist.min, MAX_MIN, max_age, readq_now)) break;
            if (!ReadDQ(f, hist.hour, MAX_HOUR, max_age, readq_now)) break;
            if (!ReadDQ(f, hist.day, MAX_DAY, max_age, readq_now)) break;
        } else {
            if (!ReadDQ(f, hist.raw, MAX_RAW, max_age, readq_now)) break;
        }
        // Convert old uptime ticks to wall-clock time
        if (needs_tick_convert) {
            auto convert = [&](std::deque<HistorySnapshot>& dq) {
                for (auto& s : dq) {
                    if (s.tick <= uptime_now)
                        s.tick = wall_now - (uptime_now - s.tick);
                    else
                        s.tick = wall_now;
                }
            };
            convert(hist.raw);
            convert(hist.min);
            convert(hist.hour);
            convert(hist.day);
        }
        // Convert cumulative values to deltas (version < 4)
        if (needs_delta_convert) {
            auto to_delta = [](std::deque<HistorySnapshot>& dq) {
                if (dq.size() < 2) return;
                for (size_t i = dq.size() - 1; i > 0; i--) {
                    uint64_t dr = (dq[i].cum_recv >= dq[i-1].cum_recv) ? (dq[i].cum_recv - dq[i-1].cum_recv) : dq[i].cum_recv;
                    uint64_t ds = (dq[i].cum_sent >= dq[i-1].cum_sent) ? (dq[i].cum_sent - dq[i-1].cum_sent) : dq[i].cum_sent;
                    dq[i].cum_recv = dr;
                    dq[i].cum_sent = ds;
                }
                // First entry: treat as delta from 0
                // (keep as-is, it's the first snapshot's cum which is reasonable as delta)
            };
            to_delta(hist.raw);
            to_delta(hist.min);
            to_delta(hist.hour);
            to_delta(hist.day);
        }
    }
    fclose(f);

    // Compress loaded data: SUM deltas within each time window
    ULONGLONG now = WallClockMs();
    for (auto& [name, h] : m_history) {
        // raw → min: SUM deltas per minute
        while (!h.raw.empty()) {
            ULONGLONG win_end = h.raw.front().tick + 60000;
            HistorySnapshot acc = { 0, 0, 0, false };
            while (!h.raw.empty() && h.raw.front().tick < win_end) {
                acc.tick = h.raw.front().tick;
                acc.cum_recv += h.raw.front().cum_recv;
                acc.cum_sent += h.raw.front().cum_sent;
                h.raw.pop_front();
            }
            h.min.push_back(acc);
        }
        while ((int)h.min.size() > MAX_MIN) h.min.pop_front();

        // min → hour: SUM deltas per hour
        while (!h.min.empty()) {
            ULONGLONG win_end = h.min.front().tick + 3600000;
            HistorySnapshot acc = { 0, 0, 0, false };
            while (!h.min.empty() && h.min.front().tick < win_end) {
                acc.tick = h.min.front().tick;
                acc.cum_recv += h.min.front().cum_recv;
                acc.cum_sent += h.min.front().cum_sent;
                h.min.pop_front();
            }
            h.hour.push_back(acc);
        }
        while ((int)h.hour.size() > MAX_HOUR) h.hour.pop_front();

        // hour → day: SUM deltas per day
        while (!h.hour.empty()) {
            ULONGLONG win_end = h.hour.front().tick + 86400000;
            HistorySnapshot acc = { 0, 0, 0, false };
            while (!h.hour.empty() && h.hour.front().tick < win_end) {
                acc.tick = h.hour.front().tick;
                acc.cum_recv += h.hour.front().cum_recv;
                acc.cum_sent += h.hour.front().cum_sent;
                h.hour.pop_front();
            }
            h.day.push_back(acc);
        }
        while ((int)h.day.size() > MAX_DAY) h.day.pop_front();
    }

    // Adjust m_history_start_tick to earliest surviving data (wall-clock ms)
    ULONGLONG earliest = ULLONG_MAX;
    for (auto& [name, h] : m_history) {
        if (!h.min.empty() && h.min.front().tick < earliest) earliest = h.min.front().tick;
        if (!h.hour.empty() && h.hour.front().tick < earliest) earliest = h.hour.front().tick;
        if (!h.day.empty() && h.day.front().tick < earliest) earliest = h.day.front().tick;
    }
    if (earliest < m_history_start_tick && earliest != ULLONG_MAX) {
        m_history_start_tick = earliest;
    }
    wchar_t dbg[128];
    swprintf_s(dbg, 128, L"[PNM] LoadHistory: loaded %u processes, earliest=%llu", (unsigned)m_history.size(), earliest);
    OutputDebugStringW(dbg);
}

// ============================================================
// Data update
// ============================================================

void CDetailWindow::UpdateData(const std::vector<ProcTraffic>& stats, double total_up, double total_down) {
    m_total_up = total_up;
    m_total_down = total_down;

    // Always record history
    RecordHistory(stats);

    // Auto-save every 30 seconds (was 60, reduced for safety on TM restart)
    ULONGLONG now = GetTickCount64();
    if (now - m_last_save_tick > 30000) {
        m_last_save_tick = now;
        SaveHistory();
    }

    // Pause visual updates while context menu is open
    if (m_context_menu_open) {
        m_cached_stats = stats;
        return;
    }

    m_cached_stats = stats;
    RebuildRows();
}

void CDetailWindow::RebuildRows() {
    auto& stats = m_cached_stats;

    if (m_active_tab == 1) {
        // History tab - only rebuild when new data arrives
        if (!m_history_dirty) {
            if (m_visible) {
                RECT rc;
                GetClientRect(m_hwnd, &rc);
                rc.top = GetTableAreaTop();
                InvalidateRect(m_hwnd, &rc, FALSE);
            }
            return;
        }
        m_history_dirty = false;
        BuildHistoryRows();
        ResortRows();
        // Clamp scroll to pixel max
        m_scroll_pos = min(m_scroll_pos, GetScrollMax());
        if (m_visible) {
            RECT rc = { PADDING, GetTableAreaTop(), 0, 0 };
            GetClientRect(m_hwnd, &rc);
            rc.top = GetTableAreaTop();
            InvalidateRect(m_hwnd, &rc, FALSE);
        }
        return;
    }

    // Tab 0: real-time - merge by process name
    // Save expanded state (by name now, not PID)
    std::set<std::wstring> expanded_names;
    std::map<std::wstring, std::vector<SubProcess>> saved_subprocs;  // name -> subprocs with connections
    for (auto& r : m_rows) {
        if (r.expanded) {
            expanded_names.insert(r.name);
            if (!r.sub_processes.empty()) {
                saved_subprocs[r.name] = std::move(r.sub_processes);
            }
        }
    }

    m_rows.clear();

    // Group stats by process name
    std::map<std::wstring, std::vector<const ProcTraffic*>> name_groups;
    for (auto& st : stats) {
        name_groups[st.name].push_back(&st);
    }

    for (auto& [name, group] : name_groups) {
        DisplayRow row;
        row.name = name;
        row.pid = group[0]->pid;
        row.exe_path = group[0]->exe_path;

        // Aggregate
        for (auto* st : group) {
            row.speed_up += st->speed_up;
            row.speed_down += st->speed_down;
            row.conn_count += st->conn_count;
        }

        // Category
        std::wstring lower = name;
        for (auto& c : lower) c = towlower(c);
        if (!name.empty() && name[0] == L'<') {
            row.category = TR(L"\u672A\u77E5");  // unresolved pid - can't classify
        } else {
            // signature-based classification (background worker + cache)
            SigClass sc = SignatureCache::Instance().Check(row.exe_path);
            if (sc == SigClass::Unknown && row.exe_path.empty()) {
                // kernel pseudo-processes have no exe (System/Registry/Idle/
                // Memory Compression) - classify by well-known names
                if (lower.find(L"system") != std::wstring::npos ||
                    lower.find(L"registry") != std::wstring::npos ||
                    lower.find(L"idle") != std::wstring::npos ||
                    lower.find(L"memory compression") != std::wstring::npos)
                    row.category = TR(L"\u7CFB\u7EDF\u8FDB\u7A0B");
                else
                    row.category = TR(L"\u672A\u77E5");
            } else {
                switch (sc) {
                case SigClass::System:     row.category = TR(L"\u7CFB\u7EDF\u8FDB\u7A0B"); break;
                case SigClass::ThirdParty: row.category = TR(L"\u7B2C\u4E09\u65B9\u7A0B\u5E8F"); break;
                case SigClass::Unsigned:   row.category = TR(L"\u672A\u7B7E\u540D"); break;
                default:                   row.category = TR(L"\u672A\u77E5"); break;
                }
            }
        }

        // Build sub-processes
        for (auto* st : group) {
            SubProcess sp;
            sp.pid = st->pid;
            sp.exe_path = st->exe_path;
            sp.speed_up = st->speed_up;
            sp.speed_down = st->speed_down;
            sp.conn_count = st->conn_count;
            row.sub_processes.push_back(sp);
        }

        // Restore expanded state
        if (expanded_names.count(name)) {
            row.expanded = true;
            // Restore saved sub-process connection data
            auto it = saved_subprocs.find(name);
            if (it != saved_subprocs.end() && it->second.size() == row.sub_processes.size()) {
                for (size_t i = 0; i < row.sub_processes.size(); i++) {
                    row.sub_processes[i].connections = std::move(it->second[i].connections);
                    row.sub_processes[i].connections_loaded = it->second[i].connections_loaded;
                    row.sub_processes[i].conn_expanded = it->second[i].conn_expanded;
                }
            }
        }

        m_rows.push_back(row);
    }

    ResortRows();

    // Clamp scroll to pixel max
    m_scroll_pos = min(m_scroll_pos, GetScrollMax());
    if (m_visible) {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        rc.top = GetTableAreaTop();
        InvalidateRect(m_hwnd, &rc, FALSE);
    }
}

// ============================================================
// Layout helpers
// ============================================================

int CDetailWindow::GetHeaderHeight() const { return TITLE_BAR_H; }
int CDetailWindow::GetTabsHeight() const { return TAB_BAR_H; }
int CDetailWindow::GetTimeRangeHeight() const { return m_active_tab == 1 ? TIME_RANGE_H : 0; }
int CDetailWindow::GetSummaryHeight() const { return SUMMARY_H; }
int CDetailWindow::GetTableHeaderHeight() const { return TABLE_HEADER_H; }
int CDetailWindow::GetRowHeight() const { return ROW_H; }

int CDetailWindow::GetTableAreaTop() const {
    return PADDING + GetHeaderHeight() + GetTabsHeight() + GetTimeRangeHeight()
           + GetSummaryHeight() + GetTableHeaderHeight();
}

int CDetailWindow::GetVisibleRows(int client_h) const {
    int table_h = client_h - GetTableAreaTop() - PADDING;
    return max(1, table_h / GetRowHeight());
}

// ============================================================
// Sorting
// ============================================================

void CDetailWindow::ResortRows() {
    bool is_hist = (m_active_tab == 1);
    int sc = m_sort_col[m_active_tab];
    bool sa = m_sort_asc[m_active_tab];
    std::sort(m_rows.begin(), m_rows.end(), [&](const DisplayRow& a, const DisplayRow& b) {
        int cmp = 0;
        if (is_hist) {
            switch (sc) {
            case 1: cmp = _wcsicmp(a.name.c_str(), b.name.c_str()); break;
            case 2: cmp = _wcsicmp(a.category.c_str(), b.category.c_str()); break;
            case 3: cmp = (a.hist_recv > b.hist_recv) ? 1 : (a.hist_recv < b.hist_recv ? -1 : 0); break;
            case 4: cmp = (a.hist_sent > b.hist_sent) ? 1 : (a.hist_sent < b.hist_sent ? -1 : 0); break;
            case 5: cmp = (a.hist_avg_down > b.hist_avg_down) ? 1 : (a.hist_avg_down < b.hist_avg_down ? -1 : 0); break;
            case 6: cmp = (a.hist_avg_up > b.hist_avg_up) ? 1 : (a.hist_avg_up < b.hist_avg_up ? -1 : 0); break;
            default: cmp = 0;
            }
        } else {
            switch (sc) {
            case 1: cmp = _wcsicmp(a.name.c_str(), b.name.c_str()); break;
            case 2: cmp = _wcsicmp(a.category.c_str(), b.category.c_str()); break;
            case 3: cmp = (a.speed_down > b.speed_down) ? 1 : (a.speed_down < b.speed_down ? -1 : 0); break;
            case 4: cmp = (a.speed_up > b.speed_up) ? 1 : (a.speed_up < b.speed_up ? -1 : 0); break;
            case 5: cmp = (a.conn_count > b.conn_count) ? 1 : (a.conn_count < b.conn_count ? -1 : 0); break;
            default: cmp = 0;
            }
        }
        // Stable tiebreaker: PID first, then name
        if (cmp == 0) {
            if (a.pid != 0 && b.pid != 0 && a.pid != b.pid)
                cmp = (a.pid > b.pid) ? 1 : -1;
            else
                cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
        }
        return sa ? (cmp < 0) : (cmp > 0);
    });
}

void CDetailWindow::SortByColumn(int col) {
    if (m_sort_col[m_active_tab] == col) {
        m_sort_asc[m_active_tab] = !m_sort_asc[m_active_tab];
    } else {
        m_sort_col[m_active_tab] = col;
        m_sort_asc[m_active_tab] = true;
    }
    ResortRows();
    SaveSettings();
}

// ============================================================
// Settings persistence
// ============================================================

void CDetailWindow::SaveSettings() {
    if (m_config_dir.empty()) return;
    CreateDirectoryW(m_config_dir.c_str(), NULL);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\settings.json", m_config_dir.c_str());
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fprintf(f, "{\n");
    fprintf(f, "  \"sort_col\": [%d, %d],\n", m_sort_col[0], m_sort_col[1]);
    fprintf(f, "  \"sort_asc\": [%s, %s],\n", m_sort_asc[0] ? "true" : "false", m_sort_asc[1] ? "true" : "false");
    fprintf(f, "  \"refresh_ms\": %d,\n", m_refresh_ms);
    fprintf(f, "  \"transparent_width\": %d,\n", m_transparent_width);
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    fprintf(f, "  \"win_w\": %d,\n", rc.right - rc.left);
    fprintf(f, "  \"win_h\": %d,\n", rc.bottom - rc.top);
    // IP 归属地设置 (proxy / update_days / enabled)
    {
        const std::wstring& proxy = IpGeo::Instance().GetProxy();
        std::string proxy8;
        if (!proxy.empty()) {
            int n = WideCharToMultiByte(CP_UTF8, 0, proxy.c_str(), (int)proxy.size(), NULL, 0, NULL, NULL);
            if (n > 0) {
                proxy8.resize(n);
                WideCharToMultiByte(CP_UTF8, 0, proxy.c_str(), (int)proxy.size(), &proxy8[0], n, NULL, NULL);
            }
        }
        fprintf(f, "  \"geo_proxy\": \"%s\",\n", proxy8.c_str());
        fprintf(f, "  \"geo_update_days\": %d,\n", IpGeo::Instance().GetUpdateDays());
        fprintf(f, "  \"geo_enabled\": %s,\n", IpGeo::Instance().IsEnabled() ? "true" : "false");
    }
    fprintf(f, "  \"show_speed_items\": %s,\n", m_show_speed_items ? "true" : "false");
    fprintf(f, "  \"debug_logs\": %s,\n", m_debug_logs ? "true" : "false");
    // 界面语言：auto 或 BCP-47（ASCII，安全直接写）
    fprintf(f, "  \"lang\": \"%s\"\n", std::string(m_lang.begin(), m_lang.end()).c_str());
    fprintf(f, "}\n");
    fclose(f);
}

void CDetailWindow::LoadSettings() {
    if (m_config_dir.empty()) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\settings.json", m_config_dir.c_str());
    FILE* f = _wfopen(path, L"r");
    if (!f) return;
    // Read entire file
    char buf[4096];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    buf[len] = 0;
    fclose(f);
    std::string json(buf, len);
    
    // Simple JSON parser for known fields
    // Parse sort_col
    {
        size_t pos = json.find("\"sort_col\"");
        if (pos != std::string::npos) {
            pos = json.find('[', pos);
            if (pos != std::string::npos) {
                int v0 = 0, v1 = 0;
                if (sscanf(json.c_str() + pos, "[%d, %d]", &v0, &v1) == 2 ||
                    sscanf(json.c_str() + pos, "[%d,%d]", &v0, &v1) == 2) {
                    m_sort_col[0] = v0; m_sort_col[1] = v1;
                }
            }
        }
    }
    // Parse sort_asc
    {
        size_t pos = json.find("\"sort_asc\"");
        if (pos != std::string::npos) {
            pos = json.find('[', pos);
            if (pos != std::string::npos) {
                bool a0 = false, a1 = false;
                std::string sub = json.substr(pos, 32);
                if (sub.find("true") != std::string::npos) a0 = true;
                size_t comma = sub.find(',');
                if (comma != std::string::npos && sub.substr(comma).find("true") != std::string::npos) a1 = true;
                m_sort_asc[0] = a0; m_sort_asc[1] = a1;
            }
        }
    }
    // Parse transparent_width
    {
        size_t pos = json.find("\"transparent_width\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                int tw = atoi(json.c_str() + pos + 1);
                if (tw >= 0 && tw <= 500) m_transparent_width = tw;
            }
        }
    }
    // Parse refresh_ms
    {
        size_t pos = json.find("\"refresh_ms\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                int v = atoi(json.c_str() + pos + 1);
                if (v >= 100 && v <= 2000) m_refresh_ms = v;
            }
        }
    }
    // Parse show_speed_items (default ON - master switch for Up/Down items)
    {
        size_t pos = json.find("\"show_speed_items\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                std::string sub = json.substr(pos + 1, 16);
                m_show_speed_items = (sub.find("true") != std::string::npos);
            }
        }
    }
    // Parse debug_logs (default OFF)
    {
        size_t pos = json.find("\"debug_logs\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                std::string sub = json.substr(pos + 1, 16);
                m_debug_logs = (sub.find("true") != std::string::npos);
            }
        }
    }
    // Parse lang (default "auto" = follow TM; otherwise a BCP-47 tag)
    {
        size_t pos = json.find("\"lang\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                size_t b = json.find('"', pos + 1);
                size_t e = b == std::string::npos ? std::string::npos : json.find('"', b + 1);
                if (b != std::string::npos && e != std::string::npos && e > b + 1) {
                    std::string v = json.substr(b + 1, e - b - 1);
                    if (v.size() <= 32) {
                        std::wstring wv(v.begin(), v.end());
                        m_lang = wv;
                    }
                }
            }
        }
    }
    // Parse win_w / win_h (restore window size)
    {
        int ww = 0, wh = 0;
        size_t pos = json.find("\"win_w\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) ww = atoi(json.c_str() + pos + 1);
        }
        pos = json.find("\"win_h\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) wh = atoi(json.c_str() + pos + 1);
        }
        if (ww >= MIN_WIDTH && wh >= MIN_HEIGHT && m_hwnd) {
            SetWindowPos(m_hwnd, NULL, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    // IP 归属地设置: geo_proxy / geo_update_days / geo_enabled
    {
        size_t pos = json.find("\"geo_proxy\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            pos = json.find('"', pos);
            if (pos != std::string::npos) {
                size_t end = json.find('"', pos + 1);
                if (end != std::string::npos) {
                    std::string v = json.substr(pos + 1, end - pos - 1);
                    int n = MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), NULL, 0);
                    if (n > 0) {
                        std::wstring w(n, 0);
                        MultiByteToWideChar(CP_UTF8, 0, v.c_str(), (int)v.size(), &w[0], n);
                        IpGeo::Instance().SetProxy(w);
                    }
                }
            }
        }
        pos = json.find("\"geo_update_days\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                int v = atoi(json.c_str() + pos + 1);
                if (v >= 1 && v <= 365) IpGeo::Instance().SetUpdateDays(v);
            }
        }
        pos = json.find("\"geo_enabled\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                std::string sub = json.substr(pos + 1, 8);
                IpGeo::Instance().SetEnabled(sub.find("true") != std::string::npos);
            }
        }
    }
    // Sync debug-log switch to capture backends (called after SetCapture in OnInitialize)
    EtwCapture::SetDebugLogs(m_debug_logs);
    if (m_capture) {
        wchar_t dbg[MAX_PATH] = L"";
        m_capture->SetLogDir(m_debug_logs && PNM_GetDebugDir(dbg, MAX_PATH) ? dbg : L"");
    }
}

// Debug-log master switch: persists to settings.json and syncs the capture
// backends (etw_capture.log / capture.log). Crash diagnostics stay ON.
void CDetailWindow::SetDebugLogs(bool on) {
    m_debug_logs = on;
    SaveSettings();
    EtwCapture::SetDebugLogs(m_debug_logs);
    if (m_capture) {
        wchar_t dbg[MAX_PATH] = L"";
        m_capture->SetLogDir(m_debug_logs && PNM_GetDebugDir(dbg, MAX_PATH) ? dbg : L"");
    }
}

// ============================================================
// Hit testing
// ============================================================

int CDetailWindow::HitTestRow(int y) const {
    int table_top = GetTableAreaTop();
    if (y < table_top) return -1;
    int cur_y = table_top - m_scroll_pos;
    for (int i = 0; i < (int)m_rows.size(); i++) {
        int this_h = GetExpandedRowHeight(m_rows[i]);
        if (y >= cur_y && y < cur_y + this_h) return i;
        cur_y += this_h;
    }
    return -1;
}

int CDetailWindow::HitTestColumn(int x) const {
    const Column* cols = (m_active_tab == 0) ? m_rt_cols : m_hist_cols;
    int cx = PADDING;
    for (int i = 0; i < NUM_COLS; i++) {
        if (x >= cx && x < cx + cols[i].width) return i;
        cx += cols[i].width;
    }
    return -1;
}

void CDetailWindow::ToggleExpand(int row) {
    if (row < 0 || row >= (int)m_rows.size()) return;

    DisplayRow& r = m_rows[row];
    r.expanded = !r.expanded;

    // Load connection details for all sub-processes when expanding
    if (r.expanded && m_capture) {
        for (auto& sp : r.sub_processes) {
            if (!sp.connections_loaded && sp.pid > 0) {
                sp.connections = m_capture->GetProcessConnections(sp.pid);
                sp.connections_loaded = true;
            }
        }
    }

    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::ScrollTo(int pos) {
    m_scroll_pos = max(0, min(pos, GetScrollMax()));
    InvalidateRect(m_hwnd, NULL, FALSE);
}

int CDetailWindow::GetConnRowHeight(const ConnDetail& conn) const {
    int h = CONN_ROW_H;
    std::wstring geo = IpGeo::Instance().Query(conn.remote_addr);
    if (geo.empty() || geo == L"-") return h;
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return h;
    HFONT old = (HFONT)SelectObject(hdc, m_font_row);
    RECT rc = { 0, 0, m_conn_cols[3].width, 0 };
    DrawTextW(hdc, geo.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    int lines = (rc.bottom + CONN_ROW_H - 1) / CONN_ROW_H;
    if (lines < 1) lines = 1;
    return lines * CONN_ROW_H;
}

int CDetailWindow::GetExpandedRowHeight(const DisplayRow& row) const {
    int h = ROW_H;  // Main row

    if (row.expanded) {
        if (row.sub_processes.empty()) {
            // Simple mode: single PID + path (history tab or single process)
            h += CHILD_ROW_H;  // PID row
            if (!row.exe_path.empty()) h += CHILD_ROW_H;  // Path row
        } else {
            // Multi-process mode: iterate sub-processes
            for (auto& sp : row.sub_processes) {
                h += SUBPROC_HEADER_H;
                if (!sp.exe_path.empty()) h += CHILD_ROW_H;
                if (!sp.connections.empty()) {
                    h += 4;  // padding before connection table (matches DrawTableRows)
                    h += CHILD_ROW_H + CONN_HEADER_H;
                    int max_rows = sp.conn_expanded ? (int)sp.connections.size() : min((int)sp.connections.size(), MAX_CONN_ROWS);
                    for (int ci = 0; ci < max_rows; ci++)
                        h += GetConnRowHeight(sp.connections[ci]);
                    if ((int)sp.connections.size() > MAX_CONN_ROWS)
                        h += CONN_ROW_H;  // expand/collapse button row
                }
            }
        }
    }

    return h;
}

int CDetailWindow::GetTotalHeight() const {
    int h = 0;
    for (auto& r : m_rows)
        h += GetExpandedRowHeight(r);
    return h;
}

int CDetailWindow::GetVisibleHeight() const {
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    return rc.bottom - GetTableAreaTop() - PADDING;
}

int CDetailWindow::GetScrollMax() const {
    return max(0, GetTotalHeight() - GetVisibleHeight());
}

// ============================================================
// Context menu
// ============================================================

void CDetailWindow::ShowContextMenu(int row, int x, int y) {
    if (row < 0 || row >= (int)m_rows.size()) return;
    auto& r = m_rows[row];

    // Lock target process before showing menu
    m_context_menu_pid = r.pid;
    m_context_menu_path = r.exe_path;
    m_context_menu_open = true;

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, TR(L"\u5B9A\u4F4D\u6587\u4EF6"));
    AppendMenuW(hMenu, MF_STRING, 2, TR(L"\u6587\u4EF6\u5C5E\u6027"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 3, TR(L"\u7ED3\u675F\u8FDB\u7A0B"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    // Debug-log master switch (default OFF; crash diagnostics stay ON)
    AppendMenuW(hMenu, MF_STRING, 100,
                m_debug_logs ? TR(L"\u8C03\u8BD5\u65E5\u5FD7: \u5F00 (\u70B9\u51FB\u5173\u95ED)")
                             : TR(L"\u8C03\u8BD5\u65E5\u5FD7: \u5173 (\u70B9\u51FB\u5F00\u542F)"));

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, m_hwnd, NULL);
    DestroyMenu(hMenu);

    // Use locked target, not current row
    DWORD pid = m_context_menu_pid;
    std::wstring path = m_context_menu_path;
    m_context_menu_open = false;

    switch (cmd) {
    case 1: // \u5B9A\u4F4D\u6587\u4EF6
        if (!path.empty()) {
            wchar_t arg[MAX_PATH + 32];
            swprintf_s(arg, L"/select,\"%s\"", path.c_str());
            ShellExecuteW(NULL, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
        }
        break;
    case 2: // \u6587\u4EF6\u5C5E\u6027
        if (!path.empty()) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        break;
    case 3: // \u7ED3\u675F\u8FDB\u7A0B
        if (pid > 0) {
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) {
                TerminateProcess(hProc, 1);
                CloseHandle(hProc);
            }
        }
        break;
    case 100: { // \u8C03\u8BD5\u65E5\u5FD7\u5F00\u5173
        SetDebugLogs(!m_debug_logs);
        break;
    }
    }
}

// ============================================================
// Message handler
// ============================================================

LRESULT CDetailWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: OnPaint(); return 0;
    case WM_SIZE: OnSize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_LBUTTONDOWN: OnLButtonDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONUP: OnLButtonUp((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONDBLCLK: OnLButtonDblClk((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_RBUTTONDOWN: OnRButtonDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_MOUSEMOVE: OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_MOUSELEAVE: OnMouseLeave(); return 0;
    case WM_MOUSEWHEEL: OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_VSCROLL: OnVScroll(LOWORD(wp)); return 0;
    case WM_DESTROY:
        SaveHistory();
        break;
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: {
        UpdateDpiScale(m_hwnd);
        RecreateGdiObjects();
        // Apply suggested window rect from lParam
        RECT* prc = (RECT*)lp;
        SetWindowPos(m_hwnd, NULL, prc->left, prc->top,
            prc->right - prc->left, prc->bottom - prc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(m_hwnd, NULL, FALSE);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(m_hwnd, &pt);
        // 窗口边缘: 支持拖拽缩放 (边缘优先于标题栏)
        int edge = (int)(6 * m_dpi_scale);
        if (edge < 4) edge = 4;
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        bool hit_left = pt.x < edge;
        bool hit_right = pt.x >= rc.right - edge;
        bool hit_top = pt.y < edge;
        bool hit_bottom = pt.y >= rc.bottom - edge;
        if (hit_left && hit_top) return HTTOPLEFT;
        if (hit_right && hit_top) return HTTOPRIGHT;
        if (hit_left && hit_bottom) return HTBOTTOMLEFT;
        if (hit_right && hit_bottom) return HTBOTTOMRIGHT;
        if (hit_left) return HTLEFT;
        if (hit_right) return HTRIGHT;
        if (hit_top) return HTTOP;
        if (hit_bottom) return HTBOTTOM;
        if (pt.y < TITLE_BAR_H && pt.y >= 0) {
            if (PtInRect(&m_rcClose, pt)) return HTCLIENT;
            if (PtInRect(&m_rcMin, pt)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = MIN_WIDTH;
        mmi->ptMinTrackSize.y = MIN_HEIGHT;
        return 0;
    }

    case WM_EXITSIZEMOVE:
        // 拖拽调整大小结束 - 保存窗口宽高
        SaveSettings();
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) { Hide(); return 0; }
        break;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Hide(); return 0; }
        break;

    case WM_TIMER:
        if (wp == TIMER_DEFER_REBUILD) {
            KillTimer(m_hwnd, TIMER_DEFER_REBUILD);
            m_history_dirty = true;
            RebuildRows();
        }
        return 0;

    case WM_PNM_REFRESH:
        // pull latest snapshot on the UI thread (posted by refresh timer)
        CProcessNetPlugin::Instance().DetailRefreshFromSnapshot();
        return 0;

    case WM_PNM_LANG_CHANGED:
        // 运行期语言变化：列头重取翻译 + 重建行（分类文本等）+ 重绘
        InitColumns();
        CProcessNetPlugin::Instance().DetailRefreshFromSnapshot();
        InvalidateRect(m_hwnd, NULL, FALSE);
        return 0;

    case WM_APP + 2:
        // background signature verification finished - refresh so the new
        // categories (系统进程/第三方程序/未签名) appear
        CProcessNetPlugin::Instance().DetailRefreshFromSnapshot();
        return 0;

    case WM_APP + 5:
        // IP 库首次下载失败提示
        MessageBoxW(m_hwnd,
            TR(L"IP \u5F52\u5C5E\u5730\u6570\u636E\u5E93\u4E0B\u8F7D\u5931\u8D25\u3002\n\u53EF\u5728 \u63D2\u4EF6\u8BBE\u7F6E \u4E2D\u914D\u7F6E\u4EE3\u7406\u670D\u52A1\u5668\uFF0C\u6216\u5173\u95ED\u201C\u542F\u7528\u8FDE\u63A5\u5F52\u5C5E\u5730\u663E\u793A\u201D\u3002"),
            L"ProcessNetMonitor", MB_OK | MB_ICONWARNING);
        return 0;

    default:
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

void CDetailWindow::OnSize(int w, int h) {
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::OnLButtonDown(int x, int y) {
    // Check scrollbar thumb click
    if (HitTestScrollbar(x, y)) {
        m_dragging_scrollbar = true;
        m_drag_start_y = y;
        m_drag_start_scroll = m_scroll_pos;
        ::SetCapture(m_hwnd);
        return;
    }
    // Check scrollbar track click (page up/down)
    int w = 0, h = 0;
    RECT rc; GetClientRect(m_hwnd, &rc); w = rc.right; h = rc.bottom;
    int total_h = GetTotalHeight();
    int visible_h = GetVisibleHeight();
    if (total_h > visible_h && x >= w - SCROLL_W - 2 && x <= w - 2) {
        RECT thumb_rc;
        GetScrollbarThumbRect(&thumb_rc, w, h);
        if (y < thumb_rc.top) {
            ScrollTo(m_scroll_pos - visible_h);  // page up
        } else if (y >= thumb_rc.bottom) {
            ScrollTo(m_scroll_pos + visible_h);  // page down
        }
        return;
    }
    
    if (PtInRect(&m_rcClose, { x, y })) { Hide(); return; }
    if (PtInRect(&m_rcMin, { x, y })) { ShowWindow(m_hwnd, SW_MINIMIZE); return; }

    // Tabs
    if (PtInRect(&m_rcTab0, { x, y })) {
        if (m_active_tab != 0) {
            m_active_tab = 0;
            m_scroll_pos = 0;
            m_rows.clear();
            InvalidateRect(m_hwnd, NULL, FALSE);  // full redraw with empty table
            SetTimer(m_hwnd, TIMER_DEFER_REBUILD, 10, NULL);  // rebuild after paint
        }
        return;
    }
    if (PtInRect(&m_rcTab1, { x, y })) {
        if (m_active_tab != 1) {
            m_active_tab = 1;
            m_scroll_pos = 0;
            m_rows.clear();
            InvalidateRect(m_hwnd, NULL, FALSE);  // full redraw for tab switch
            SetTimer(m_hwnd, TIMER_DEFER_REBUILD, 10, NULL);
        }
        return;
    }

    // Time range buttons (history tab only)
    if (m_active_tab == 1) {
        for (int i = 0; i < 5; i++) {
            if (PtInRect(&m_rcTRButtons[i], { x, y })) {
                if (m_time_range != (TimeRange)i) {
                    m_time_range = (TimeRange)i;
                    m_scroll_pos = 0;
                    m_rows.clear();
                    InvalidateRect(m_hwnd, NULL, FALSE);  // full redraw for time range change
                    SetTimer(m_hwnd, TIMER_DEFER_REBUILD, 10, NULL);
                }
                return;
            }
        }
        // Clear history button
        if (PtInRect(&m_rcClearBtn, { x, y })) {
            int ret = MessageBoxW(m_hwnd, TR(L"\u786E\u5B9A\u8981\u6E05\u9664\u6240\u6709\u5386\u53F2\u6D41\u91CF\u6570\u636E\u5417\uFF1F"), TR(L"\u6E05\u9664\u6570\u636E"), MB_YESNO | MB_ICONQUESTION);
            if (ret == IDYES) {
                ClearHistory();
            }
            return;
        }
    }

    // Table header click -> sort
    int th_top = PADDING + GetHeaderHeight() + GetTabsHeight() + GetTimeRangeHeight() + GetSummaryHeight();
    if (y >= th_top && y < th_top + GetTableHeaderHeight()) {
        int col = HitTestColumn(x);
        if (col >= 1) {
            SortByColumn(col);
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
        return;
    }

    // Table row click
    int row = HitTestRow(y);
    if (row >= 0) {
        // Check if click is on "... and N more connections" row for any sub-process
        if (m_rows[row].expanded) {
            int table_top = GetTableAreaTop();
            int cur_y = table_top - m_scroll_pos;
            for (int i = 0; i < row; i++)
                cur_y += GetExpandedRowHeight(m_rows[i]);
            cur_y += ROW_H;  // main row

            // Walk through sub-processes to find which "more" row was clicked
            for (auto& sp : m_rows[row].sub_processes) {
                cur_y += SUBPROC_HEADER_H;
                if (!sp.exe_path.empty()) cur_y += CHILD_ROW_H;
                if (!sp.connections.empty()) {
                    cur_y += 4;  // padding before connection table (matches DrawTableRows)
                    cur_y += CHILD_ROW_H + CONN_HEADER_H;  // title + header
                    int conn_count = sp.conn_expanded ? (int)sp.connections.size() : min((int)sp.connections.size(), MAX_CONN_ROWS);
                    for (int ci = 0; ci < conn_count; ci++)
                        cur_y += GetConnRowHeight(sp.connections[ci]);
                    if ((int)sp.connections.size() > MAX_CONN_ROWS) {
                        // expand/collapse button row
                        if (y >= cur_y && y < cur_y + CONN_ROW_H) {
                            sp.conn_expanded = !sp.conn_expanded;
                            InvalidateRect(m_hwnd, NULL, FALSE);
                            return;
                        }
                        cur_y += CONN_ROW_H;
                    }
                }
            }
        }

        int rel_x = x - PADDING;
        // Icon + name area (0~212px): toggle expand
        if (rel_x >= 0 && rel_x < 32 + 180) {
            ToggleExpand(row);
        }
        // Action column (last col): show context menu
        if (m_active_tab == 0) {
            Column* c = GetActiveCols();
            int action_start = c[0].width + c[1].width + c[2].width + c[3].width + c[4].width + c[5].width;
            if (rel_x >= action_start && rel_x < action_start + c[6].width) {
                POINT pt = { x, y };
                ClientToScreen(m_hwnd, &pt);
                ShowContextMenu(row, pt.x, pt.y);
            }
        } else {
            // History tab: action col at different position
            int action_start = 32 + 180 + 90 + 100 + 100 + 100 + 100; // = 702 (7 cols)
            // History tab has no action col, so no click handler needed
        }
    }

    // Title bar drag
    if (y < TITLE_BAR_H) {
        ReleaseCapture();
        SendMessage(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

void CDetailWindow::OnLButtonUp(int x, int y) {
    if (m_dragging_scrollbar) {
        m_dragging_scrollbar = false;
        ReleaseCapture();
    }
}

void CDetailWindow::OnLButtonDblClk(int x, int y) {
    int row = HitTestRow(y);
    if (row >= 0) ToggleExpand(row);
}

void CDetailWindow::OnRButtonDown(int x, int y) {
    int row = HitTestRow(y);
    if (row >= 0) {
        POINT pt = { x, y };
        ClientToScreen(m_hwnd, &pt);
        ShowContextMenu(row, pt.x, pt.y);
    }
}

void CDetailWindow::OnMouseMove(int x, int y) {
    // Handle scrollbar dragging
    if (m_dragging_scrollbar) {
        RECT rc; GetClientRect(m_hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        int total_h = GetTotalHeight();
        int visible_h = GetVisibleHeight();
        int track_top = GetTableAreaTop();
        int track_bottom = h - PADDING;
        int track_h = track_bottom - track_top;
        float ratio = (float)visible_h / total_h;
        int thumb_h = max(20, (int)(track_h * ratio));
        int scrollable_pixels = total_h - visible_h;
        int thumb_range = track_h - thumb_h;
        if (thumb_range > 0) {
            int dy = y - m_drag_start_y;
            int new_scroll = m_drag_start_scroll + (int)((float)dy * scrollable_pixels / thumb_range);
            ScrollTo(new_scroll);
        }
        return;
    }

    if (!m_tracking_mouse) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        TrackMouseEvent(&tme);
        m_tracking_mouse = true;
    }

    bool old_close = m_hovering_close;
    bool old_min = m_hovering_min;
    int old_row = m_hovered_row;

    m_hovering_close = PtInRect(&m_rcClose, { x, y }) != 0;
    m_hovering_min = PtInRect(&m_rcMin, { x, y }) != 0;
    m_hovered_row = HitTestRow(y);

    if (m_hovering_close != old_close || m_hovering_min != old_min || m_hovered_row != old_row) {
        InvalidateRect(m_hwnd, NULL, FALSE);
    }

    if (m_hovering_close || m_hovering_min) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
    } else if (HitTestScrollbar(x, y)) {
        SetCursor(LoadCursor(NULL, IDC_HAND));
    } else {
        SetCursor(LoadCursor(NULL, IDC_ARROW));
    }
}

void CDetailWindow::OnMouseLeave() {
    m_tracking_mouse = false;
    m_hovering_close = false;
    m_hovering_min = false;
    m_hovered_row = -1;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::OnMouseWheel(int delta) {
    ScrollTo(m_scroll_pos - (delta > 0 ? ROW_H * 3 : -ROW_H * 3));
}

void CDetailWindow::OnVScroll(int code) {
    int page = GetVisibleHeight();
    switch (code) {
    case SB_LINEUP: ScrollTo(m_scroll_pos - ROW_H); break;
    case SB_LINEDOWN: ScrollTo(m_scroll_pos + ROW_H); break;
    case SB_PAGEUP: ScrollTo(m_scroll_pos - page); break;
    case SB_PAGEDOWN: ScrollTo(m_scroll_pos + page); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        // For custom scrollbar, we handle dragging in OnMouseMove
        break;
    }
    }
}

bool CDetailWindow::HitTestScrollbar(int x, int y) const {
    int total_h = GetTotalHeight();
    int visible_h = GetVisibleHeight();
    if (total_h <= visible_h) return false;
    RECT rc; GetClientRect(m_hwnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    RECT thumb_rc;
    const_cast<CDetailWindow*>(this)->GetScrollbarThumbRect(&thumb_rc, w, h);
    return (x >= thumb_rc.left && x < thumb_rc.right && y >= thumb_rc.top && y < thumb_rc.bottom);
}

void CDetailWindow::GetScrollbarThumbRect(RECT* rc, int w, int h) const {
    int total_h = GetTotalHeight();
    int visible_h = GetVisibleHeight();
    int track_x = w - SCROLL_W - 2;
    int track_top = GetTableAreaTop();
    int track_bottom = h - PADDING;
    int track_h = track_bottom - track_top;
    float ratio = (float)visible_h / total_h;
    int thumb_h = max(20, (int)(track_h * ratio));
    float scroll_ratio = (float)m_scroll_pos / max(1, total_h - visible_h);
    int thumb_y = track_top + (int)((track_h - thumb_h) * scroll_ratio);
    rc->left = track_x + 1;
    rc->top = thumb_y;
    rc->right = track_x + SCROLL_W - 1;
    rc->bottom = thumb_y + thumb_h;
}

// ============================================================
// Drawing
// ============================================================

void CDetailWindow::DrawTitleBar(HDC hdc, int w) {
    RECT rc = { 0, 0, w, TITLE_BAR_H };
    HBRUSH hBrush = CreateSolidBrush(GetTitleBarColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HPEN hPen = CreatePen(PS_SOLID, 1, GetBorderColor());
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, 0, TITLE_BAR_H - 1, NULL);
    LineTo(hdc, w, TITLE_BAR_H - 1);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);

    int title_icon_sz = (int)(16 * m_dpi_scale);
    int icon_text_gap = (int)(8 * m_dpi_scale);  // gap between icon and title text
    HICON hIcon = (HICON)LoadImageW(m_hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, title_icon_sz, title_icon_sz, 0);
    if (!hIcon) hIcon = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, title_icon_sz, title_icon_sz, LR_SHARED);
    if (hIcon) DrawIconEx(hdc, PADDING, (TITLE_BAR_H - title_icon_sz) / 2, hIcon, title_icon_sz, title_icon_sz, 0, NULL, DI_NORMAL);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetTextColor());
    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_title);
    RECT title_rc = { PADDING + title_icon_sz + icon_text_gap, 0, w - 100, TITLE_BAR_H };
    DrawTextW(hdc, L"ProcessNetMonitor", -1, &title_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    int btn_size = 28;
    int btn_y = (TITLE_BAR_H - btn_size) / 2;
    m_rcClose = { w - btn_size - 4, btn_y, w - 4, btn_y + btn_size };
    SetTextColor(hdc, m_hovering_close ? RGB(255, 80, 80) : GetSecondaryTextColor());
    DrawTextW(hdc, L"\u2715", -1, &m_rcClose, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    m_rcMin = { w - btn_size * 2 - 4, btn_y, w - btn_size - 4, btn_y + btn_size };
    SetTextColor(hdc, m_hovering_min ? GetTextColor() : GetSecondaryTextColor());
    DrawTextW(hdc, L"\u2013", -1, &m_rcMin, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hOldFont);
}

void CDetailWindow::DrawTabs(HDC hdc, int w, int y) {
    RECT rc = { 0, y, w, y + TAB_BAR_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_row);
    SetBkMode(hdc, TRANSPARENT);

    SIZE sz;
    const wchar_t* tab0 = TR(L"\u5B9E\u65F6\u6D41\u91CF");
    GetTextExtentPoint32W(hdc, tab0, (int)wcslen(tab0), &sz);
    m_rcTab0 = { PADDING, y + 4, PADDING + sz.cx + 16, y + TAB_BAR_H - 4 };
    SetTextColor(hdc, m_active_tab == 0 ? GetAccentColor(false) : GetSecondaryTextColor());
    DrawTextW(hdc, tab0, -1, &m_rcTab0, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (m_active_tab == 0) {
        HPEN hPen = CreatePen(PS_SOLID, 2, GetAccentColor(false));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, m_rcTab0.left + 2, y + TAB_BAR_H - 4, NULL);
        LineTo(hdc, m_rcTab0.right - 2, y + TAB_BAR_H - 4);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    const wchar_t* tab1 = TR(L"\u5386\u53F2\u6D41\u91CF");
    GetTextExtentPoint32W(hdc, tab1, (int)wcslen(tab1), &sz);
    m_rcTab1 = { m_rcTab0.right + 12, y + 4, m_rcTab0.right + 12 + sz.cx + 16, y + TAB_BAR_H - 4 };
    SetTextColor(hdc, m_active_tab == 1 ? GetAccentColor(false) : GetSecondaryTextColor());
    DrawTextW(hdc, tab1, -1, &m_rcTab1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (m_active_tab == 1) {
        HPEN hPen = CreatePen(PS_SOLID, 2, GetAccentColor(false));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, m_rcTab1.left + 2, y + TAB_BAR_H - 4, NULL);
        LineTo(hdc, m_rcTab1.right - 2, y + TAB_BAR_H - 4);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    SelectObject(hdc, hOldFont);
}

void CDetailWindow::DrawTimeRangeButtons(HDC hdc, int w, int y) {
    if (m_active_tab != 1) return;

    RECT rc = { 0, y, w, y + TIME_RANGE_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_time);
    SetBkMode(hdc, TRANSPARENT);

    const wchar_t* labels[5] = {
        TR(L"\u4ECA\u65E5"),        // 今日
        TR(L"\u6628\u65E5"),        // 昨日
        TR(L"\u8FD13\u5929"),       // 近3天
        TR(L"\u8FD17\u5929"),       // 近7天
        TR(L"\u8FD130\u5929")       // 近30天
    };
    int x = PADDING;
    for (int i = 0; i < 5; i++) {
        SIZE sz;
        GetTextExtentPoint32W(hdc, labels[i], (int)wcslen(labels[i]), &sz);
        int bw = sz.cx + 16;
        m_rcTRButtons[i] = { x, y + 3, x + bw, y + TIME_RANGE_H - 3 };

        // Draw button
        bool selected = (m_time_range == (TimeRange)i);
        HBRUSH hBtnBrush = CreateSolidBrush(selected ? GetAccentColor(false) : GetHeaderBgColor());
        HPEN hBtnPen = CreatePen(PS_SOLID, 1, selected ? GetAccentColor(false) : GetBorderColor());
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBtnBrush);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hBtnPen);
        RoundRect(hdc, m_rcTRButtons[i].left, m_rcTRButtons[i].top,
                  m_rcTRButtons[i].right, m_rcTRButtons[i].bottom, 4, 4);
        SelectObject(hdc, hOldBrush);
        SelectObject(hdc, hOldPen);
        DeleteObject(hBtnBrush);
        DeleteObject(hBtnPen);

        SetTextColor(hdc, selected ? RGB(255, 255, 255) : GetTextColor());
        DrawTextW(hdc, labels[i], -1, &m_rcTRButtons[i], DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        x += bw + 6;
    }

    // Clear history button (right-aligned)
    const wchar_t* clear_label = TR(L"\u6E05\u9664\u6570\u636E");
    SIZE clear_sz;
    GetTextExtentPoint32W(hdc, clear_label, (int)wcslen(clear_label), &clear_sz);
    int clear_bw = clear_sz.cx + 16;
    m_rcClearBtn = { w - PADDING - clear_bw, y + 3, w - PADDING, y + TIME_RANGE_H - 3 };

    HBRUSH hClearBrush = CreateSolidBrush(RGB(220, 53, 69));
    HPEN hClearPen = CreatePen(PS_SOLID, 1, RGB(220, 53, 69));
    HBRUSH hOldBrush2 = (HBRUSH)SelectObject(hdc, hClearBrush);
    HPEN hOldPen2 = (HPEN)SelectObject(hdc, hClearPen);
    RoundRect(hdc, m_rcClearBtn.left, m_rcClearBtn.top,
              m_rcClearBtn.right, m_rcClearBtn.bottom, 4, 4);
    SelectObject(hdc, hOldBrush2);
    SelectObject(hdc, hOldPen2);
    DeleteObject(hClearBrush);
    DeleteObject(hClearPen);

    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, clear_label, -1, &m_rcClearBtn, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hOldFont);
}

void CDetailWindow::DrawSpeedSummary(HDC hdc, int w, int y) {
    RECT rc = { 0, y, w, y + SUMMARY_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_header);
    SetBkMode(hdc, TRANSPARENT);

    int x = PADDING;

    if (m_active_tab == 1) {
        // History: show total traffic for selected range
        wchar_t recv_str[32], sent_str[32];
        FormatBytes(m_hist_total_recv, recv_str, 32);
        FormatBytes(m_hist_total_sent, sent_str, 32);

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l1 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, TR(L"\u603B\u4E0B\u8F7D\uFF1A"), -1, &l1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 55;
        SetTextColor(hdc, GetAccentColor(false));
        RECT v1 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, recv_str, -1, &v1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 110;

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l2 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, TR(L"\u603B\u4E0A\u4F20\uFF1A"), -1, &l2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 55;
        SetTextColor(hdc, GetAccentColor(true));
        RECT v2 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, sent_str, -1, &v2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    } else {
        // Real-time: show current speed
        wchar_t down_str[32], up_str[32];
        FormatSpeed(m_total_down, down_str, 32);
        FormatSpeed(m_total_up, up_str, 32);

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l1 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, TR(L"\u4E0B\u8F7D\u603B\u901F\u5EA6\uFF1A"), -1, &l1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 80;
        SetTextColor(hdc, GetAccentColor(false));
        wchar_t d_full[64]; swprintf_s(d_full, 64, L"\u2193 %s", down_str);
        RECT v1 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, d_full, -1, &v1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 120;

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l2 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, TR(L"\u4E0A\u4F20\u603B\u901F\u5EA6\uFF1A"), -1, &l2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 80;
        SetTextColor(hdc, GetAccentColor(true));
        wchar_t u_full[64]; swprintf_s(u_full, 64, L"\u2191 %s", up_str);
        RECT v2 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, u_full, -1, &v2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

    SelectObject(hdc, hOldFont);
}

void CDetailWindow::DrawTableHeader(HDC hdc, int w, int y) {
    m_rcTableHeader = { PADDING, y, w - PADDING, y + TABLE_HEADER_H };

    HBRUSH hBrush = CreateSolidBrush(GetHeaderBgColor());
    HPEN hPen = CreatePen(PS_SOLID, 1, GetBorderColor());
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    RoundRect(hdc, m_rcTableHeader.left, m_rcTableHeader.top,
              m_rcTableHeader.right, m_rcTableHeader.bottom, 4, 4);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_header);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSecondaryTextColor());

    Column* cols = GetActiveCols();
    int cx = PADDING;
    for (int i = 0; i < NUM_COLS; i++) {
        RECT rc = { cx + 4, y, cx + cols[i].width - 4, y + TABLE_HEADER_H };
        UINT fmt = DT_SINGLELINE | DT_VCENTER;
        switch (cols[i].align) {
        case Column::LEFT: fmt |= DT_LEFT; break;
        case Column::RIGHT: fmt |= DT_RIGHT; break;
        case Column::CENTER: fmt |= DT_CENTER; break;
        }
        DrawTextW(hdc, cols[i].title, -1, &rc, fmt);

        if (i == m_sort_col[m_active_tab] && i >= 1) {
            SetTextColor(hdc, GetAccentColor(false));
            const wchar_t* arrow = m_sort_asc[m_active_tab] ? L" \u25B2" : L" \u25BC";
            RECT arrow_rc = { rc.right - 16, y, rc.right + 4, y + TABLE_HEADER_H };
            DrawTextW(hdc, arrow, -1, &arrow_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(hdc, GetSecondaryTextColor());
        }

        cx += cols[i].width;
    }

    SelectObject(hdc, hOldFont);
}

    // 连接表格列宽自适应窗口: 协议/状态固定, 本地/远程/归属地按权重分配
    // 权重: 本地 100 : 远程 100 : 归属地 120 (归属地关闭时权重 0)
    void CDetailWindow::UpdateConnColWidths(int win_w) {
        int table_w = win_w - PADDING - 20 - (int)(32 * m_dpi_scale) - SUBPROC_INDENT - CONN_TABLE_PADDING;
        if (table_w < 320) table_w = 320;
        int fixed = m_conn_cols[0].width + m_conn_cols[4].width;  // 协议 + 状态 (已 DPI 缩放)
        int rest = table_w - fixed;
        if (rest < 200) rest = 200;
        if (IpGeo::Instance().IsEnabled()) {
            m_conn_cols[1].width = (int)(rest * 100 / 320);
            m_conn_cols[2].width = (int)(rest * 100 / 320);
            m_conn_cols[3].width = rest - m_conn_cols[1].width - m_conn_cols[2].width;
        } else {
            // 归属地关闭: 本地/远程对半分
            m_conn_cols[1].width = rest / 2;
            m_conn_cols[2].width = rest - m_conn_cols[1].width;
            m_conn_cols[3].width = 0;
        }
    }

void CDetailWindow::DrawTableRows(HDC hdc, int w, int y, int client_h) {
    int row_h = GetRowHeight();

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_row);
    SetBkMode(hdc, TRANSPARENT);
    Column* cols = GetActiveCols();

    // 连接表格列宽随窗口宽度自适应
    UpdateConnColWidths(w);

    int table_bottom = client_h - PADDING;
    bool is_hist = (m_active_tab == 1);

    // Empty state
    if (m_rows.empty()) {
        SetTextColor(hdc, GetSecondaryTextColor());
        RECT rc = { PADDING, y, w - PADDING, y + 60 };
        const wchar_t* msg = is_hist
            ? TR(L"\u6682\u65E0\u5386\u53F2\u6570\u636E\n\u4F7F\u7528\u4E00\u6BB5\u65F6\u95F4\u540E\u8FD9\u91CC\u5C06\u663E\u793A\u6D41\u91CF\u7EDF\u8BA1")
            : TR(L"\u65E0\u6D3B\u8DC3\u8FDB\u7A0B\n\u6B63\u5728\u76D1\u63A7\u7F51\u7EDC\u6D41\u91CF...");
        DrawTextW(hdc, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(hdc, hOldFont);
        return;
    }

    // Draw rows with dynamic height (expanded rows take more space)
    // Pixel-based scrolling: skip pixels, then draw from there
    int pixels_to_skip = m_scroll_pos;
    int cur_y = y;
    for (int ri = 0; ri < (int)m_rows.size(); ri++) {
        auto& row = m_rows[ri];
        int this_row_h = GetExpandedRowHeight(row);

        // Skip rows entirely above visible area
        if (pixels_to_skip >= this_row_h) {
            pixels_to_skip -= this_row_h;
            continue;
        }

        // This row is partially or fully visible
        int row_offset = pixels_to_skip;  // pixels of this row that are scrolled above
        pixels_to_skip = 0;

        if (cur_y >= table_bottom) break;

        int ry = cur_y - row_offset;  // Virtual Y position (may be above visible area)

        // Clip drawing to visible area
        int clip_top = max(ry, y);
        int clip_bottom = min(ry + this_row_h, table_bottom);
        if (clip_bottom <= clip_top) { cur_y += this_row_h - row_offset; continue; }

        HRGN hClipRgn = CreateRectRgn(PADDING, clip_top, w - PADDING, clip_bottom);
        SelectClipRgn(hdc, hClipRgn);

        // Row background
        bool hovered = (ri == m_hovered_row);
        RECT row_rc = { PADDING, ry, w - PADDING, ry + row_h };
        FillRect(hdc, &row_rc, hovered ? m_br_hover : m_br_row[ri % 2]);

        // Bottom border
        HPEN hOldPen = (HPEN)SelectObject(hdc, m_pen_border);
        MoveToEx(hdc, PADDING, ry + row_h - 1, NULL);
        LineTo(hdc, w - PADDING, ry + row_h - 1);
        SelectObject(hdc, hOldPen);

        int cx = PADDING;

        // Col 0: Icon + expand arrow
        SelectObject(hdc, m_font_small);
        SetTextColor(hdc, GetSecondaryTextColor());
        const wchar_t* arrow = row.expanded ? L"\u25BC" : L"\u25B6";
        RECT arrow_rc = { cx, ry, cx + (int)(14 * m_dpi_scale), ry + row_h };
        DrawTextW(hdc, arrow, -1, &arrow_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        HICON hIcon = GetProcessIcon(row.exe_path);
        int arrow_w = (int)(14 * m_dpi_scale);
        int icon_gap = (int)(4 * m_dpi_scale);
        int icon_x = cx + arrow_w + icon_gap;
        if (hIcon) DrawIconEx(hdc, icon_x, ry + (row_h - ICON_SIZE) / 2, hIcon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
        cx += arrow_w + icon_gap + ICON_SIZE + (int)(4 * m_dpi_scale);  // advance past icon

        // Col 1: Name
        SelectObject(hdc, m_font_row);
        bool name_unresolved = (!row.name.empty() && row.name[0] == L'<');
        SetTextColor(hdc, name_unresolved ? GetSecondaryTextColor() : GetTextColor());
        {
            std::wstring display = row.name;
            if (display.size() > 4) {
                std::wstring ext = display.substr(display.size() - 4);
                if (ext == L".exe" || ext == L".EXE")
                    display = display.substr(0, display.size() - 4);
            }
            SIZE sz;
            int max_w = cols[1].width - 8;
            GetTextExtentPoint32W(hdc, display.c_str(), (int)display.size(), &sz);
            while ((int)display.size() > 3 && sz.cx > max_w) {
                display = display.substr(0, display.size() - 1);
                display.back() = L'\u2026';
                GetTextExtentPoint32W(hdc, display.c_str(), (int)display.size(), &sz);
            }
            RECT name_rc = { cx + 4, ry, cx + cols[1].width - 4, ry + row_h };
            DrawTextW(hdc, display.c_str(), -1, &name_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += cols[1].width;

        // Col 2: Category
        SetTextColor(hdc, GetSecondaryTextColor());
        {
            RECT cat_rc = { cx + 4, ry, cx + cols[2].width - 4, ry + row_h };
            DrawTextW(hdc, row.category.c_str(), -1, &cat_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += cols[2].width;

        if (is_hist) {
            // History: total_down, total_up, avg_down, avg_up
            wchar_t buf[32];

            FormatBytes(row.hist_recv, buf, 32);
            SetTextColor(hdc, row.hist_recv > 0 ? GetAccentColor(false) : GetSecondaryTextColor());
            RECT r3 = { cx + 4, ry, cx + cols[3].width - 4, ry + row_h };
            DrawTextW(hdc, buf, -1, &r3, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            cx += cols[3].width;

            FormatBytes(row.hist_sent, buf, 32);
            SetTextColor(hdc, row.hist_sent > 0 ? GetAccentColor(true) : GetSecondaryTextColor());
            RECT r4 = { cx + 4, ry, cx + cols[4].width - 4, ry + row_h };
            DrawTextW(hdc, buf, -1, &r4, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            cx += cols[4].width;

            FormatSpeed(row.hist_avg_down, buf, 32);
            SetTextColor(hdc, row.hist_avg_down > 0.01 ? GetAccentColor(false) : GetSecondaryTextColor());
            RECT r5 = { cx + 4, ry, cx + cols[5].width - 4, ry + row_h };
            DrawTextW(hdc, buf, -1, &r5, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            cx += cols[5].width;

            FormatSpeed(row.hist_avg_up, buf, 32);
            SetTextColor(hdc, row.hist_avg_up > 0.01 ? GetAccentColor(true) : GetSecondaryTextColor());
            RECT r6 = { cx + 4, ry, cx + cols[6].width - 4, ry + row_h };
            DrawTextW(hdc, buf, -1, &r6, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        } else {
            // Real-time: speed_down, speed_up, conn_count, action
            wchar_t spd[32];

            FormatSpeed(row.speed_down, spd, 32);
            SetTextColor(hdc, row.speed_down > 0.01 ? GetAccentColor(false) : GetSecondaryTextColor());
            RECT r3 = { cx + 4, ry, cx + cols[3].width - 4, ry + row_h };
            DrawTextW(hdc, spd, -1, &r3, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            cx += cols[3].width;

            FormatSpeed(row.speed_up, spd, 32);
            SetTextColor(hdc, row.speed_up > 0.01 ? GetAccentColor(true) : GetSecondaryTextColor());
            RECT r4 = { cx + 4, ry, cx + cols[4].width - 4, ry + row_h };
            DrawTextW(hdc, spd, -1, &r4, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            cx += cols[4].width;

            wchar_t conn[16]; swprintf_s(conn, L"%d", row.conn_count);
            SetTextColor(hdc, row.conn_count > 0 ? GetAccentColor(true) : GetSecondaryTextColor());
            RECT r5 = { cx + 4, ry, cx + cols[5].width - 4, ry + row_h };
            DrawTextW(hdc, conn, -1, &r5, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            cx += cols[5].width;

            SetTextColor(hdc, m_dark_mode ? RGB(255, 165, 0) : RGB(230, 126, 34));
            SelectObject(hdc, m_font_small);
            RECT r6 = { cx + 4, ry, cx + cols[6].width - 4, ry + row_h };
            DrawTextW(hdc, L"\u00B7\u00B7\u00B7", -1, &r6, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        // Expanded children
        if (row.expanded) {
            int child_y = ry + row_h;
            SelectObject(hdc, m_font_row);
            SetTextColor(hdc, GetSecondaryTextColor());
            
            if (row.sub_processes.empty()) {
                // Simple mode: PID + path (history tab)
                RECT pid_rc = { PADDING + (int)(32 * m_dpi_scale), child_y, w - PADDING, child_y + CHILD_ROW_H };
                FillRect(hdc, &pid_rc, m_br_child);
                wchar_t pid_text[64];
                if (row.pid > 0)
                    swprintf_s(pid_text, 64, TR(L"\u8FDB\u7A0BID: %u"), row.pid);
                else
                    swprintf_s(pid_text, 64, TR(L"\u5386\u53F2\u8BB0\u5F55"));
                DrawTextW(hdc, pid_text, -1, &pid_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                HPEN hOldPen2 = (HPEN)SelectObject(hdc, m_pen_border_exp);
                MoveToEx(hdc, PADDING, child_y + CHILD_ROW_H - 1, NULL);
                LineTo(hdc, w - PADDING, child_y + CHILD_ROW_H - 1);
                SelectObject(hdc, hOldPen2);
                child_y += CHILD_ROW_H;
                
                if (!row.exe_path.empty()) {
                    RECT path_rc = { PADDING + (int)(32 * m_dpi_scale), child_y, w - PADDING, child_y + CHILD_ROW_H };
                    FillRect(hdc, &path_rc, m_br_child);
                    std::wstring path_display = row.exe_path;
                    SIZE sz;
                    int max_path_w = w - PADDING - 28 - PADDING;
                    GetTextExtentPoint32W(hdc, path_display.c_str(), (int)path_display.size(), &sz);
                    while ((int)path_display.size() > 5 && sz.cx > max_path_w) {
                        path_display = L"\u2026" + path_display.substr(4);
                        GetTextExtentPoint32W(hdc, path_display.c_str(), (int)path_display.size(), &sz);
                    }
                    DrawTextW(hdc, path_display.c_str(), -1, &path_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                    HPEN hOldPen3 = (HPEN)SelectObject(hdc, m_pen_border_exp);
                    MoveToEx(hdc, PADDING, child_y + CHILD_ROW_H - 1, NULL);
                    LineTo(hdc, w - PADDING, child_y + CHILD_ROW_H - 1);
                    SelectObject(hdc, hOldPen3);
                }
            } else {
                // Multi-process mode: iterate sub-processes
                int sp_idx = 0;
                for (auto& sp : row.sub_processes) {
                sp_idx++;

                // Sub-process header: "进程N (PID: xxxx)"
                RECT sp_rc = { PADDING + (int)(32 * m_dpi_scale), child_y, w - PADDING, child_y + SUBPROC_HEADER_H };
                FillRect(hdc, &sp_rc, m_br_child);
                wchar_t sp_text[128];
                if (row.sub_processes.size() > 1)
                    swprintf_s(sp_text, 128, TR(L"\u8FDB\u7A0B%d (PID: %u)"), sp_idx, sp.pid);
                else
                    swprintf_s(sp_text, 128, L"PID: %u", sp.pid);
                SetTextColor(hdc, GetAccentColor(false));
                DrawTextW(hdc, sp_text, -1, &sp_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                HPEN hOldPenSP = (HPEN)SelectObject(hdc, m_pen_border_exp);
                MoveToEx(hdc, PADDING, child_y + SUBPROC_HEADER_H - 1, NULL);
                LineTo(hdc, w - PADDING, child_y + SUBPROC_HEADER_H - 1);
                SelectObject(hdc, hOldPenSP);
                child_y += SUBPROC_HEADER_H;

                // Path row
                if (!sp.exe_path.empty()) {
                    RECT path_rc = { PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT, child_y, w - PADDING, child_y + CHILD_ROW_H };
                    FillRect(hdc, &path_rc, m_br_child);

                    std::wstring path_display = sp.exe_path;
                    SIZE sz;
                    int max_path_w = w - PADDING - 28 - SUBPROC_INDENT - PADDING;
                    GetTextExtentPoint32W(hdc, path_display.c_str(), (int)path_display.size(), &sz);
                    while ((int)path_display.size() > 5 && sz.cx > max_path_w) {
                        path_display = L"\u2026" + path_display.substr(4);
                        GetTextExtentPoint32W(hdc, path_display.c_str(), (int)path_display.size(), &sz);
                    }
                    SetTextColor(hdc, GetSecondaryTextColor());
                    DrawTextW(hdc, path_display.c_str(), -1, &path_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                    HPEN hOldPenP = (HPEN)SelectObject(hdc, m_pen_border_exp);
                    MoveToEx(hdc, PADDING, child_y + CHILD_ROW_H - 1, NULL);
                    LineTo(hdc, w - PADDING, child_y + CHILD_ROW_H - 1);
                    SelectObject(hdc, hOldPenP);
                    child_y += CHILD_ROW_H;
                }

                // Connection table for this sub-process
                if (!sp.connections.empty()) {
                    child_y += 4;

                    // "连接列表:" title
                    RECT title_rc = { PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT, child_y, w - PADDING, child_y + CHILD_ROW_H };
                    FillRect(hdc, &title_rc, m_br_child);
                    SetTextColor(hdc, GetSecondaryTextColor());
                    DrawTextW(hdc, TR(L"\u8FDE\u63A5\u5217\u8868:"), -1, &title_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                    child_y += CHILD_ROW_H;

                    // Table header
                    RECT header_rc = { PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT, child_y, w - PADDING - 20, child_y + CONN_HEADER_H };
                    HBRUSH hHeaderBrush = CreateSolidBrush(GetHeaderBgColor());
                    FillRect(hdc, &header_rc, hHeaderBrush);
                    DeleteObject(hHeaderBrush);

                    SelectObject(hdc, m_font_header);
                    SetTextColor(hdc, GetSecondaryTextColor());
                    int col_x = PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT + CONN_TABLE_PADDING;
                    for (int ci = 0; ci < NUM_CONN_COLS; ci++) {
                        RECT col_rc = { col_x, child_y, col_x + m_conn_cols[ci].width, child_y + CONN_HEADER_H };
                        DrawTextW(hdc, m_conn_cols[ci].title, -1, &col_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                        col_x += m_conn_cols[ci].width;
                    }
                    child_y += CONN_HEADER_H;

                    // Connection rows
                    int conn_count = sp.conn_expanded ? (int)sp.connections.size() : min((int)sp.connections.size(), MAX_CONN_ROWS);
                    for (int ci = 0; ci < conn_count; ci++) {
                        auto& conn = sp.connections[ci];

                        // 行高: 归属地自动换行后所需高度
                        int row_h = GetConnRowHeight(conn);
                        RECT conn_rc = { PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT, child_y, w - PADDING - 20, child_y + row_h };
                        FillRect(hdc, &conn_rc, m_br_row[ci % 2]);
                        SelectObject(hdc, m_font_row);

                        int col_x = PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT + CONN_TABLE_PADDING;

                        // Protocol
                        SetTextColor(hdc, conn.protocol == ConnDetail::TCP ? GetAccentColor(false) : GetAccentColor(true));
                        RECT proto_rc = { col_x, child_y, col_x + m_conn_cols[0].width, child_y + row_h };
                        DrawTextW(hdc, conn.protocol == ConnDetail::TCP ? L"TCP" : L"UDP", -1, &proto_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                        col_x += m_conn_cols[0].width;

                        // Local address
                        SetTextColor(hdc, GetTextColor());
                        RECT local_rc = { col_x, child_y, col_x + m_conn_cols[1].width, child_y + row_h };
                        DrawTextW(hdc, conn.local_addr.c_str(), -1, &local_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                        col_x += m_conn_cols[1].width;

                        // Remote address
                        RECT remote_rc = { col_x, child_y, col_x + m_conn_cols[2].width, child_y + row_h };
                        DrawTextW(hdc, conn.remote_addr.c_str(), -1, &remote_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                        col_x += m_conn_cols[2].width;

                        // Geo location (归属地, 超长自动换行完整显示)
                        SetTextColor(hdc, GetSecondaryTextColor());
                        std::wstring geo = IpGeo::Instance().Query(conn.remote_addr);
                        RECT geo_rc = { col_x, child_y, col_x + m_conn_cols[3].width, child_y + row_h };
                        if (!geo.empty()) {
                            // 垂直居中多行文本
                            RECT mrc = geo_rc;
                            DrawTextW(hdc, geo.c_str(), -1, &mrc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                            int text_h = mrc.bottom - mrc.top;
                            int top_off = max(0, (row_h - text_h) / 2);
                            RECT draw_rc = { geo_rc.left, geo_rc.top + top_off, geo_rc.right, geo_rc.top + top_off + text_h };
                            DrawTextW(hdc, geo.c_str(), -1, &draw_rc, DT_WORDBREAK | DT_LEFT | DT_TOP | DT_NOPREFIX);
                        }
                        col_x += m_conn_cols[3].width;

                        // State
                        COLORREF state_color = GetSecondaryTextColor();
                        if (conn.state == L"ESTABLISHED") state_color = RGB(76, 175, 80);
                        else if (conn.state == L"LISTENING") state_color = RGB(33, 150, 243);
                        else if (conn.state == L"TIME_WAIT") state_color = RGB(255, 152, 0);
                        else if (conn.state == L"CLOSE_WAIT") state_color = RGB(255, 87, 34);
                        SetTextColor(hdc, state_color);
                        RECT state_rc = { col_x, child_y, col_x + m_conn_cols[4].width, child_y + row_h };
                        DrawTextW(hdc, conn.state.c_str(), -1, &state_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                        child_y += row_h;
                    }

                    // expand/collapse button
                    if ((int)sp.connections.size() > MAX_CONN_ROWS) {
                        SetTextColor(hdc, GetAccentColor(true));
                        RECT more_rc = { PADDING + (int)(32 * m_dpi_scale) + SUBPROC_INDENT, child_y, w - PADDING, child_y + CONN_ROW_H };
                        wchar_t more_text[64];
                        if (sp.conn_expanded) {
                            swprintf_s(more_text, TR(L"\u6536\u7F29\u8FDE\u63A5\u5217\u8868 (\u70B9\u51FB\u6536\u8D77, \u5171 %d \u4E2A\u8FDE\u63A5)"), (int)sp.connections.size());
                        } else {
                            swprintf_s(more_text, TR(L"... \u8FD8\u6709 %d \u4E2A\u8FDE\u63A5 (\u70B9\u51FB\u5C55\u5F00)"), (int)sp.connections.size() - MAX_CONN_ROWS);
                        }
                        DrawTextW(hdc, more_text, -1, &more_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                        child_y += CONN_ROW_H;
                    }
                }
                }  // close for
            }  // close else
        }  // close if expanded

        SelectClipRgn(hdc, NULL);
        DeleteObject(hClipRgn);
        cur_y += this_row_h - row_offset;
    }

    SelectObject(hdc, hOldFont);
}

void CDetailWindow::DrawScrollbar(HDC hdc, int w, int h) {
    int total_h = GetTotalHeight();
    int visible_h = GetVisibleHeight();
    if (total_h <= visible_h) return;

    int track_x = w - SCROLL_W - 2;
    int track_top = GetTableAreaTop();
    int track_bottom = h - PADDING;
    int track_h = track_bottom - track_top;

    HBRUSH hTrack = CreateSolidBrush(m_dark_mode ? RGB(45, 45, 50) : RGB(230, 230, 230));
    RECT track_rc = { track_x, track_top, track_x + SCROLL_W, track_bottom };
    FillRect(hdc, &track_rc, hTrack);
    DeleteObject(hTrack);

    float ratio = (float)visible_h / total_h;
    int thumb_h = max(20, (int)(track_h * ratio));
    float scroll_ratio = (float)m_scroll_pos / max(1, total_h - visible_h);
    int thumb_y = track_top + (int)((track_h - thumb_h) * scroll_ratio);

    HBRUSH hThumb = CreateSolidBrush(m_dark_mode ? RGB(80, 80, 90) : RGB(190, 190, 190));
    RECT thumb_rc = { track_x + 1, thumb_y, track_x + SCROLL_W - 1, thumb_y + thumb_h };
    FillRect(hdc, &thumb_rc, hThumb);
    DeleteObject(hThumb);
}

void CDetailWindow::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right;
    int h = client.bottom;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    HBRUSH hBg = CreateSolidBrush(GetBgColor());
    FillRect(memDC, &client, hBg);
    DeleteObject(hBg);

    int y = PADDING;

    DrawTitleBar(memDC, w);
    y += GetHeaderHeight();

    DrawTabs(memDC, w, y);
    y += GetTabsHeight();

    DrawTimeRangeButtons(memDC, w, y);
    y += GetTimeRangeHeight();

    DrawSpeedSummary(memDC, w, y);
    y += GetSummaryHeight();

    DrawTableHeader(memDC, w, y);
    y += GetTableHeaderHeight();

    DrawTableRows(memDC, w, y, h);
    DrawScrollbar(memDC, w, h);

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(m_hwnd, &ps);
}
