#include "tooltip_popup.h"
#include "plugin_main.h"  
#include "utils.h"        
#include "signature.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")

CTooltipPopup* CTooltipPopup::s_instance = nullptr;
HINSTANCE s_dll_hinst = nullptr;
volatile bool g_shutting_down = false;

void SafeDestroyWindow(HWND hwnd) {
    if (!hwnd) return;
    // Detach IME first so third-party IME hooks (e.g. Sogou) don't run inside
    // the destroy callback, then guard the destruction with SEH as a last
    // resort (issue #7).
    ImmAssociateContext(hwnd, nullptr);
    __try {
        DestroyWindow(hwnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ============================================================
// Helper: find TrafficMonitor's native tooltip windows
// ============================================================

// TrafficMonitor uses MFC's CToolTipCtrl, which creates top-level windows of
// class "tooltips_class32".  Enumerate the visible ones that belong to the
// same process as the anchor TM window.  If the anchor handle is missing we
// still collect all visible tooltips so the popup can avoid them.
static void FindTMTooltips(HWND anchor_hwnd, std::vector<RECT>& out) {
    out.clear();
    DWORD tm_pid = 0;
    if (anchor_hwnd && IsWindow(anchor_hwnd))
        GetWindowThreadProcessId(anchor_hwnd, &tm_pid);

    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"tooltips_class32", nullptr)) != nullptr) {
        if (tm_pid != 0) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != tm_pid) continue;
        }
        if (!IsWindowVisible(hwnd)) continue;
        RECT rc;
        GetWindowRect(hwnd, &rc);
        if (rc.left >= rc.right || rc.top >= rc.bottom) continue;
        out.push_back(rc);
    }
}

static RECT InflateRectScreen(const RECT& rc, int dx, int dy) {
    return { rc.left - dx, rc.top - dy, rc.right + dx, rc.bottom + dy };
}

static bool RectsIntersect(const RECT& a, const RECT& b) {
    return a.left < b.right && a.right > b.left &&
           a.top < b.bottom && a.bottom > b.top;
}

// Standalone WndProc to avoid member function pointer ABI issues
static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (CTooltipPopup::s_instance)
        return CTooltipPopup::s_instance->HandleMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Capture DLL instance handle for window class registration
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        s_dll_hinst = (HINSTANCE)hModule;
    } else if (reason == DLL_PROCESS_DETACH) {
        // Mark shutdown BEFORE doing anything else: window destructors that
        // run later (CRT static teardown) must not DestroyWindow through
        // synchronous callbacks (third-party IME hooks can crash the host,
        // issue #7).
        g_shutting_down = true;
        // Save history on DLL unload (destructor may not run)
        if (CDetailWindow::s_instance) {
            CDetailWindow::s_instance->SaveHistory();
        }
        // Stop ETW capture cleanly
        CProcessNetPlugin::Instance().StopEtwCapture();
        // Stop the signature verification worker (file IO must not outlive us)
        SignatureCache::Instance().Shutdown();
    }
    return TRUE;
}

// ============================================================
// Helper: extract icon from exe path
// ============================================================

HICON CTooltipPopup::GetProcessIcon(const std::wstring& exe_path) {
    if (exe_path.empty()) return nullptr;

    auto it = m_icon_cache.find(exe_path);
    if (it != m_icon_cache.end()) return it->second;

    HICON hIcon = nullptr;

    // Try SHGetFileInfo for exe icon
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(exe_path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        hIcon = sfi.hIcon;
    }

    if (!hIcon) {
        // Fallback: try ExtractAssociatedIcon
        wchar_t path_buf[MAX_PATH] = {};
        wcsncpy_s(path_buf, MAX_PATH, exe_path.c_str(), _TRUNCATE);
        WORD idx = 0;
        hIcon = ExtractAssociatedIconW(m_hinst, path_buf, &idx);
    }

    m_icon_cache[exe_path] = hIcon;
    return hIcon;
}

// ============================================================
// Construction
// ============================================================

CTooltipPopup::CTooltipPopup() {
    s_instance = this;
    m_dark_mode = IsDarkMode();
    m_dark_mode_tick = GetTickCount64();
}

CTooltipPopup::~CTooltipPopup() {
    if (m_hwnd) {
        if (g_shutting_down) {
            // DLL unload / process exit: skip DestroyWindow. It fires
            // synchronous window-proc/IME callbacks that can crash the host
            // during teardown (issue #7); the window dies with the process.
            m_hwnd = nullptr;
        } else {
            SafeDestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }
    for (auto& [path, icon] : m_icon_cache) {
        if (icon) DestroyIcon(icon);
    }
    if (m_font_normal) DeleteObject(m_font_normal);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_pen_separator) DeleteObject(m_pen_separator);
    s_instance = nullptr;
}

bool CTooltipPopup::IsDarkMode() {
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

// ============================================================
// Window creation & message loop
// ============================================================

bool CTooltipPopup::Initialize(HINSTANCE hInst) {
    // Use DLL's HINSTANCE captured in DllMain
    m_hinst = s_dll_hinst ? s_dll_hinst : hInst;

    // Use DefWindowProcW for initial creation, then subclass
    wchar_t className[64];
    swprintf_s(className, 64, L"PNMTooltip_%p", m_hinst);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = m_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        className, L"",
        WS_POPUP,
        0, 0, 300, 400,
        NULL, NULL, m_hinst, NULL);

    if (!m_hwnd) {
        return false;
    }

    // Subclass: replace the WndProc with ours
    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)StaticWndProc);

    // Enable rounded corners via DWM (Windows 11+)
    int corner_pref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                          &corner_pref, sizeof(corner_pref));

    // Set initial window region for rounded corners (will be updated on resize)
    HRGN rgn = CreateRoundRectRgn(0, 0, 300, 400, CORNER_RADIUS * 2, CORNER_RADIUS * 2);
    SetWindowRgn(m_hwnd, rgn, FALSE);

    // Pre-create cached GDI objects (recreated on DPI change via RecreateScaledResources)
    RecreateScaledResources();

    // Hover state machine: 100ms timer dispatched through TM's message loop
    // (this window lives on TM's main thread)
    SetTimer(m_hwnd, TIMER_HOVER, 100, nullptr);

    return true;
}

LRESULT CTooltipPopup::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_HOVER)
            CProcessNetPlugin::Instance().HoverTick();
        return 0;

    case WM_PNM_REFRESH:
        // pull latest snapshot on the UI thread (posted by refresh timer)
        CProcessNetPlugin::Instance().PopupRefreshFromSnapshot();
        return 0;

    case WM_PNM_LANG_CHANGED:
        // 运行期语言变化：刷新快照 + 重绘（文本在绘制时 TR 实时取词）
        CProcessNetPlugin::Instance().PopupRefreshFromSnapshot();
        InvalidateRect(m_hwnd, NULL, FALSE);
        return 0;

    case WM_DPICHANGED: {
        RECT* prc = (RECT*)lp;
        if (prc) UpdateDpiScale(*prc);
        InvalidateRect(m_hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_MOUSEMOVE:
        if (!m_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
            TrackMouseEvent(&tme);
            m_tracking = true;
        }
        m_hovering = true;
        return 0;

    case WM_LBUTTONDOWN:
        // Only open detail window when clicking the "查看详细" button
        {
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            if (PtInRect(&m_rcDetailBtn, pt)) {
                auto& plugin = CProcessNetPlugin::Instance();
                plugin.m_popup_pinned = false;
                plugin.ToggleDetailWindow(m_hwnd);
                Hide();
            }
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; // prevent flicker

    case WM_NCHITTEST:
        return HTCLIENT;

    default:
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }
}

void CTooltipPopup::OnMouseLeave() {
    m_tracking = false;
    m_hovering = false;
    // Don't hide immediately - the hover timer handles hide delays
}

// ============================================================
// Colors & theming
// ============================================================

COLORREF CTooltipPopup::GetBgColor() {
    return m_dark_mode ? RGB(32, 32, 32) : RGB(249, 249, 249);
}

COLORREF CTooltipPopup::GetTextColor() {
    return m_dark_mode ? RGB(240, 240, 240) : RGB(30, 30, 30);
}

COLORREF CTooltipPopup::GetAccentColor(bool is_upload) {
    if (is_upload) return m_dark_mode ? RGB(255, 165, 0) : RGB(230, 126, 34);  // orange
    return m_dark_mode ? RGB(80, 200, 120) : RGB(46, 160, 67);                // green
}

COLORREF CTooltipPopup::GetSectionBgColor() {
    return m_dark_mode ? RGB(44, 44, 44) : RGB(238, 238, 238);
}

// FormatSpeed delegated to shared utils.h

// ============================================================
// Layout calculation
// ============================================================

void CTooltipPopup::CalcLayout(HDC hdc, int& out_w, int& out_h, int& out_up_count, int& out_down_count) {
    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_normal);

    int max_text_w = 0;

    // Count displayable processes per section (direction-specific)
    int up_count = 0, down_count = 0;
    for (size_t i = 0; i < m_procs.size(); i++) {
        if (m_procs[i].speed_up > 0.01 && up_count < MAX_SHOW) up_count++;
        if (m_procs[i].speed_down > 0.01 && down_count < MAX_SHOW) down_count++;
    }
    // Fill remaining slots with historical processes
    int up_total = min((int)m_procs.size(), MAX_SHOW);
    int down_total = min((int)m_procs.size(), MAX_SHOW);
    up_count = max(up_count, min(up_total, MAX_SHOW));
    down_count = max(down_count, min(down_total, MAX_SHOW));
    if (up_count == 0) up_count = 1;
    if (down_count == 0) down_count = 1;

    out_up_count = up_count;
    out_down_count = down_count;

    // Measure text widths
    for (size_t i = 0; i < m_procs.size() && i < MAX_SHOW; i++) {
        SIZE sz;
        GetTextExtentPoint32W(hdc, m_procs[i].name.c_str(), (int)m_procs[i].name.size(), &sz);
        max_text_w = max(max_text_w, sz.cx);

        wchar_t spd[64];
        FormatSpeed(max(m_procs[i].speed_up, m_procs[i].speed_down), spd, 64);
        GetTextExtentPoint32W(hdc, spd, (int)wcslen(spd), &sz);
        max_text_w = max(max_text_w, sz.cx);
    }

    SelectObject(hdc, hOldFont);

    // Fixed width: long process names are truncated with an ellipsis instead
    // of stretching the window (user request 2026-08-04).
    // Name column: ~20 chars at 100% DPI (SunloginClient fits, longer names
    // get ellipsized by DrawProcRow's truncation logic).
    const int NAME_COL_W = (int)(105 * m_dpi_scale);
    out_w = PADDING + ICON_SIZE + (int)(6 * m_dpi_scale) + NAME_COL_W
            + (int)(12 * m_dpi_scale) + SPEED_AREA_W + PADDING;
    out_w = max(out_w, MIN_WIDTH);

    out_h = PADDING + HEADER_H
            + (m_status.empty() ? 0 : STATUS_H + 2)
            + SECTION_H + up_count * ROW_HEIGHT + GAP_H
            + SECTION_H + down_count * ROW_HEIGHT
            + BTN_AREA_H
            + PADDING;
}

// ============================================================
// Rendering
// ============================================================

void CTooltipPopup::DrawBackground(HDC hdc, int w, int h) {
    // Fill with rounded rectangle
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    HPEN hPen = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(55, 55, 55) : RGB(220, 220, 220));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

    RoundRect(hdc, 0, 0, w, h, CORNER_RADIUS * 2, CORNER_RADIUS * 2);

    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void CTooltipPopup::DrawSectionTitle(HDC hdc, int y, const wchar_t* title, bool is_upload) {
    // Section title bar with slightly different bg
    HBRUSH hBrush = CreateSolidBrush(GetSectionBgColor());
    int bar_h = (int)(20 * m_dpi_scale);
    RECT rc = { PADDING - 2, y, 0, y + bar_h }; // will set right later
    // We'll draw a small rounded rect behind the title text
    SIZE sz;
    GetTextExtentPoint32W(hdc, title, (int)wcslen(title), &sz);
    rc.right = PADDING + sz.cx + 12;
    // Actually let's just fill a full-width bar
    // Get client rect
    RECT client;
    GetClientRect(m_hwnd, &client);
    rc.right = client.right - PADDING + 2;

    // Use round rect for section bg
    int radius = max(2, (int)(6 * m_dpi_scale));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
    HPEN hNullPen = CreatePen(PS_SOLID, 0, GetSectionBgColor());
    HPEN hOldPen = (HPEN)SelectObject(hdc, hNullPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom + 2, radius, radius);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hNullPen);

    // Draw title text with accent color
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetAccentColor(is_upload));

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_small);

    RECT text_rc = { PADDING + 2, y, client.right - PADDING, y + (int)(22 * m_dpi_scale) };
    DrawTextW(hdc, title, -1, &text_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hOldFont);
}

void CTooltipPopup::DrawProcRow(HDC hdc, int y, const ProcDisplayInfo& proc, bool is_upload) {
    RECT client;
    GetClientRect(m_hwnd, &client);

    int x = PADDING + 2;

    // 1. Process icon
    HICON hIcon = GetProcessIcon(proc.exe_path);
    if (hIcon) {
        DrawIconEx(hdc, x, y + (ROW_HEIGHT - ICON_SIZE) / 2, hIcon,
                   ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);
    }
    x += ICON_SIZE + 6;

    // 2. Process name
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetTextColor());

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_normal);

    // Truncate name if too long
    std::wstring display_name = proc.name;
    // Remove .exe for cleaner display
    if (display_name.size() > 4) {
        std::wstring ext = display_name.substr(display_name.size() - 4);
        if (ext == L".exe" || ext == L".EXE") {
            display_name = display_name.substr(0, display_name.size() - 4);
        }
    }

    SIZE name_sz;
    GetTextExtentPoint32W(hdc, display_name.c_str(), (int)display_name.size(), &name_sz);
    int max_name_w = client.right - PADDING - SPEED_AREA_W - (int)(10 * m_dpi_scale) - x; // leave room for speed
    if (name_sz.cx > max_name_w) {
        // Truncate with ellipsis
        while (display_name.size() > 3 && name_sz.cx > max_name_w) {
            display_name = display_name.substr(0, display_name.size() - 1);
            display_name.back() = L'\u2026'; // ellipsis
            GetTextExtentPoint32W(hdc, display_name.c_str(), (int)display_name.size(), &name_sz);
        }
    }

    RECT name_rc = { x, y, client.right - PADDING - SPEED_AREA_W + (int)(10 * m_dpi_scale), y + ROW_HEIGHT };
    DrawTextW(hdc, display_name.c_str(), -1, &name_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // 3. Speed (right-aligned, accent color)
    double speed = is_upload ? proc.speed_up : proc.speed_down;
    wchar_t spd_buf[64];
    FormatSpeed(speed, spd_buf, 64);

    SetTextColor(hdc, GetAccentColor(is_upload));
    RECT spd_rc = { client.right - PADDING - SPEED_AREA_W, y, client.right - PADDING, y + ROW_HEIGHT };
    DrawTextW(hdc, spd_buf, -1, &spd_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    // 4. Subtle separator line
    HPEN hOldPen = (HPEN)SelectObject(hdc, m_pen_separator);
    MoveToEx(hdc, PADDING + ICON_SIZE + 6, y + ROW_HEIGHT - 1, NULL);
    LineTo(hdc, client.right - PADDING, y + ROW_HEIGHT - 1);
    SelectObject(hdc, hOldPen);

    SelectObject(hdc, hOldFont);
}

void CTooltipPopup::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right - client.left;
    int h = client.bottom - client.top;

    // Double-buffer
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background
    DrawBackground(memDC, w, h);

    // Set up fonts
    HFONT hOldFont = (HFONT)SelectObject(memDC, m_font_normal);
    SetBkMode(memDC, TRANSPARENT);

    int y = PADDING;

    // === Header: total speed ===
    SetTextColor(memDC, GetTextColor());
    wchar_t header[128];
    wchar_t up_str[32], down_str[32];
    FormatSpeed(m_total_up, up_str, 32);
    FormatSpeed(m_total_down, down_str, 32);
    swprintf_s(header, 128, L"\u2191 %s   \u2193 %s", up_str, down_str);
    RECT header_rc = { PADDING, y, w - PADDING, y + HEADER_H };
    DrawTextW(memDC, header, -1, &header_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += HEADER_H;

    // === Status / warning line (e.g. ETW attach mode) ===
    if (!m_status.empty()) {
        SetTextColor(memDC, m_dark_mode ? RGB(255, 200, 80) : RGB(190, 120, 0));
        HFONT hOldF2 = (HFONT)SelectObject(memDC, m_font_small ? m_font_small : m_font_normal);
        RECT st_rc = { PADDING, y, w - PADDING, y + STATUS_H };
        DrawTextW(memDC, m_status.c_str(), -1, &st_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(memDC, hOldF2);
        y += STATUS_H + 2;
        SetTextColor(memDC, GetTextColor());
    }

    // === Upload section ===
    DrawSectionTitle(memDC, y, TR(L"\u25B2 \u5B9E\u65F6\u4E0A\u4F20"), true);
    y += SECTION_H;

    // Upload section: show processes with upload activity first, then fill with historical
    int up_count = 0;
    for (size_t i = 0; i < m_procs.size() && up_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_up > 0.01) {
            DrawProcRow(memDC, y, m_procs[i], true);
            y += ROW_HEIGHT;
            up_count++;
        }
    }
    // Fill remaining with historical processes (sorted by total speed)
    for (size_t i = 0; i < m_procs.size() && up_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_up <= 0.01) {
            DrawProcRow(memDC, y, m_procs[i], true);
            y += ROW_HEIGHT;
            up_count++;
        }
    }
    if (up_count == 0) {
        SetTextColor(memDC, m_dark_mode ? RGB(120, 120, 120) : RGB(160, 160, 160));
        RECT rc = { PADDING + 2, y, w - PADDING, y + ROW_HEIGHT };
        DrawTextW(memDC, TR(L"\u65E0\u4E0A\u4F20\u6D41\u91CF"), -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        y += ROW_HEIGHT;
    }
    y += GAP_H;

    // === Download section ===
    DrawSectionTitle(memDC, y, TR(L"\u25BC \u5B9E\u65F6\u4E0B\u8F7D"), false);
    y += SECTION_H;

    // Download section: show processes with download activity first, then fill
    int down_count = 0;
    for (size_t i = 0; i < m_procs.size() && down_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_down > 0.01) {
            DrawProcRow(memDC, y, m_procs[i], false);
            y += ROW_HEIGHT;
            down_count++;
        }
    }
    for (size_t i = 0; i < m_procs.size() && down_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_down <= 0.01) {
            DrawProcRow(memDC, y, m_procs[i], false);
            y += ROW_HEIGHT;
            down_count++;
        }
    }
    if (down_count == 0) {
        SetTextColor(memDC, m_dark_mode ? RGB(120, 120, 120) : RGB(160, 160, 160));
        RECT rc = { PADDING + 2, y, w - PADDING, y + ROW_HEIGHT };
        DrawTextW(memDC, TR(L"\u65E0\u4E0B\u8F7D\u6D41\u91CF"), -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        y += ROW_HEIGHT;
    }
    y += BTN_AREA_H - BTN_H;

    // === "\u67E5\u770B\u8BE6\u7EC6" button ===
    m_rcDetailBtn = { PADDING, y, w - PADDING, y + BTN_H };
    HBRUSH hBtnBrush = CreateSolidBrush(m_dark_mode ? RGB(50, 50, 54) : RGB(235, 235, 235));
    HPEN hBtnPen = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(70, 70, 74) : RGB(210, 210, 210));
    HBRUSH hOldBtnBrush = (HBRUSH)SelectObject(memDC, hBtnBrush);
    HPEN hOldBtnPen = (HPEN)SelectObject(memDC, hBtnPen);
    RoundRect(memDC, m_rcDetailBtn.left, m_rcDetailBtn.top, m_rcDetailBtn.right, m_rcDetailBtn.bottom,
              max(2, (int)(6 * m_dpi_scale)), max(2, (int)(6 * m_dpi_scale)));
    SelectObject(memDC, hOldBtnBrush);
    SelectObject(memDC, hOldBtnPen);
    DeleteObject(hBtnBrush);
    DeleteObject(hBtnPen);

    HFONT hOldBtnFont = (HFONT)SelectObject(memDC, m_font_small);
    SetTextColor(memDC, m_dark_mode ? RGB(255, 165, 0) : RGB(230, 126, 34));
    DrawTextW(memDC, TR(L"\u67E5\u770B\u8BE6\u7EC6"), -1, &m_rcDetailBtn, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(memDC, hOldBtnFont);

    // Cleanup
    SelectObject(memDC, hOldFont);

    // Blit
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(m_hwnd, &ps);
}

// ============================================================
// Positioning
// ============================================================

void CTooltipPopup::PositionWindow(const RECT& anchor_rect) {
    // Get popup size
    HDC hdc = GetDC(m_hwnd);
    int pw = 0, ph = 0, up_c = 0, down_c = 0;
    CalcLayout(hdc, pw, ph, up_c, down_c);
    ReleaseDC(m_hwnd, hdc);

    // Work area of the monitor that contains the anchor (multi-monitor safe,
    // fixes popup landing on the primary screen when TM is on a secondary one)
    RECT work, mon;
    PNM_GetWorkAreaForRect(anchor_rect, work, mon);

    int aw = anchor_rect.right - anchor_rect.left;
    int ah = anchor_rect.bottom - anchor_rect.top;
    int margin = max(4, (int)(4 * m_dpi_scale));

    // Decide which side of the anchor to pop out from, based on which edge of
    // the work area the anchor hugs (i.e. where the taskbar is).
    bool at_bottom = anchor_rect.bottom >= work.bottom - 2;
    bool at_top    = anchor_rect.top <= work.top + 2;
    bool at_left   = anchor_rect.left <= work.left + 2;
    bool at_right  = anchor_rect.right >= work.right - 2;

    int x, y;
    if (at_bottom) {          // taskbar at bottom -> popup above anchor
        x = anchor_rect.left + (aw - pw) / 2;
        y = anchor_rect.top - ph - margin;
    } else if (at_top) {      // taskbar at top -> popup below anchor
        x = anchor_rect.left + (aw - pw) / 2;
        y = anchor_rect.bottom + margin;
    } else if (at_left) {     // taskbar at left -> popup right of anchor
        x = anchor_rect.right + margin;
        y = anchor_rect.top + (ah - ph) / 2;
    } else if (at_right) {    // taskbar at right -> popup left of anchor
        x = anchor_rect.left - pw - margin;
        y = anchor_rect.top + (ah - ph) / 2;
    } else {
        // Floating window away from edges: prefer above, fall back to below
        x = anchor_rect.left + (aw - pw) / 2;
        if (anchor_rect.top - ph - margin >= work.top)
            y = anchor_rect.top - ph - margin;
        else
            y = anchor_rect.bottom + margin;
    }

    // Clamp to the anchor's own monitor work area
    if (x + pw > work.right) x = work.right - pw - margin;
    if (x < work.left) x = work.left + margin;
    if (y + ph > work.bottom) y = work.bottom - ph - margin;
    if (y < work.top) y = work.top + margin;

    // --- Avoid overlapping TrafficMonitor's native tooltip -----------------
    // TM's tooltip is a small "tooltips_class32" popup owned by the hovered TM
    // window. If it overlaps our popup, nudge our popup away from it.
    {
        std::vector<RECT> tips;
        FindTMTooltips(m_anchor_hwnd, tips);
        int tip_margin = max(4, (int)(8 * m_dpi_scale));
        bool moved = true;
        for (int pass = 0; moved && pass < 4; ++pass) {
            moved = false;
            for (const auto& tip : tips) {
                RECT popup_rc = { x, y, x + pw, y + ph };
                RECT tip_rc = InflateRectScreen(tip, tip_margin, tip_margin);
                if (!RectsIntersect(popup_rc, tip_rc)) continue;

                // Candidate 1: push further away from anchor along the primary axis.
                int c1x = x, c1y = y; bool c1_ok = false;
                if (at_bottom || (!at_top && !at_left && !at_right)) {
                    int cand = tip_rc.top - ph;
                    if (cand >= work.top + margin) { c1y = cand; c1_ok = true; }
                } else if (at_top) {
                    int cand = tip_rc.bottom;
                    if (cand + ph <= work.bottom - margin) { c1y = cand; c1_ok = true; }
                } else if (at_left) {
                    int cand = tip_rc.right;
                    if (cand + pw <= work.right - margin) { c1x = cand; c1_ok = true; }
                } else if (at_right) {
                    int cand = tip_rc.left - pw;
                    if (cand >= work.left + margin) { c1x = cand; c1_ok = true; }
                }

                // Candidate 2: shift along the secondary axis (horizontal for
                // vertical layouts, vertical for horizontal layouts).
                int c2x = x, c2y = y; bool c2_ok = false;
                if (at_bottom || at_top || (!at_left && !at_right)) {
                    int cand_right = tip_rc.right + margin;
                    int cand_left = tip_rc.left - pw - margin;
                    if (cand_right + pw <= work.right - margin) { c2x = cand_right; c2_ok = true; }
                    else if (cand_left >= work.left + margin) { c2x = cand_left; c2_ok = true; }
                } else {
                    int cand_down = tip_rc.bottom + margin;
                    int cand_up = tip_rc.top - ph - margin;
                    if (cand_down + ph <= work.bottom - margin) { c2y = cand_down; c2_ok = true; }
                    else if (cand_up >= work.top + margin) { c2y = cand_up; c2_ok = true; }
                }

                // Candidate 3: flip to the opposite side of the anchor.
                int c3x = x, c3y = y;
                if (at_bottom || (!at_top && !at_left && !at_right)) {
                    c3y = anchor_rect.bottom + margin;
                } else if (at_top) {
                    c3y = anchor_rect.top - ph - margin;
                } else if (at_left) {
                    c3x = anchor_rect.left - pw - margin;
                } else if (at_right) {
                    c3x = anchor_rect.right + margin;
                }

                auto NoIntersect = [&](int cx, int cy) {
                    RECT rc = { cx, cy, cx + pw, cy + ph };
                    return !RectsIntersect(rc, tip_rc);
                };

                // Prefer horizontal displacement (left/right of the tooltip)
                // so our popup never sits on top of the official tooltip.
                if (c2_ok && NoIntersect(c2x, c2y)) {
                    x = c2x; y = c2y;
                } else if (c1_ok && NoIntersect(c1x, c1y)) {
                    x = c1x; y = c1y;
                } else {
                    x = c3x; y = c3y;
                }
                moved = true;
                break; // recompute intersections after this adjustment
            }
        }

        m_last_tooltips = tips;
    }

    // Final clamp after tooltip avoidance
    if (x + pw > work.right) x = work.right - pw - margin;
    if (x < work.left) x = work.left + margin;
    if (y + ph > work.bottom) y = work.bottom - ph - margin;
    if (y < work.top) y = work.top + margin;

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pw, ph,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Update window region for rounded corners (fixes black corner issue)
    HRGN rgn = CreateRoundRectRgn(0, 0, pw, ph, CORNER_RADIUS * 2, CORNER_RADIUS * 2);
    SetWindowRgn(m_hwnd, rgn, FALSE);

    m_visible = true;

    // Start mouse tracking
    if (!m_tracking) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hwnd, 0 };
        TrackMouseEvent(&tme);
        m_tracking = true;
    }
}

// ============================================================
// Public API
// ============================================================

void CTooltipPopup::UpdateAndShow(const std::vector<ProcDisplayInfo>& procs,
                                   double total_up, double total_down,
                                   const RECT& anchor_rect,
                                   const wchar_t* status,
                                   HWND anchor_hwnd) {
    m_procs = procs;
    m_total_up = total_up;
    m_total_down = total_down;
    m_status = status ? status : L"";
    m_last_anchor = anchor_rect;
    if (anchor_hwnd)
        m_anchor_hwnd = anchor_hwnd;

    // Re-check dark mode
    bool old_dark = m_dark_mode;
    m_dark_mode = IsDarkMode();
    if (m_dark_mode != old_dark) {
        // Recreate separator pen for new theme
        if (m_pen_separator) DeleteObject(m_pen_separator);
        m_pen_separator = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(45, 45, 45) : RGB(235, 235, 235));
    }

    // Per-monitor DPI: the anchor may sit on another monitor with different scaling
    UpdateDpiScale(anchor_rect);

    // Resize and show
    HDC hdc = GetDC(m_hwnd);
    int pw = 0, ph = 0, up_c = 0, down_c = 0;
    CalcLayout(hdc, pw, ph, up_c, down_c);
    ReleaseDC(m_hwnd, hdc);

    SetWindowPos(m_hwnd, NULL, 0, 0, pw, ph, SWP_NOMOVE | SWP_NOZORDER);

    PositionWindow(anchor_rect);
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CTooltipPopup::UpdateData(const std::vector<ProcDisplayInfo>& procs,
                               double total_up, double total_down,
                               const wchar_t* status) {
    m_procs = procs;
    m_total_up = total_up;
    m_total_down = total_down;
    m_status = status ? status : L"";
    if (m_visible) {
        // Recalculate size: new processes may have appeared since we first showed
        RECT old_rc;
        GetWindowRect(m_hwnd, &old_rc);
        int old_w = old_rc.right - old_rc.left;
        int old_h = old_rc.bottom - old_rc.top;

        HDC hdc = GetDC(m_hwnd);
        int pw = 0, ph = 0, up_c = 0, down_c = 0;
        CalcLayout(hdc, pw, ph, up_c, down_c);
        ReleaseDC(m_hwnd, hdc);

        if (pw != old_w || ph != old_h) {
            // Size changed: resize and reposition (anchor may need re-clamping)
            SetWindowPos(m_hwnd, NULL, 0, 0, pw, ph, SWP_NOMOVE | SWP_NOZORDER);
            PositionWindow(m_last_anchor);
        }
        InvalidateRect(m_hwnd, NULL, FALSE);
    }
}

bool CTooltipPopup::RepositionIfTooltipsChanged() {
    if (!m_visible) return false;

    std::vector<RECT> tips;
    FindTMTooltips(m_anchor_hwnd, tips);
    if (tips.size() != m_last_tooltips.size()) {
        PositionWindow(m_last_anchor);
        return true;
    }
    for (size_t i = 0; i < tips.size(); ++i) {
        if (!EqualRect(&tips[i], &m_last_tooltips[i])) {
            PositionWindow(m_last_anchor);
            return true;
        }
    }
    return false;
}

void CTooltipPopup::UpdateDpiScale(const RECT& anchor_rect) {
    float s = PNM_GetDpiScaleForRect(anchor_rect);
    if (s < 0.5f) s = 1.0f;
    if (fabsf(s - m_dpi_scale) < 0.01f) return;
    m_dpi_scale = s;
    PADDING       = (int)(BASE_PADDING * s);
    ROW_HEIGHT    = (int)(BASE_ROW_HEIGHT * s);
    ICON_SIZE     = (int)(BASE_ICON_SIZE * s);
    CORNER_RADIUS = max(1, (int)(BASE_CORNER_RADIUS * s));
    MIN_WIDTH     = (int)(BASE_MIN_WIDTH * s);
    HEADER_H      = (int)(BASE_HEADER_H * s);
    SECTION_H     = (int)(BASE_SECTION_H * s);
    GAP_H         = (int)(BASE_GAP_H * s);
    BTN_AREA_H    = (int)(BASE_BTN_AREA_H * s);
    STATUS_H      = (int)(BASE_STATUS_H * s);
    BTN_H         = (int)(BASE_BTN_H * s);
    SPEED_AREA_W  = (int)(BASE_SPEED_AREA_W * s);
    RecreateScaledResources();
}

void CTooltipPopup::RecreateScaledResources() {
    if (m_font_normal) DeleteObject(m_font_normal);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_pen_separator) DeleteObject(m_pen_separator);
    m_font_normal = CreateFontW((int)(-15 * m_dpi_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    m_font_small = CreateFontW((int)(-13 * m_dpi_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    m_pen_separator = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(45, 45, 45) : RGB(235, 235, 235));
}

void CTooltipPopup::Hide() {
    if (m_visible) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        m_hovering = false;
        m_tracking = false;
    }
}

// Public helper to get cached proc display info
std::vector<CTooltipPopup::ProcDisplayInfo> CProcessNetPlugin::GetCachedProcDisplayInfo() {
    std::vector<CTooltipPopup::ProcDisplayInfo> out;
    GetProcessDisplayInfo(out, m_cached_stats);
    return out;
}


