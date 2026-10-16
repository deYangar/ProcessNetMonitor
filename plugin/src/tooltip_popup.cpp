#include "tooltip_popup.h"
#include "plugin_main.h"  
#include "utils.h"        
#include "signature.h"
#include <shellapi.h>
#include <shobjidl.h>
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

// Subclass watchdog proc. Natively registered in the window class (never
// installed via GWLP_WNDPROC), and the window is message-only so it is
// invisible to EnumWindows-based injector sweeps. Runs on the thread that
// owns the popup: repairing the subclass from the owning thread is the
// documented-safe way to touch GWLP_WNDPROC.
LRESULT CALLBACK CTooltipPopup::WatchdogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == TIMER_WATCHDOG && s_instance && s_instance->m_hwnd) {
        HWND popup = s_instance->m_hwnd;
        if (GetWindowLongPtrW(popup, GWLP_WNDPROC) != (LONG_PTR)StaticWndProc) {
            // Subclass stripped by an external injector: the hover WM_TIMER
            // is being dispatched to DefWindowProc (or to the injector's
            // proc) and HoverTick is dead. Re-install both.
            SetWindowLongPtrW(popup, GWLP_WNDPROC, (LONG_PTR)StaticWndProc);
            SetTimer(popup, TIMER_HOVER, 100, nullptr);
        }
        return 0;
    }
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
// Helper: process icon extraction (frame-split, review P2)
// ============================================================

// 缓存 key 容量保护（review 低危1）：exe 路径超长时 swprintf_s 失败把
// key 置空串，所有超长路径进程共享空 key、图标张冠李戴。超长路径不参
// 与缓存与提取（留空位）——深网络路径/UWP 才可能超，概率极低
static const size_t PNM_ICON_KEY_MAX_PATH = 512;

// UNC 或远程映射盘：Shell 工厂/ExtractAssociatedIcon 的同步磁盘 IO 在
// 断开的映射盘上可卡数秒（review P2），网络路径跳过它们
static bool PNM_IsNetworkPath(const std::wstring& path) {
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') {
        wchar_t root[4] = { path[0], L':', L'\\', 0 };
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

// 只查缓存（O(1) 零 IO）。miss 时把路径记入提取队列（去重）并返回
// nullptr——绘制路径画空位占位，真图标由 PumpIconExtraction 分帧提取
// 后的重绘填充
HICON CTooltipPopup::LookupProcessIcon(const std::wstring& exe_path) {
    if (exe_path.empty() || exe_path.size() >= PNM_ICON_KEY_MAX_PATH) return nullptr;
    int want = max(16, ICON_SIZE);
    wchar_t key[600];
    swprintf_s(key, 600, L"%s|%d", exe_path.c_str(), want);
    auto it = m_icon_cache.find(key);
    if (it != m_icon_cache.end()) return it->second;
    if (std::find(m_icon_pending.begin(), m_icon_pending.end(), exe_path)
            == m_icon_pending.end()) {
        m_icon_pending.push_back(exe_path);
    }
    return nullptr;
}

// 同步提取一个进程图标（Shell 图像工厂 + 两级兜底）。只被
// PumpIconExtraction 调用，不进 WM_PAINT——GetImage 缓存未命中时同步
// 做磁盘 IO + 高质量缩放（单个 5-20ms），悬浮窗一屏 ~10 行、详情窗
// 几十上百行逐个提取会卡首帧（review P2）
HICON CTooltipPopup::ExtractProcessIcon(const std::wstring& exe_path) {
    bool net = PNM_IsNetworkPath(exe_path);
    int want = max(16, ICON_SIZE);
    HICON hIcon = nullptr;

    // 首选：Shell 图像工厂按目标尺寸渲染（咩咩反馈 logo 锯齿，2026-10-16：
    // 旧路径 SHGFI_SMALLICON 拿 16×16 再被 DrawIconEx 最近邻拉到 22px——
    // 放大+无平滑=硬边锯齿。GetImage 内部从大图标高质量下缩并保留
    // alpha 抗锯齿，高 DPI 场景的系统正解）
    if (!net) {
        IShellItemImageFactory* factory = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(exe_path.c_str(), nullptr,
                                                  IID_PPV_ARGS(&factory)))) {
            SIZE sz = { want, want };
            HBITMAP hbm = nullptr;
            if (SUCCEEDED(factory->GetImage(sz, SIIGBF_ICONONLY, &hbm)) && hbm) {
                BITMAP bm = {};
                int iw = 0, ih = 0;
                if (GetObjectW(hbm, sizeof(bm), &bm)) {
                    iw = bm.bmWidth; ih = bm.bmHeight;
                }
                if (iw > 0 && iw <= want * 2) {
                    // GetImage 给 32bpp ARGB 位图；转 HICON（alpha 图标的
                    // mask 形式上要有效但显示时被忽略）
                    HBITMAP mask = CreateBitmap(iw, ih, 1, 1, nullptr);
                    ICONINFO ii = { TRUE, 0, 0, mask, hbm };
                    hIcon = CreateIconIndirect(&ii);
                    DeleteObject(mask);
                }
                DeleteObject(hbm);
            }
            factory->Release();
        }
    }

    if (!hIcon) {
        // 兜底 1：大图标（32px 源，仍优于 SMALLICON 的 16px）。
        // USEFILEATTRIBUTES 不碰文件系统——零磁盘 IO，网络路径同样可用
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(exe_path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
            hIcon = sfi.hIcon;
        }
    }

    if (!hIcon && !net) {
        // 兜底 2：ExtractAssociatedIcon（32px，要打开文件——网络路径跳过）
        wchar_t path_buf[MAX_PATH] = {};
        wcsncpy_s(path_buf, MAX_PATH, exe_path.c_str(), _TRUNCATE);
        WORD idx = 0;
        hIcon = ExtractAssociatedIconW(m_hinst, path_buf, &idx);
    }
    return hIcon;
}

// 分帧提取：一次最多 count 个（含失败 nullptr 的负缓存——防止不可取图
// 的路径每帧重复入队）。有产出返回 true，调用方触发重绘把新图标上屏
bool CTooltipPopup::PumpIconExtraction(int count) {
    bool produced = false;
    while (count-- > 0 && !m_icon_pending.empty()) {
        std::wstring path = m_icon_pending.front();
        m_icon_pending.erase(m_icon_pending.begin());
        int want = max(16, ICON_SIZE);
        wchar_t key[600];
        swprintf_s(key, 600, L"%s|%d", path.c_str(), want);
        m_icon_cache[key] = ExtractProcessIcon(path);
        produced = true;
    }
    return produced;
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
    if (m_watchdog_hwnd) {
        if (g_shutting_down) {
            m_watchdog_hwnd = nullptr;
        } else {
            SafeDestroyWindow(m_watchdog_hwnd);
            m_watchdog_hwnd = nullptr;
        }
    }
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
    DestroyBlurLayer();
    // ULW + coverage 双底色 + 采样表面资源释放
    HDC cov_dcs[4] = { m_cov_a_dc, m_cov_c_dc, m_ulw_dc, m_snap_dc };
    HBITMAP cov_bmps[4] = { m_cov_a_bmp, m_cov_c_bmp, m_ulw_bmp, m_snap_bmp };
    for (int i = 0; i < 4; i++) {
        if (cov_dcs[i]) DeleteDC(cov_dcs[i]);
        if (cov_bmps[i]) DeleteObject(cov_bmps[i]);
    }
    m_cov_a_dc = m_cov_c_dc = m_ulw_dc = m_snap_dc = nullptr;
    m_cov_a_bmp = m_cov_c_bmp = m_ulw_bmp = m_snap_bmp = nullptr;
    m_cov_a_bits = m_cov_c_bits = m_ulw_bits = m_snap_bits = nullptr;
    for (auto& [path, icon] : m_icon_cache) {
        if (icon) DestroyIcon(icon);
    }
    if (m_font_normal) DeleteObject(m_font_normal);
    if (m_font_small) DeleteObject(m_font_small);
    if (m_pen_separator) DeleteObject(m_pen_separator);
    // 类注销：当前 TM 无插件热重载不会触发，但若未来支持热卸载且新 DLL
    // 分到同一 hInst 地址，残留的已注册类会让 CreateWindow 沿用指向已
    // 卸载代码的 wndproc（review C2）。进程退出路径窗口未销毁时反注册
    // 失败，无害
    if (m_hinst) {
        wchar_t cls[64];
        swprintf_s(cls, 64, L"PNMTooltip_%p", m_hinst);
        UnregisterClassW(cls, m_hinst);
        swprintf_s(cls, 64, L"PNMWatchdog_%p", m_hinst);
        UnregisterClassW(cls, m_hinst);
        swprintf_s(cls, 64, L"PNMBlur_%p", m_hinst);
        UnregisterClassW(cls, m_hinst);
    }
    s_instance = nullptr;
}

bool CTooltipPopup::IsDarkMode() {
    // 与详情窗共享插件设置的「颜色模式」：强制深/浅优先于系统
    int cm = CDetailWindow::GetColorMode();
    if (cm == 1) return true;
    if (cm == 2) return false;
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

    // 圆角由 ULW 表面 alpha 自绘（ComposeLayeredSurface 的几何 coverage）。
    // DWM 侧显式「不要圆角」：删除设置≠禁用（DWMWCP_DEFAULT 仍可能被
    // 系统策略圆角并附带背板/阴影——2026-10-16 咩咩反馈的窗外一圈暗晕，
    // 第一轮只删设置没禁掉）。DONOTROUND + NC 渲染策略禁用双保险
    int corner_pref = 1;  // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                          &corner_pref, sizeof(corner_pref));
    int nc_policy = 2;    // DWMNCRP_DISABLED
    DwmSetWindowAttribute(m_hwnd, 2 /* DWMWA_NCRENDERING_POLICY */,
                          &nc_policy, sizeof(nc_policy));

    // Set initial window region for rounded corners (will be updated on resize)
    HRGN rgn = CreateRoundRectRgn(0, 0, 300, 400, CORNER_RADIUS * 2, CORNER_RADIUS * 2);
    SetWindowRgn(m_hwnd, rgn, FALSE);

    // Pre-create cached GDI objects (recreated on DPI change via RecreateScaledResources)
    RecreateScaledResources();

    // Hover state machine: 100ms timer dispatched through TM's message loop
    // (this window lives on TM's main thread)
    SetTimer(m_hwnd, TIMER_HOVER, 100, nullptr);

    // Subclass watchdog: message-only, natively-registered proc, 1s tick.
    // Guards the subclass above (see WatchdogProc). Created on the same
    // thread as the popup so its WM_TIMER - and the repair - run there.
    wchar_t wdClass[64];
    swprintf_s(wdClass, 64, L"PNMWatchdog_%p", m_hinst);
    WNDCLASSEXW wcWd = {};
    wcWd.cbSize = sizeof(wcWd);
    wcWd.lpfnWndProc = WatchdogProc;
    wcWd.hInstance = m_hinst;
    wcWd.lpszClassName = wdClass;
    RegisterClassExW(&wcWd);
    m_watchdog_hwnd = CreateWindowExW(
        0, wdClass, L"", WS_OVERLAPPED,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, m_hinst, nullptr);
    if (m_watchdog_hwnd)
        SetTimer(m_watchdog_hwnd, TIMER_WATCHDOG, 1000, nullptr);

    return true;
}

LRESULT CTooltipPopup::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_HOVER) {
            // 图标分帧提取：每 tick 补 2 个未缓存图标后重绘（首帧空位
            // 占位，~1s 内逐个亮起，提取不再卡 WM_PAINT）
            if (PumpIconExtraction(2))
                InvalidateRect(m_hwnd, NULL, FALSE);
            CProcessNetPlugin::Instance().HoverTick();
        }
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
        {
            // Hover 反馈：状态翻转才重绘（移动过程零开销）
            POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
            bool up = PtInRect(&m_rcSectionUp, pt) != FALSE;
            bool down = PtInRect(&m_rcSectionDown, pt) != FALSE;
            bool btn = PtInRect(&m_rcDetailBtn, pt) != FALSE;
            if (up != m_hover_up || down != m_hover_down || btn != m_hover_btn) {
                m_hover_up = up;
                m_hover_down = down;
                m_hover_btn = btn;
                InvalidateRect(m_hwnd, NULL, FALSE);
            }
        }
        return 0;

    case WM_SETCURSOR:
        // 查看详细按钮上手型光标，其余箭头（HTCLIENT 恒定，见 WM_NCHITTEST）
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(m_hwnd, &pt);
            SetCursor(LoadCursor(nullptr,
                      PtInRect(&m_rcDetailBtn, pt) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);

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
    // 清 hover 高亮（有状态翻转才重绘）
    if (m_hover_up || m_hover_down || m_hover_btn) {
        m_hover_up = m_hover_down = m_hover_btn = false;
        if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
    }
    // Don't hide immediately - the hover timer handles hide delays
}

// ============================================================
// Colors & theming
// ============================================================

COLORREF CTooltipPopup::GetBgColor() {
    return UseDarkTheme() ? RGB(32, 32, 32) : RGB(249, 249, 249);
}

COLORREF CTooltipPopup::GetTextColor() {
    return UseTextTheme() ? RGB(240, 240, 240) : RGB(30, 30, 30);
}

COLORREF CTooltipPopup::GetAccentColor(bool is_upload, bool on_panel) {
    // UseDarkTheme()/UseTextTheme() 的 true 都表示「深色语境配亮色」，
    // 两分支色值一致；区别只在反色翻转时是否跟随
    bool light = on_panel ? UseDarkTheme() : UseTextTheme();
    if (is_upload) return light ? RGB(255, 165, 0) : RGB(230, 126, 34);  // orange
    return light ? RGB(80, 200, 120) : RGB(46, 160, 67);                // green
}

COLORREF CTooltipPopup::GetSectionBgColor() {
    return UseDarkTheme() ? RGB(44, 44, 44) : RGB(238, 238, 238);
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
    // 2026-10-16 咩咩反馈「进程名和流速之间留白大」：速度右对齐贴右缘，
    // 间隙=窗口宽-两段文本宽，收窄四件套（名字列 105→90 / 列间隔 12→8 /
    // 速度列 110→85 / 窗口下限 300→260）整体收 ~40px，长名照旧省略
    const int NAME_COL_W = (int)(90 * m_dpi_scale);
    out_w = PADDING + ICON_SIZE + (int)(6 * m_dpi_scale) + NAME_COL_W
            + (int)(8 * m_dpi_scale) + SPEED_AREA_W + PADDING;
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

void CTooltipPopup::DrawBackground(HDC hdc, int w, int h, bool content_only) {
    if (m_acrylic_active) {
        // ULW 模式：coverage pass（白/黑底）上只画边框线——背景由
        // ComposeLayeredSurface 提供（自绘档=烘焙模糊背景，透明档=
        // 面板色×opacity alpha）；边框作为内容参与双底色解算（精确
        // 半透明边缘）。边框跟面板主题（装饰元素，反色翻转不触碰）
        HPEN hPen = CreatePen(PS_SOLID, 1, UseDarkTheme() ? RGB(55, 55, 55) : RGB(220, 220, 220));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 0, 0, w, h);
        SelectObject(hdc, hOldBrush);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        return;
    }
    if (content_only) return;  // 防御：非 SWCA 模式不跑 coverage pass
    // Fill with rounded rectangle
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    HPEN hPen = CreatePen(PS_SOLID, 1, UseDarkTheme() ? RGB(55, 55, 55) : RGB(220, 220, 220));
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

    RoundRect(hdc, 0, 0, w, h, CORNER_RADIUS * 2, CORNER_RADIUS * 2);

    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void CTooltipPopup::DrawSectionTitle(HDC hdc, int y, const wchar_t* title, bool is_upload, bool hover) {
    // Section title bar with slightly different bg（hover 提亮一档）
    COLORREF sec_bg = GetSectionBgColor();
    if (hover) sec_bg = UseDarkTheme() ? RGB(60, 60, 60) : RGB(227, 227, 227);
    HBRUSH hBrush = CreateSolidBrush(sec_bg);
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
    HPEN hNullPen = CreatePen(PS_SOLID, 0, sec_bg);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hNullPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom + 2, radius, radius);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hNullPen);

    // Draw title text with accent color. 标题条是实心自绘底：文字跟面板
    // 主题（on_panel=true），不随反色翻转——翻成暗橙叠深色条上对比度塌
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetAccentColor(is_upload, true));

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
    HICON hIcon = LookupProcessIcon(proc.exe_path);
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

// ============================================================
// ULW per-pixel presentation + 自绘模糊背景管线
// ============================================================

// 实测（2026-10-16 harness 四轮 + SWCA 探针矩阵）：本机 DWM 对含内容
// 像素的 SWCA 窗口忽略 tint alpha（一律全浓度渲染）——「浓度」在 SWCA
// 单窗口架构上必然失控。终局（方案 B，2026-10-16 咩咩拍板）：背景像素
// ============================================================
// SWCA（SetWindowCompositionAttribute，未公开但自 Win8 起稳定存在）
// ============================================================

enum PNM_ACCENT_STATE {
    PNM_ACCENT_DISABLED = 0,
    PNM_ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,   // TranslucentTB "Clear"
    PNM_ACCENT_ENABLE_BLURBEHIND = 3,            // TranslucentTB "Blur"
    PNM_ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,     // TranslucentTB "Acrylic"
};

struct PNM_ACCENT_POLICY {
    int nAccentState;
    int nFlags;
    UINT nColor;       // ABGR: alpha<<24 | B<<16 | G<<8 | R
    int nAnimationId;
};
struct PNM_WINDOWCOMPOSITIONATTRIBDATA {
    int nAttribute;    // 19 = WCA_ACCENT_POLICY
    void* pData;
    ULONG dataSize;
};
typedef BOOL(WINAPI* PFN_SetWindowCompositionAttribute)(HWND, PNM_WINDOWCOMPOSITIONATTRIBDATA*);

static PFN_SetWindowCompositionAttribute PNM_GetSWCA() {
    static PFN_SetWindowCompositionAttribute pfn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32)
            pfn = (PFN_SetWindowCompositionAttribute)GetProcAddress(
                user32, "SetWindowCompositionAttribute");
    }
    return pfn;
}

// blur 效果层（方案 A'，2026-10-16 咩咩拍板回归系统实时通道）：无内容
// 纯黑窗 + SWCA 只取系统模糊（tint alpha=1 近全透）。DWM 硬件逐帧合成
// ——背后视频/滚动实时跟随（探针实测 50-100ms），零 CPU 永不闪。上
// 层 ULW 内容层以 panel×op 半透明盖在其上（浓度公式探针实测精确吻合）
void CTooltipPopup::EnsureBlurLayer() {
    if (m_blur_hwnd) return;

    wchar_t className[64];
    swprintf_s(className, 64, L"PNMBlur_%p", m_hinst);
    // 以类名实际查重（不依赖进程级 static 标志）：热卸载重载后类可能
    // 仍存活，static 标志却已归零导致重复注册失败（review C2）
    WNDCLASSEXW wcExist = {};
    wcExist.cbSize = sizeof(wcExist);
    if (!GetClassInfoExW(m_hinst, className, &wcExist)) {
        WNDCLASSEXW wcB = {};
        wcB.cbSize = sizeof(wcB);
        wcB.lpfnWndProc = DefWindowProcW;
        wcB.hInstance = m_hinst;
        wcB.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcB.lpszClassName = className;
        RegisterClassExW(&wcB);
    }
    m_blur_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,  // 点击穿透
        className, L"", WS_POPUP,
        0, 0, 300, 400, NULL, NULL, m_hinst, NULL);
    if (!m_blur_hwnd) return;

    // blur 层圆角跟 ULW 层对齐（模糊直角会从圆角外露出）
    int corner_pref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(m_blur_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                          &corner_pref, sizeof(corner_pref));
    MARGINS glass = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(m_blur_hwnd, &glass);

    // ⚠️ 不用 DWMSBT：26300 实测纯黑 GDI 窗上只渲染 tint 纯色（不透不
    // 模糊，2026-10-16 二次复核 extend/bare 两姿势同样死）。SWCA
    // ACRYLICBLURBEHIND + 近全透 tint 实测提供真 blur behind（探针
    // tools/systembackdrop_probe：棋盘 lap_var 2.3 vs 无模糊数千）。
    // 亚克力/模糊两档的区分由上层面板噪声纹理实现（st3/st4 渲染已合并）
    PNM_ACCENT_POLICY policy = {};
    policy.nAccentState = PNM_ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.nColor = 0x01000000;  // alpha=1：只取系统模糊，不叠色
    PNM_WINDOWCOMPOSITIONATTRIBDATA data = {};
    data.nAttribute = 19;
    data.pData = &policy;
    data.dataSize = sizeof(policy);
    if (PNM_GetSWCA()) PNM_GetSWCA()(m_blur_hwnd, &data);
}

void CTooltipPopup::DestroyBlurLayer() {
    if (!m_blur_hwnd) return;
    if (!g_shutting_down) SafeDestroyWindow(m_blur_hwnd);
    m_blur_hwnd = nullptr;
}

// ULW 表面（32bpp DIB）+ coverage 双底 pass（A 白底 / C 黑底）+ 背景
// 采样表面。尺寸变化时整体重建；直接像素访问
bool CTooltipPopup::EnsureLayeredSurface(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (m_ulw_dc && m_ulw_bmp && m_cov_a_dc && m_cov_c_dc && m_snap_dc &&
        w == m_ulw_w && h == m_ulw_h)
        return true;

    struct { HDC* dc; HBITMAP* bmp; void** bits; int bw, bh; } surfs[4] = {
        { &m_cov_a_dc, &m_cov_a_bmp, &m_cov_a_bits, w, h },
        { &m_cov_c_dc, &m_cov_c_bmp, &m_cov_c_bits, w, h },
        { &m_ulw_dc, &m_ulw_bmp, &m_ulw_bits, w, h },
        { &m_snap_dc, &m_snap_bmp, &m_snap_bits, w, h },
    };
    for (auto& s : surfs) {
        if (*s.dc) { DeleteDC(*s.dc); *s.dc = nullptr; }
        if (*s.bmp) { DeleteObject(*s.bmp); *s.bmp = nullptr; }
        *s.bits = nullptr;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = s.bw;
        bi.bmiHeader.biHeight = -s.bh;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        *s.bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, s.bits, NULL, 0);
        if (*s.bmp) {
            *s.dc = CreateCompatibleDC(NULL);
            if (*s.dc) SelectObject(*s.dc, *s.bmp);
        }
        if (!*s.bmp || !*s.dc) {
            // 全有或全无：中途失败时部分表面已是新尺寸而簿记还是旧值，
            // 下次若目标尺寸恰好等于旧值会命中开头「已就绪」短路，
            // ComposeLayeredSurface 按错误假设跨新旧表面读写越界。回收
            // 四个表面、簿记归零
            for (auto& t : surfs) {
                if (*t.dc) { DeleteDC(*t.dc); *t.dc = nullptr; }
                if (*t.bmp) { DeleteObject(*t.bmp); *t.bmp = nullptr; }
                *t.bits = nullptr;
            }
            m_ulw_w = m_ulw_h = 0;
            return false;
        }
    }
    m_ulw_w = w;
    m_ulw_h = h;
    return true;
}

// coverage 双底色解覆盖率（白底 A / 黑底 C 各画一遍）：
//   c = 1 - (A - C) / 255      （真实覆盖率，三通道均值消抖动）
//   f = (A - 255×(1-c)) / c    （还原内容色）
// 直接在 ULW 表面上合成 premultiplied ARGB。透明系三档统一公式（方案 A'）：
//   纯背景 (c=0)  → rgb = panel×op（预乘）, a = op——透出 blur 层系统
//                   模糊（亚克力/模糊档）或桌面（透明档）；亚克力档背景
//                   叠确定性噪声
//   实心内容 (c=1) → rgb = f, a = 255
//   边缘 → a = 255c + op(1-c)；rgb = f×c + panel×op×(1-c)（预乘形式）
void CTooltipPopup::ComposeLayeredSurface(int w, int h) {
    if (!m_cov_a_bits || !m_cov_c_bits || !m_ulw_bits) return;
    unsigned char* pa = (unsigned char*)m_cov_a_bits;
    unsigned char* pc = (unsigned char*)m_cov_c_bits;
    unsigned char* pw = (unsigned char*)m_ulw_bits;

    COLORREF panel = GetBgColor();
    int pb = GetBValue(panel), pg = GetGValue(panel), pr = GetRValue(panel);
    int op = m_backdrop_opacity < 0 ? 100 : m_backdrop_opacity;
    int bg_a = op * 255 / 100;
    int r = max(2, CORNER_RADIUS);

    for (int y = 0; y < h; y++) {
        float cy = y + 0.5f;
        for (int x = 0; x < w; x++) {
            // 圆角几何 coverage（1px 抗锯齿带）
            float cov = 1.0f;
            float cx = x + 0.5f, ccx = -1.0f, ccy = -1.0f;
            if (x < r && y < r) { ccx = (float)r; ccy = (float)r; }
            else if (x >= w - r && y < r) { ccx = (float)(w - r); ccy = (float)r; }
            else if (x < r && y >= h - r) { ccx = (float)r; ccy = (float)(h - r); }
            else if (x >= w - r && y >= h - r) { ccx = (float)(w - r); ccy = (float)(h - r); }
            if (ccx >= 0.0f) {
                float dx = cx - ccx, dy = cy - ccy;
                float dist = sqrtf(dx * dx + dy * dy);
                cov = (float)r - dist + 0.5f;
                if (cov < 0.0f) cov = 0.0f;
                if (cov > 1.0f) cov = 1.0f;
            }

            size_t i = ((size_t)y * w + x) * 4;
            int da = (pa[i] - pc[i]) + (pa[i + 1] - pc[i + 1]) + (pa[i + 2] - pc[i + 2]);
            int cov255 = 255 - (da + 1) / 3;   // 内容覆盖率 c×255

            // 亚克力噪声（Fluent 亚克力与纯模糊的标志性区别；确定性
            // 哈希避免逐帧闪烁）——叠在半透明面板色上，随浓度透出
            int nz = 0;
            if (m_noise_acrylic) {
                nz = ((x * 73856093) ^ (y * 19349663)) & 7;  // 0..7
                nz = nz - 3;                                   // -3..+3
            }

            // ---- 透明系统一公式（透出 blur 层系统模糊 / 桌面）----
            int a = cov255 + (bg_a * (255 - cov255) + 127) / 255;
            a = (int)(a * cov + 0.5f);
            if (a <= 0) {
                pw[i] = pw[i + 1] = pw[i + 2] = pw[i + 3] = 0;
                continue;
            }
            if (cov255 >= 255) {
                // 实心内容
                pw[i]     = (unsigned char)min(255, (int)(pc[i]     * cov + 0.5f));
                pw[i + 1] = (unsigned char)min(255, (int)(pc[i + 1] * cov + 0.5f));
                pw[i + 2] = (unsigned char)min(255, (int)(pc[i + 2] * cov + 0.5f));
            } else if (cov255 <= 0) {
                // 纯背景：panel(+噪声)×op 预乘
                int bb = max(0, min(255, pb + nz)) * bg_a / 255;
                int bgg = max(0, min(255, pg + nz)) * bg_a / 255;
                int brr = max(0, min(255, pr + nz)) * bg_a / 255;
                pw[i]     = (unsigned char)min(255, (int)(bb * cov + 0.5f));
                pw[i + 1] = (unsigned char)min(255, (int)(bgg * cov + 0.5f));
                pw[i + 2] = (unsigned char)min(255, (int)(brr * cov + 0.5f));
            } else {
                // 边缘：内容与背景层的预乘复合（背景带噪声）
                int inv = 255 - cov255;        // 255(1-c)
                int fr = (pa[i + 2] - inv) * 255 / cov255;
                int fg = (pa[i + 1] - inv) * 255 / cov255;
                int fb = (pa[i]     - inv) * 255 / cov255;
                // ClearType 亚像素覆盖率的通道差经 f 还原落进内容色：
                // 黑字叠黑背景时边缘呈紫边（白字在白 pass 全 255、f 解出
                // 纯白，天然无此问题）。向通道均值收敛——子像素信息保留
                // 在 cov 标量里（锐度不变），颜色污染清零；彩字边缘轻微
                // 去饱和（1-2px 半透明带，肉眼不可辨）
                int fm = (fr + fg + fb) / 3;
                fr = fg = fb = fm;
                int pnr = max(0, pr + nz), png = max(0, pg + nz), pnb = max(0, pb + nz);
                int er = fr * cov255 / 255 + pnr * bg_a / 255 * inv / 255;
                int eg = fg * cov255 / 255 + png * bg_a / 255 * inv / 255;
                int eb = fb * cov255 / 255 + pnb * bg_a / 255 * inv / 255;
                pw[i]     = (unsigned char)max(0, min(255, (int)(eb * cov + 0.5f)));
                pw[i + 1] = (unsigned char)max(0, min(255, (int)(eg * cov + 0.5f)));
                pw[i + 2] = (unsigned char)max(0, min(255, (int)(er * cov + 0.5f)));
            }
            pw[i + 3] = (unsigned char)a;
        }
    }

    SIZE size = { w, h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(m_hwnd, screen, NULL, &size, m_ulw_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

// 截屏前把 ULW 表面清空提交：窗口可见时直接 BitBlt 会把自己截进去
// （探针实证：带/不带 CAPTUREBLT 均 100% 自截），提交全透明表面后同步
// 立即截则 0% 自截。清空→截屏→新表面提交都在同一消息处理内完成，
// DWM 合成看不到中间帧。清空提交带新尺寸时窗口同步 resize（透明状态
// 下不可见，无副作用）
void CTooltipPopup::ClearLayeredSurfaceForCapture() {
    if (!m_hwnd || !m_ulw_dc || !m_ulw_bits || m_ulw_w <= 0 || m_ulw_h <= 0)
        return;
    memset(m_ulw_bits, 0, (size_t)m_ulw_w * m_ulw_h * 4);
    SIZE size = { m_ulw_w, m_ulw_h };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(m_hwnd, screen, NULL, &size, m_ulw_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

// affinity 无闪截屏：WDA_EXCLUDEFROMCAPTURE 期间窗口从一切屏幕捕获中
// 剔除、用户肉眼显示不受影响——BitBlt 拿到窗口脚下真实桌面，全程窗口
// 可见零闪。探针实测（tools/affinity_probe，2026-10-16）：对 ULW 分层
// 窗口即时生效（无需等合成帧）、可逆，SRCCOPY/CAPTUREBLT 两 flag 均剔除。
// 设置失败（Win10 2004 前的老系统）记 m_affinity_ok=false 永久禁用静止
// 刷新——那种系统只能清空法，每周期闪一次不可接受
bool CTooltipPopup::CaptureSnapNoFlash(const RECT& popup_rect, int w, int h) {
    if (!m_hwnd || !m_snap_dc || !m_snap_bits) return false;
    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        m_affinity_ok = false;
        return false;
    }
    HDC scr = GetDC(NULL);
    BitBlt(m_snap_dc, 0, 0, w, h, scr, popup_rect.left, popup_rect.top,
           SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, scr);
    SetWindowDisplayAffinity(m_hwnd, 0);
    return true;
}

// 背景亮度采样 + 文字反色判定（方案 A'：背景磨砂由 blur 层系统实时合
// 成，本函数只负责文字深浅跟随背景亮度）。截屏来源两通道：
//   a) affinity 无闪截屏——窗口从捕获中剔除、显示不受影响（探针
//      tools/affinity_probe 实测 ULW 即时生效可逆）；亚克力/模糊档截到
//      的正是 blur 层的模糊画面（天然平滑，均值稳定）
//   b) 清空表面→截屏——affinity 不可用的兜底（首显时窗口未上屏天然
//      干净；静止刷新在 affinity 不可用时不跑，不会走这条闪的路）
// 亮度公式承自旧 UpdateAdaptiveTextTheme：观感亮度与主题文字色对比
// 不足则翻转深浅
void CTooltipPopup::RefreshBackdrop(const RECT& popup_rect) {
    m_text_adaptive = false;
    m_text_dark = m_dark_mode;
    if (!m_acrylic_active) return;

    int w = popup_rect.right - popup_rect.left;
    int h = popup_rect.bottom - popup_rect.top;
    if (w <= 0 || h <= 0) return;

    int op = m_backdrop_opacity < 0 ? 100 : m_backdrop_opacity;
    // 高浓度（>=80%）背景≈面板色，主题文字色已保证对比——无需截屏
    // 判定。原判断在截屏+采样之后，浓度高档每 300ms 一轮的截屏与遍历
    // 采样全是无用功；提前到截屏之前直接保持开头的主题复位返回
    if (op >= 80) return;

    if (!EnsureLayeredSurface(w, h)) return;

    if (!CaptureSnapNoFlash(popup_rect, w, h)) {
        if (m_visible) ClearLayeredSurfaceForCapture();
        HDC scr = GetDC(NULL);
        BitBlt(m_snap_dc, 0, 0, w, h, scr, popup_rect.left, popup_rect.top,
               SRCCOPY | CAPTUREBLT);
        ReleaseDC(NULL, scr);
    }

    // 亮度判定（4px 步进采样足够；截到的画面已是模糊合成结果）。
    // 只按左侧进程名列区域判（咩咩 2026-10-16 指定）：文字深浅对比的
    // 主体是进程名/标题，右侧 SPEED_AREA_W 是流速数字列，右半边亮暗
    // 与文字可读性无关。取**中位数**而非均值：局部黑块（窗口边缘压
    // 到深色内容，<50%）不该拉偏整列判色——「绝大部分白就按白判」
    // 正是中位数的语义（咩咩 13:38 实测：进程列沾一条黑边就误翻白字）
    int judge_w = w - SPEED_AREA_W + (int)(10 * m_dpi_scale);
    if (judge_w < w / 2) judge_w = w / 2;   // 布局异常时保底半宽
    if (judge_w > w) judge_w = w;
    std::vector<unsigned short> lums;
    lums.reserve((size_t)((h / 4 + 1) * (judge_w / 4 + 1)));
    {
        unsigned char* p8 = (unsigned char*)m_snap_bits;
        for (int y = 0; y < h; y += 4)
            for (int x = 0; x < judge_w; x += 4) {
                size_t i = ((size_t)y * w + x) * 4;
                lums.push_back((unsigned short)(
                    (299 * p8[i + 2] + 587 * p8[i + 1] + 114 * p8[i]) / 1000));
            }
    }
    double screen_lum = -1.0;
    if (!lums.empty()) {
        size_t mid = lums.size() / 2;
        std::nth_element(lums.begin(), lums.begin() + mid, lums.end());
        screen_lum = (double)lums[mid];   // 中位亮度
    }

    // 反色判定（op>=80 已在截屏前提前返回）
    if (screen_lum < 0.0) return;
    COLORREF panel = GetBgColor();
    double panel_lum = (299.0 * GetRValue(panel) + 587.0 * GetGValue(panel)
                        + 114.0 * GetBValue(panel)) / 1000.0;
    double bg_lum = (op / 100.0) * panel_lum + (1.0 - op / 100.0) * screen_lum;
    // 文字色亮度（深色主题=浅字 240 / 浅色主题=深字 30）
    double text_lum = m_dark_mode ? 240.0 : 30.0;
    bool flip = (fabs(bg_lum - text_lum) < 70.0);
    if (flip) {
        // 撞色：翻转文字深浅
        m_text_dark = !m_dark_mode;
        m_text_adaptive = true;
    }
}

void CTooltipPopup::DrawContent(HDC dc, int w, int h, bool content_only) {
    // Background（coverage pass 用 content_only：白/黑底上只画边框等内容，
    // 面板由 ULW 背景 alpha 提供）
    DrawBackground(dc, w, h, content_only);

    // Set up fonts
    HFONT hOldFont = (HFONT)SelectObject(dc, m_font_normal);
    SetBkMode(dc, TRANSPARENT);

    int y = PADDING;

    // === Header: total speed ===
    SetTextColor(dc, GetTextColor());
    wchar_t header[128];
    wchar_t up_str[32], down_str[32];
    FormatSpeed(m_total_up, up_str, 32);
    FormatSpeed(m_total_down, down_str, 32);
    swprintf_s(header, 128, L"\u2191 %s   \u2193 %s", up_str, down_str);
    RECT header_rc = { PADDING, y, w - PADDING, y + HEADER_H };
    DrawTextW(dc, header, -1, &header_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += HEADER_H;

    // === Status / warning line (e.g. ETW attach mode) ===
    if (!m_status.empty()) {
        SetTextColor(dc, UseTextTheme() ? RGB(255, 200, 80) : RGB(190, 120, 0));
        HFONT hOldF2 = (HFONT)SelectObject(dc, m_font_small ? m_font_small : m_font_normal);
        RECT st_rc = { PADDING, y, w - PADDING, y + STATUS_H };
        DrawTextW(dc, m_status.c_str(), -1, &st_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        SelectObject(dc, hOldF2);
        y += STATUS_H + 2;
        SetTextColor(dc, GetTextColor());
    }

    // === Upload section ===
    m_rcSectionUp = { PADDING - 2, y, w - PADDING + 2, y + (int)(20 * m_dpi_scale) + 2 };
    DrawSectionTitle(dc, y, TR(L"\u25B2 \u5B9E\u65F6\u4E0A\u4F20"), true, m_hover_up);
    y += SECTION_H;

    // Upload section: show processes with upload activity first, then fill with historical
    int up_count = 0;
    for (size_t i = 0; i < m_procs.size() && up_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_up > 0.01) {
            DrawProcRow(dc, y, m_procs[i], true);
            y += ROW_HEIGHT;
            up_count++;
        }
    }
    // Fill remaining with historical processes (sorted by total speed)
    for (size_t i = 0; i < m_procs.size() && up_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_up <= 0.01) {
            DrawProcRow(dc, y, m_procs[i], true);
            y += ROW_HEIGHT;
            up_count++;
        }
    }
    if (up_count == 0) {
        SetTextColor(dc, UseTextTheme() ? RGB(120, 120, 120) : RGB(160, 160, 160));
        RECT rc = { PADDING + 2, y, w - PADDING, y + ROW_HEIGHT };
        DrawTextW(dc, TR(L"\u65E0\u4E0A\u4F20\u6D41\u91CF"), -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        y += ROW_HEIGHT;
    }
    y += GAP_H;

    // === Download section ===
    m_rcSectionDown = { PADDING - 2, y, w - PADDING + 2, y + (int)(20 * m_dpi_scale) + 2 };
    DrawSectionTitle(dc, y, TR(L"\u25BC \u5B9E\u65F6\u4E0B\u8F7D"), false, m_hover_down);
    y += SECTION_H;

    // Download section: show processes with download activity first, then fill
    int down_count = 0;
    for (size_t i = 0; i < m_procs.size() && down_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_down > 0.01) {
            DrawProcRow(dc, y, m_procs[i], false);
            y += ROW_HEIGHT;
            down_count++;
        }
    }
    for (size_t i = 0; i < m_procs.size() && down_count < MAX_SHOW; i++) {
        if (m_procs[i].speed_down <= 0.01) {
            DrawProcRow(dc, y, m_procs[i], false);
            y += ROW_HEIGHT;
            down_count++;
        }
    }
    if (down_count == 0) {
        SetTextColor(dc, UseTextTheme() ? RGB(120, 120, 120) : RGB(160, 160, 160));
        RECT rc = { PADDING + 2, y, w - PADDING, y + ROW_HEIGHT };
        DrawTextW(dc, TR(L"\u65E0\u4E0B\u8F7D\u6D41\u91CF"), -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        y += ROW_HEIGHT;
    }
    y += BTN_AREA_H - BTN_H;

    // === "\u67E5\u770B\u8BE6\u7EC6" button ===
    m_rcDetailBtn = { PADDING, y, w - PADDING, y + BTN_H };
    // 实心自绘控件：底色/描边/文字整套跟面板主题——按钮可见底色永远
    // 是自身不透明填充色，磨砂背景透不进来，反色翻转不触碰（曾整体误
    // 挂 UseTextTheme，反色时按钮跟着翻成亮灰底，2026-10-16 咩咩实测
    // 抓出）。hover：背景提亮一档 + accent 描边 + 文字提亮（与 section
    // 条同一方向）
    COLORREF btn_bg  = UseDarkTheme() ? RGB(50, 50, 54)  : RGB(235, 235, 235);
    COLORREF btn_pen = UseDarkTheme() ? RGB(70, 70, 74)  : RGB(210, 210, 210);
    COLORREF btn_txt = UseDarkTheme() ? RGB(255, 165, 0) : RGB(230, 126, 34);
    if (m_hover_btn) {
        btn_bg  = UseDarkTheme() ? RGB(68, 68, 74)  : RGB(222, 222, 222);
        btn_pen = UseDarkTheme() ? RGB(255, 165, 0) : RGB(230, 126, 34);
        btn_txt = UseDarkTheme() ? RGB(255, 190, 80) : RGB(196, 100, 16);
    }
    HBRUSH hBtnBrush = CreateSolidBrush(btn_bg);
    HPEN hBtnPen = CreatePen(PS_SOLID, 1, btn_pen);
    HBRUSH hOldBtnBrush = (HBRUSH)SelectObject(dc, hBtnBrush);
    HPEN hOldBtnPen = (HPEN)SelectObject(dc, hBtnPen);
    RoundRect(dc, m_rcDetailBtn.left, m_rcDetailBtn.top, m_rcDetailBtn.right, m_rcDetailBtn.bottom,
              max(2, (int)(6 * m_dpi_scale)), max(2, (int)(6 * m_dpi_scale)));
    SelectObject(dc, hOldBtnBrush);
    SelectObject(dc, hOldBtnPen);
    DeleteObject(hBtnBrush);
    DeleteObject(hBtnPen);

    HFONT hOldBtnFont = (HFONT)SelectObject(dc, m_font_small);
    SetTextColor(dc, btn_txt);
    DrawTextW(dc, TR(L"\u67E5\u770B\u8BE6\u7EC6"), -1, &m_rcDetailBtn, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, hOldBtnFont);

    // Cleanup
    SelectObject(dc, hOldFont);
}

void CTooltipPopup::OnPaint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(m_hwnd, &ps);

    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right - client.left;
    int h = client.bottom - client.top;

    if (m_acrylic_active) {
        // ULW：内容在白底/黑底各画一遍（coverage pass）解出每像素真实
        // 覆盖率，ComposeLayeredSurface 合成 premultiplied ARGB（背景=
        // 面板×opacity、内容不透明、边缘精确复合、圆角几何 coverage）后
        // UpdateLayeredWindow 提交——窗口外观完全由 ULW 表面接管
        if (EnsureLayeredSurface(w, h)) {
            RECT rc_all = { 0, 0, w, h };
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(m_cov_a_dc, &rc_all, white);
            DeleteObject(white);
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(m_cov_c_dc, &rc_all, black);
            DeleteObject(black);

            DrawContent(m_cov_a_dc, w, h, true);
            DrawContent(m_cov_c_dc, w, h, true);
            ComposeLayeredSurface(w, h);
        }
        EndPaint(m_hwnd, &ps);
        return;
    }

    // 普通窗口：双缓冲
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    DrawContent(memDC, w, h, false);

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
    }

    // Final clamp after tooltip avoidance
    if (x + pw > work.right) x = work.right - pw - margin;
    if (x < work.left) x = work.left + margin;
    if (y + ph > work.bottom) y = work.bottom - ph - margin;
    if (y < work.top) y = work.top + margin;

    // 文字反色判定（首显：窗口未上屏，截屏天然干净）。背景磨砂由
    // blur 层 DWM 实时合成，移动中无需任何重采样——但文字深浅要跟随
    // 背景亮度：显示中的矩形变化（anchor 跟随/拖动/避让）重判，节流
    // 到 250ms（单轮截屏+判定 2-6ms 在 UI 线程，拖动 10Hz 全跑低端机
    // 帧率有感知；翻转本身带迟滞带，250ms 更新观感无差）。跳过期间
    // m_last_snap_rect 保持旧值，移动停止后由 UpdateData 的静止判定
    // （300ms）接管补判
    RECT will = { x, y, x + pw, y + ph };
    if (m_acrylic_active && !m_visible) {
        RefreshBackdrop(will);
        m_last_snap_rect = will;
        m_last_refresh_tick = GetTickCount64();
    } else if (m_acrylic_active && m_affinity_ok &&
               !EqualRect(&will, &m_last_snap_rect) &&
               GetTickCount64() - m_last_refresh_tick >= 250) {
        bool text_dark_before = m_text_dark;
        bool adapt_before = m_text_adaptive;
        RefreshBackdrop(will);
        m_last_snap_rect = will;
        m_last_refresh_tick = GetTickCount64();
        if (m_text_dark != text_dark_before || m_text_adaptive != adapt_before) {
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
    }

    // 双层呈现：blur 效果层先就位（同 rect），内容层随后压在其上
    if (m_blur_hwnd) {
        SetWindowPos(m_blur_hwnd, HWND_TOPMOST, x, y, pw, ph,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, pw, ph,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Update window region for rounded corners (fixes black corner issue).
    // Skipped in layered mode: the rounded shape lives in the ULW alpha
    // (SetWindowRgn would tear the layered surface).
    if (!m_acrylic_active) {
        HRGN rgn = CreateRoundRectRgn(0, 0, pw, ph, CORNER_RADIUS * 2, CORNER_RADIUS * 2);
        SetWindowRgn(m_hwnd, rgn, FALSE);
    }

    m_visible = true;

    // 重采样的「清空表面 → 新表面」之间不落消息间隙：同步画一帧提交
    // ULW，DWM 合成不到中间的全透明帧（重定位/拖动跟随路径全走这里）
    if (m_acrylic_active) UpdateWindow(m_hwnd);

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

void CTooltipPopup::ApplyBackdrop(int effect, int opacity) {
    if (!m_hwnd) return;
    if (opacity < 10) opacity = 10;
    if (opacity > 100) opacity = 100;
    if (effect < 0 || effect > 3) effect = 0;
    // 低系统门控：SWCA accent 3/4（真系统磨砂）Win10 1703 起才渲染，更早
    // 系统亚克力/模糊档自动降级透明档——黑窗 blur 层在 Win7 Aero 上近全
    // 黑、Basic（无 DWM）下纯黑块，不可交付（2026-10-16 兼容审查）
    if ((effect == 0 || effect == 2) && !PNM_IsAccentBlurSupported())
        effect = 1;

    // 模式集：0=亚克力 1=透明 2=模糊 3=不透明。透明系三档（0/1/2）全部
    // 走 ULW per-pixel（实测本机 DWM 对含内容的 SWCA 窗口忽略 tint alpha，
    // 浓度不可控——ULW 是唯一稳定通道）。磨砂由自绘模糊管线提供（方案
    // B）：亚克力=模糊烘焙+噪声，模糊=纯模糊烘焙，透明=真半透明不烘焙
    bool want_layered = (effect != 3);
    int target = want_layered ? 1 : 0;
    bool dark = IsDarkMode();
    if (target == m_backdrop_mode &&
        (target != 1 || (effect == m_backdrop_effect &&
                         dark == m_acrylic_dark &&
                         opacity == m_backdrop_opacity)))
        return;  // idempotent（效果/主题/浓度任一变化都要重建呈现状态）

    // --- Tear down: leave layered mode ----------------------------------
    if (m_backdrop_mode == 1 && target != 1) {
        m_acrylic_active = false;
        // ULW 用过 WS_EX_LAYERED：复位成普通窗口（下一轮显示前普通
        // 双缓冲路径直接 BitBlt 即可，LWA_ALPHA 从未设置过）
        LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED)
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    }

    if (target == 1) {
        // 上层内容窗：per-pixel layered。region 必须清空（region 裁剪会
        // 撕裂 ULW 表面），圆角与浓度由 ComposeLayeredSurface 写进 alpha
        SetWindowRgn(m_hwnd, nullptr, FALSE);
        LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        // blur 效果层：亚克力/模糊档挂（SWCA 系统实时模糊）；「透明」档
        // 无模糊。亚克力的 Fluent 质感=blur 层+面板噪声纹理（m_noise_
        // acrylic），模糊档=纯磨砂无噪声——两档在 26300 上由此可辨
        // （SWCA st3/st4 渲染已合并，实测无差）
        m_noise_acrylic = (effect == 0);
        if (effect == 0 || effect == 2) {
            EnsureBlurLayer();
        } else if (m_blur_hwnd) {
            DestroyBlurLayer();
        }
        m_acrylic_active = true;
        m_acrylic_dark = dark;
    } else {
        // 不透明：普通窗口，圆角 region 回位——layered 路径清过它；
        // 窗口缩放时 PositionWindow 也会重设
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        HRGN rgn = CreateRoundRectRgn(0, 0, rc.right, rc.bottom,
                                      CORNER_RADIUS * 2, CORNER_RADIUS * 2);
        SetWindowRgn(m_hwnd, rgn, FALSE);
        if (m_blur_hwnd) DestroyBlurLayer();
    }

    m_backdrop_mode = target;
    m_backdrop_effect = effect;
    m_backdrop_opacity = opacity;
    // 效果/浓度变了：反色判定基准作废，下次显示时重判
    m_last_snap_rect = {};
    InvalidateRect(m_hwnd, NULL, FALSE);
}

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

    // Re-check dark mode（acrylic tint 已随主题分档，ApplyBackdrop 幂等
    // 判断同样按主题重设；可见期间的 UpdateData 刷新不重算本成员——tint
    // 与文字色同源于主题缓存，切主题在下次 UpdateAndShow 统一生效）
    m_dark_mode = IsDarkMode();

    // Backdrop effect/opacity: applied before the window is shown (and
    // before the DPI pass below - RecreateScaledResources reads the acrylic
    // state), so there is no visible "opaque first, translucent later" flash
    ApplyBackdrop(CProcessNetPlugin::Instance().m_detail.GetPopupEffect(),
                  CProcessNetPlugin::Instance().m_detail.GetPopupOpacity());

    // Per-monitor DPI: the anchor may sit on another monitor with different scaling
    UpdateDpiScale(anchor_rect);

    if (UseDarkTheme() != m_pen_dark) {
        // Recreate separator pen for the new panel theme（分隔线是面板
        // 装饰，跟面板主题；反色翻转只动文字深浅，不该动它）
        if (m_pen_separator) DeleteObject(m_pen_separator);
        m_pen_separator = CreatePen(PS_SOLID, 1, UseDarkTheme() ? RGB(45, 45, 45) : RGB(235, 235, 235));
        m_pen_dark = UseDarkTheme();
    }

    // Resize and show
    HDC hdc = GetDC(m_hwnd);
    int pw = 0, ph = 0, up_c = 0, down_c = 0;
    CalcLayout(hdc, pw, ph, up_c, down_c);
    ReleaseDC(m_hwnd, hdc);

    SetWindowPos(m_hwnd, NULL, 0, 0, pw, ph, SWP_NOMOVE | SWP_NOZORDER);
    if (m_blur_hwnd)
        SetWindowPos(m_blur_hwnd, NULL, 0, 0, pw, ph,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Fresh show session (first show, or the anchor moved): reset the
    // avoidance state - a full PositionWindow pass follows.
    m_avoided = false;
    m_no_overlap_tick = 0;
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
        // 静止期反色判定（方案 A'）：背景磨砂由 blur 层 DWM 逐帧实时合
        // 成无需插件刷新，只有文字深浅需要低频跟随背景亮度。矩形与判定
        // 基准不同=刚移动过（PositionWindow 移动路径已即时判定）：重置
        // 基准与节流钟，稳定 300ms 后本循环接管——否则旧基准永不相等，
        // 静止判定一次都不会跑（2026-10-16 咩咩实测字不实时反色的根
        // 因）。affinity 不可用（老系统）不跑；判定变化才重绘
        RECT cur;
        GetWindowRect(m_hwnd, &cur);
        if (m_acrylic_active && m_affinity_ok) {
            if (!EqualRect(&cur, &m_last_snap_rect)) {
                m_last_snap_rect = cur;
                m_last_refresh_tick = GetTickCount64();
            } else if (GetTickCount64() - m_last_refresh_tick >= 300) {
                bool text_dark_before = m_text_dark;
                bool adapt_before = m_text_adaptive;
                RefreshBackdrop(cur);
                m_last_refresh_tick = GetTickCount64();
                if (m_text_dark != text_dark_before || m_text_adaptive != adapt_before) {
                    InvalidateRect(m_hwnd, NULL, FALSE);
                }
            }
        }
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
            // Size changed: resize and reposition (anchor may need re-clamping),
            // unless the cursor is near - freeze rule: keep the top-left and
            // change the size only, so the "查看详细" button stays put.
            SetWindowPos(m_hwnd, NULL, 0, 0, pw, ph, SWP_NOMOVE | SWP_NOZORDER);
            if (m_blur_hwnd)
                SetWindowPos(m_blur_hwnd, NULL, 0, 0, pw, ph,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (!IsCursorNear())
                PositionWindow(m_last_anchor);
        }
        InvalidateRect(m_hwnd, NULL, FALSE);
    }
}

bool CTooltipPopup::IsCursorNear() const {
    if (!m_hwnd) return false;
    POINT pt;
    GetCursorPos(&pt);
    // The cursor hovering the anchor (TM window, incl. its taskbar child
    // dialog) does NOT count as near: the popup always sits right next to
    // the anchor, so the initial hover would permanently freeze avoidance
    // and let the native tooltip sit on top of the popup (2026-09-21
    // screenshot). "Near" starts once the cursor leaves the anchor and
    // approaches the popup - i.e. the user is heading for the "查看详细"
    // button.
    if (m_anchor_hwnd && IsWindow(m_anchor_hwnd)) {
        HWND hover = WindowFromPoint(pt);
        for (HWND w = hover; w; w = GetParent(w)) {
            if (w == m_anchor_hwnd) return false;
        }
    }
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    InflateRect(&rc, (int)(32 * m_dpi_scale), (int)(32 * m_dpi_scale));
    return PtInRect(&rc, pt) != 0;
}

// Reposition policy (2026-09-21, "查看详细" button missed):
//  1. Only react when a native tooltip actually overlaps the popup rect
//     (same 8px inflation as PositionWindow's avoidance loop). The tooltip
//     follows the cursor and is rebuilt every second (text width changes),
//     so reacting to mere rect changes pushed the popup across the screen.
//  2. Freeze ALL movement while the cursor is near (within 32px of) the
//     popup: the user is heading for the button - overlapping the tooltip
//     is acceptable, moving the button is not.
//  3. After being nudged away, return to the anchor position only once the
//     tooltips have stayed clear for 500ms AND the cursor is not near.
bool CTooltipPopup::RepositionIfTooltipsChanged() {
    if (!m_visible) return false;

    std::vector<RECT> tips;
    FindTMTooltips(m_anchor_hwnd, tips);

    RECT self;
    GetWindowRect(m_hwnd, &self);
    int tip_margin = max(4, (int)(8 * m_dpi_scale));
    bool overlap = false;
    for (const auto& tip : tips) {
        if (RectsIntersect(self, InflateRectScreen(tip, tip_margin, tip_margin))) {
            overlap = true;
            break;
        }
    }

    if (overlap) {
        m_no_overlap_tick = 0;
        if (!IsCursorNear()) {
            PositionWindow(m_last_anchor);
            m_avoided = true;
            return true;
        }
        return false;
    }

    // No overlap: nothing to do unless we were nudged away earlier.
    if (!m_avoided) return false;

    ULONGLONG now = GetTickCount64();
    if (m_no_overlap_tick == 0) m_no_overlap_tick = now;
    if (now - m_no_overlap_tick >= 500 && !IsCursorNear()) {
        m_avoided = false;
        m_no_overlap_tick = 0;
        PositionWindow(m_last_anchor);
        return true;
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
    // 微软雅黑 + ClearType（咩咩点名，2026-10-16）：coverage 双底 pass 在
    // 已知不透明底上渲染，ClearType 可用；解出的内容色带 sub-pixel 加权，
    // 叠到面板色上时边缘会有轻微彩虹纹（ClearType-on-wrong-background），
    // 深色面板/噪声亚克力下基本不可见。若实测彩边扎眼，回退方案是
    // ANTIALIASED_QUALITY 或在 Compose 里向亮度均值收敛三通道
    // 字号/字重（2026-10-16 咩咩反馈「变大变粗」）：雅黑字面比 Segoe UI
    // 大一圈（汉字撑满 em 框），-16 视觉大一号；SEMIBOLD 叠雅黑 Regular
    // 本身的粗笔画=明显粗体。回归 FW_NORMAL + -14（雅黑≈Segoe×7/8 视觉
    // 等价），字体与 ClearType 保留
    m_font_normal = CreateFontW((int)(-14 * m_dpi_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    m_font_small = CreateFontW((int)(-13 * m_dpi_scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    m_pen_separator = CreatePen(PS_SOLID, 1, UseDarkTheme() ? RGB(45, 45, 45) : RGB(235, 235, 235));
    m_pen_dark = UseDarkTheme();
}

void CTooltipPopup::Hide() {
    if (m_visible) {
        ShowWindow(m_hwnd, SW_HIDE);
        if (m_blur_hwnd) ShowWindow(m_blur_hwnd, SW_HIDE);
        m_visible = false;
        m_hovering = false;
        m_tracking = false;
        m_hover_up = m_hover_down = m_hover_btn = false;
        // 反色判定基准过期：下次显示时 PositionWindow 必重判（隐含期间
        // 背后内容已变化）
        m_last_snap_rect = {};
    }
}

// Public helper to get cached proc display info (UI thread: locks inside)
std::vector<CTooltipPopup::ProcDisplayInfo> CProcessNetPlugin::GetCachedProcDisplayInfo() {
    std::vector<CTooltipPopup::ProcDisplayInfo> out;
    std::vector<ProcTraffic> snap;
    {
        EnterCriticalSection(&m_data_lock);
        snap = m_cached_stats;
        LeaveCriticalSection(&m_data_lock);
    }
    GetProcessDisplayInfo(out, snap);
    return out;
}


