#include "detail_window.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <set>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

CDetailWindow* CDetailWindow::s_instance = nullptr;
static HINSTANCE s_detail_hinst = nullptr;

BOOL APIENTRY DllMain_Detail(HMODULE hModule, DWORD reason, LPVOID);

// Reuse DllMain from tooltip_popup.cpp - we capture hinst differently
static struct DetailHinstCapture {
    HINSTANCE hinst = nullptr;
} s_dhinst;

// ============================================================
// Static WndProc
// ============================================================
static LRESULT CALLBACK DetailStaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (CDetailWindow::s_instance)
        return CDetailWindow::s_instance->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// Construction
// ============================================================

CDetailWindow::CDetailWindow() {
    s_instance = this;
    m_dark_mode = IsDarkMode();
}

CDetailWindow::~CDetailWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    for (auto& [path, icon] : m_icon_cache) {
        if (icon) DestroyIcon(icon);
    }
    s_instance = nullptr;
}

bool CDetailWindow::IsDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1, size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hKey);
        return val == 0;
    }
    return true;
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

void CDetailWindow::FormatSpeed(double bps, wchar_t* buf, int n) {
    if (bps < 0.01) wcsncpy_s(buf, n, L"0 B/s", _TRUNCATE);
    else if (bps < 1024) swprintf_s(buf, n, L"%.0f B/s", bps);
    else if (bps < 1048576) swprintf_s(buf, n, L"%.1f KB/s", bps / 1024.0);
    else swprintf_s(buf, n, L"%.2f MB/s", bps / 1048576.0);
}

// ============================================================
// Window creation
// ============================================================

bool CDetailWindow::Initialize(HINSTANCE hInst) {
    m_hinst = hInst;

    wchar_t className[64];
    swprintf_s(className, 64, L"PNMDetail_%p", (void*)this);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = m_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,  // no taskbar icon
        className, L"ProcessNetMonitor",
        WS_POPUP | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, MIN_WIDTH + 40, MIN_HEIGHT + 80,
        NULL, NULL, m_hinst, NULL);

    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)DetailStaticWndProc);

    // Rounded corners (Win11+)
    int corner_pref = 2;
    DwmSetWindowAttribute(m_hwnd, 33, &corner_pref, sizeof(corner_pref));

    return true;
}

void CDetailWindow::Show(HWND parent_wnd) {
    m_parent_wnd = parent_wnd;
    m_dark_mode = IsDarkMode();

    // Center on screen
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = min(720, sw - 100);
    int h = min(560, sh - 100);
    int x = (sw - w) / 2;
    int y = (sh - h) / 2;

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

void CDetailWindow::UpdateData(const std::vector<ProcTraffic>& stats, double total_up, double total_down) {
    m_total_up = total_up;
    m_total_down = total_down;
    m_cached_stats = stats;

    RebuildRows();
}

void CDetailWindow::RebuildRows() {
    auto& stats = m_cached_stats;

    // Save expanded PIDs before rebuild
    std::set<DWORD> expanded_pids;
    for (auto& r : m_rows) {
        if (r.expanded) expanded_pids.insert(r.pid);
    }

    // Build display rows from stats
    std::map<DWORD, DisplayRow> row_map;
    for (auto& st : stats) {
        // Tab 0 = 实时(only active), Tab 1 = 历史(all)
        if (m_active_tab == 0 && st.speed_up < 0.01 && st.speed_down < 0.01)
            continue;
        auto& r = row_map[st.pid];
        r.pid = st.pid;
        r.name = st.name;
        r.exe_path = st.exe_path;
        r.speed_up = st.speed_up;
        r.speed_down = st.speed_down;
        r.conn_count = st.conn_count;
        if (r.category.empty()) {
            std::wstring lower = st.name;
            for (auto& c : lower) c = towlower(c);
            if (lower.find(L"svchost") != std::wstring::npos ||
                lower.find(L"system") != std::wstring::npos ||
                lower.find(L"csrss") != std::wstring::npos ||
                lower.find(L"lsass") != std::wstring::npos)
                r.category = L"系统进程";
            else
                r.category = L"第三方程序";
        }
    }

    m_rows.clear();
    for (auto& [pid, r] : row_map) {
        if (expanded_pids.count(pid)) r.expanded = true;
        m_rows.push_back(std::move(r));
    }

    // Re-sort using current column/direction (no toggle)
    ResortRows();

    // Clamp scroll
    int max_scroll = (int)m_rows.size() - 1;
    if (m_scroll_pos > max_scroll) m_scroll_pos = max(0, max_scroll);

    if (m_visible) {
        InvalidateRect(m_hwnd, NULL, FALSE);
    }
}

// ============================================================
// Layout helpers
// ============================================================

int CDetailWindow::GetHeaderHeight() const { return TITLE_BAR_H; }
int CDetailWindow::GetTabsHeight() const { return TAB_BAR_H; }
int CDetailWindow::GetSummaryHeight() const { return SUMMARY_H; }
int CDetailWindow::GetTableHeaderHeight() const { return TABLE_HEADER_H; }
int CDetailWindow::GetRowHeight() const { return ROW_H; }

int CDetailWindow::GetTableAreaTop() const {
    return PADDING + GetHeaderHeight() + GetTabsHeight() + GetSummaryHeight() + GetTableHeaderHeight();
}

int CDetailWindow::GetVisibleRows(int client_h) const {
    int table_h = client_h - GetTableAreaTop() - PADDING;
    return max(1, table_h / GetRowHeight());
}

// ============================================================
// Sorting
// ============================================================

void CDetailWindow::ResortRows() {
    // Sort using current m_sort_col / m_sort_desc without toggling
    std::sort(m_rows.begin(), m_rows.end(), [&](const DisplayRow& a, const DisplayRow& b) {
        int cmp = 0;
        switch (m_sort_col) {
        case COL_NAME: cmp = _wcsicmp(a.name.c_str(), b.name.c_str()); break;
        case COL_CATEGORY: cmp = _wcsicmp(a.category.c_str(), b.category.c_str()); break;
        case COL_DOWN: cmp = (a.speed_down > b.speed_down) ? 1 : (a.speed_down < b.speed_down ? -1 : 0); break;
        case COL_UP: cmp = (a.speed_up > b.speed_up) ? 1 : (a.speed_up < b.speed_up ? -1 : 0); break;
        case COL_CONN: cmp = (a.conn_count > b.conn_count) ? 1 : (a.conn_count < b.conn_count ? -1 : 0); break;
        default: cmp = 0;
        }
        return m_sort_desc ? (cmp > 0) : (cmp < 0);
    });
}

void CDetailWindow::SortByColumn(int col) {
    if (m_sort_col == col) {
        m_sort_desc = !m_sort_desc;
    } else {
        m_sort_col = col;
        m_sort_desc = true;
    }
    ResortRows();
}

// ============================================================
// Hit testing
// ============================================================

int CDetailWindow::HitTestRow(int y) const {
    int table_top = GetTableAreaTop();
    if (y < table_top) return -1;
    int row_idx = (y - table_top) / GetRowHeight() + m_scroll_pos;
    if (row_idx < 0 || row_idx >= (int)m_rows.size()) return -1;
    return row_idx;
}

int CDetailWindow::HitTestColumn(int x) const {
    int cx = PADDING;
    for (int i = 0; i < 7; i++) {
        if (x >= cx && x < cx + m_columns[i].width) return i;
        cx += m_columns[i].width;
    }
    return -1;
}

void CDetailWindow::ToggleExpand(int row) {
    if (row < 0 || row >= (int)m_rows.size()) return;
    m_rows[row].expanded = !m_rows[row].expanded;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::ScrollTo(int pos) {
    int max_pos = max(0, (int)m_rows.size() - GetVisibleRows(GetSystemMetrics(SM_CYSCREEN)));
    m_scroll_pos = max(0, min(pos, max_pos));
    InvalidateRect(m_hwnd, NULL, FALSE);
}

// ============================================================
// Context menu
// ============================================================

void CDetailWindow::ShowContextMenu(int row, int x, int y) {
    if (row < 0 || row >= (int)m_rows.size()) return;
    auto& r = m_rows[row];

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1, L"定位文件");
    AppendMenuW(hMenu, MF_STRING, 2, L"文件属性");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 3, L"结束进程");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (r.speed_up + r.speed_down > 0.01 ? MF_ENABLED : MF_GRAYED), 4, L"查看详情");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, m_hwnd, NULL);
    DestroyMenu(hMenu);

    switch (cmd) {
    case 1: // 定位文件
        if (!r.exe_path.empty()) {
            wchar_t arg[MAX_PATH + 32];
            swprintf_s(arg, L"/select,\"%s\"", r.exe_path.c_str());
            ShellExecuteW(NULL, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
        }
        break;
    case 2: // 文件属性
        if (!r.exe_path.empty()) {
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.fMask = SEE_MASK_INVOKEIDLIST;
            sei.lpVerb = L"properties";
            sei.lpFile = r.exe_path.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
        }
        break;
    case 3: // 结束进程
        if (r.pid > 0) {
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, r.pid);
            if (hProc) {
                TerminateProcess(hProc, 1);
                CloseHandle(hProc);
            }
        }
        break;
    case 4: // 查看详情 (expand)
        ToggleExpand(row);
        break;
    }
}

// ============================================================
// Message handler
// ============================================================

LRESULT CDetailWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_LBUTTONDBLCLK:
        OnLButtonDblClk((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_RBUTTONDOWN:
        OnRButtonDown((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_MOUSEWHEEL:
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_VSCROLL:
        OnVScroll(LOWORD(wp));
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(m_hwnd, &pt);
        // Title bar area: allow drag
        if (pt.y < TITLE_BAR_H && pt.y >= 0) {
            if (PtInRect(&m_rcClose, pt)) return HTCLIENT;
            if (PtInRect(&m_rcMin, pt)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_CLOSE) { Hide(); return 0; }
        break;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Hide(); return 0; }
        break;

    default:
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }
    return DefWindowProcW(m_hwnd, msg, wp, lp);
}

void CDetailWindow::OnSize(int w, int h) {
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::OnLButtonDown(int x, int y) {
    // Close button
    if (PtInRect(&m_rcClose, { x, y })) {
        Hide();
        return;
    }
    // Minimize button
    if (PtInRect(&m_rcMin, { x, y })) {
        ShowWindow(m_hwnd, SW_MINIMIZE);
        return;
    }
    // Tabs
    if (PtInRect(&m_rcTab0, { x, y })) {
        if (m_active_tab != 0) { m_active_tab = 0; RebuildRows(); InvalidateRect(m_hwnd, NULL, FALSE); }
        return;
    }
    if (PtInRect(&m_rcTab1, { x, y })) {
        if (m_active_tab != 1) { m_active_tab = 1; RebuildRows(); InvalidateRect(m_hwnd, NULL, FALSE); }
        return;
    }

    // Table header click -> sort
    if (y >= PADDING + GetHeaderHeight() + GetTabsHeight() + GetSummaryHeight() &&
        y < PADDING + GetHeaderHeight() + GetTabsHeight() + GetSummaryHeight() + GetTableHeaderHeight()) {
        int col = HitTestColumn(x);
        if (col >= COL_NAME && col <= COL_CONN) {
            SortByColumn(col);
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
        return;
    }

    // Table row click -> select / expand
    int row = HitTestRow(y);
    if (row >= 0) {
        // Check if click is on expand arrow area (first 20px of row)
        int table_left = PADDING;
        if (x >= table_left && x < table_left + 20) {
            ToggleExpand(row);
        }
    }

    // Start drag for title bar
    if (y < TITLE_BAR_H) {
        ReleaseCapture();
        SendMessage(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
}

void CDetailWindow::OnLButtonDblClk(int x, int y) {
    int row = HitTestRow(y);
    if (row >= 0) {
        ToggleExpand(row);
    }
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
    if (!m_tracking_mouse) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        TrackMouseEvent(&tme);
        m_tracking_mouse = true;
    }

    // Update hover states
    bool old_close = m_hovering_close;
    bool old_min = m_hovering_min;
    int old_row = m_hovered_row;

    m_hovering_close = PtInRect(&m_rcClose, { x, y }) != 0;
    m_hovering_min = PtInRect(&m_rcMin, { x, y }) != 0;
    m_hovered_row = HitTestRow(y);

    if (m_hovering_close != old_close || m_hovering_min != old_min || m_hovered_row != old_row) {
        InvalidateRect(m_hwnd, NULL, FALSE);
    }

    // Change cursor for title bar buttons
    if (m_hovering_close || m_hovering_min) {
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
    ScrollTo(m_scroll_pos - (delta > 0 ? 3 : -3));
}

void CDetailWindow::OnVScroll(int code) {
    switch (code) {
    case SB_LINEUP: ScrollTo(m_scroll_pos - 1); break;
    case SB_LINEDOWN: ScrollTo(m_scroll_pos + 1); break;
    case SB_PAGEUP: ScrollTo(m_scroll_pos - GetVisibleRows(MIN_HEIGHT)); break;
    case SB_PAGEDOWN: ScrollTo(m_scroll_pos + GetVisibleRows(MIN_HEIGHT)); break;
    case SB_THUMBPOSITION: /* handled */ break;
    }
}

// ============================================================
// Drawing
// ============================================================

void CDetailWindow::DrawTitleBar(HDC hdc, int w) {
    RECT rc = { 0, 0, w, TITLE_BAR_H };
    HBRUSH hBrush = CreateSolidBrush(GetTitleBarColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    // Bottom border
    HPEN hPen = CreatePen(PS_SOLID, 1, GetBorderColor());
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, 0, TITLE_BAR_H - 1, NULL);
    LineTo(hdc, w, TITLE_BAR_H - 1);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);

    // App icon (shield-like)
    HICON hIcon = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, 16, 16, LR_SHARED);
    if (hIcon) {
        DrawIconEx(hdc, PADDING, (TITLE_BAR_H - 16) / 2, hIcon, 16, 16, 0, NULL, DI_NORMAL);
    }

    // Title text
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetTextColor());
    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    RECT title_rc = { PADDING + 22, 0, w - 100, TITLE_BAR_H };
    DrawTextW(hdc, L"ProcessNetMonitor - \u6D41\u91CF\u76D1\u63A7", -1, &title_rc,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Close button (X)
    int btn_size = 28;
    int btn_y = (TITLE_BAR_H - btn_size) / 2;
    m_rcClose = { w - btn_size - 4, btn_y, w - 4, btn_y + btn_size };
    SetTextColor(hdc, m_hovering_close ? RGB(255, 80, 80) : GetSecondaryTextColor());
    RECT close_rc = m_rcClose;
    DrawTextW(hdc, L"\u2715", -1, &close_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // Minimize button (-)
    m_rcMin = { w - btn_size * 2 - 4, btn_y, w - btn_size - 4, btn_y + btn_size };
    SetTextColor(hdc, m_hovering_min ? GetTextColor() : GetSecondaryTextColor());
    RECT min_rc = m_rcMin;
    DrawTextW(hdc, L"\u2013", -1, &min_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

void CDetailWindow::DrawTabs(HDC hdc, int w, int y) {
    RECT rc = { 0, y, w, y + TAB_BAR_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    // Tab 0: 实时流量
    SIZE sz;
    const wchar_t* tab0 = L"\u5B9E\u65F6\u6D41\u91CF";
    GetTextExtentPoint32W(hdc, tab0, (int)wcslen(tab0), &sz);
    m_rcTab0 = { PADDING, y + 4, PADDING + sz.cx + 16, y + TAB_BAR_H - 4 };
    SetTextColor(hdc, m_active_tab == 0 ? GetAccentColor(false) : GetSecondaryTextColor());
    RECT t0rc = m_rcTab0;
    DrawTextW(hdc, tab0, -1, &t0rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (m_active_tab == 0) {
        HPEN hPen = CreatePen(PS_SOLID, 2, GetAccentColor(false));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, m_rcTab0.left + 2, y + TAB_BAR_H - 4, NULL);
        LineTo(hdc, m_rcTab0.right - 2, y + TAB_BAR_H - 4);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    // Tab 1: 历史流量
    const wchar_t* tab1 = L"\u5386\u53F2\u6D41\u91CF";
    GetTextExtentPoint32W(hdc, tab1, (int)wcslen(tab1), &sz);
    m_rcTab1 = { m_rcTab0.right + 12, y + 4, m_rcTab0.right + 12 + sz.cx + 16, y + TAB_BAR_H - 4 };
    SetTextColor(hdc, m_active_tab == 1 ? GetAccentColor(false) : GetSecondaryTextColor());
    RECT t1rc = m_rcTab1;
    DrawTextW(hdc, tab1, -1, &t1rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (m_active_tab == 1) {
        HPEN hPen = CreatePen(PS_SOLID, 2, GetAccentColor(false));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, m_rcTab1.left + 2, y + TAB_BAR_H - 4, NULL);
        LineTo(hdc, m_rcTab1.right - 2, y + TAB_BAR_H - 4);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

void CDetailWindow::DrawSpeedSummary(HDC hdc, int w, int y) {
    RECT rc = { 0, y, w, y + SUMMARY_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    wchar_t down_str[32], up_str[32];
    FormatSpeed(m_total_down, down_str, 32);
    FormatSpeed(m_total_up, up_str, 32);

    // Download total
    int x = PADDING;
    SetTextColor(hdc, GetSecondaryTextColor());
    RECT label1 = { x, y, x + 80, y + SUMMARY_H };
    DrawTextW(hdc, L"\u4E0B\u8F7D\u603B\u901F\u5EA6\uFF1A", -1, &label1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    x += 80;

    SetTextColor(hdc, GetAccentColor(false));
    RECT val1 = { x, y, x + 100, y + SUMMARY_H };
    wchar_t down_full[64];
    swprintf_s(down_full, 64, L"\u2193 %s", down_str);
    DrawTextW(hdc, down_full, -1, &val1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    x += 120;

    // Upload total
    SetTextColor(hdc, GetSecondaryTextColor());
    RECT label2 = { x, y, x + 80, y + SUMMARY_H };
    DrawTextW(hdc, L"\u4E0A\u4F20\u603B\u901F\u5EA6\uFF1A", -1, &label2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    x += 80;

    SetTextColor(hdc, GetAccentColor(true));
    RECT val2 = { x, y, x + 100, y + SUMMARY_H };
    wchar_t up_full[64];
    swprintf_s(up_full, 64, L"\u2191 %s", up_str);
    DrawTextW(hdc, up_full, -1, &val2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
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

    HFONT hFont = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSecondaryTextColor());

    int cx = PADDING;
    for (int i = 0; i < 7; i++) {
        RECT rc = { cx + 4, y, cx + m_columns[i].width - 4, y + TABLE_HEADER_H };
        UINT fmt = DT_SINGLELINE | DT_VCENTER;
        switch (m_columns[i].align) {
        case Column::LEFT: fmt |= DT_LEFT; break;
        case Column::RIGHT: fmt |= DT_RIGHT; break;
        case Column::CENTER: fmt |= DT_CENTER; break;
        }
        DrawTextW(hdc, m_columns[i].title, -1, &rc, fmt);

        // Sort indicator
        if (i == m_sort_col && i >= COL_NAME && i <= COL_CONN) {
            SetTextColor(hdc, GetAccentColor(false));
            const wchar_t* arrow = m_sort_desc ? L" \u25BC" : L" \u25B2";
            RECT arrow_rc = { rc.right - 16, y, rc.right + 4, y + TABLE_HEADER_H };
            DrawTextW(hdc, arrow, -1, &arrow_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(hdc, GetSecondaryTextColor());
        }

        cx += m_columns[i].width;
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

void CDetailWindow::DrawTableRows(HDC hdc, int w, int y, int client_h) {
    int visible = GetVisibleRows(client_h);
    int row_h = GetRowHeight();

    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hSmallFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    int table_bottom = client_h - PADDING;

    for (int vi = 0; vi < visible && m_scroll_pos + vi < (int)m_rows.size(); vi++) {
        int ri = m_scroll_pos + vi;
        auto& row = m_rows[ri];
        int ry = y + vi * row_h;
        if (ry + row_h > table_bottom) break;

        // Row background
        bool hovered = (ri == m_hovered_row);
        HBRUSH hBrush = CreateSolidBrush(GetRowBgColor(ri, hovered));
        RECT row_rc = { PADDING, ry, w - PADDING, ry + row_h };
        FillRect(hdc, &row_rc, hBrush);
        DeleteObject(hBrush);

        // Bottom border
        HPEN hPen = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, PADDING, ry + row_h - 1, NULL);
        LineTo(hdc, w - PADDING, ry + row_h - 1);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);

        int cx = PADDING;
        int text_y = ry;

        // 0. Icon + expand arrow
        {
            // Expand arrow (if row has connections concept)
            SelectObject(hdc, hSmallFont);
            SetTextColor(hdc, GetSecondaryTextColor());
            const wchar_t* arrow = row.expanded ? L"\u25BC" : L"\u25B6";
            RECT arrow_rc = { cx, text_y, cx + 14, text_y + row_h };
            DrawTextW(hdc, arrow, -1, &arrow_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

            // Process icon
            HICON hIcon = GetProcessIcon(row.exe_path);
            if (hIcon) {
                DrawIconEx(hdc, cx + 16, text_y + (row_h - ICON_SIZE) / 2,
                           hIcon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
            }
        }
        cx += m_columns[0].width;

        // 1. Name
        SelectObject(hdc, hFont);
        SetTextColor(hdc, GetTextColor());
        {
            std::wstring display = row.name;
            // Remove .exe
            if (display.size() > 4) {
                std::wstring ext = display.substr(display.size() - 4);
                if (ext == L".exe" || ext == L".EXE")
                    display = display.substr(0, display.size() - 4);
            }
            // Truncate if needed
            SIZE sz;
            int max_w = m_columns[1].width - 8;
            GetTextExtentPoint32W(hdc, display.c_str(), (int)display.size(), &sz);
            while ((int)display.size() > 3 && sz.cx > max_w) {
                display = display.substr(0, display.size() - 1);
                display.back() = L'\u2026';
                GetTextExtentPoint32W(hdc, display.c_str(), (int)display.size(), &sz);
            }
            RECT name_rc = { cx + 4, text_y, cx + m_columns[1].width - 4, text_y + row_h };
            DrawTextW(hdc, display.c_str(), -1, &name_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += m_columns[1].width;

        // 2. Category
        SetTextColor(hdc, GetSecondaryTextColor());
        {
            RECT cat_rc = { cx + 4, text_y, cx + m_columns[2].width - 4, text_y + row_h };
            DrawTextW(hdc, row.category.c_str(), -1, &cat_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += m_columns[2].width;

        // 3. Download speed
        {
            wchar_t spd[32];
            FormatSpeed(row.speed_down, spd, 32);
            SetTextColor(hdc, row.speed_down > 0.01 ? GetAccentColor(false) : GetSecondaryTextColor());
            RECT spd_rc = { cx + 4, text_y, cx + m_columns[3].width - 4, text_y + row_h };
            DrawTextW(hdc, spd, -1, &spd_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += m_columns[3].width;

        // 4. Upload speed
        {
            wchar_t spd[32];
            FormatSpeed(row.speed_up, spd, 32);
            SetTextColor(hdc, row.speed_up > 0.01 ? GetAccentColor(true) : GetSecondaryTextColor());
            RECT spd_rc = { cx + 4, text_y, cx + m_columns[4].width - 4, text_y + row_h };
            DrawTextW(hdc, spd, -1, &spd_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += m_columns[4].width;

        // 5. Connection count
        {
            wchar_t conn[16];
            swprintf_s(conn, L"%d", row.conn_count);
            SetTextColor(hdc, row.conn_count > 0 ? GetAccentColor(true) : GetSecondaryTextColor());
            RECT conn_rc = { cx + 4, text_y, cx + m_columns[5].width - 4, text_y + row_h };
            DrawTextW(hdc, conn, -1, &conn_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
        cx += m_columns[5].width;

        // 6. Action
        {
            SetTextColor(hdc, m_dark_mode ? RGB(255, 165, 0) : RGB(230, 126, 34));
            SelectObject(hdc, hSmallFont);
            RECT act_rc = { cx + 4, text_y, cx + m_columns[6].width - 4, text_y + row_h };
            DrawTextW(hdc, L"\u67E5\u770B\u8FDE\u63A5", -1, &act_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        // Draw expanded children
        if (row.expanded) {
            int child_y = ry + row_h;
            // Show PID info as first child
            SelectObject(hdc, hSmallFont);
            SetTextColor(hdc, GetSecondaryTextColor());

            // PID row
            RECT pid_rc = { PADDING + 28, child_y, w - PADDING, child_y + CHILD_ROW_H };
            wchar_t pid_text[64];
            swprintf_s(pid_text, 64, L"\u8FDB\u7A0BID: %u", row.pid);
            FillRect(hdc, &pid_rc, CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247)));
            DrawTextW(hdc, pid_text, -1, &pid_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            // Separator
            HPEN hPen2 = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
            HPEN hOldPen2 = (HPEN)SelectObject(hdc, hPen2);
            MoveToEx(hdc, PADDING, child_y + CHILD_ROW_H - 1, NULL);
            LineTo(hdc, w - PADDING, child_y + CHILD_ROW_H - 1);
            SelectObject(hdc, hOldPen2);
            DeleteObject(hPen2);

            child_y += CHILD_ROW_H;

            // Path row
            if (!row.exe_path.empty()) {
                RECT path_rc = { PADDING + 28, child_y, w - PADDING, child_y + CHILD_ROW_H };
                HBRUSH hBg = CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247));
                FillRect(hdc, &path_rc, hBg);
                DeleteObject(hBg);

                std::wstring path_display = row.exe_path;
                SIZE sz;
                int max_path_w = w - PADDING - 28 - PADDING;
                GetTextExtentPoint32W(hdc, path_display.c_str(), (int)path_display.size(), &sz);
                while ((int)path_display.size() > 5 && sz.cx > max_path_w) {
                    path_display = L"\u2026" + path_display.substr(4);
                    GetTextExtentPoint32W(hdc, path_display.c_str(), (int)path_display.size(), &sz);
                }
                DrawTextW(hdc, path_display.c_str(), -1, &path_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                HPEN hPen3 = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
                HPEN hOldPen3 = (HPEN)SelectObject(hdc, hPen3);
                MoveToEx(hdc, PADDING, child_y + CHILD_ROW_H - 1, NULL);
                LineTo(hdc, w - PADDING, child_y + CHILD_ROW_H - 1);
                SelectObject(hdc, hOldPen3);
                DeleteObject(hPen3);

                child_y += CHILD_ROW_H;
            }

            // Adjust: we need to skip extra rows in the visible count
            // This is handled by checking row height dynamically - for now, simple approach
        }
    }

    // No data message
    if (m_rows.empty()) {
        SetTextColor(hdc, GetSecondaryTextColor());
        RECT rc = { PADDING, y, w - PADDING, y + 60 };
        DrawTextW(hdc, L"\u65E0\u7F51\u7EDC\u6D41\u91CF\u6570\u636E\n\u8BF7\u786E\u4FDD\u4EE5\u7BA1\u7406\u5458\u6743\u9650\u8FD0\u884C TrafficMonitor",
                  -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
    DeleteObject(hSmallFont);
}

void CDetailWindow::DrawScrollbar(HDC hdc, int w, int h) {
    if ((int)m_rows.size() <= GetVisibleRows(h)) return;

    int track_x = w - SCROLL_W - 2;
    int track_top = GetTableAreaTop();
    int track_bottom = h - PADDING;
    int track_h = track_bottom - track_top;

    // Track
    HBRUSH hTrack = CreateSolidBrush(m_dark_mode ? RGB(45, 45, 50) : RGB(230, 230, 230));
    RECT track_rc = { track_x, track_top, track_x + SCROLL_W, track_bottom };
    FillRect(hdc, &track_rc, hTrack);
    DeleteObject(hTrack);

    // Thumb
    int total_rows = (int)m_rows.size();
    int visible_rows = GetVisibleRows(h);
    float ratio = (float)visible_rows / total_rows;
    int thumb_h = max(20, (int)(track_h * ratio));
    float scroll_ratio = (float)m_scroll_pos / max(1, total_rows - visible_rows);
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

    // Double buffer
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background
    HBRUSH hBg = CreateSolidBrush(GetBgColor());
    FillRect(memDC, &client, hBg);
    DeleteObject(hBg);

    int y = PADDING;

    // Title bar
    DrawTitleBar(memDC, w);
    y += GetHeaderHeight();

    // Tabs
    DrawTabs(memDC, w, y);
    y += GetTabsHeight();

    // Speed summary
    DrawSpeedSummary(memDC, w, y);
    y += GetSummaryHeight();

    // Table header
    DrawTableHeader(memDC, w, y);
    y += GetTableHeaderHeight();

    // Table rows
    DrawTableRows(memDC, w, y, h);

    // Scrollbar
    DrawScrollbar(memDC, w, h);

    // Blit
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(m_hwnd, &ps);
}
