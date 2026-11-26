#include "detail_window.h"
#include "history_calendar.h"
#include "history_load.h"
#include "plugin_main.h"
#include "utils.h"
#include "signature.h"
#include "ip_geo.h"
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <ctime>
#include <fstream>
#include <filesystem>

#pragma comment(lib, "imm32.lib")

// Kept in the binary so support reports can identify the native chart build.
extern "C" __declspec(dllexport) const char PNM_HOURLY_UI_BUILD[] = "PNM-HOURLY-UI-V2";

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

namespace {
using Calendar = pnm::HistoryCalendar;

std::wstring DateText(Calendar::Number date) {
    const auto d = Calendar::AsUtc(date);
    wchar_t text[16];
    swprintf_s(text, L"%04u-%02u-%02u", unsigned(d.wYear), unsigned(d.wMonth), unsigned(d.wDay));
    return text;
}

struct DateDialog {
    Calendar::DateRange range;
    Calendar::Number today;
    bool single;
    Calendar::Number oldest = 0;   // 日期可选的最早一天；0 = 保留窗口的起点
    HWND first = nullptr, last = nullptr;
    HFONT font = nullptr;

    static INT_PTR CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* dialog = reinterpret_cast<DateDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER));
        if (message == WM_INITDIALOG) {
            dialog = reinterpret_cast<DateDialog*>(lp);
            SetWindowLongPtrW(hwnd, DWLP_USER, lp);
            SetWindowTextW(hwnd, dialog->single ? TR(L"图表日期") : TR(L"自定义日期"));
            const auto font = dialog->font;
            auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                               int id, int x, int y, int w, int h) {
                RECT rc{x, y, x + w, y + h};
                MapDialogRect(hwnd, &rc);
                auto child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                    rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                    hwnd, reinterpret_cast<HMENU>(INT_PTR(id)), nullptr, nullptr);
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
                return child;
            };
            auto picker = [&](const wchar_t* label, int y, Calendar::Number date) {
                control(L"STATIC", label, SS_CENTERIMAGE, -1, 10, y, 112, 20);
                const auto child = control(DATETIMEPICK_CLASSW, nullptr, WS_TABSTOP | DTS_SHORTDATEFORMAT,
                                           y + 100, 128, y, 112, 20);
                SYSTEMTIME limits[] = {Calendar::AsUtc(dialog->oldest ? dialog->oldest
                                                                     : Calendar::OldestDate(dialog->today)),
                                       Calendar::AsUtc(dialog->today)};
                auto value = Calendar::AsUtc(date);
                SendMessageW(child, DTM_SETFORMATW, 0, reinterpret_cast<LPARAM>(L"yyyy-MM-dd"));
                SendMessageW(child, DTM_SETMCFONT, reinterpret_cast<WPARAM>(font), FALSE);
                DateTime_SetRange(child, GDTR_MIN | GDTR_MAX, limits);
                DateTime_SetSystemtime(child, GDT_VALID, &value);
                return child;
            };
            dialog->first = picker(dialog->single ? TR(L"图表日期") : TR(L"开始日期"), 12, dialog->range.first);
            if (!dialog->single) dialog->last = picker(TR(L"结束日期（含当天）"), 38, dialog->range.last);
            const int actionY = dialog->single ? 42 : 68;
            control(L"BUTTON", TR(L"应用"), WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, 116, actionY, 58, 20);
            control(L"BUTTON", TR(L"取消"), WS_TABSTOP, IDCANCEL, 182, actionY, 58, 20);
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
            if (!dialog->first || (!dialog->single && !dialog->last)) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            SetFocus(dialog->first);
            return FALSE;
        }
        if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        if (message == WM_COMMAND && LOWORD(wp) == IDOK && dialog) {
            const auto read = [](HWND child) -> Calendar::Number {
                SYSTEMTIME d{};
                return DateTime_GetSystemtime(child, &d) == GDT_VALID ? Calendar::AsUnix(d) / Calendar::Day * Calendar::Day : 0;
            };
            const auto first = read(dialog->first);
            Calendar::DateRange range{first, dialog->single ? first : read(dialog->last)};
            if (!range.Valid(dialog->today)) {
                MessageBoxW(hwnd, TR(L"开始日期不能晚于结束日期。"), TR(L"自定义日期"), MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            dialog->range = range;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        return FALSE;
    }

    bool Show(HINSTANCE instance, HWND owner, HFONT bodyFont) {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_DATE_CLASSES};
        if (!InitCommonControlsEx(&controls)) return false;
        // A standard dialog template with all controls created in WM_INITDIALOG.
        // The template takes the style as two WORDs; mask instead of truncating a
        // constant, which the compiler flags (C4310).
        constexpr DWORD kStyle = static_cast<DWORD>(WS_POPUP) | WS_CAPTION | WS_SYSMENU |
                                 DS_MODALFRAME | DS_CENTER;
        alignas(DWORD) const WORD layout[] = {
            WORD(kStyle & 0xFFFF), WORD(kStyle >> 16),
            0, 0, 0, 0, 0, 250, WORD(single ? 72 : 98), 0, 0, 0
        };
        // Keep a copy alive even if the owner recreates its fonts during the modal loop.
        LOGFONTW description{};
        if (bodyFont && GetObjectW(bodyFont, sizeof(description), &description))
            font = CreateFontIndirectW(&description);
        const auto result = DialogBoxIndirectParamW(instance, reinterpret_cast<const DLGTEMPLATE*>(layout), owner,
                                                    Proc, reinterpret_cast<LPARAM>(this));
        if (font) DeleteObject(font);
        font = nullptr;
        return result == IDOK;
    }
};

void DrawDateButton(HDC hdc, RECT rect, const wchar_t* text, COLORREF fill, COLORREF border, COLORREF color) {
    const auto brush = CreateSolidBrush(fill);
    const auto pen = CreatePen(PS_SOLID, 1, border);
    const auto oldBrush = SelectObject(hdc, brush);
    const auto oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, 4, 4);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text, -1, &rect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}
}

CDetailWindow::CDetailWindow() {
    s_instance = this;
    m_dark_mode = IsDarkMode();
    m_dark_mode_tick = GetTickCount64();
    m_last_save_tick = GetTickCount64();  // prevent immediate overwrite on first UpdateData
}

CDetailWindow::~CDetailWindow() {
    OutputDebugStringW(L"[PNM] ~CDetailWindow: calling SaveHistory");
    SaveSettings();
    if (!g_shutting_down) SaveHistory(true);
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
    // 类注销（与悬浮窗同款，review C2）：热卸载重载场景防新 DLL 沿用
    // 指向已卸载代码的残留类；进程退出时窗口未销毁则反注册失败，无害
    if (m_hinst) {
        wchar_t cls[64];
        swprintf_s(cls, 64, L"PNMDetail_%p", (void*)this);
        UnregisterClassW(cls, m_hinst);
    }
    s_instance = nullptr;
}

bool CDetailWindow::IsDarkMode() {
    // 插件设置的颜色模式优先：强制深/浅直接短路，不再读系统
    if (s_color_mode == 1) return true;
    if (s_color_mode == 2) return false;
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

// 缓存 key 容量保护（review 低危1，与悬浮窗同款）：路径超长时
// swprintf_s 失败 key 置空串，超长路径进程共享空 key、图标张冠李戴。
// 超长路径不参与缓存与提取（留空位）
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

// 只查缓存（O(1) 零 IO）。miss 记入提取队列（去重）并返回 nullptr——
// 绘制画空位占位，真图标由 PumpIconExtraction 分帧提取后的重绘填充
HICON CDetailWindow::LookupProcessIcon(const std::wstring& exe_path) {
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
// PumpIconExtraction 调用——详情窗进程表几十上百行，GetImage 未命中
// 缓存时单个 5-20ms 的同步磁盘 IO 直接放 WM_PAINT 里首帧可卡
// 0.25-1s+（review P2）
HICON CDetailWindow::ExtractProcessIcon(const std::wstring& exe_path) {
    bool net = PNM_IsNetworkPath(exe_path);
    int want = max(16, ICON_SIZE);
    HICON hIcon = nullptr;

    // 首选：Shell 图像工厂按目标尺寸渲染（内部从大图标高质量下缩并
    // 保留 alpha 抗锯齿；ARGB HBITMAP 经 CreateIconIndirect 转 HICON）
    if (!net) {
        IShellItemImageFactory* factory = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(exe_path.c_str(), nullptr,
                                                  IID_PPV_ARGS(&factory)))) {
            SIZE sz = { want, want };
            HBITMAP hbm = nullptr;
            if (SUCCEEDED(factory->GetImage(sz, SIIGBF_ICONONLY, &hbm)) && hbm) {
                BITMAP bm = {};
                int iw = 0;
                if (GetObjectW(hbm, sizeof(bm), &bm)) iw = bm.bmWidth;
                if (iw > 0 && iw <= want * 2) {
                    HBITMAP mask = CreateBitmap(iw, bm.bmHeight, 1, 1, nullptr);
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
        // 兜底 1：大图标（32px 源，优于 SMALLICON 的 16px）。
        // USEFILEATTRIBUTES 不碰文件系统——零磁盘 IO，网络路径同样可用
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(exe_path.c_str(), 0, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
            hIcon = sfi.hIcon;
        }
    }
    if (!hIcon && !net) {
        // 兜底 2：ExtractAssociatedIcon（要打开文件——网络路径跳过）
        wchar_t path_buf[MAX_PATH] = {};
        wcsncpy_s(path_buf, MAX_PATH, exe_path.c_str(), _TRUNCATE);
        WORD idx = 0;
        hIcon = ExtractAssociatedIconW(m_hinst, path_buf, &idx);
    }
    return hIcon;
}

// 分帧提取：一次最多 count 个（含失败 nullptr 的负缓存——防止不可取图
// 的路径每帧重复入队）。有产出返回 true，调用方触发重绘
bool CDetailWindow::PumpIconExtraction(int count) {
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

static int SortArrowSpace(float scale) {
    return max(8, (int)(16 * scale));
}

static void DrawSortArrow(HDC hdc, const RECT& slot, bool ascending, COLORREF color) {
    const int width = slot.right - slot.left;
    const int height = slot.bottom - slot.top;
    if (width < 4 || height < 4) return;
    const int half_base = max(1, width / 3);
    const int arrow_height = min(height - 2, width / 2);
    const int cx = slot.left + width / 2;
    const int top = slot.top + (height - arrow_height) / 2;
    const int bottom = top + arrow_height;
    const int base_y = ascending ? bottom : top;
    POINT triangle[] = {
        { cx - half_base, base_y },
        { cx + half_base, base_y },
        { cx, ascending ? top : bottom },
    };
    HBRUSH brush = CreateSolidBrush(color);
    if (!brush) return;
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    HGDIOBJ old_pen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, triangle, ARRAYSIZE(triangle));
    SelectObject(hdc, old_pen);
    SelectObject(hdc, old_brush);
    DeleteObject(brush);
}

// Measure header text and fit column widths to the current language.
// shrink=true : reset to DPI baseline first, then expand - language switch
//               shrinks back columns of a shorter language (bi-directional).
// shrink=false: expand only, never shrink (safe one-shot after font creation).
void CDetailWindow::AutoSizeColumns(bool shrink) {
    if (!m_font_header) return;
    HDC hdc = GetDC(m_hwnd);
    HFONT hOld = (HFONT)SelectObject(hdc, m_font_header);

    // 主表列（rt/hist）：标题 + 内边距 + 排序箭头余量
    auto adjust = [&](Column* cols, const int* bases, int count) {
        for (int i = 1; i < count; i++) {
            if (!cols[i].title || !cols[i].title[0]) continue;
            int measured = PNM_MeasureText(hdc, m_font_header, cols[i].title);
            int needed = measured + 12 + SortArrowSpace(m_dpi_scale);
            if (shrink && bases) cols[i].width = bases[i];
            if (needed > cols[i].width) cols[i].width = needed;
        }
    };
    adjust(m_rt_cols, m_base_rt_widths, NUM_COLS);
    adjust(m_hist_cols, m_base_hist_widths, NUM_COLS);

    // 连接表：协议(0)/状态(4)是固定列，按标题实测；地址类弹性列由 UpdateConnColWidths 每帧分配
    for (int i : { 0, 4 }) {
        if (!m_conn_cols[i].title || !m_conn_cols[i].title[0]) continue;
        int measured = PNM_MeasureText(hdc, m_font_header, m_conn_cols[i].title);
        int needed = measured + 16;  // 无排序箭头，左右边距
        if (shrink) m_conn_cols[i].width = m_base_conn_widths[i];
        if (needed > m_conn_cols[i].width) m_conn_cols[i].width = needed;
    }

    SelectObject(hdc, hOld);
    ReleaseDC(m_hwnd, hdc);
    m_cols_sized = true;
}

// 内容所需最小窗口宽度：最宽 tab 的列宽总和 + 内边距 + 滚动条
int CDetailWindow::ComputeMinWidth() const {
    int rt_sum = 0, hist_sum = 0;
    for (int i = 0; i < NUM_COLS; i++) {
        rt_sum += m_rt_cols[i].width;
        hist_sum += m_hist_cols[i].width;
    }
    int need = (rt_sum > hist_sum ? rt_sum : hist_sum) + PADDING * 2 + SCROLL_W + 2;
    if (need < MIN_WIDTH) need = MIN_WIDTH;
    return need;
}

// 窗口宽度跟随内容：
// - 用户拉伸过窗口（m_saved_w > 0）→ 只保底不收缩（新语言需要更宽才扩展）
// - 从未拉伸过 → 完全跟随当前语言内容（语言切换可收缩/扩展）
void CDetailWindow::FitWindowWidth() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    int cur_w = rc.right - rc.left;
    int need = ComputeMinWidth();
    int target = need;
    if (m_saved_w > 0 && m_saved_w > target) target = m_saved_w;
    // 不超出所在显示器工作区
    RECT work, mon;
    PNM_GetWorkAreaForRect(rc, work, mon);
    int max_w = (work.right - work.left) - 20;
    if (target > max_w) target = max_w;
    if (target != cur_w) {
        SetWindowPos(m_hwnd, nullptr, 0, 0, target, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void CDetailWindow::InitColumns() {
    // NOT static - TR() must re-evaluate on every call (language may have changed)
    const wchar_t* rt_titles[NUM_COLS] = {
        L"", TR(L"\u7A0B\u5E8F\u540D\u79F0"), TR(L"\u7A0B\u5E8F\u7C7B\u522B"),
        TR(L"\u4E0B\u8F7D\u901F\u5EA6"), TR(L"\u4E0A\u4F20\u901F\u5EA6"),
        TR(L"\u8FDE\u63A5\u6570"), TR(L"\u64CD\u4F5C")
    };
    for (int i = 0; i < NUM_COLS; i++) m_rt_cols[i].title = rt_titles[i];

    const wchar_t* hist_titles[NUM_COLS] = {
        L"", TR(L"\u7A0B\u5E8F\u540D\u79F0"), TR(L"\u7A0B\u5E8F\u7C7B\u522B"),
        TR(L"\u603B\u4E0B\u8F7D"), TR(L"\u603B\u4E0A\u4F20"),
        TR(L"\u5E73\u5747\u4E0B\u8F7D"), TR(L"\u5E73\u5747\u4E0A\u4F20")
    };
    for (int i = 0; i < NUM_COLS; i++) m_hist_cols[i].title = hist_titles[i];

    const wchar_t* conn_titles[NUM_CONN_COLS] = {
        TR(L"\u534F\u8BAE"), TR(L"\u672C\u5730\u5730\u5740"),
        TR(L"\u8FDC\u7A0B\u5730\u5740"), TR(L"\u5F52\u5C5E\u5730"), TR(L"\u72B6\u6001")
    };
    for (int i = 0; i < NUM_CONN_COLS; i++) m_conn_cols[i].title = conn_titles[i];
    m_cols_sized = false;  // force re-measure on next paint
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

    // 无 WS_THICKFRAME：DWM 对带它的顶层窗口强制绘制主题边框（激活=强调色、
    // 失焦=白的 1px 线，issue #13），DWMWA_BORDER_COLOR 对 WS_POPUP 不生效、
    // WM_NCPAINT 自绘也盖不住（DWM 合成层在自绘之上），唯一能盖住的
    // WM_NCCALCSIZE 全吞客户区又会在拖动时撕裂（DWM frame 几何与客户区
    // 失配）。索性不要 sizing border：DWM 无边框可画，线物理消失；resize
    // 由 BeginResizeDrag/UpdateResizeDrag/EndResizeDrag 自己实现。
    // WS_EX_APPWINDOW（issue #18）：窗口在任务栏保留按钮，最小化后点任务栏
    // 图标恢复原大小（火绒式）。此前是 WS_EX_TOOLWINDOW——无任务栏按钮，
    // SW_MINIMIZE 退化成 iconic 小标题条，用户没有任何恢复入口。
    // WS_SYSMENU|WS_MINIMIZEBOX（issue #18 反馈）：任务栏按钮点击的 toggle
    // 最小化分支只对「可最小化」窗口生效（explorer 看这两个样式位），纯
    // WS_POPUP 点任务栏图标只激活不最小化。二者在无 WS_CAPTION 的 WS_POPUP
    // 上不绘制任何非客户区，自绘外观不变；MAXIMIZEBOX 有意不给（不支持
    // 最大化）。
    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        className, L"ProcessNetMonitor",
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, MIN_WIDTH + 40, MIN_HEIGHT + 80,
        NULL, NULL, m_hinst, NULL);

    if (!m_hwnd) return false;

    // Signature verification worker notifies this window on completion
    SignatureCache::Instance().SetNotifyHwnd(m_hwnd);

    // Now that window exists, get accurate DPI for this monitor
    UpdateDpiScale(m_hwnd);

    SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC, (LONG_PTR)DetailStaticWndProc);

    ApplyDwmFramePolicy();

    CreateFonts();
    AutoSizeColumns();  // one-shot after fonts exist
    m_pen_border = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
    m_pen_border_exp = CreatePen(PS_SOLID, 1, m_dark_mode ? RGB(42, 42, 46) : RGB(238, 238, 238));
    m_br_row[0] = CreateSolidBrush(GetBgColor());
    m_br_row[1] = CreateSolidBrush(m_dark_mode ? RGB(35, 35, 39) : RGB(245, 245, 245));
    m_br_hover = CreateSolidBrush(m_dark_mode ? RGB(48, 48, 54) : RGB(232, 232, 232));
    m_br_child = CreateSolidBrush(m_dark_mode ? RGB(36, 36, 40) : RGB(247, 247, 247));

    return true;
}

void CDetailWindow::ApplyDwmFramePolicy() {
    // 窗口已无 WS_THICKFRAME，DWM 不再有边框可画（issue #13 的线、拖动
    // 撕裂随之物理消失），这里只剩圆角偏好需要维护——它在 DPI 变化后
    // 可能被系统重置，所以 WM_DPICHANGED / 每次显示前都会重设。
    // DWMWA_BORDER_COLOR 不再设置：该属性对 WS_POPUP 窗口不生效。
    int corner_pref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                          &corner_pref, sizeof(corner_pref));
    // 把玻璃框延伸进客户区 1px：让 DWM 重新把这扇窗口当作「有框窗」
    // 合成、找回系统投影（Chrome/Edge 无边框窗口同款手法）；延伸区被
    // 自绘内容盖住，视觉上只多投影。themed border 线跟随的是
    // WS_THICKFRAME 样式而非这个调用，理论上不会回来——实测验证。
    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
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
        m_base_rt_widths[i] = (int)(BASE_RT_WIDTHS[i] * m_dpi_scale);
        m_base_hist_widths[i] = (int)(BASE_HIST_WIDTHS[i] * m_dpi_scale);
        m_rt_cols[i].width = m_base_rt_widths[i];
        m_hist_cols[i].width = m_base_hist_widths[i];
    }
    for (int i = 0; i < NUM_CONN_COLS; i++) {
        m_base_conn_widths[i] = (int)(BASE_CONN_WIDTHS[i] * m_dpi_scale);
        m_conn_cols[i].width = m_base_conn_widths[i];
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
    // 行字体变了（初始化/DPI 变化）→ 归属地换行行数变，行高缓存整批失效
    m_geo_layout_serial++;
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

void CDetailWindow::RecreateThemeColors() {
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

void CDetailWindow::SetColorMode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    if (s_color_mode == mode) return;
    s_color_mode = mode;
    bool old_dark = m_dark_mode;
    m_dark_mode = IsDarkMode();
    if (m_dark_mode != old_dark) RecreateThemeColors();
    if (m_hwnd) InvalidateRect(m_hwnd, NULL, FALSE);
    // hover 悬浮窗不在此处刷新：它每次显示前都会重读 IsDarkMode()，
    // 拿到的已是新模式；强刷反而要重建字体等一串对象
}

void CDetailWindow::Show(HWND parent_wnd) {
    m_parent_wnd = parent_wnd;
    bool old_dark = m_dark_mode;
    m_dark_mode = IsDarkMode();
    if (m_dark_mode != old_dark) RecreateThemeColors();

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
    // 窗口宽度跟随当前语言内容（用户拉伸过则保底），高度不自动：
    // 没手动拉过 → 固定默认 560*DPI；手动拉过 → 记住拉过的值
    AutoSizeColumns(true);
    FitWindowWidth();
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    int w = rc.right - rc.left;
    int h = m_saved_h > 0 ? m_saved_h : (int)(800 * m_dpi_scale);
    if (h > ah - 40) h = ah - 40;
    int x = work.left + (aw - w) / 2;
    int y = work.top + (ah - h) / 2;

    // DWM frame attributes can be reset by the first show or by hide/show
    // cycles - reapply right before the window goes visible so the themed
    // border stays suppressed (the "white line" of issue #13).
    ApplyDwmFramePolicy();
    // issue #18：最小化状态下 Hide/Show 循环不清 iconic 位，SetWindowPos 的
    // 尺寸会被系统忽略、窗口照 iconic 尺寸显示——先以不激活方式恢复正常
    // 尺寸再走下面的定位
    if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    m_visible = true;
    InvalidateRect(m_hwnd, NULL, FALSE);
    ShowHistoryNotice();
}

void CDetailWindow::ShowHistoryNotice() {
    if (!m_visible || !m_history_notice) return;
    const auto message = m_history_notice;
    m_history_notice = nullptr;
    MessageBoxW(m_hwnd, TR(message), TR(L"历史流量"), MB_OK | MB_ICONWARNING);
}

void CDetailWindow::Hide() {
    // 拉伸进行中被隐藏（ESC 等）：清掉状态，否则残留的起始矩形会在
    // 下次显示后污染第一条 WM_MOUSEMOVE
    if (m_resize_hit) {
        m_resize_hit = 0;
        ::ReleaseCapture();
    }
    if (m_visible) {
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
    }
}

// ============================================================
// History recording
// ============================================================

void CDetailWindow::RecordHistory(const std::vector<ProcTraffic>& stats, bool usesEtw,
                                  ULONGLONG sampleTick, ULONGLONG sampleWall) {
    
    // Aggregate bytes by process name (multiple PIDs may share same name)
    struct AggBytes { uint64_t recv = 0, sent = 0; std::wstring exe_path; };
    std::map<std::wstring, AggBytes> agg;
    for (auto& st : stats) {
        auto& a = agg[st.name];
        a.recv += st.bytes_recv;
        a.sent += st.bytes_sent;
        if (a.exe_path.empty() && !st.exe_path.empty()) a.exe_path = st.exe_path;
    }
    
    if (m_hourly_writable && sampleWall && sampleTick != m_hourly_tick) {
        const auto elapsed = sampleTick > m_hourly_tick ? sampleTick - m_hourly_tick : 0;
        const auto wallElapsed = sampleWall >= m_hourly_wall ? sampleWall - m_hourly_wall : 0;
        const bool continuous = m_hourly_tick && elapsed && elapsed <= 10000 &&
            sampleWall >= m_hourly_wall && wallElapsed <= elapsed + 2000 &&
            elapsed <= wallElapsed + 2000 && usesEtw == m_hourly_etw;
        bool changed = m_hourly.Prune(sampleWall);
        std::unordered_map<std::wstring, CumBytes> next;
        for (const auto& [name, value] : agg) {
            if (!value.exe_path.empty()) m_history_exe_paths[name] = value.exe_path;
            next.emplace(name, CumBytes{value.recv, value.sent});
            const auto prev = m_hourly_previous.find(name);
            if (!continuous || prev == m_hourly_previous.end() ||
                value.recv < prev->second.recv || value.sent < prev->second.sent) continue;
            changed |= m_hourly.Add(sampleWall, std::u16string(name.begin(), name.end()),
                value.recv - prev->second.recv, value.sent - prev->second.sent);
        }
        if (changed) m_history_dirty = true;
        m_hourly_dirty |= changed;
        m_hourly_previous.swap(next);
        m_hourly_tick = sampleTick;
        m_hourly_wall = sampleWall;
        m_hourly_etw = usesEtw;
    }

}

void CDetailWindow::ClearHistory() {
    if (!m_hourly_writable) {
        LoadHourlyHistory();
        if (!m_hourly_writable) {
            ShowHistoryNotice();
            return;
        }
    }
    if (m_hourly_writable) {
        m_hourly = {};
        m_hourly_previous.clear();
        m_hourly_tick = 0;
        m_hourly_dirty = true;
        m_hist_cache_valid = false;   // 库已清空，汇总缓存作废
    }
    m_history_exe_paths.clear();
    m_rows.clear();
    m_hist_total_recv = 0;
    m_hist_total_sent = 0;
    m_history_dirty = true;

    SaveHistory(true);
    if (m_hourly_dirty) m_history_notice = L"无法保存小时历史，将重试。";
    ShowHistoryNotice();

    if (m_visible) InvalidateRect(m_hwnd, NULL, FALSE);
}

std::pair<ULONGLONG, ULONGLONG> CDetailWindow::HistoryDates(ULONGLONG today) const {
    const auto day = Calendar::Day;
    switch (m_time_range) {
    case TR_YESTERDAY: return {today - day, today - day};
    case TR_3D: return {today - 2 * day, today};
    case TR_7D: return {today - 6 * day, today};
    case TR_30D: return {Calendar::OldestDate(today), today};
    case TR_CUSTOM: {
        const auto oldest = Calendar::OldestDate(today);
        return {max(oldest, min(today, m_custom_first)), max(oldest, min(today, m_custom_last))};
    }
    default: return {today, today};
    }
}

void CDetailWindow::RefreshHistoryRange() {
    m_scroll_pos = 0;
    m_history_dirty = true;
    RebuildRows();
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::SelectChartDate(ULONGLONG date, ULONGLONG today) {
    m_chart_date = date == today ? 0 : date;
    m_chart_hover_hour = m_chart_selected_hour = m_chart_hover_row = -1;
    m_chart_table_scroll = 0;
    m_chart_hours.clear();
    m_chart_hits.clear();
    InvalidateRect(m_hwnd, NULL, FALSE);
}

ULONGLONG CDetailWindow::ChartOldest(ULONGLONG today) const {
    // 作者把边界定在保留窗口的起点（29 天前）。小时库启用没几天、或中间断档时，
    // 那之前的日子图表上什么都没有，「前一天」却能一直翻进去；所以再和「最早
    // 还有数据的那天」取较晚的一个。完全没有数据时最早就是今天：按钮灰显、点了不动
    const auto window_start = Calendar::OldestDate(today);
    ULONGLONG oldest = 0;   // 最早还有数据的那个小时桶（小时表或旧日汇总表）
    if (!m_hourly.buckets.empty()) oldest = m_hourly.buckets.begin()->first;
    if (!m_hourly.daily.empty()) {
        const auto day = m_hourly.daily.begin()->first;
        if (!oldest || day < oldest) oldest = day;
    }
    return oldest ? max(window_start, Calendar().Date(oldest)) : today;
}

// 两段 Apps 汇总合并（review P2 的增量路径用）：饱和加与 HourlyHistory::Summarize 一致
static void MergeApps(pnm::HourlyHistory::Apps& into, const pnm::HourlyHistory::Apps& from) {
    for (const auto& [name, usage] : from) {
        auto& total = into[name];
        total.down = pnm::HourlyHistory::SaturatingAdd(total.down, usage.down);
        total.up = pnm::HourlyHistory::SaturatingAdd(total.up, usage.up);
    }
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
    using History = pnm::HourlyHistory;
    const Calendar calendar;
    m_history_rows_date = calendar.Date(now);
    const auto [first, last] = HistoryDates(m_history_rows_date);
    const auto [start_ms, end_ms] = calendar.Bounds({first, last}, now);
    const auto span_sec = end_ms > start_ms ? max(1.0, (end_ms - start_ms) / 1000.0) : 1.0;

    // (review P2) 全量 Summarize 在每次重建时把所选范围的每个小时桶重新聚合
    // 一遍（「近30天」最多 720 个桶，其中只有当前小时的桶在变，默认 500ms 一
    // 次）。已结束的小时桶不再写入：缓存到 watermark 为止，跨小时/换范围才重
    // 算；当前小时的桶单独汇总后并入。
    const auto hour_start = now / History::Hour * History::Hour;
    ULONGLONG cache_limit = min(end_ms, hour_start);
    if (cache_limit < start_ms) cache_limit = start_ms;
    if (!m_hist_cache_valid || m_hist_cache_start != start_ms || cache_limit < m_hist_cache_watermark) {
        m_hist_cache = m_hourly.Summarize(start_ms, cache_limit);
        m_hist_cache_start = start_ms;
        m_hist_cache_watermark = cache_limit;
        m_hist_cache_valid = true;
    } else if (cache_limit > m_hist_cache_watermark) {
        MergeApps(m_hist_cache, m_hourly.Summarize(m_hist_cache_watermark, cache_limit));
        m_hist_cache_watermark = cache_limit;
    }
    auto totals = m_hist_cache;
    if (end_ms > cache_limit) MergeApps(totals, m_hourly.Summarize(cache_limit, end_ms));
    for (const auto& [name16, usage] : totals) {
        const std::wstring name(name16.begin(), name16.end());
        const auto total_recv = usage.down, total_sent = usage.up;

        // Average speed over the actual elapsed time in the calendar range.
        double elapsed_sec = span_sec;

        m_hist_total_recv = History::SaturatingAdd(m_hist_total_recv, total_recv);
        m_hist_total_sent = History::SaturatingAdd(m_hist_total_sent, total_sent);

        DisplayRow row;
        row.name = name;
        const auto path = m_history_exe_paths.find(name);
        if (path != m_history_exe_paths.end()) row.exe_path = path->second;
        row.hist_recv = total_recv;
        row.hist_sent = total_sent;
        row.hist_avg_down = (double)total_recv / elapsed_sec;
        row.hist_avg_up = (double)total_sent / elapsed_sec;

        std::wstring lower = name;
        for (auto& c : lower) c = towlower(c);
        if (!name.empty() && name[0] == L'<') {
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

// B3 的失败提示文案用独立常量：恢复成功时按地址精确撤掉这一条，不误清
// LoadHourlyHistory 挂着的加载类提示（「原文件已备份」是一次性事实，不该被撤）
static const wchar_t kSaveFailNotice[] =
    L"小时历史已连续多次保存失败，数据可能没有写入磁盘，将继续自动重试。";

void CDetailWindow::SaveHistory(bool flush) {
    if (m_config_dir.empty() || !m_hourly_writable) return;
    const auto status = m_history_save.Status();
    // 后台保存持续失败原来只保留脏标志静默重试，用户无从察觉「看着在记、
    // 重启就没」（review B3）：连续失败 3 次（约 90 秒）提示一次，恢复成功
    // 后重置，不反复打扰。
    if (status == pnm::HistorySave::Saved || status == pnm::HistorySave::Superseded) {
        m_save_fail_count = 0;
        m_save_fail_notified = false;
        if (m_history_notice == kSaveFailNotice) m_history_notice = nullptr;
    } else if (status == pnm::HistorySave::Failed && ++m_save_fail_count >= 3 && !m_save_fail_notified) {
        m_save_fail_notified = true;
        if (!g_shutting_down) {   // 退出收尾（DllMain）里绝不弹框
            m_history_notice = kSaveFailNotice;
            ShowHistoryNotice();
        }
    }
    if (!m_hourly_dirty && status != pnm::HistorySave::Failed &&
        !(flush && status == pnm::HistorySave::Pending)) return;
    const auto path = std::filesystem::path(m_config_dir) / L"history-hourly-v1.dat";
    if (flush) {
        const bool saved = g_shutting_down ? m_history_save.FlushAtDetach(path, m_hourly)
                                           : m_history_save.Flush(path, m_hourly);
        m_hourly_dirty = !saved;
        if (!saved) OutputDebugStringW(L"[PNM] Hourly history flush failed.\n");
    } else if (m_history_save.Queue(path, m_hourly)) {
        m_hourly_dirty = false;
    } else {
        ++m_save_fail_count;   // 连后台线程都起不了（极端情形），同样计入连续失败
    }
}

// 清理上次退出残留的后台保存临时文件（review P3）：保存是「写临时文件 +
// 原子替换」，写到一半进程被结束就留下 history-hourly-v1.dat.<pid>.<序号>.tmp，
// 此后没有任何代码删它，长期使用会攒下单个最大 32 MB 的残留。只删 PID 不是
// 本进程的——本进程的 .tmp 可能正被后台保存线程使用。
static void CleanStaleHistoryTemps(const std::filesystem::path& dir) {
    static const wchar_t kPrefix[] = L"history-hourly-v1.dat.";
    static const wchar_t kSuffix[] = L".tmp";
    const size_t stem = wcslen(kPrefix), tail = wcslen(kSuffix);
    const auto digits = [](const std::wstring& s) {
        if (s.empty()) return false;
        for (wchar_t c : s) if (c < L'0' || c > L'9') return false;
        return true;
    };
    std::error_code error;
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, error), end;
    for (; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().wstring();
        if (name.size() <= stem + tail ||
            name.compare(0, stem, kPrefix) != 0 ||
            name.compare(name.size() - tail, tail, kSuffix) != 0) continue;
        // 中段必须是 <pid>.<序号> 两个纯数字段：与 MakeJob 的命名严格一致，
        // 其余同名形态（含用户手工文件）一律不碰
        const auto body = name.substr(stem, name.size() - stem - tail);
        const auto dot = body.find(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= body.size() ||
            body.find(L'.', dot + 1) != std::wstring::npos) continue;
        if (!digits(body.substr(0, dot)) || !digits(body.substr(dot + 1))) continue;
        if (wcstoul(body.c_str(), nullptr, 10) == GetCurrentProcessId()) continue;
        DeleteFileW(it->path().c_str());
    }
}

void CDetailWindow::LoadHourlyHistory() {
    if (m_config_dir.empty()) return;
    const auto path = std::filesystem::path(m_config_dir) / L"history-hourly-v1.dat";
    CleanStaleHistoryTemps(path.parent_path());
    using pnm::HistoryLoad;
    const auto result = pnm::LoadHistoryFile(path, m_hourly);
    m_history_notice = nullptr;
    m_hourly_writable = result != HistoryLoad::Blocked && result != HistoryLoad::NewerVersion;
    if (result == HistoryLoad::Loaded) m_hourly_dirty = m_hourly.Prune(WallClockMs());
    if (result == HistoryLoad::Recovered) {
        m_history_notice = L"无法读取小时历史。原文件已备份，将重新记录。";
        m_hourly_dirty = true;
        SaveHistory(true);
    } else if (result == HistoryLoad::Blocked) {
        m_history_notice = L"无法读取或备份小时历史。原文件未改动，记录已暂停。";
    } else if (result == HistoryLoad::NewerVersion) {
        m_history_notice = L"小时历史由更新版本的插件写入。原文件未改动，当前版本暂停记录。";
    }
    m_hist_cache_valid = false;   // 库内容可能整体换了，汇总缓存一律作废
}

void CDetailWindow::LoadHistory() {
    // Only PNH1 is loaded; leave the previous release's file untouched.
    LoadHourlyHistory();
    m_history_dirty = true;
}

// ============================================================
// Data update
// ============================================================

void CDetailWindow::UpdateData(const std::vector<ProcTraffic>& stats, double total_up, double total_down,
                               bool usesEtw, ULONGLONG sampleTick, ULONGLONG sampleWall) {
    m_total_up = total_up;
    m_total_down = total_down;

    // Always record history
    RecordHistory(stats, usesEtw, sampleTick, sampleWall);

    // Auto-save every 30 seconds (was 60, reduced for safety on TM restart)
    ULONGLONG now = GetTickCount64();
    if (now - m_last_save_tick > 30000) {
        m_last_save_tick = now;
        SaveHistory();
    }

    // Pause visual updates while context menu is open; skip the rebuild
    // entirely while hidden (history recording above must keep running,
    // but per-tick row rebuilds for an invisible window are pure waste)
    if (m_context_menu_open || !m_visible) {
        m_cached_stats = stats;
        return;
    }

    m_cached_stats = stats;
    RebuildRows();
}

void CDetailWindow::RebuildRows() {
    if (m_active_tab == 2) {
        if (m_visible) InvalidateRect(m_hwnd, NULL, FALSE);
        return;
    }
    auto& stats = m_cached_stats;

    if (m_active_tab == 1) {
        if (Calendar().Date(WallClockMs()) != m_history_rows_date) {
            m_history_dirty = true;
            if (m_visible) InvalidateRect(m_hwnd, NULL, FALSE);
        }
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
            // include the summary bar (total up/down change with the time
            // range): its top is above the table header AND the summary
            // strip itself, both of which sit inside GetTableAreaTop()
            rc.top = GetTableAreaTop() - GetSummaryHeight() - GetTableHeaderHeight();
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
                    // 行高/归属地缓存一并搬回——RebuildRows 每秒新建 SubProcess，
                    // 不搬的话缓存每秒清零，等于没缓存（组内 PID 顺序按 map
                    // 有序输出，index 对应稳定，缓存不会串进程）
                    row.sub_processes[i].geo_serial = it->second[i].geo_serial;
                    row.sub_processes[i].conn_heights = std::move(it->second[i].conn_heights);
                    row.sub_processes[i].conn_geo = std::move(it->second[i].conn_geo);
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
        // include the summary bar (same layout math as the history branch)
        rc.top = GetTableAreaTop() - GetSummaryHeight() - GetTableHeaderHeight();
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
int CDetailWindow::GetTableHeaderHeight() const { return m_active_tab == 2 ? 0 : TABLE_HEADER_H; }
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
    if (m_active_tab == 2) return;
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
    if (m_active_tab == 2) return;
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
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    // 最小化/异常状态时 GetWindowRect 返回垃圾尺寸，改用上次用户确定的尺寸；
    // 从未拉伸过（0）则不写，下次打开回退固定默认值
    if (IsIconic(m_hwnd)) {
        ww = m_saved_w;
        wh = m_saved_h;
    }
    fprintf(f, "  \"win_w\": %d,\n", ww);
    fprintf(f, "  \"win_h\": %d,\n", wh);
    // IP 归属地设置 (proxy / update_days / enabled)
    {
        std::wstring proxy = IpGeo::Instance().GetProxy();
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
    fprintf(f, "  \"popup_enabled\": %s,\n", m_popup_enabled ? "true" : "false");
    fprintf(f, "  \"show_host_tooltip\": %s,\n", m_show_host_tooltip ? "true" : "false");
    fprintf(f, "  \"popup_effect\": %d,\n", m_popup_effect);
    fprintf(f, "  \"popup_opacity\": %d,\n", m_popup_opacity);
    fprintf(f, "  \"debug_logs\": %s,\n", m_debug_logs ? "true" : "false");
    fprintf(f, "  \"crash_dump\": %s,\n", m_crash_dump ? "true" : "false");
    // 颜色模式：0=跟随系统 1=深色 2=浅色（详情窗与悬浮提示共享）
    fprintf(f, "  \"color_mode\": %d,\n", s_color_mode);
    // 界面语言：auto 或 BCP-47（现值域纯 ASCII；显式 UTF-8 转换以防将来塞入非 ASCII）
    {
        std::string lang8;
        int n = WideCharToMultiByte(CP_UTF8, 0, m_lang.c_str(), (int)m_lang.size(), NULL, 0, NULL, NULL);
        if (n > 0) {
            lang8.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, m_lang.c_str(), (int)m_lang.size(), &lang8[0], n, NULL, NULL);
        }
        fprintf(f, "  \"lang\": \"%s\"\n", lang8.c_str());
    }
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
    // Parse color_mode (0=auto 1=dark 2=light); applied to the static so both
    // detail window and tooltip popup pick it up before first draw
    {
        size_t pos = json.find("\"color_mode\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                int v = atoi(json.c_str() + pos + 1);
                s_color_mode = (v >= 0 && v <= 2) ? v : 0;
            }
        }
        // 构造函数在 LoadSettings 之前跑（那时 s_color_mode 还是 0），
        // 强制模式下 m_dark_mode 需在此对齐，Initialize 创建的 pens 才是对的
        m_dark_mode = IsDarkMode();
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
    // Parse popup_enabled (default ON - hover popup master switch; absent
    // field in older settings.json keeps it ON)
    {
        size_t pos = json.find("\"popup_enabled\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                std::string sub = json.substr(pos + 1, 16);
                m_popup_enabled = (sub.find("true") != std::string::npos);
            }
        }
    }
    m_show_host_tooltip = true;
    {
        size_t pos = json.find("\"show_host_tooltip\"");
        if (pos != std::string::npos && (pos = json.find(':', pos)) != std::string::npos) {
            pos = json.find_first_not_of(" \t\r\n", pos + 1);
            if (pos != std::string::npos && json.compare(pos, 5, "false") == 0)
                m_show_host_tooltip = false;
        }
    }
    // Parse popup_effect (0=acrylic, 1=transparent, 2=blur, 3=opaque;
    // default 0. 旧值语义延续：1 原为「纯透明度」→ 现为「透明」)
    {
        size_t pos = json.find("\"popup_effect\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                int v = atoi(json.c_str() + pos + 1);
                m_popup_effect = (v >= 0 && v <= 3) ? v : 0;
            }
        }
    }
    // Parse popup_opacity (10-100, default 100; out-of-range values keep the
    // default so a hand-edited settings.json can't brick the popup)
    {
        size_t pos = json.find("\"popup_opacity\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                int v = atoi(json.c_str() + pos + 1);
                if (v >= 10 && v <= 100) m_popup_opacity = v;
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
    // Parse crash_dump (default ON; issue #20)
    {
        size_t pos = json.find("\"crash_dump\"");
        if (pos != std::string::npos) {
            pos = json.find(':', pos);
            if (pos != std::string::npos) {
                std::string sub = json.substr(pos + 1, 16);
                m_crash_dump = (sub.find("false") == std::string::npos);
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
        // 同步成员：保存的宽高即用户确定的尺寸（语言切换窗口宽度只保底不收缩）
        if (ww >= MIN_WIDTH) m_saved_w = ww;
        if (wh >= MIN_HEIGHT) m_saved_h = wh;
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
    SyncDebugLogs();
}

void CDetailWindow::SyncDebugLogs() {
    EtwCapture::SetDebugLogs(m_debug_logs);
    if (m_capture) {
        wchar_t dbg[MAX_PATH] = L"";
        m_capture->SetLogDir(m_debug_logs && PNM_GetDebugDir(dbg, MAX_PATH) ? dbg : L"");
    }
}

// Debug-log master switch: persists to settings.json and syncs the capture
// backends (etw_capture.log / capture.log). crash.log stays ON; crash.dmp /
// WER dumps follow the separate "crash_dump" switch.
void CDetailWindow::SetDebugLogs(bool on) {
    m_debug_logs = on;
    SaveSettings();
    SyncDebugLogs();
}

// ============================================================
// Hit testing
// ============================================================

int CDetailWindow::HitTestRow(int y) const {
    if (m_active_tab == 2) return -1;
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

int CDetailWindow::GetConnRowHeight(const SubProcess& sp, int ci) const {
    // 缓存失效：布局代次变化（geo 列宽/字体），或新建的 SubProcess（数组为空）。
    // 越界防御：ci 超出缓存数组（connections 拉取后追加的场景）按未算处理。
    if (sp.geo_serial != m_geo_layout_serial || ci >= (int)sp.conn_heights.size()) {
        sp.conn_heights.assign(sp.connections.size(), -1);
        sp.conn_geo.assign(sp.connections.size(), std::wstring());
        sp.geo_serial = m_geo_layout_serial;
    }
    int cached = sp.conn_heights[ci];
    if (cached > 0) return cached;

    const ConnDetail& conn = sp.connections[ci];
    std::wstring geo = IpGeo::Instance().Query(conn.remote_addr);
    sp.conn_geo[ci] = geo;
    if (geo.empty() || geo == L"-") { sp.conn_heights[ci] = CONN_ROW_H; return CONN_ROW_H; }
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc) return CONN_ROW_H;   // DC 创建失败不写缓存，下帧重试
    HFONT old = (HFONT)SelectObject(hdc, m_font_row);
    RECT rc = { 0, 0, m_conn_cols[3].width, 0 };
    DrawTextW(hdc, geo.c_str(), -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    int lines = (rc.bottom + CONN_ROW_H - 1) / CONN_ROW_H;
    if (lines < 1) lines = 1;
    int h = lines * CONN_ROW_H;
    sp.conn_heights[ci] = h;
    return h;
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
                        h += GetConnRowHeight(sp, ci);
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
    if (m_active_tab == 2) return;
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
    if (m_active_tab == 0 && m_context_menu_pid)
        AppendMenuW(hMenu, MF_STRING, 3, TR(L"\u7ED3\u675F\u8FDB\u7A0B"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    // Debug-log master switch (default OFF; crash.log stays ON, dumps follow "crash_dump")
    AppendMenuW(hMenu, MF_STRING, 100,
                m_debug_logs ? TR(L"\u8C03\u8BD5\u65E5\u5FD7: \u5F00 (\u70B9\u51FB\u5173\u95ED)")
                             : TR(L"\u8C03\u8BD5\u65E5\u5FD7: \u5173 (\u70B9\u51FB\u5F00\u542F)"));

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, m_hwnd, NULL);
    DestroyMenu(hMenu);

    // Use locked target, not current row
    DWORD pid = m_context_menu_pid;
    std::wstring path = m_context_menu_path;
    m_context_menu_open = false;

    if (m_active_tab == 2) return;
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
        if (m_active_tab == 0 && pid > 0) {
            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (hProc) {
                wchar_t prompt[256];
                swprintf_s(prompt, TR(L"确定要结束进程 PID %lu 吗？"), pid);
                if (MessageBoxW(m_hwnd, prompt, TR(L"\u7ED3\u675F\u8FDB\u7A0B"),
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES && m_active_tab == 0)
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

void CDetailWindow::BeginResizeDrag(int hit) {
    m_resize_hit = hit;
    GetCursorPos(&m_resize_start);
    GetWindowRect(m_hwnd, &m_resize_org);
    ::SetCapture(m_hwnd);   // 类内有同名成员 SetCapture(PacketCapture*)，显式走 Win32
}

void CDetailWindow::UpdateResizeDrag() {
    POINT now;
    GetCursorPos(&now);
    int dx = now.x - m_resize_start.x;
    int dy = now.y - m_resize_start.y;
    RECT rc = m_resize_org;
    bool left   = (m_resize_hit == HTLEFT     || m_resize_hit == HTTOPLEFT    || m_resize_hit == HTBOTTOMLEFT);
    bool right  = (m_resize_hit == HTRIGHT    || m_resize_hit == HTTOPRIGHT   || m_resize_hit == HTBOTTOMRIGHT);
    bool top    = (m_resize_hit == HTTOP      || m_resize_hit == HTTOPLEFT    || m_resize_hit == HTTOPRIGHT);
    bool bottom = (m_resize_hit == HTBOTTOM   || m_resize_hit == HTBOTTOMLEFT || m_resize_hit == HTBOTTOMRIGHT);
    // 从起始边拉伸，反侧固定；两侧 clamp 保住 MIN_WIDTH / MIN_HEIGHT
    if (left)   { int v = m_resize_org.left + dx;   rc.left   = (v > rc.right - MIN_WIDTH)  ? rc.right - MIN_WIDTH  : v; }
    if (right)  { int v = m_resize_org.right + dx;  rc.right  = (v < rc.left + MIN_WIDTH)   ? rc.left + MIN_WIDTH   : v; }
    if (top)    { int v = m_resize_org.top + dy;    rc.top    = (v > rc.bottom - MIN_HEIGHT) ? rc.bottom - MIN_HEIGHT : v; }
    if (bottom) { int v = m_resize_org.bottom + dy; rc.bottom = (v < rc.top + MIN_HEIGHT)   ? rc.top + MIN_HEIGHT   : v; }
    SetWindowPos(m_hwnd, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void CDetailWindow::EndResizeDrag() {
    ::ReleaseCapture();
    m_resize_hit = 0;
    // 对齐 WM_EXITSIZEMOVE 的语义：记录用户确定的宽高（语言切换不再收缩）
    RECT rc;
    GetWindowRect(m_hwnd, &rc);
    m_saved_w = rc.right - rc.left;
    m_saved_h = rc.bottom - rc.top;
    SaveSettings();
}

LRESULT CDetailWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: OnPaint(); return 0;
    case WM_SIZE:
        // 最小化交给任务栏按钮恢复（issue #18），iconic 时 cx/cy 无意义，跳过重绘
        if (wp == SIZE_MINIMIZED) return 0;
        OnSize(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_LBUTTONDOWN: OnLButtonDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONUP:
        if (m_resize_hit) { EndResizeDrag(); return 0; }
        OnLButtonUp((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_LBUTTONDBLCLK: OnLButtonDblClk((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_RBUTTONDOWN: OnRButtonDown((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_MOUSEMOVE:
        if (m_resize_hit) { UpdateResizeDrag(); return 0; }
        OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp)); return 0;
    case WM_NCLBUTTONDOWN:
        // 无 WS_THICKFRAME 后系统对边缘 HT* 不再启动 resize loop，自己接管。
        // HTCAPTION 的移动 loop 不依赖 THICKFRAME，仍走 DefWindowProc。
        if (wp >= HTLEFT && wp <= HTBOTTOMRIGHT) { BeginResizeDrag((int)wp); return 0; }
        break;
    case WM_MOUSELEAVE: OnMouseLeave(); return 0;
    case WM_MOUSEWHEEL: OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    case WM_VSCROLL: OnVScroll(LOWORD(wp)); return 0;
    case WM_ENDSESSION:
        if (wp) SaveHistory(true);
        break;
    case WM_DESTROY:
        SaveHistory(true);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: {
        UpdateDpiScale(m_hwnd);
        // DWM may reset frame attributes (border color / corner preference)
        // when the window crosses a monitor boundary - reapply them.
        ApplyDwmFramePolicy();
        RecreateGdiObjects();
        // Apply suggested window rect from lParam
        RECT* prc = (RECT*)lp;
        SetWindowPos(m_hwnd, NULL, prc->left, prc->top,
            prc->right - prc->left, prc->bottom - prc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        // 列宽基准已按新 DPI 重算，重新实测列头 + 窗口宽度
        AutoSizeColumns(true);
        FitWindowWidth();
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

    case WM_SETCURSOR: {
        // 无 WS_THICKFRAME 时 DefWindowProc 对边缘命中不设 sizing 光标、一律
        // 回箭头（tools/cursor_probe 实测 HTLEFT..HTBOTTOMRIGHT 全给 IDC_ARROW），
        // 光标反馈随自绘 resize 一并接管，否则用户无从得知窗口可拉伸。
        int ht = LOWORD(lp);
        if (ht >= HTLEFT && ht <= HTBOTTOMRIGHT) {
            LPCWSTR id;
            switch (ht) {
            case HTLEFT: case HTRIGHT:          id = IDC_SIZEWE;   break;
            case HTTOPLEFT: case HTBOTTOMRIGHT: id = IDC_SIZENWSE; break;
            case HTTOPRIGHT: case HTBOTTOMLEFT: id = IDC_SIZENESW; break;
            default:                             id = IDC_SIZENS;   break;  // HTTOP / HTBOTTOM
            }
            SetCursor(LoadCursor(NULL, (LPCWSTR)id));
            return TRUE;
        }
        break;  // 客户区/标题栏：客户区由 OnMouseMove 细化，其余交给 DefWindowProc
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = MIN_WIDTH;
        mmi->ptMinTrackSize.y = MIN_HEIGHT;
        return 0;
    }

    case WM_EXITSIZEMOVE: {
        // 用户拖拽调整结束：记录用户确定的窗口宽高（语言切换不再收缩）
        RECT rc;
        GetWindowRect(m_hwnd, &rc);
        m_saved_w = rc.right - rc.left;
        m_saved_h = rc.bottom - rc.top;
        SaveSettings();
        return 0;
    }

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

    case WM_CAPTURECHANGED:
        // 拖滚动条（表格页签与图表页签两处）时被别的窗口抢走捕获（切窗、弹
        // 窗）：不清拖动状态，表格会一直跟着鼠标滚，直到下一次左键按下
        // （review B5）
        m_dragging_scrollbar = false;
        m_chart_dragging = false;
        return 0;

    case WM_PNM_REFRESH:
        // pull latest snapshot on the UI thread (posted by refresh timer)
        CProcessNetPlugin::Instance().DetailRefreshFromSnapshot();
        return 0;

    case WM_PNM_LANG_CHANGED:
        // 运行期语言变化：列头重取翻译，双向重测列宽，窗口宽度跟随内容
        // （用户拉伸过窗口则只保底不收缩），最后重建行（分类文本等）+ 重绘
        InitColumns();
        AutoSizeColumns(true);
        FitWindowWidth();
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
    if (PtInRect(&m_rcTab2, { x, y })) {
        if (m_active_tab != 2) {
            m_active_tab = 2;
            m_scroll_pos = 0;
            m_rows.clear();
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
        return;
    }

    if (m_active_tab == 2) {
        if (m_chart_total_rows > m_chart_visible_rows && PtInRect(&m_rcChartScrollTrack, { x, y })) {
            if (PtInRect(&m_rcChartScrollThumb, { x, y })) {
                m_chart_dragging = true;
                m_chart_drag_y = y;
                m_chart_drag_scroll = m_chart_table_scroll;
                ::SetCapture(m_hwnd);
            } else {
                // Click on the track pages toward the click position.
                m_chart_table_scroll += (y < m_rcChartScrollThumb.top ? -m_chart_visible_rows : m_chart_visible_rows);
                m_chart_table_scroll = max(0, min(m_chart_table_scroll,
                    m_chart_total_rows - m_chart_visible_rows));
                InvalidateRect(m_hwnd, NULL, FALSE);
            }
            return;
        }
        if (PtInRect(&m_rcChartPrev, { x, y })) {
            const auto today = Calendar().Date(WallClockMs());
            const auto date = m_chart_date ? m_chart_date : today;
            if (date > ChartOldest(today)) SelectChartDate(date - Calendar::Day, today);
            return;
        }
        if (PtInRect(&m_rcChartDate, { x, y })) {
            const auto today = Calendar().Date(WallClockMs());
            const auto date = max(ChartOldest(today), min(today, m_chart_date ? m_chart_date : today));
            DateDialog dialog{{date, date}, today, true, ChartOldest(today)};
            if (dialog.Show(m_hinst, m_hwnd, m_font_row)) SelectChartDate(dialog.range.first, today);
            return;
        }
        if (PtInRect(&m_rcChartToday, { x, y })) {
            const auto today = Calendar().Date(WallClockMs());
            SelectChartDate(today, today);
            return;
        }
        if (PtInRect(&m_rcChartNext, { x, y })) {
            const auto today = Calendar().Date(WallClockMs());
            if (m_chart_date && m_chart_date < today) SelectChartDate(m_chart_date + Calendar::Day, today);
            return;
        }
        if (m_chart_selected_hour >= 0 && PtInRect(&m_rcChartAll, { x, y })) {
            m_chart_selected_hour = -1;
            m_chart_table_scroll = 0;
            InvalidateRect(m_hwnd, NULL, FALSE);
            return;
        }
        const POINT table_pt = { x, y };
        if (PtInRect(&m_rcChartHeader, table_pt)) {
            const int col = x < m_chart_col_x[1] ? 0 : (x < m_chart_col_x[2] ? 1 : 2);
            if (m_chart_sort_col == col) {
                m_chart_sort_asc = !m_chart_sort_asc;
            } else {
                m_chart_sort_col = col;
                m_chart_sort_asc = (col == 0);  // names ascending, amounts largest first
            }
            m_chart_table_scroll = 0;
            InvalidateRect(m_hwnd, NULL, FALSE);
            return;
        }
        if (PtInRect(&m_rcChartTable, table_pt)) {
            for (const auto& hit : m_chart_hits) {
                if (hit.hour != -1 || !PtInRect(&hit.rc, table_pt)) continue;
                const auto found = m_chart_expanded.find(hit.name);
                if (found != m_chart_expanded.end()) m_chart_expanded.erase(found);
                else m_chart_expanded.insert(hit.name);
                InvalidateRect(m_hwnd, NULL, FALSE);
                break;
            }
            return;
        }
        const int clicked_hour = HitTestChartHour(x, y);
        if (clicked_hour >= 0) {
            m_chart_selected_hour = clicked_hour == m_chart_selected_hour ? -1 : clicked_hour;
            m_chart_table_scroll = 0;
            InvalidateRect(m_hwnd, NULL, FALSE);
        }
        return;
    }

    // Time range buttons (history tab only)
    if (m_active_tab == 1) {
        for (int i = 0; i < 5; i++) {
            if (PtInRect(&m_rcTRButtons[i], { x, y })) {
                if (m_time_range != (TimeRange)i) {
                    m_time_range = (TimeRange)i;
                    RefreshHistoryRange();
                }
                return;
            }
        }
        if (PtInRect(&m_rcCustomDates, { x, y })) {
            const auto today = Calendar().Date(WallClockMs());
            const auto [first, last] = HistoryDates(today);
            DateDialog dialog{{first, last}, today, false};
            if (dialog.Show(m_hinst, m_hwnd, m_font_row)) {
                m_time_range = TR_CUSTOM;
                m_custom_first = dialog.range.first;
                m_custom_last = dialog.range.last;
                RefreshHistoryRange();
            }
            return;
        }
        if (PtInRect(&m_rcOpenChart, { x, y })) {
            const auto today = Calendar().Date(WallClockMs());
            m_active_tab = 2;
            m_rows.clear();
            SelectChartDate(HistoryDates(today).second, today);
            return;
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
                        cur_y += GetConnRowHeight(sp, ci);
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
    if (m_chart_dragging) {
        m_chart_dragging = false;
        ReleaseCapture();
    }
}

void CDetailWindow::OnLButtonDblClk(int x, int y) {
    if (m_active_tab == 2) return;
    int row = HitTestRow(y);
    if (row >= 0) ToggleExpand(row);
}

void CDetailWindow::OnRButtonDown(int x, int y) {
    if (m_active_tab == 2) return;
    int row = HitTestRow(y);
    if (row >= 0) {
        POINT pt = { x, y };
        ClientToScreen(m_hwnd, &pt);
        ShowContextMenu(row, pt.x, pt.y);
    }
}

int CDetailWindow::HitTestChartHour(int x, int y) const {
    if (!PtInRect(&m_rcChartPlot, {x, y}) || m_chart_bar_w <= 0) return -1;
    const int dx = x - m_rcChartPlot.left;
    const int slot = m_chart_bar_w + m_chart_bar_gap;
    const int index = dx / slot;
    return index < (int)m_chart_hours.size() && dx % slot < m_chart_bar_w &&
        m_hourly.buckets.count(m_chart_hours[index]) ? index : -1;
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

    if (m_chart_dragging) {
        const int track_h = m_rcChartScrollTrack.bottom - m_rcChartScrollTrack.top;
        const int range = track_h - m_chart_thumb_h;
        const int max_scroll = m_chart_total_rows - m_chart_visible_rows;
        if (range > 0 && max_scroll > 0) {
            m_chart_table_scroll = max(0, min(max_scroll,
                m_chart_drag_scroll + (y - m_chart_drag_y) * max_scroll / range));
            InvalidateRect(m_hwnd, NULL, FALSE);
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
    int old_chart_hour = m_chart_hover_hour;
    int old_chart_row = m_chart_hover_row;

    m_hovering_close = PtInRect(&m_rcClose, { x, y }) != 0;
    m_hovering_min = PtInRect(&m_rcMin, { x, y }) != 0;
    m_hovered_row = HitTestRow(y);

    if (m_active_tab == 2) {
        RECT client;
        GetClientRect(m_hwnd, &client);
        m_chart_hover_hour = HitTestChartHour(x, y);
        m_chart_mouse_x = x;
        m_chart_mouse_y = y;
        m_chart_hover_row = -1;
        const POINT table_pt = { x, y };
        if (PtInRect(&m_rcChartTable, table_pt)) {
            for (const auto& hit : m_chart_hits) {
                if (PtInRect(&hit.rc, table_pt)) { m_chart_hover_row = hit.vrow; break; }
            }
        }
    } else {
        m_chart_hover_hour = -1;
        m_chart_hover_row = -1;
    }

    if (m_hovering_close != old_close || m_hovering_min != old_min ||
        m_hovered_row != old_row || m_chart_hover_hour != old_chart_hour ||
        m_chart_hover_row != old_chart_row) {
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
    m_chart_hover_hour = -1;
    m_chart_hover_row = -1;
    InvalidateRect(m_hwnd, NULL, FALSE);
}

void CDetailWindow::OnMouseWheel(int delta) {
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(m_hwnd, &cursor);
    // Page by what the last chart layout could show. Recomputing the table
    // geometry here drifted from DrawHourlyChart and made the step one row too large.
    if (m_active_tab == 2 && PtInRect(&m_rcChartTable, cursor)) {
        const int step = max(1, m_chart_visible_rows - 1);
        m_chart_table_scroll = max(0, m_chart_table_scroll + (delta > 0 ? -step : step));
        InvalidateRect(m_hwnd, &m_rcChartTable, FALSE);
        return;
    }
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

    const wchar_t* tab2 = TR(L"流量图表");
    GetTextExtentPoint32W(hdc, tab2, (int)wcslen(tab2), &sz);
    m_rcTab2 = { m_rcTab1.right + 12, y + 4, m_rcTab1.right + 12 + sz.cx + 16, y + TAB_BAR_H - 4 };
    SetTextColor(hdc, m_active_tab == 2 ? GetAccentColor(false) : GetSecondaryTextColor());
    DrawTextW(hdc, tab2, -1, &m_rcTab2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    if (m_active_tab == 2) {
        HPEN hPen = CreatePen(PS_SOLID, 2, GetAccentColor(false));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, m_rcTab2.left + 2, y + TAB_BAR_H - 4, NULL);
        LineTo(hdc, m_rcTab2.right - 2, y + TAB_BAR_H - 4);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    SelectObject(hdc, hOldFont);
}

void CDetailWindow::DrawHourlyChart(HDC hdc, int w, int y, int client_h, int speed_right) {
    RECT area = { PADDING, y, w - PADDING, client_h - PADDING };
    HBRUSH bg = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &area, bg);
    DeleteObject(bg);

    const auto now = WallClockMs();
    using History = pnm::HourlyHistory;
    const pnm::HistoryCalendar calendar;
    const auto today = calendar.Date(now);
    const auto oldest = ChartOldest(today);
    const auto date = m_chart_date ? max(oldest, min(today, m_chart_date)) : today;
    if (m_chart_date && m_chart_date != date) m_chart_date = date == today ? 0 : date;
    const auto start = calendar.Start(date);
    const auto end = calendar.Start(date + History::Day);
    const auto day_bucket = m_hourly.daily.lower_bound(start);
    const bool aggregate_day = day_bucket != m_hourly.daily.end() && day_bucket->first < end;
    auto hours = calendar.Hours(date);
    if (m_chart_hours != hours) {
        m_chart_hover_hour = m_chart_selected_hour = -1;
        m_chart_hours = std::move(hours);
    }
    const int hour_count = (int)m_chart_hours.size();
    std::vector<History::Usage> bars(hour_count);
    std::vector<std::wstring> labels(hour_count);
    std::vector<SYSTEMTIME> local_hours;
    for (const auto tick : m_chart_hours) local_hours.push_back(calendar.Local(tick));
    auto hour_apps = [&](int index) {
        History::Apps apps;
        const auto tick = m_chart_hours[index];
        const auto bucket = m_hourly.buckets.find(tick);
        if (bucket == m_hourly.buckets.end()) return apps;
        const auto daily = m_hourly.daily.find(History::DayStart(tick));
        for (const auto& [name, usage] : *bucket->second)
            if (daily == m_hourly.daily.end() || !daily->second->count(name)) apps.emplace(name, usage);
        return apps;
    };
    uint64_t max_total = 0;
    for (int i = 0; i < hour_count; ++i) {
        const auto& local = local_hours[i];
        const auto offset = (int64_t( pnm::HistoryCalendar::AsUnix(local)) - int64_t(m_chart_hours[i])) / 60000;
        const auto magnitude = int(offset < 0 ? -offset : offset);
        wchar_t label[40];
        swprintf_s(label, L"%02d:%02d (UTC%c%02d:%02d)", local.wHour, local.wMinute,
                   offset < 0 ? L'-' : L'+', magnitude / 60, magnitude % 60);
        labels[i] = label;
        bars[i] = History::TotalUsage(hour_apps(i));
        max_total = max(max_total, pnm::HourlyHistory::SaturatingAdd(bars[i].down, bars[i].up));
    }

    HFONT old_font = (HFONT)SelectObject(hdc, m_font_row);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSecondaryTextColor());
    const auto local_date = pnm::HistoryCalendar::AsUtc(date);
    wchar_t date_text[64];
    swprintf_s(date_text, L"%04d-%02d-%02d", local_date.wYear, local_date.wMonth, local_date.wDay);
    const auto date_button = std::wstring(date_text) + L" \u25BE";
    const wchar_t* nav_labels[] = {TR(L"前一天"), date_button.c_str(), TR(L"后一天"), TR(L"今天")};
    RECT* nav_buttons[] = {&m_rcChartPrev, &m_rcChartDate, &m_rcChartNext, &m_rcChartToday};
    const bool nav_enabled[] = {date > oldest, true, date < today, true};
    const int nav_gap = max(6, (int)(10 * m_dpi_scale));
    SIZE nav_sizes[4]{};
    int nav_width = 3 * nav_gap;
    int nav_height = 0;
    for (int i = 0; i < 4; ++i) {
        GetTextExtentPoint32W(hdc, nav_labels[i], (int)wcslen(nav_labels[i]), &nav_sizes[i]);
        nav_width += nav_sizes[i].cx + 18;
        nav_height = max(nav_height, (int)nav_sizes[i].cy);
    }
    nav_height = min(SUMMARY_H, nav_height + max(4, (int)(6 * m_dpi_scale)));
    int nav_left = area.right - nav_width;
    const bool fits_summary = nav_left >= speed_right + nav_gap;
    const int nav_top = fits_summary
        ? area.top - (SUMMARY_H + nav_height) / 2
        : area.top;
    nav_left = max(area.left, nav_left);
    for (int i = 0; i < 4; ++i) {
        const int right = nav_left + nav_sizes[i].cx + 18;
        *nav_buttons[i] = {nav_left, nav_top, right, nav_top + nav_height};
        DrawDateButton(hdc, *nav_buttons[i], nav_labels[i], GetHeaderBgColor(), GetBorderColor(),
                       nav_enabled[i] ? GetTextColor() : GetSecondaryTextColor());
        nav_left = right + nav_gap;
    }
    const int nav_bottom = nav_top + nav_height;

    // Measure every Y-axis tick label so the widest one always fits left of the axis.
    int axis_text_w = 0;
    for (int tick = 0; tick <= 4; ++tick) {
        wchar_t label[32];
        const uint64_t value = max_total / 4 * tick + (max_total % 4) * tick / 4;
        FormatBytes(value, label, 32);
        SIZE label_size = {};
        GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &label_size);
        axis_text_w = max(axis_text_w, (int)label_size.cx);
    }
    const int scale_pad = max(48, axis_text_w + 14);
    const int chart_left = area.left + scale_pad;
    const int chart_right = max(chart_left + 24, area.right - 4);
    // Keep the highest bar and the top tick visibly below the date buttons.  When the
    // buttons fit beside the speed summary, their bottom can extend into this area.
    const int chart_toolbar_gap = max(12, (int)(10 * m_dpi_scale));
    const int chart_top = max(area.top + (fits_summary ? 0 : nav_height) + (int)(16 * m_dpi_scale),
                              nav_bottom + chart_toolbar_gap);
    // Reserve the lower part for the selected date's complete program table.
    const int detail_reserve = max(190, (int)(220 * m_dpi_scale));
    const int chart_bottom = max(chart_top + 24, area.bottom - detail_reserve);
    m_rcChartPlot = { chart_left, chart_top, chart_right, chart_bottom };
    const int chart_h = max(1, chart_bottom - chart_top);
    const int chart_w = max(1, chart_right - chart_left);
    const int gap = max(1, min(4, (int)(4 * m_dpi_scale)));
    const int bar_w = max(1, (chart_w - gap * (hour_count - 1)) / max(1, hour_count));
    m_chart_bar_w = bar_w;
    m_chart_bar_gap = gap;

    HPEN axis_pen = CreatePen(PS_SOLID, 1, GetBorderColor());
    HPEN old_axis_pen = (HPEN)SelectObject(hdc, axis_pen);
    MoveToEx(hdc, chart_left, chart_top, NULL);
    LineTo(hdc, chart_left, chart_bottom);
    LineTo(hdc, chart_right, chart_bottom);
    SelectObject(hdc, old_axis_pen);
    DeleteObject(axis_pen);

    SIZE axis_sz = {};
    GetTextExtentPoint32W(hdc, L"0", 1, &axis_sz);
    const int label_half = axis_sz.cy / 2 + 4;
    wchar_t axis_value[32];
    for (int tick = 0; tick <= 4; ++tick) {
        const int tick_y = chart_bottom - chart_h * tick / 4;
        const uint64_t value = max_total / 4 * tick + (max_total % 4) * tick / 4;
        FormatBytes(value, axis_value, 32);
        RECT axis_label = { PADDING - 4, tick_y - label_half, chart_left - 6, tick_y + label_half };
        DrawTextW(hdc, axis_value, -1, &axis_label, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        if (tick > 0 && tick < 4) {
            HPEN grid_pen = CreatePen(PS_DOT, 1, GetBorderColor());
            HPEN old_grid_pen = (HPEN)SelectObject(hdc, grid_pen);
            MoveToEx(hdc, chart_left + 1, tick_y, NULL);
            LineTo(hdc, chart_right, tick_y);
            SelectObject(hdc, old_grid_pen);
            DeleteObject(grid_pen);
        }
    }
    HBRUSH down_brush = CreateSolidBrush(GetAccentColor(false));
    HBRUSH up_brush = CreateSolidBrush(GetAccentColor(true));
    for (int i = 0; i < hour_count; ++i) {
        const int x = chart_left + i * (bar_w + gap);
        const uint64_t total = pnm::HourlyHistory::SaturatingAdd(bars[i].down, bars[i].up);
        // Clamp to the plot so a bar can never rise above the top tick or past the right axis.
        const int total_h = max_total ? min(chart_h, static_cast<int>((long double)total / max_total * chart_h)) : 0;
        const int down_h = total ? static_cast<int>((double)bars[i].down / total * total_h) : 0;
        const int bx2 = min(x + bar_w, chart_right);
        RECT down_rc = { x, chart_bottom - down_h, bx2, chart_bottom };
        RECT up_rc = { x, chart_bottom - total_h, bx2, chart_bottom - down_h };
        if (down_h) FillRect(hdc, &down_rc, down_brush);
        if (total_h - down_h) FillRect(hdc, &up_rc, up_brush);
        wchar_t hour[8]; swprintf_s(hour, L"%02d", local_hours[i].wHour);
        RECT label = { x - 2, chart_bottom + 4, x + bar_w + 2, area.bottom };
        DrawTextW(hdc, hour, -1, &label, DT_CENTER | DT_SINGLELINE | DT_TOP);
    }
    DeleteObject(down_brush);
    DeleteObject(up_brush);

    if (max_total == 0) {
        RECT empty = { chart_left, chart_top, chart_right, chart_bottom };
        const wchar_t* empty_text = aggregate_day
            ? TR(L"此日期包含旧日汇总数据，无法还原小时分布")
            : TR(L"此日期没有可显示的小时流量数据");
        DrawTextW(hdc, empty_text, -1, &empty,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    pnm::HourlyHistory::Apps day_apps;
    const bool selected = m_chart_selected_hour >= 0 && m_chart_selected_hour < hour_count;
    day_apps = selected ? hour_apps(m_chart_selected_hour) : m_hourly.Summarize(start, end);
    const auto [day_down, day_up] = pnm::HourlyHistory::TotalUsage(day_apps);
    wchar_t summary[256];
    wchar_t down_text[32], up_text[32];
    FormatBytes(day_down, down_text, 32); FormatBytes(day_up, up_text, 32);
    if (selected) {
        swprintf_s(summary, L"%s %s  %s: %s    %s: %s", date_text,
                   labels[m_chart_selected_hour].c_str(),
                   TR(L"下载"), down_text, TR(L"上传"), up_text);
    } else {
    swprintf_s(summary, L"%s  %s: %s    %s: %s", date_text, TR(L"下载"), down_text,
               TR(L"上传"), up_text);
    }
    const int summary_top = area.bottom - max(24, (int)(24 * m_dpi_scale));
    m_rcChartAll = {};
    if (selected) {
        const auto label = TR(L"查看全天");
        SIZE size{};
        GetTextExtentPoint32W(hdc, label, (int)wcslen(label), &size);
        m_rcChartAll = {area.right - size.cx - 18, summary_top, area.right, area.bottom};
        DrawDateButton(hdc, m_rcChartAll, label, GetHeaderBgColor(), GetBorderColor(), GetTextColor());
    }
    RECT summary_rc = { area.left, summary_top, selected ? m_rcChartAll.left - nav_gap : area.right, area.bottom };
    SetTextColor(hdc, GetTextColor());
    DrawTextW(hdc, summary, -1, &summary_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // The selected date controls the scope; every program is shown in one detail table.
    // Detail table: sortable header, hover highlight, and per-program expansion into hour rows.
    const int tbl_left = area.left;
    const int tbl_right = area.right;
    const int pad = max(6, (int)(8 * m_dpi_scale));
    // Hour labels sit below the plot; the table starts below them with a gap.
    SIZE hour_size = {};
    GetTextExtentPoint32W(hdc, L"00", 2, &hour_size);
    const int table_top = chart_bottom + 4 + hour_size.cy + 10;
    const int row_h = max(20, (int)(22 * m_dpi_scale));
    const int table_bottom = area.bottom - 30;
    const int visible_rows = max(0, (table_bottom - table_top - row_h) / row_h);
    const int name_w = max(120, (tbl_right - tbl_left) * 2 / 5);
    const int down_x = tbl_left + name_w;
    const int up_x = tbl_left + (tbl_right - tbl_left) * 3 / 5;
    m_rcChartTable = { tbl_left, table_top, tbl_right, table_bottom };
    m_rcChartHeader = { tbl_left, table_top, tbl_right, table_top + row_h };
    m_chart_col_x[0] = tbl_left;
    m_chart_col_x[1] = down_x;
    m_chart_col_x[2] = up_x;
    const int sb_w = max(6, (int)(8 * m_dpi_scale));
    m_chart_col_x[3] = tbl_right - sb_w;

    // Sort programs by the chosen column (0 name, 1 download, 2 upload).
    struct AppEntry { std::u16string name; pnm::HourlyHistory::Usage usage; };
    std::vector<AppEntry> entries;
    entries.reserve(day_apps.size());
    for (const auto& [name, usage] : day_apps) entries.push_back({ name, usage });
    const int sort_col = m_chart_sort_col;
    const bool sort_asc = m_chart_sort_asc;
    std::stable_sort(entries.begin(), entries.end(),
        [sort_col, sort_asc](const AppEntry& a, const AppEntry& b) {
            int cmp = 0;
            if (sort_col == 0) cmp = a.name.compare(b.name);
            else if (sort_col == 1) cmp = a.usage.down < b.usage.down ? -1 : (a.usage.down > b.usage.down ? 1 : 0);
            else cmp = a.usage.up < b.usage.up ? -1 : (a.usage.up > b.usage.up ? 1 : 0);
            return sort_asc ? cmp < 0 : cmp > 0;
        });

    // Hour usage of one program on the selected day (empty for old daily-only dates).
    auto hour_usage = [&](const std::u16string& name, int h) {
        pnm::HourlyHistory::Usage u{};
        if (h < 0 || h >= hour_count) return u;
        const auto tick = m_chart_hours[h];
        const auto daily = m_hourly.daily.find(History::DayStart(tick));
        if (daily != m_hourly.daily.end() && daily->second->count(name)) return u;
        const auto bucket_it = m_hourly.buckets.find(tick);
        if (bucket_it == m_hourly.buckets.end()) return u;
        const auto app_it = bucket_it->second->find(name);
        if (app_it != bucket_it->second->end()) u = app_it->second;
        return u;
    };

    // Visible rows: each program, followed by its hour rows when expanded. hour -1 = program, -2 = note.
    struct VisRow { int app; int hour; };
    std::vector<VisRow> vrows;
    for (int ai = 0; ai < (int)entries.size(); ++ai) {
        vrows.push_back({ ai, -1 });
        if (!m_chart_expanded.count(entries[ai].name)) continue;
        bool any = false;
        for (int h = selected ? m_chart_selected_hour : 0;
             h < (selected ? m_chart_selected_hour + 1 : hour_count); ++h) {
            const auto u = hour_usage(entries[ai].name, h);
            if (u.down == 0 && u.up == 0) continue;
            vrows.push_back({ ai, h });
            any = true;
        }
        if (!any) vrows.push_back({ ai, -2 });
    }
    m_chart_table_scroll = max(0, min(m_chart_table_scroll,
        max(0, (int)vrows.size() - visible_rows)));

    // Header: same rounded header strip, font and colors as DrawTableHeader.
    {
        HBRUSH hHeadBrush = CreateSolidBrush(GetHeaderBgColor());
        HPEN hHeadPen = CreatePen(PS_SOLID, 1, GetBorderColor());
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hHeadBrush);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hHeadPen);
        RoundRect(hdc, m_rcChartHeader.left, m_rcChartHeader.top,
                  m_rcChartHeader.right, m_rcChartHeader.bottom, 4, 4);
        SelectObject(hdc, hOldBrush);
        SelectObject(hdc, hOldPen);
        DeleteObject(hHeadBrush);
        DeleteObject(hHeadPen);
    }
    HFONT hOldTableFont = (HFONT)SelectObject(hdc, m_font_header);
    SetBkMode(hdc, TRANSPARENT);
    wchar_t name_header[128];
    if (selected) {
        swprintf_s(name_header, L"%s (%s)", TR(L"程序名"), labels[m_chart_selected_hour].c_str());
    } else {
        wcscpy_s(name_header, TR(L"程序名"));
    }
    const wchar_t* header_titles[3] = { name_header, TR(L"下载量"), TR(L"上传量") };
    for (int c = 0; c < 3; ++c) {
        RECT cell = { m_chart_col_x[c] + pad, table_top, m_chart_col_x[c + 1] - pad, table_top + row_h };
        const bool active = (c == sort_col);
        SetTextColor(hdc, active ? GetAccentColor(false) : GetSecondaryTextColor());
        if (active) {
            RECT arrow = cell;
            arrow.left = max(cell.left, cell.right - SortArrowSpace(m_dpi_scale));
            DrawSortArrow(hdc, arrow, sort_asc, GetAccentColor(false));
            cell.right = arrow.left;
        }
        DrawTextW(hdc, header_titles[c], -1, &cell,
                  (c == 0 ? DT_LEFT : DT_RIGHT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // Rows: same alternating and hover brushes, bottom border and accent colors as DrawTableRows.
    SelectObject(hdc, m_font_row);
    m_chart_hits.clear();
    int row_y = table_top + row_h;
    auto draw_values = [&](const pnm::HourlyHistory::Usage& u) {
        wchar_t down_value[32], up_value[32];
        FormatBytes(u.down, down_value, 32);
        FormatBytes(u.up, up_value, 32);
        RECT down_rc = { down_x, row_y, up_x - 4, row_y + row_h };
        RECT up_rc = { up_x, row_y, tbl_right - sb_w - pad, row_y + row_h };
        SetTextColor(hdc, u.down > 0 ? GetAccentColor(false) : GetSecondaryTextColor());
        DrawTextW(hdc, down_value, -1, &down_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        SetTextColor(hdc, u.up > 0 ? GetAccentColor(true) : GetSecondaryTextColor());
        DrawTextW(hdc, up_value, -1, &up_rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    };
    const int indent = (int)(24 * m_dpi_scale);
    const int arrow_w = (int)(14 * m_dpi_scale);
    for (int vi = m_chart_table_scroll; vi < (int)vrows.size(); ++vi) {
        if (row_y + row_h > table_bottom) break;
        const VisRow vr = vrows[vi];
        const auto& e = entries[vr.app];
        RECT row_rc = { tbl_left, row_y, tbl_right, row_y + row_h };
        FillRect(hdc, &row_rc, vi == m_chart_hover_row ? m_br_hover : m_br_row[vi % 2]);
        HPEN hRowPen = (HPEN)SelectObject(hdc, m_pen_border);
        MoveToEx(hdc, tbl_left, row_y + row_h - 1, NULL);
        LineTo(hdc, tbl_right, row_y + row_h - 1);
        SelectObject(hdc, hRowPen);
        m_chart_hits.push_back({ row_rc, e.name, vr.hour, vi });

        if (vr.hour == -1) {
            const bool expanded = m_chart_expanded.count(e.name) != 0;
            SelectObject(hdc, m_font_small);
            SetTextColor(hdc, GetSecondaryTextColor());
            RECT arrow_rc = { tbl_left + pad, row_y, tbl_left + pad + arrow_w, row_y + row_h };
            DrawTextW(hdc, expanded ? L"\u25BC" : L"\u25B6", -1, &arrow_rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            SelectObject(hdc, m_font_row);
            std::wstring name(e.name.begin(), e.name.end());
            RECT name_rc = { tbl_left + pad + arrow_w + 4, row_y, down_x - 6, row_y + row_h };
            SetTextColor(hdc, GetTextColor());
            DrawTextW(hdc, name.c_str(), -1, &name_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            draw_values(e.usage);
        } else if (vr.hour >= 0) {
            RECT label_rc = { tbl_left + pad + indent, row_y, down_x - 6, row_y + row_h };
            SetTextColor(hdc, GetSecondaryTextColor());
            DrawTextW(hdc, labels[vr.hour].c_str(), -1, &label_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            draw_values(hour_usage(e.name, vr.hour));
        } else {
            RECT note_rc = { tbl_left + pad + indent, row_y, tbl_right - sb_w - pad, row_y + row_h };
            SetTextColor(hdc, GetSecondaryTextColor());
            DrawTextW(hdc, aggregate_day ? TR(L"此日期为旧日汇总，无小时明细")
                                         : TR(L"该程序在此日期没有小时明细"),
                      -1, &note_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        row_y += row_h;
    }
    SelectObject(hdc, hOldTableFont);
    // Vertical scrollbar at the table's right edge replaces the page indicator.
    m_chart_visible_rows = visible_rows;
    m_chart_total_rows = (int)vrows.size();
    if (m_chart_total_rows > visible_rows && visible_rows > 0) {
        m_rcChartScrollTrack = { tbl_right - sb_w, table_top + row_h, tbl_right, table_bottom };
        const int track_h = m_rcChartScrollTrack.bottom - m_rcChartScrollTrack.top;
        const int max_scroll = m_chart_total_rows - visible_rows;
        m_chart_thumb_h = max(20, track_h * visible_rows / m_chart_total_rows);
        const int range = track_h - m_chart_thumb_h;
        const int thumb_top = m_rcChartScrollTrack.top +
            (max_scroll > 0 ? range * m_chart_table_scroll / max_scroll : 0);
        m_rcChartScrollThumb = { m_rcChartScrollTrack.left + 1, thumb_top,
                                 m_rcChartScrollTrack.right - 1, thumb_top + m_chart_thumb_h };
        HBRUSH track_brush = CreateSolidBrush(GetHeaderBgColor());
        FillRect(hdc, &m_rcChartScrollTrack, track_brush);
        DeleteObject(track_brush);
        HBRUSH thumb_brush = CreateSolidBrush(GetBorderColor());
        FillRect(hdc, &m_rcChartScrollThumb, thumb_brush);
        DeleteObject(thumb_brush);
    } else {
        m_rcChartScrollTrack = {};
        m_rcChartScrollThumb = {};
        m_chart_thumb_h = 0;
    }

    if (m_chart_hover_hour >= 0 && m_chart_hover_hour < hour_count) {
        const int i = m_chart_hover_hour;
        const uint64_t total = pnm::HourlyHistory::SaturatingAdd(bars[i].down, bars[i].up);
        wchar_t down_value[32], up_value[32], total_value[32], tooltip[192];
        FormatBytes(bars[i].down, down_value, 32);
        FormatBytes(bars[i].up, up_value, 32);
        FormatBytes(total, total_value, 32);
        if (m_chart_selected_hour == i) {
            swprintf_s(tooltip, L"%s\n%s: %s\n%s: %s\n%s: %s\n%s",
                       labels[i].c_str(), TR(L"下载"), down_value, TR(L"上传"), up_value,
                       TR(L"合计"), total_value, TR(L"已筛选此小时"));
        } else {
            swprintf_s(tooltip, L"%s\n%s: %s\n%s: %s\n%s: %s",
                       labels[i].c_str(), TR(L"下载"), down_value, TR(L"上传"), up_value,
                       TR(L"合计"), total_value);
        }
        // Scale the tooltip width with DPI so the value lines are not wrapped.
        const int tip_width = min((int)(260 * m_dpi_scale), max(80, m_rcChartPlot.right - m_rcChartPlot.left - 4));
        // Size the box from the measured text, so every line (including 下载/上传/合计) stays visible.
        RECT tip_calc = { 0, 0, tip_width - 20, 0 };
        DrawTextW(hdc, tooltip, -1, &tip_calc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        const int tip_height = max(48, (int)(tip_calc.bottom - tip_calc.top) + 16);
        int tip_left = m_chart_mouse_x + 12;
        int tip_top = m_chart_mouse_y + 12;
        if (tip_left + tip_width > m_rcChartPlot.right - 2)
            tip_left = m_chart_mouse_x - 12 - tip_width;
        if (tip_top + tip_height > m_rcChartPlot.bottom - 2)
            tip_top = m_chart_mouse_y - 12 - tip_height;
        tip_left = max(m_rcChartPlot.left + 2, min(tip_left, m_rcChartPlot.right - tip_width - 2));
        tip_top = max(m_rcChartPlot.top + 2, min(tip_top, m_rcChartPlot.bottom - tip_height - 2));
        RECT tip = { tip_left, tip_top, tip_left + tip_width, tip_top + tip_height };
        HBRUSH tip_brush = CreateSolidBrush(GetHeaderBgColor());
        HPEN tip_pen = CreatePen(PS_SOLID, 1, GetBorderColor());
        HBRUSH old_brush = (HBRUSH)SelectObject(hdc, tip_brush);
        HPEN old_pen = (HPEN)SelectObject(hdc, tip_pen);
        RoundRect(hdc, tip.left, tip.top, tip.right, tip.bottom, 5, 5);
        SelectObject(hdc, old_brush); SelectObject(hdc, old_pen);
        DeleteObject(tip_brush); DeleteObject(tip_pen);
        SetTextColor(hdc, GetTextColor());
        RECT tip_text = { tip.left + 10, tip.top + 8, tip.right - 10, tip.bottom - 8 };
        DrawTextW(hdc, tooltip, -1, &tip_text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectObject(hdc, old_font);
}

void CDetailWindow::DrawTimeRangeButtons(HDC hdc, int w, int y) {
    if (m_active_tab != 1) return;

    RECT rc = { 0, y, w, y + GetTimeRangeHeight() };
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

    const int row_top = y + 3;
    const int row_bottom = y + TIME_RANGE_H - 3;

    // Keep the two history actions on the same row, with the destructive action
    // at the far right and the end-date chart action immediately to its left.
    const wchar_t* clear_label = TR(L"\u6E05\u9664\u6570\u636E");
    SIZE clear_sz;
    GetTextExtentPoint32W(hdc, clear_label, (int)wcslen(clear_label), &clear_sz);
    int clear_bw = clear_sz.cx + 16;
    m_rcClearBtn = { w - PADDING - clear_bw, row_top, w - PADDING, row_bottom };

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

    const auto jump = TR(L"查看结束日图表");
    SIZE jump_size{};
    GetTextExtentPoint32W(hdc, jump, (int)wcslen(jump), &jump_size);
    const int action_gap = max(6, (int)(6 * m_dpi_scale));
    const int jump_bw = jump_size.cx + 16;
    m_rcOpenChart = {m_rcClearBtn.left - action_gap - jump_bw, row_top,
                     m_rcClearBtn.left - action_gap, row_bottom};
    const auto [first, last] = HistoryDates(Calendar().Date(WallClockMs()));
    std::wstring range = DateText(first);
    if (first != last) range += L" \u2013 " + DateText(last);
    range = std::wstring(TR(L"自定义日期")) + L"  " + range + L" \u25BE";
    SIZE range_size{};
    GetTextExtentPoint32W(hdc, range.c_str(), (int)range.size(), &range_size);
    const int date_right = min(x + range_size.cx + 16, m_rcOpenChart.left - action_gap);
    m_rcCustomDates = {x, row_top, max(x, date_right), row_bottom};
    DrawDateButton(hdc, m_rcCustomDates, range.c_str(),
                   m_time_range == TR_CUSTOM ? GetAccentColor(false) : GetHeaderBgColor(), GetBorderColor(),
                   m_time_range == TR_CUSTOM ? RGB(255, 255, 255) : GetTextColor());
    DrawDateButton(hdc, m_rcOpenChart, jump, GetHeaderBgColor(), GetBorderColor(), GetTextColor());

    SelectObject(hdc, hOldFont);
}

int CDetailWindow::DrawSpeedSummary(HDC hdc, int w, int y) {
    RECT rc = { 0, y, w, y + SUMMARY_H };
    HBRUSH hBrush = CreateSolidBrush(GetBgColor());
    FillRect(hdc, &rc, hBrush);
    DeleteObject(hBrush);

    HFONT hOldFont = (HFONT)SelectObject(hdc, m_font_header);
    SetBkMode(hdc, TRANSPARENT);

    int x = PADDING;

    // Helper: measure text width with current font
    auto measure = [&](const wchar_t* text) -> int {
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
        return sz.cx;
    };

    if (m_active_tab == 1) {
        // History: show total traffic for selected range
        wchar_t recv_str[32], sent_str[32];
        FormatBytes(m_hist_total_recv, recv_str, 32);
        FormatBytes(m_hist_total_sent, sent_str, 32);

        const wchar_t* lbl1 = TR(L"\u603B\u4E0B\u8F7D\uFF1A");
        const wchar_t* lbl2 = TR(L"\u603B\u4E0A\u4F20\uFF1A");
        int lw1 = measure(lbl1);
        int vw1 = measure(recv_str);
        int lw2 = measure(lbl2);
        int vw2 = measure(sent_str);
        int gap = (int)(8 * m_dpi_scale);

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT r1 = { x, y, x + lw1, y + SUMMARY_H };
        DrawTextW(hdc, lbl1, -1, &r1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += lw1 + gap;
        SetTextColor(hdc, GetAccentColor(false));
        RECT rv1 = { x, y, x + vw1 + gap, y + SUMMARY_H };
        DrawTextW(hdc, recv_str, -1, &rv1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += vw1 + gap * 3;

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT r2 = { x, y, x + lw2, y + SUMMARY_H };
        DrawTextW(hdc, lbl2, -1, &r2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += lw2 + gap;
        SetTextColor(hdc, GetAccentColor(true));
        RECT rv2 = { x, y, x + vw2 + gap, y + SUMMARY_H };
        DrawTextW(hdc, sent_str, -1, &rv2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += vw2 + gap;
    } else {
        // Real-time: show current speed
        wchar_t down_str[32], up_str[32];
        FormatSpeed(m_total_down, down_str, 32);
        FormatSpeed(m_total_up, up_str, 32);

        const wchar_t* lbl1 = TR(L"\u4E0B\u8F7D\u603B\u901F\u5EA6\uFF1A");
        const wchar_t* lbl2 = TR(L"\u4E0A\u4F20\u603B\u901F\u5EA6\uFF1A");
        wchar_t d_full[64]; swprintf_s(d_full, 64, L"\u2193 %s", down_str);
        wchar_t u_full[64]; swprintf_s(u_full, 64, L"\u2191 %s", up_str);
        int lw1 = measure(lbl1);
        int vw1 = measure(d_full);
        int lw2 = measure(lbl2);
        int vw2 = measure(u_full);
        int gap = (int)(8 * m_dpi_scale);

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT r1 = { x, y, x + lw1, y + SUMMARY_H };
        DrawTextW(hdc, lbl1, -1, &r1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += lw1 + gap;
        SetTextColor(hdc, GetAccentColor(false));
        RECT rv1 = { x, y, x + vw1 + gap, y + SUMMARY_H };
        DrawTextW(hdc, d_full, -1, &rv1, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += vw1 + gap * 3;

        SetTextColor(hdc, GetSecondaryTextColor());
        RECT r2 = { x, y, x + lw2, y + SUMMARY_H };
        DrawTextW(hdc, lbl2, -1, &r2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += lw2 + gap;
        SetTextColor(hdc, GetAccentColor(true));
        RECT rv2 = { x, y, x + vw2 + gap, y + SUMMARY_H };
        DrawTextW(hdc, u_full, -1, &rv2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        x += vw2 + gap;
    }

    SelectObject(hdc, hOldFont);
    return x;
}

void CDetailWindow::DrawTableHeader(HDC hdc, int w, int y) {
    if (m_active_tab == 2) return;
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
        bool has_arrow = (i == m_sort_col[m_active_tab] && i >= 1);
        RECT rc = { cx + 4, y, cx + cols[i].width - 4, y + TABLE_HEADER_H };
        if (has_arrow) {
            RECT arrow = rc;
            arrow.left = arrow.right - SortArrowSpace(m_dpi_scale);
            DrawSortArrow(hdc, arrow, m_sort_asc[m_active_tab], GetAccentColor(false));
            rc.right = arrow.left;
        }
        UINT fmt = DT_SINGLELINE | DT_VCENTER;
        switch (cols[i].align) {
        case Column::LEFT: fmt |= DT_LEFT; break;
        case Column::RIGHT: fmt |= DT_RIGHT; break;
        case Column::CENTER: fmt |= DT_CENTER; break;
        }
        DrawTextW(hdc, cols[i].title, -1, &rc, fmt);

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
        int old_geo_w = m_conn_cols[3].width;
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
        // geo 列宽变了（窗口拉伸 / DPI / 归属地开关）→ 换行行数变，行高缓存整批失效
        if (m_conn_cols[3].width != old_geo_w) m_geo_layout_serial++;
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

        HICON hIcon = LookupProcessIcon(row.exe_path);
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

                        // 行高: 归属地自动换行后所需高度（GetConnRowHeight 顺带
                        // 填充 sp.conn_geo 缓存，下方直接取用）
                        int row_h = GetConnRowHeight(sp, ci);
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
                        const std::wstring& geo = sp.conn_geo[ci];  // GetConnRowHeight 已填充
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
    // Re-measure columns if language changed (before any drawing)
    if (!m_cols_sized) AutoSizeColumns();

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

    const int speed_right = DrawSpeedSummary(memDC, w, y);
    y += GetSummaryHeight();

    if (m_active_tab == 2) {
        DrawHourlyChart(memDC, w, y, h, speed_right);
    } else {
        DrawTableHeader(memDC, w, y);
        y += GetTableHeaderHeight();
        DrawTableRows(memDC, w, y, h);
        DrawScrollbar(memDC, w, h);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    EndPaint(m_hwnd, &ps);

    // 图标分帧提取（review P2）：本帧提取 1 个后 Invalidate 自驱动
    // paint→pump→repaint 链，几十行图标 1-2s 内渐进亮起，WM_PAINT 里
    // 不再有同步磁盘 IO；WM_PAINT 低优先级，链间输入消息照常派发
    if (PumpIconExtraction(1))
        InvalidateRect(m_hwnd, NULL, FALSE);
}
