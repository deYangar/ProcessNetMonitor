#include "plugin_main.h"
#include "utils.h"
#include <algorithm>
#include <shellapi.h>

// From tooltip_popup.cpp DllMain - DLL's HINSTANCE
extern HINSTANCE s_dll_hinst;

CProcessNetPlugin CProcessNetPlugin::s_instance;
wchar_t CProcessNetItem::s_value_buf[2][256] = { L"starting...", L"starting..." };
int CProcessNetItem::s_transparent_width = 100;  // default 100px

static void FmtSpeed(double bps, wchar_t* buf, int n) {
    FormatSpeed(bps, buf, n);
}

const wchar_t* CProcessNetItem::GetItemName() const {
    if (m_dir == DIR_TRANSPARENT) return L"\u900F\u660E\u533A\u57DF";
    return m_dir == DIR_UPLOAD ? L"Up" : L"Down";
}
const wchar_t* CProcessNetItem::GetItemId() const {
    if (m_dir == DIR_TRANSPARENT) return L"TransparentArea";
    return m_dir == DIR_UPLOAD ? L"SpdUp01" : L"SpdDn01";
}
const wchar_t* CProcessNetItem::GetItemLableText() const {
    return L"";
}
const wchar_t* CProcessNetItem::GetItemValueText() const {
    if (m_dir == DIR_TRANSPARENT) return L"";
    return s_value_buf[m_dir];
}
const wchar_t* CProcessNetItem::GetItemValueSampleText() const {
    if (m_dir == DIR_TRANSPARENT) return L"\u900F\u660E\u533A\u57DF";
    return m_dir == DIR_UPLOAD ? L"U:chrome.exe 5.6KB/s" : L"D:mihomo 1.4KB/s";
}

int CProcessNetItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) {
    auto& plugin = CProcessNetPlugin::Instance();
    bool is_taskbar = (flag & MF_TASKBAR_WND) != 0;

    if (type == MT_LCLICKED) {
        if (is_taskbar) {
            // Taskbar click: toggle tooltip popup at click position
            POINT pt = { x, y };
            ClientToScreen((HWND)hWnd, &pt);
            plugin.m_popup.ToggleAtPosition(pt.x, pt.y, plugin.m_cached_up, plugin.m_cached_down);
            plugin.SetPopupClickTime(GetTickCount64());
        } else {
            // Main window click: toggle detail window
            plugin.ToggleDetailWindow((HWND)hWnd);
        }
        return 1;
    }
    if (type == MT_RCLICKED) {
        // Right-click on taskbar: hide popup
        if (is_taskbar) {
            plugin.m_popup.Hide();
        }
        return 1;
    }
    if (type == MT_DBCLICKED) {
        // Double-click always opens detail window
        plugin.ToggleDetailWindow((HWND)hWnd);
        return 1;
    }
    return 0;
}

void CProcessNetItem::Update(const std::vector<ProcTraffic>& stats, double sys_up, double sys_down) {
    const double EMA_ALPHA = 0.3;

    for (auto& [pid, rp] : m_recent) rp.idle_rounds++;
    for (const auto& st : stats) {
        auto& rp = m_recent[st.pid];
        rp.name = st.name;
        if (rp.idle_rounds == 0 || rp.ema_up == 0) {
            rp.ema_up = st.speed_up;
            rp.ema_down = st.speed_down;
        } else {
            rp.ema_up = EMA_ALPHA * st.speed_up + (1.0 - EMA_ALPHA) * rp.ema_up;
            rp.ema_down = EMA_ALPHA * st.speed_down + (1.0 - EMA_ALPHA) * rp.ema_down;
        }
        rp.speed_up = st.speed_up;
        rp.speed_down = st.speed_down;
        rp.idle_rounds = 0;
    }
    for (auto& [pid, rp] : m_recent) {
        if (rp.idle_rounds > 0) {
            rp.ema_up *= (1.0 - EMA_ALPHA);
            rp.ema_down *= (1.0 - EMA_ALPHA);
        }
    }
    for (auto it = m_recent.begin(); it != m_recent.end(); ) {
        if (it->second.idle_rounds > MAX_IDLE_ROUNDS) it = m_recent.erase(it); else ++it;
    }

    std::vector<RecentProc*> list;
    for (auto& [pid, rp] : m_recent) {
        list.push_back(&rp);
    }
    if (m_dir == DIR_UPLOAD)
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) {
            double sa = (a->speed_up > 0.01) ? a->speed_up : a->ema_up * 0.5;
            double sb = (b->speed_up > 0.01) ? b->speed_up : b->ema_up * 0.5;
            return sa > sb;
        });
    else
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) {
            double sa = (a->speed_down > 0.01) ? a->speed_down : a->ema_down * 0.5;
            double sb = (b->speed_down > 0.01) ? b->speed_down : b->ema_down * 0.5;
            return sa > sb;
        });

    double sys_spd = (m_dir == DIR_UPLOAD) ? sys_up : sys_down;
    wchar_t proc_name[16] = L"-";
    wchar_t proc_str[32];
    if (!list.empty()) {
        double spd = (m_dir == DIR_UPLOAD) ? list[0]->speed_up : list[0]->speed_down;
        const auto& n = list[0]->name;
        wcsncpy_s(proc_name, 16, n.c_str(), _TRUNCATE);
        if (n.size() > 12) { proc_name[10] = L'.'; proc_name[11] = L'.'; proc_name[12] = 0; }
        FmtSpeed(spd, proc_str, 32);
    } else {
        FmtSpeed(0, proc_str, 32);
    }

    wchar_t prefix = (m_dir == DIR_UPLOAD) ? L'U' : L'D';
    swprintf_s(s_value_buf[m_dir], 256, L"%c:%s %s", prefix, proc_name, proc_str);
}

CProcessNetPlugin& CProcessNetPlugin::Instance() { return s_instance; }
IPluginItem* CProcessNetPlugin::GetItem(int i) {
    if (i >= 0 && i <= 2) return &m_items[i];
    return nullptr;
}

void CProcessNetPlugin::DataRequired() {
    if (!m_started) {
        m_items[0].Init(CProcessNetItem::DIR_UPLOAD);
        m_items[1].Init(CProcessNetItem::DIR_DOWNLOAD);
        m_items[2].Init(CProcessNetItem::DIR_TRANSPARENT);
        m_started = m_capture.Start();
        m_last_time = GetTickCount64();
        if (!m_started) {
            swprintf_s(CProcessNetItem::s_value_buf[0], 256, L"ERR: %s", m_capture.GetLastError());
            swprintf_s(CProcessNetItem::s_value_buf[1], 256, L"ERR: %s", m_capture.GetLastError());
        }
        return;
    }

    ULONGLONG now = GetTickCount64();
    double dt = (double)(now - m_last_time) / 1000.0;
    if (dt < 0.1) dt = 0.1;
    m_last_time = now;

    auto stats = m_capture.GetStats(dt);
    double su = 0, sd = 0;
    for (auto& s : stats) { su += s.speed_up; sd += s.speed_down; }

    m_items[0].Update(stats, su, sd);
    m_items[1].Update(stats, su, sd);

    m_cached_stats = stats;
    m_cached_up = su;
    m_cached_down = sd;

    // Update detail window (always, for history recording)
    if (m_detail_created) {
        m_detail.UpdateData(stats, su, sd);
    }

    // Build tooltip text (TM's default text tooltip, kept as fallback)
    wchar_t line[256];
    wcscpy_s(m_tooltip, L"Process Net Monitor\n");
    swprintf_s(line, 256, L"Total: U:%.1fKB/s D:%.1fKB/s\n", su/1024.0, sd/1024.0);
    wcscat_s(m_tooltip, line);

    wcscat_s(m_tooltip, L"\n--- Upload ---\n");
    std::vector<RecentProc*> up_list, down_list;
    for (auto& [pid, rp] : m_items[0].m_recent) {
        if (rp.speed_up > 0.01 || rp.idle_rounds == 0) up_list.push_back(&rp);
    }
    for (auto& [pid, rp] : m_items[1].m_recent) {
        if (rp.speed_down > 0.01 || rp.idle_rounds == 0) down_list.push_back(&rp);
    }
    std::sort(up_list.begin(), up_list.end(), [](auto* a, auto* b) { return a->speed_up > b->speed_up; });
    std::sort(down_list.begin(), down_list.end(), [](auto* a, auto* b) { return a->speed_down > b->speed_down; });

    int count = 0;
    for (auto* rp : up_list) {
        if (count >= 5) break;
        wchar_t spd[32]; FmtSpeed(rp->speed_up, spd, 32);
        swprintf_s(line, 256, L"  %-14s %s\n", rp->name.c_str(), spd);
        wcscat_s(m_tooltip, line); count++;
    }
    while (count < 5) { wcscat_s(m_tooltip, L"  -\n"); count++; }

    wcscat_s(m_tooltip, L"\n--- Download ---\n");
    count = 0;
    for (auto* rp : down_list) {
        if (count >= 5) break;
        wchar_t spd[32]; FmtSpeed(rp->speed_down, spd, 32);
        swprintf_s(line, 256, L"  %-14s %s\n", rp->name.c_str(), spd);
        wcscat_s(m_tooltip, line); count++;
    }
    while (count < 5) { wcscat_s(m_tooltip, L"  -\n"); count++; }

    CheckHoverAndShowPopup();
}

const wchar_t* CProcessNetPlugin::GetInfo(PluginInfoIndex i) {
    switch (i) {
    case TMI_NAME: return L"ProcessNetMonitor";
    case TMI_DESCRIPTION: return L"Per-process network speed";
    case TMI_AUTHOR: return L"Aemeath";
    case TMI_COPYRIGHT: return L"MIT";
    case TMI_VERSION: return L"1.8.0";
    case TMI_URL: return L"https://github.com";
    default: return L"";
    }
}

const wchar_t* CProcessNetPlugin::GetTooltipInfo() { return m_tooltip; }

void CProcessNetPlugin::OnInitialize(ITrafficMonitor* p) {
    m_app = p;
    // Use DLL's HINSTANCE (not EXE's) so resource loading (icons, etc.) works
    HINSTANCE hInst = s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(NULL);
    m_popup_created = m_popup.Initialize(hInst);
    m_detail_created = m_detail.Initialize(hInst);
    
    // Set PacketCapture pointer for connection details
    m_detail.SetCapture(&m_capture);

    // Use TM's plugin config dir + plugin name (GetPluginConfigDir returns the plugins/ folder)
    const wchar_t* cfg_base = p->GetPluginConfigDir();
    if (cfg_base && cfg_base[0]) {
        std::wstring cfg_dir = std::wstring(cfg_base) + L"\\ProcessNetMonitor";
        m_detail.SetConfigDir(cfg_dir.c_str());
    }
    m_detail.LoadHistory();
    m_detail.LoadSettings();
    // Sync transparent width from settings to static member
    CProcessNetItem::s_transparent_width = m_detail.GetTransparentWidth();
    // Pass TUN ranges from settings to capture
    m_capture.SetTunRanges(m_detail.GetTunRanges());
    // Record history whenever GetStats is called (independent of detail panel)
    m_capture.SetOnStats([this](const std::vector<ProcTraffic>& stats) {
        m_detail.RecordHistory(stats);
    });
}

void CProcessNetPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {
    if (index == EI_CONFIG_DIR && data && data[0]) {
        m_tm_config_dir = data;
        m_capture.SetTMConfigDir(m_tm_config_dir);
    }
}

// ============================================================
// Hover detection & popup management
// ============================================================

// Check if a TrafficMonitor window is the floating main window (not on taskbar)
static bool IsTMMainWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return false;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (!state) return true;

    RECT tb = abd.rc;
    switch (abd.uEdge) {
    case ABE_BOTTOM: return rc.top < tb.top;
    case ABE_TOP:    return rc.bottom > tb.bottom;
    case ABE_LEFT:   return rc.right > tb.right;
    case ABE_RIGHT:  return rc.left < tb.left;
    }
    return true;
}

// Check if a TrafficMonitor window is embedded in the taskbar
static bool IsTMTaskbarWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return false;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (!state) return false;  // can't determine taskbar position

    // Check if window overlaps with taskbar
    RECT tb = abd.rc;
    RECT inter;
    return IntersectRect(&inter, &rc, &tb) != 0;
}

// Walk the parent chain to find a TrafficMonitor window
static HWND FindTMWindowInChain(HWND hwnd) {
    for (HWND cur = hwnd; cur; cur = GetParent(cur)) {
        wchar_t cls[64] = {};
        GetClassNameW(cur, cls, 64);
        if (wcsncmp(cls, L"TrafficMonitor", 14) == 0)
            return cur;
    }
    return nullptr;
}

// EnumWindows callback to find all TM windows
struct FindTMWindowsCtx {
    HWND main_wnd = nullptr;   // floating
    HWND taskbar_wnd = nullptr; // in taskbar
};
static BOOL CALLBACK FindTMWindowsProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return TRUE;
    auto* ctx = reinterpret_cast<FindTMWindowsCtx*>(lp);
    if (IsTMMainWindow(hwnd)) ctx->main_wnd = hwnd;
    else if (IsTMTaskbarWindow(hwnd)) ctx->taskbar_wnd = hwnd;
    return TRUE;
}

// Get the TM floating window (not on taskbar)
// Note: the hovered window itself must also be outside the taskbar,
// because TM creates #32770 dialog windows in the taskbar area
// whose parent is the main TM window.
static HWND GetTMMainWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    // Check if the hovered window is in the taskbar
    RECT rc;
    GetWindowRect(hwnd, &rc);
    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (state) {
        RECT inter;
        if (IntersectRect(&inter, &rc, &abd.rc))
            return nullptr;  // hovered window is in taskbar area
    }
    HWND tm = FindTMWindowInChain(hwnd);
    if (tm && IsTMMainWindow(tm)) return tm;
    return nullptr;
}

// Get the TM taskbar window by checking if mouse is within its rect
static HWND GetTMTaskbarByPoint(const POINT& pt) {
    FindTMWindowsCtx ctx;
    EnumWindows(FindTMWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.taskbar_wnd) return nullptr;
    RECT rc;
    GetWindowRect(ctx.taskbar_wnd, &rc);
    if (PtInRect(&rc, pt)) return ctx.taskbar_wnd;
    return nullptr;
}

void CProcessNetPlugin::ToggleDetailWindow(HWND parent_wnd) {
    if (!m_detail_created) return;
    if (m_detail.IsVisible())
        m_detail.Hide();
    else
        m_detail.Show(parent_wnd);
}

void CProcessNetPlugin::GetProcessDisplayInfo(
        std::vector<CTooltipPopup::ProcDisplayInfo>& out,
        const std::vector<ProcTraffic>& stats) {
    std::unordered_map<DWORD, CTooltipPopup::ProcDisplayInfo> merged;

    for (auto& [pid, rp] : m_items[0].m_recent) {
        auto& d = merged[pid];
        d.name = rp.name;
        d.speed_up = rp.speed_up;
    }
    for (auto& [pid, rp] : m_items[1].m_recent) {
        auto& d = merged[pid];
        d.name = rp.name;
        d.speed_down = rp.speed_down;
    }
    for (auto& st : stats) {
        auto it = merged.find(st.pid);
        if (it != merged.end() && !st.exe_path.empty()) {
            it->second.exe_path = st.exe_path;
        }
    }

    out.clear();
    for (auto& [pid, d] : merged) {
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
        double sa = (a.speed_up + a.speed_down > 0.01) ? (a.speed_up + a.speed_down) : 0;
        double sb = (b.speed_up + b.speed_down > 0.01) ? (b.speed_up + b.speed_down) : 0;
        return sa > sb;
    });

    if (out.size() > CTooltipPopup::MAX_SHOW)
        out.resize(CTooltipPopup::MAX_SHOW);
}

void CProcessNetPlugin::CheckHoverAndShowPopup() {
    if (!m_popup_created) return;

    ULONGLONG now = GetTickCount64();
    int interval = m_was_hovering ? 80 : 300;
    if (now - m_last_hover_check < (ULONGLONG)interval) return;
    m_last_hover_check = now;

    POINT pt;
    GetCursorPos(&pt);
    HWND hover_wnd = WindowFromPoint(pt);

    bool over_popup = (hover_wnd == m_popup.GetHwnd());

    // Only detect hover on the main floating window
    // (taskbar uses click-based popup via OnMouseEvent instead)
    HWND tm_wnd = GetTMMainWindow(hover_wnd);

    // Debug logging
    static FILE* s_dbg2 = nullptr;
    if (!s_dbg2) s_dbg2 = _wfopen(L"C:\\Users\\Public\\Temp\\pnm_hover2.log", L"w");
    if (s_dbg2) {
        wchar_t cls[64] = {};
        GetClassNameW(hover_wnd, cls, 64);
        RECT rc; GetWindowRect(hover_wnd, &rc);
        wchar_t tm_cls[64] = {};
        if (tm_wnd) GetClassNameW(tm_wnd, tm_cls, 64);
        fwprintf(s_dbg2, L"hover=%p cls=%s rect=(%d,%d,%d,%d) tm_wnd=%p tm_cls=%s over_popup=%d\n",
            hover_wnd, cls, rc.left, rc.top, rc.right, rc.bottom,
            tm_wnd, tm_cls, over_popup);
        fflush(s_dbg2);
    }

    // Check if mouse is over the taskbar area
    bool over_taskbar = false;
    {
        APPBARDATA abd = { sizeof(abd) };
        UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
        if (state) over_taskbar = PtInRect(&abd.rc, pt) != 0;
    }

    if (tm_wnd) {
        RECT wnd_rect;
        GetWindowRect(tm_wnd, &wnd_rect);
        std::vector<CTooltipPopup::ProcDisplayInfo> procs;
        GetProcessDisplayInfo(procs, m_cached_stats);
        m_popup.UpdateAndShow(procs, m_cached_up, m_cached_down, wnd_rect, false);
        m_was_hovering = true;
    } else if (over_popup) {
        m_was_hovering = true;
    } else if (over_taskbar && m_popup.IsVisible()) {
        // Click-shown popup: keep visible while mouse is on the taskbar
        m_was_hovering = true;
    } else {
        // Mouse left both popup and taskbar — hide
        bool click_grace = (now - m_popup_click_time < 800);
        if ((m_was_hovering || m_popup.IsVisible()) && !click_grace) {
            m_popup.Hide();
            m_was_hovering = false;
        }
    }
}

// ============================================================
// Options dialog - TUN address range settings
// ============================================================

static std::vector<std::wstring> g_new_ranges;  // temp storage for dialog result
static bool g_option_changed = false;

static LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // TUN label
        CreateWindowW(L"STATIC", L"TUN \x5730\x5740\x6BB5\xFF08\x6BCF\x884C\x4E00\x4E2A CIDR\xFF09:",
            WS_CHILD | WS_VISIBLE, 10, 10, 360, 20, hwnd, (HMENU)1000, nullptr, nullptr);
        // TUN edit (multiline)
        CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL,
            10, 35, 360, 80, hwnd, (HMENU)1001, nullptr, nullptr);

        // Transparent area width label
        CreateWindowW(L"STATIC", L"\x900F\x660E\x533A\x57DF\x5BBD\x5EA6\xFF08px\xFF0C\x4EFB\x52A1\x680F\x663E\x793A\x533A\x57DF\xFF09:",
            WS_CHILD | WS_VISIBLE, 10, 125, 250, 20, hwnd, (HMENU)1002, nullptr, nullptr);
        // Width edit
        wchar_t width_buf[16];
        swprintf_s(width_buf, L"%d", CProcessNetItem::s_transparent_width);
        CreateWindowW(L"EDIT", width_buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            260, 123, 110, 24, hwnd, (HMENU)1003, nullptr, nullptr);

        // OK button
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 230, 165, 65, 24, hwnd, (HMENU)IDOK, nullptr, nullptr);
        // Cancel button
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 305, 165, 65, 24, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        
        // Fill edit with current ranges
        auto ranges = CProcessNetPlugin::Instance().m_detail.GetTunRanges();
        std::wstring text;
        for (size_t i = 0; i < ranges.size(); i++) {
            if (i > 0) text += L"\r\n";
            text += ranges[i];
        }
        SetDlgItemTextW(hwnd, 1001, text.c_str());
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            // Save TUN ranges
            wchar_t buf[2048] = {};
            GetDlgItemTextW(hwnd, 1001, buf, 2048);
            std::vector<std::wstring> new_ranges;
            wchar_t* ctx = nullptr;
            wchar_t* line = wcstok_s(buf, L"\r\n", &ctx);
            while (line) {
                std::wstring trimmed = line;
                while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(0, 1);
                while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
                if (!trimmed.empty()) new_ranges.push_back(trimmed);
                line = wcstok_s(nullptr, L"\r\n", &ctx);
            }
            // Save transparent width
            wchar_t wbuf[16] = {};
            GetDlgItemTextW(hwnd, 1003, wbuf, 16);
            int new_width = _wtoi(wbuf);
            if (new_width < 0) new_width = 0;
            if (new_width > 500) new_width = 500;
            CProcessNetItem::s_transparent_width = new_width;

            auto& plugin = CProcessNetPlugin::Instance();
            plugin.m_detail.SetTunRanges(new_ranges);
            plugin.m_detail.SetTransparentWidth(new_width);
            plugin.m_detail.SaveSettings();
            plugin.m_capture.SetTunRanges(new_ranges);
            g_option_changed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            g_option_changed = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ITMPlugin::OptionReturn CProcessNetPlugin::ShowOptionsDialog(void* hParent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = OptionsWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ProcessNetMonitorOptionsDlg";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    
    g_option_changed = false;
    
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"ProcessNetMonitorOptionsDlg",
        L"\x63D2\x4EF6\x8BBE\x7F6E",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 395, 245,
        (HWND)hParent, nullptr, GetModuleHandleW(NULL), nullptr
    );
    
    if (!hwnd) return OR_OPTION_NOT_PROVIDED;
    
    // Center on parent
    if (hParent) {
        RECT rcParent, rcDlg;
        GetWindowRect((HWND)hParent, &rcParent);
        GetWindowRect(hwnd, &rcDlg);
        int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
        int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    } else {
        // Center on screen
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        SetWindowPos(hwnd, nullptr, (sw - (rc.right - rc.left)) / 2, (sh - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    
    // Modal loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    
    return g_option_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

extern "C" {
    __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
        return &CProcessNetPlugin::Instance();
    }
}
