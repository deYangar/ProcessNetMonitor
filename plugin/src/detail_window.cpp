#include "detail_window.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <set>

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

CDetailWindow::CDetailWindow() {
    s_instance = this;
    m_dark_mode = IsDarkMode();
    m_history_start_tick = GetTickCount64();
}

CDetailWindow::~CDetailWindow() {
    SaveHistory();
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

void CDetailWindow::FormatBytes(uint64_t bytes, wchar_t* buf, int n) {
    if (bytes == 0) wcsncpy_s(buf, n, L"0 B", _TRUNCATE);
    else if (bytes < 1024ULL) swprintf_s(buf, n, L"%llu B", bytes);
    else if (bytes < 1048576ULL) swprintf_s(buf, n, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824ULL) swprintf_s(buf, n, L"%.2f MB", bytes / 1048576.0);
    else swprintf_s(buf, n, L"%.2f GB", bytes / 1073741824.0);
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
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        className, L"ProcessNetMonitor",
        WS_POPUP | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, MIN_WIDTH + 40, MIN_HEIGHT + 80,
        NULL, NULL, m_hinst, NULL);

    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)DetailStaticWndProc);

    int corner_pref = 2;
    DwmSetWindowAttribute(m_hwnd, 33, &corner_pref, sizeof(corner_pref));

    return true;
}

void CDetailWindow::Show(HWND parent_wnd) {
    m_parent_wnd = parent_wnd;
    m_dark_mode = IsDarkMode();

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

// ============================================================
// History recording
// ============================================================

void CDetailWindow::RecordHistory(const std::vector<ProcTraffic>& stats) {
    ULONGLONG now = GetTickCount64();
    for (auto& st : stats) {
        auto& hist = m_history[st.name];
        if (hist.name.empty()) { hist.name = st.name; hist.exe_path = st.exe_path; }
        if (hist.exe_path.empty() && !st.exe_path.empty()) hist.exe_path = st.exe_path;
        HistorySnapshot snap = { now, st.bytes_recv, st.bytes_sent };
        hist.raw.push_back(snap);
        while ((int)hist.raw.size() > MAX_RAW) hist.raw.pop_front();
    }
    static int compress_counter = 0;
    if (++compress_counter >= 60) { compress_counter = 0; CompressHistory(); }
}

void CDetailWindow::CompressHistory() {
    ULONGLONG now = GetTickCount64();
    for (auto& [name, h] : m_history) {
        ULONGLONG min_cutoff = now - 3600000ULL;
        while (!h.raw.empty() && h.raw.front().tick < min_cutoff) {
            h.min.push_back(h.raw.front()); h.raw.pop_front();
        }
        while ((int)h.min.size() > MAX_MIN) h.min.pop_front();
        ULONGLONG hour_cutoff = now - 86400000ULL;
        while (!h.min.empty() && h.min.front().tick < hour_cutoff) {
            h.hour.push_back(h.min.front()); h.min.pop_front();
        }
        while ((int)h.hour.size() > MAX_HOUR) h.hour.pop_front();
        ULONGLONG day_cutoff = now - 604800000ULL;
        while (!h.hour.empty() && h.hour.front().tick < day_cutoff) {
            h.day.push_back(h.hour.front()); h.hour.pop_front();
        }
        while ((int)h.day.size() > MAX_DAY) h.day.pop_front();
    }
}

void CDetailWindow::GetMergedSnapshots(const ProcessHistory& h,
    std::vector<const HistorySnapshot*>& out, ULONGLONG cutoff) const {
    for (auto& s : h.day)  { if (s.tick >= cutoff) out.push_back(&s); }
    for (auto& s : h.hour) { if (s.tick >= cutoff) out.push_back(&s); }
    for (auto& s : h.min)  { if (s.tick >= cutoff) out.push_back(&s); }
    for (auto& s : h.raw)  { if (s.tick >= cutoff) out.push_back(&s); }
}

void CDetailWindow::BuildHistoryRows() {
    m_rows.clear();
    m_hist_total_recv = 0;
    m_hist_total_sent = 0;

    ULONGLONG now = GetTickCount64();
    ULONGLONG range_ms = 0;
    switch (m_time_range) {
    case TR_24H: range_ms = 24ULL * 3600 * 1000; break;
    case TR_3D:  range_ms = 3ULL * 24 * 3600 * 1000; break;
    case TR_7D:  range_ms = 7ULL * 24 * 3600 * 1000; break;
    case TR_30D: range_ms = 30ULL * 24 * 3600 * 1000; break;
    }
    ULONGLONG cutoff = (now > range_ms) ? (now - range_ms) : 0;
    if (cutoff < m_history_start_tick) cutoff = m_history_start_tick;

    for (auto& [name, hist] : m_history) {
        std::vector<const HistorySnapshot*> snaps;
        GetMergedSnapshots(hist, snaps, cutoff);
        if (snaps.size() < 2) continue;

        const HistorySnapshot* first = snaps.front();
        const HistorySnapshot* last = snaps.back();
        uint64_t recv_delta = last->cum_recv - first->cum_recv;
        uint64_t sent_delta = last->cum_sent - first->cum_sent;
        double elapsed_sec = (double)(last->tick - first->tick) / 1000.0;
        if (elapsed_sec < 1.0) elapsed_sec = 1.0;

        m_hist_total_recv += recv_delta;
        m_hist_total_sent += sent_delta;

        DisplayRow row;
        row.name = hist.name;
        row.exe_path = hist.exe_path;
        row.hist_recv = recv_delta;
        row.hist_sent = sent_delta;
        row.hist_avg_down = (double)recv_delta / elapsed_sec;
        row.hist_avg_up = (double)sent_delta / elapsed_sec;

        std::wstring lower = hist.name;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(L"svchost") != std::wstring::npos ||
            lower.find(L"system") != std::wstring::npos ||
            lower.find(L"csrss") != std::wstring::npos ||
            lower.find(L"lsass") != std::wstring::npos)
            row.category = L"\u7CFB\u7EDF\u8FDB\u7A0B";
        else
            row.category = L"\u7B2C\u4E09\u65B9\u7A0B\u5E8F";

        for (auto& old : m_rows) {
            if (old.name == name && old.expanded) { row.expanded = true; break; }
        }
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
static bool ReadDQ(FILE* f, std::deque<CDetailWindow::HistorySnapshot>& dq, int max_n, ULONGLONG max_age) {
    uint32_t n; if (fread(&n, 4, 1, f) != 1 || n > (uint32_t)max_n + 100) return false;
    ULONGLONG now = GetTickCount64();
    for (uint32_t i = 0; i < n; i++) {
        CDetailWindow::HistorySnapshot s;
        if (fread(&s.tick, sizeof(ULONGLONG), 1, f) != 1) return false;
        if (fread(&s.cum_recv, sizeof(uint64_t), 1, f) != 1) return false;
        if (fread(&s.cum_sent, sizeof(uint64_t), 1, f) != 1) return false;
        if (max_age > 0 && now - s.tick > max_age) continue;
        dq.push_back(s);
    }
    return true;
}

void CDetailWindow::SaveHistory() {
    if (m_config_dir.empty()) return;
    CreateDirectoryW(m_config_dir.c_str(), NULL);
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\history.dat", m_config_dir.c_str());
    FILE* f = _wfopen(path, L"wb");
    if (!f) return;

    uint32_t magic = 0x504E4D48, version = 2, count = (uint32_t)m_history.size();
    fwrite(&magic, 4, 1, f); fwrite(&version, 4, 1, f); fwrite(&count, 4, 1, f);
    for (auto& [name, hist] : m_history) {
        uint32_t nl = (uint32_t)name.size(); fwrite(&nl, 4, 1, f); fwrite(name.c_str(), sizeof(wchar_t), nl, f);
        uint32_t pl = (uint32_t)hist.exe_path.size(); fwrite(&pl, 4, 1, f); fwrite(hist.exe_path.c_str(), sizeof(wchar_t), pl, f);
        WriteDQ(f, hist.raw); WriteDQ(f, hist.min); WriteDQ(f, hist.hour); WriteDQ(f, hist.day);
    }
    fclose(f);
}

void CDetailWindow::LoadHistory() {
    if (m_config_dir.empty()) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\history.dat", m_config_dir.c_str());
    FILE* f = _wfopen(path, L"rb");
    if (!f) return;
    uint32_t magic, version, count;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x504E4D48) { fclose(f); return; }
    if (fread(&version, 4, 1, f) != 1) { fclose(f); return; }
    if (fread(&count, 4, 1, f) != 1) { fclose(f); return; }
    ULONGLONG max_age = 365ULL * 24 * 3600 * 1000;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nl; if (fread(&nl, 4, 1, f) != 1 || nl > 512) break;
        std::wstring name(nl, L'\0'); if (fread(&name[0], sizeof(wchar_t), nl, f) != nl) break;
        uint32_t pl; if (fread(&pl, 4, 1, f) != 1 || pl > MAX_PATH) break;
        std::wstring exe_path(pl, L'\0'); if (fread(&exe_path[0], sizeof(wchar_t), pl, f) != pl) break;
        auto& hist = m_history[name]; hist.name = name; hist.exe_path = exe_path;
        if (version >= 2) {
            if (!ReadDQ(f, hist.raw, MAX_RAW, max_age)) break;
            if (!ReadDQ(f, hist.min, MAX_MIN, max_age)) break;
            if (!ReadDQ(f, hist.hour, MAX_HOUR, max_age)) break;
            if (!ReadDQ(f, hist.day, MAX_DAY, max_age)) break;
        } else {
            if (!ReadDQ(f, hist.raw, MAX_RAW, max_age)) break;
        }
    }
    fclose(f);
}

// ============================================================
// Data update
// ============================================================

void CDetailWindow::UpdateData(const std::vector<ProcTraffic>& stats, double total_up, double total_down) {
    m_total_up = total_up;
    m_total_down = total_down;

    // Always record history
    RecordHistory(stats);

    // Auto-save every 60 seconds
    ULONGLONG now = GetTickCount64();
    if (now - m_last_save_tick > 60000) {
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
        // History tab
        BuildHistoryRows();
        ResortRows();
        int max_scroll = (int)m_rows.size() - 1;
        if (m_scroll_pos > max_scroll) m_scroll_pos = max(0, max_scroll);
        if (m_visible) InvalidateRect(m_hwnd, NULL, FALSE);
        return;
    }

    // Tab 0: real-time - show ALL processes with connections or traffic
    std::set<DWORD> expanded_pids;
    for (auto& r : m_rows) {
        if (r.expanded) expanded_pids.insert(r.pid);
    }

    m_rows.clear();
    for (auto& st : stats) {
        DisplayRow row;
        row.pid = st.pid;
        row.name = st.name;
        row.exe_path = st.exe_path;
        row.speed_up = st.speed_up;
        row.speed_down = st.speed_down;
        row.conn_count = st.conn_count;
        // Category
        std::wstring lower = st.name;
        for (auto& c : lower) c = towlower(c);
        if (lower.find(L"svchost") != std::wstring::npos ||
            lower.find(L"system") != std::wstring::npos ||
            lower.find(L"csrss") != std::wstring::npos ||
            lower.find(L"lsass") != std::wstring::npos)
            row.category = L"\u7CFB\u7EDF\u8FDB\u7A0B";
        else
            row.category = L"\u7B2C\u4E09\u65B9\u7A0B\u5E8F";

        if (expanded_pids.count(st.pid)) row.expanded = true;
        m_rows.push_back(row);
    }

    ResortRows();

    int max_scroll = (int)m_rows.size() - 1;
    if (m_scroll_pos > max_scroll) m_scroll_pos = max(0, max_scroll);
    if (m_visible) InvalidateRect(m_hwnd, NULL, FALSE);
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
    bool sd = m_sort_desc[m_active_tab];
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
        return sd ? (cmp > 0) : (cmp < 0);
    });
}

void CDetailWindow::SortByColumn(int col) {
    if (m_sort_col[m_active_tab] == col) {
        m_sort_desc[m_active_tab] = !m_sort_desc[m_active_tab];
    } else {
        m_sort_col[m_active_tab] = col;
        m_sort_desc[m_active_tab] = true;
    }
    ResortRows();
}

// ============================================================
// Hit testing
// ============================================================

int CDetailWindow::HitTestRow(int y) const {
    int table_top = GetTableAreaTop();
    if (y < table_top) return -1;
    int cur_y = table_top;
    int row_h = GetRowHeight();
    for (int i = m_scroll_pos; i < (int)m_rows.size(); i++) {
        int this_h = row_h;
        if (m_rows[i].expanded) {
            this_h += CHILD_ROW_H;
            if (!m_rows[i].exe_path.empty()) this_h += CHILD_ROW_H;
        }
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
    m_rows[row].expanded = !m_rows[row].expanded;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::ScrollTo(int pos) {
    int max_pos = max(0, (int)m_rows.size() - 10);
    m_scroll_pos = max(0, min(pos, max_pos));
    InvalidateRect(m_hwnd, NULL, FALSE);
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
    AppendMenuW(hMenu, MF_STRING, 1, L"\u5B9A\u4F4D\u6587\u4EF6");
    AppendMenuW(hMenu, MF_STRING, 2, L"\u6587\u4EF6\u5C5E\u6027");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, 3, L"\u7ED3\u675F\u8FDB\u7A0B");

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
    case WM_LBUTTONDBLCLK: OnLButtonDblClk((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_RBUTTONDOWN: OnRButtonDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_MOUSEMOVE: OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_MOUSELEAVE: OnMouseLeave(); return 0;
    case WM_MOUSEWHEEL: OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_VSCROLL: OnVScroll(LOWORD(wp)); return 0;
    case WM_ERASEBKGND: return 1;

    case WM_NCHITTEST: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(m_hwnd, &pt);
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
    if (PtInRect(&m_rcClose, { x, y })) { Hide(); return; }
    if (PtInRect(&m_rcMin, { x, y })) { ShowWindow(m_hwnd, SW_MINIMIZE); return; }

    // Tabs
    if (PtInRect(&m_rcTab0, { x, y })) {
        if (m_active_tab != 0) { m_active_tab = 0; m_scroll_pos = 0; RebuildRows(); }
        return;
    }
    if (PtInRect(&m_rcTab1, { x, y })) {
        if (m_active_tab != 1) { m_active_tab = 1; m_scroll_pos = 0; RebuildRows(); }
        return;
    }

    // Time range buttons (history tab only)
    if (m_active_tab == 1) {
        for (int i = 0; i < 4; i++) {
            if (PtInRect(&m_rcTRButtons[i], { x, y })) {
                if (m_time_range != (TimeRange)i) {
                    m_time_range = (TimeRange)i;
                    m_scroll_pos = 0;
                    RebuildRows();
                }
                return;
            }
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
    case SB_PAGEUP: ScrollTo(m_scroll_pos - 10); break;
    case SB_PAGEDOWN: ScrollTo(m_scroll_pos + 10); break;
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

    HPEN hPen = CreatePen(PS_SOLID, 1, GetBorderColor());
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, 0, TITLE_BAR_H - 1, NULL);
    LineTo(hdc, w, TITLE_BAR_H - 1);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);

    HICON hIcon = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, 16, 16, LR_SHARED);
    if (hIcon) DrawIconEx(hdc, PADDING, (TITLE_BAR_H - 16) / 2, hIcon, 16, 16, 0, NULL, DI_NORMAL);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetTextColor());
    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    RECT title_rc = { PADDING + 22, 0, w - 100, TITLE_BAR_H };
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

    SIZE sz;
    const wchar_t* tab0 = L"\u5B9E\u65F6\u6D41\u91CF";
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

    const wchar_t* tab1 = L"\u5386\u53F2\u6D41\u91CF";
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
    DeleteObject(hFont);
}

void CDetailWindow::DrawTimeRangeButtons(HDC hdc, int w, int y) {
    if (m_active_tab != 1) return;

    RECT rc = { 0, y, w, y + TIME_RANGE_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);

    const wchar_t* labels[4] = { L"24\u5C0F\u65F6", L"3\u5929", L"7\u5929", L"30\u5929" };
    int x = PADDING;
    for (int i = 0; i < 4; i++) {
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

    int x = PADDING;

    if (m_active_tab == 1) {
        // History: show total traffic for selected range
        wchar_t recv_str[32], sent_str[32];
        FormatBytes(m_hist_total_recv, recv_str, 32);
        FormatBytes(m_hist_total_sent, sent_str, 32);

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l1 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, L"\u603B\u4E0B\u8F7D\uFF1A", -1, &l1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 55;
        SetTextColor(hdc, GetAccentColor(false));
        RECT v1 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, recv_str, -1, &v1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 110;

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l2 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, L"\u603B\u4E0A\u4F20\uFF1A", -1, &l2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
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
        DrawTextW(hdc, L"\u4E0B\u8F7D\u603B\u901F\u5EA6\uFF1A", -1, &l1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 80;
        SetTextColor(hdc, GetAccentColor(false));
        wchar_t d_full[64]; swprintf_s(d_full, 64, L"\u2193 %s", down_str);
        RECT v1 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, d_full, -1, &v1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 120;

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT l2 = { x, y, x + 80, y + SUMMARY_H };
        DrawTextW(hdc, L"\u4E0A\u4F20\u603B\u901F\u5EA6\uFF1A", -1, &l2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += 80;
        SetTextColor(hdc, GetAccentColor(true));
        wchar_t u_full[64]; swprintf_s(u_full, 64, L"\u2191 %s", up_str);
        RECT v2 = { x, y, x + 100, y + SUMMARY_H };
        DrawTextW(hdc, u_full, -1, &v2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }

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
            const wchar_t* arrow = m_sort_desc[m_active_tab] ? L" \u25BC" : L" \u25B2";
            RECT arrow_rc = { rc.right - 16, y, rc.right + 4, y + TABLE_HEADER_H };
            DrawTextW(hdc, arrow, -1, &arrow_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SetTextColor(hdc, GetSecondaryTextColor());
        }

        cx += cols[i].width;
    }

    SelectObject(hdc, hOldFont);
    DeleteObject(hFont);
}

void CDetailWindow::DrawTableRows(HDC hdc, int w, int y, int client_h) {
    int row_h = GetRowHeight();

    HFONT hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hSmallFont = CreateFontW(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    Column* cols = GetActiveCols();

    int table_bottom = client_h - PADDING;
    bool is_hist = (m_active_tab == 1);

    // Empty state
    if (m_rows.empty()) {
        SetTextColor(hdc, GetSecondaryTextColor());
        RECT rc = { PADDING, y, w - PADDING, y + 60 };
        const wchar_t* msg = is_hist
            ? L"\u6682\u65E0\u5386\u53F2\u6570\u636E\n\u4F7F\u7528\u4E00\u6BB5\u65F6\u95F4\u540E\u8FD9\u91CC\u5C06\u663E\u793A\u6D41\u91CF\u7EDF\u8BA1"
            : L"\u65E0\u6D3B\u8DC3\u8FDB\u7A0B\n\u6B63\u5728\u76D1\u63A7\u7F51\u7EDC\u6D41\u91CF...";
        DrawTextW(hdc, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        DeleteObject(hSmallFont);
        return;
    }

    // Draw rows with dynamic height (expanded rows take more space)
    int cur_y = y;
    for (int ri = m_scroll_pos; ri < (int)m_rows.size(); ri++) {
        if (cur_y >= table_bottom) break;

        auto& row = m_rows[ri];
        // Calculate this row's total height (parent + children if expanded)
        int this_row_h = row_h;
        if (row.expanded) {
            this_row_h += CHILD_ROW_H; // PID row
            if (!row.exe_path.empty()) this_row_h += CHILD_ROW_H; // path row
        }

        // Skip if this row is entirely above visible area
        if (cur_y + this_row_h < y) {
            cur_y += this_row_h;
            continue;
        }

        int ry = cur_y;

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

        // Col 0: Icon + expand arrow
        SelectObject(hdc, hSmallFont);
        SetTextColor(hdc, GetSecondaryTextColor());
        const wchar_t* arrow = row.expanded ? L"\u25BC" : L"\u25B6";
        RECT arrow_rc = { cx, ry, cx + 14, ry + row_h };
        DrawTextW(hdc, arrow, -1, &arrow_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        HICON hIcon = GetProcessIcon(row.exe_path);
        if (hIcon) DrawIconEx(hdc, cx + 16, ry + (row_h - ICON_SIZE) / 2, hIcon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
        cx += 32;

        // Col 1: Name
        SelectObject(hdc, hFont);
        SetTextColor(hdc, GetTextColor());
        {
            std::wstring display = row.name;
            if (display.size() > 4) {
                std::wstring ext = display.substr(display.size() - 4);
                if (ext == L".exe" || ext == L".EXE")
                    display = display.substr(0, display.size() - 4);
            }
            SIZE sz;
            int max_w = 180 - 8;
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
            SelectObject(hdc, hSmallFont);
            RECT r6 = { cx + 4, ry, cx + cols[6].width - 4, ry + row_h };
            DrawTextW(hdc, L"\u00B7\u00B7\u00B7", -1, &r6, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }

        // Expanded children
        if (row.expanded) {
            int child_y = ry + row_h;
            SelectObject(hdc, hSmallFont);
            SetTextColor(hdc, GetSecondaryTextColor());

            // PID row
            RECT pid_rc = { PADDING + 28, child_y, w - PADDING, child_y + CHILD_ROW_H };
            HBRUSH hCbg = CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247));
            FillRect(hdc, &pid_rc, hCbg);
            DeleteObject(hCbg);
            wchar_t pid_text[64];
            if (row.pid > 0)
                swprintf_s(pid_text, 64, L"\u8FDB\u7A0BID: %u", row.pid);
            else
                swprintf_s(pid_text, 64, L"\u5386\u53F2\u8BB0\u5F55");
            DrawTextW(hdc, pid_text, -1, &pid_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

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
                HBRUSH hPbg = CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247));
                FillRect(hdc, &path_rc, hPbg);
                DeleteObject(hPbg);

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
            }
        }

        cur_y += this_row_h;
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

    HBRUSH hTrack = CreateSolidBrush(m_dark_mode ? RGB(45, 45, 50) : RGB(230, 230, 230));
    RECT track_rc = { track_x, track_top, track_x + SCROLL_W, track_bottom };
    FillRect(hdc, &track_rc, hTrack);
    DeleteObject(hTrack);

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
