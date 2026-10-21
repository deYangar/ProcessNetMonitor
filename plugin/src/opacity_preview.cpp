// 透明度滑条实时预览窗（2026-10-21）。头文件有架构说明。
// 呈现管线复刻自 tooltip_popup.cpp（coverage 双底解算/死点回填/确定性
// 噪声/圆角几何 coverage/预乘合成），公式改动前先读那边的实测注释。

#include "opacity_preview.h"
#include "tooltip_popup.h"   // SafeDestroyWindow / g_shutting_down
#include "detail_window.h"   // CDetailWindow::GetColorMode
#include "i18n.h"            // TR
#include <dwmapi.h>
#include <vector>

extern HINSTANCE s_dll_hinst;   // tooltip_popup.cpp DllMain

// 版面（96 DPI 基准；选项对话框自身也是固定像素风格，预览跟它统一）。
// 结构与真实悬浮窗一致且行数对齐 MAX_SHOW=5（2026-10-21 咩咩两轮反馈：
// 先「和正常一样」补上传区，再「给够 5 个进程」）：
// 12 + 34(header) + 24+5x32(上传) + 4(gap) + 24+5x32(下载) + 28(btn) + 12 = 458
static const int PREV_W = 280;
static const int PREV_H = 458;
static const int PADDING = 12;
static const int HEADER_H = 34;   // 总速条
static const int SECTION_H = 24;  // 分区标题
static const int ROW_H = 32;      // 进程行
static const int GAP_H = 4;       // 上传/下载区间隔
static const int ICON_SIZE = 22;
static const int SPEED_AREA_W = 85;
static const int CORNER_RADIUS = 8;

static const UINT_PTR TIMER_AUTOHIDE = 1;

// ---- SWCA（与 tooltip_popup.cpp 同款结构，独立定义避免跨编译单元耦合）----
enum OP_ACCENT_STATE { OP_ACCENT_DISABLED = 0, OP_ACCENT_ACRYLIC = 4 };
struct OP_ACCENT_POLICY {
    int nAccentState;
    int nFlags;
    UINT nColor;       // ABGR: alpha<<24 | B<<16 | G<<8 | R
    int nAnimationId;
};
struct OP_WCA_DATA {
    int nAttribute;    // 19 = WCA_ACCENT_POLICY
    void* pData;
    ULONG dataSize;
};
typedef BOOL(WINAPI* PFN_SetWindowCompositionAttribute)(HWND, OP_WCA_DATA*);

static PFN_SetWindowCompositionAttribute OP_GetSWCA() {
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

COpacityPreview& COpacityPreview::Instance() {
    static COpacityPreview s;
    return s;
}

COpacityPreview::~COpacityPreview() {
    Hide();
    ReleaseSurfaces();
}

LRESULT CALLBACK COpacityPreview::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == TIMER_AUTOHIDE) {
        // 2 秒无操作：静默收起（销毁，下次 Show 重建）
        Instance().Hide();
        return 0;
    }
    if (msg == WM_DESTROY && Instance().m_hwnd == hwnd) {
        // Hide() 已先清句柄；外部销毁（理论上不存在）兜底防悬垂
        Instance().m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool COpacityPreview::EnsureWindow() {
    if (m_hwnd) return true;
    m_hinst = s_dll_hinst ? s_dll_hinst : GetModuleHandleW(NULL);

    // blur 效果层（复刻 CTooltipPopup::EnsureBlurLayer）：纯黑窗 + SWCA
    // ACRYLICBLURBEHIND + 近全透 tint——只取 DWM 系统模糊，不叠色
    wchar_t blur_cls[64], main_cls[64];
    swprintf_s(blur_cls, 64, L"PNMPrevBlur_%p", m_hinst);
    swprintf_s(main_cls, 64, L"PNMPrevMain_%p", m_hinst);
    WNDCLASSEXW probe = {};
    probe.cbSize = sizeof(probe);
    if (!GetClassInfoExW(m_hinst, blur_cls, &probe)) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = m_hinst;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = blur_cls;
        RegisterClassExW(&wc);
    }
    if (!GetClassInfoExW(m_hinst, main_cls, &probe)) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = m_hinst;
        wc.lpszClassName = main_cls;
        RegisterClassExW(&wc);
    }

    m_blur_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,  // 点击穿透
        blur_cls, L"", WS_POPUP,
        0, 0, PREV_W, PREV_H, NULL, NULL, m_hinst, NULL);
    if (m_blur_hwnd) {
        int corner_pref = 2;  // DWMWCP_ROUND，与 ULW 圆角对齐
        DwmSetWindowAttribute(m_blur_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                              &corner_pref, sizeof(corner_pref));
        MARGINS glass = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(m_blur_hwnd, &glass);
        OP_ACCENT_POLICY policy = {};
        policy.nAccentState = OP_ACCENT_ACRYLIC;
        policy.nColor = 0x01000000;  // alpha=1：只取系统模糊
        OP_WCA_DATA data = {};
        data.nAttribute = 19;
        data.pData = &policy;
        data.dataSize = sizeof(policy);
        if (OP_GetSWCA()) OP_GetSWCA()(m_blur_hwnd, &data);
    }

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        main_cls, L"", WS_POPUP,
        0, 0, PREV_W, PREV_H, NULL, NULL, m_hinst, NULL);
    return m_hwnd != nullptr;
}

void COpacityPreview::DestroyWindowpair() {
    if (m_hwnd) {
        KillTimer(m_hwnd, TIMER_AUTOHIDE);
        HWND h = m_hwnd;
        m_hwnd = nullptr;
        SafeDestroyWindow(h);
    }
    if (m_blur_hwnd) {
        HWND b = m_blur_hwnd;
        m_blur_hwnd = nullptr;
        if (!g_shutting_down) SafeDestroyWindow(b);
    }
}

void COpacityPreview::EnsureSurfaces() {
    if (m_ulw_dc && m_cov_a_dc && m_cov_c_dc) return;

    struct { HDC* dc; HBITMAP* bmp; void** bits; } surfs[3] = {
        { &m_cov_a_dc, &m_cov_a_bmp, &m_cov_a_bits },
        { &m_cov_c_dc, &m_cov_c_bmp, &m_cov_c_bits },
        { &m_ulw_dc, &m_ulw_bmp, &m_ulw_bits },
    };
    for (auto& s : surfs) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = PREV_W;
        bi.bmiHeader.biHeight = -PREV_H;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        *s.bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, s.bits, NULL, 0);
        if (*s.bmp) {
            *s.dc = CreateCompatibleDC(NULL);
            if (*s.dc) SelectObject(*s.dc, *s.bmp);
        }
    }

    if (!m_font_normal)
        m_font_normal = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    if (!m_font_small)
        m_font_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
}

void COpacityPreview::ReleaseSurfaces() {
    HDC dcs[3] = { m_ulw_dc, m_cov_a_dc, m_cov_c_dc };
    HBITMAP bmps[3] = { m_ulw_bmp, m_cov_a_bmp, m_cov_c_bmp };
    for (int i = 0; i < 3; i++) {
        if (dcs[i]) { DeleteDC(dcs[i]); }
        if (bmps[i]) { DeleteObject(bmps[i]); }
    }
    m_ulw_dc = m_cov_a_dc = m_cov_c_dc = nullptr;
    m_ulw_bmp = m_cov_a_bmp = m_cov_c_bmp = nullptr;
    m_ulw_bits = m_cov_a_bits = m_cov_c_bits = nullptr;
    if (m_font_normal) { DeleteObject(m_font_normal); m_font_normal = nullptr; }
    if (m_font_small) { DeleteObject(m_font_small); m_font_small = nullptr; }
}

bool COpacityPreview::IsDarkTheme() {
    // 与悬浮窗 IsDarkMode 同源：插件颜色模式强制档优先，否则跟系统应用主题
    int cm = CDetailWindow::GetColorMode();
    if (cm == 1) return true;
    if (cm == 2) return false;
    HKEY hKey;
    DWORD val = 1, size = sizeof(DWORD);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (BYTE*)&val, &size);
        RegCloseKey(hKey);
    }
    return val == 0;
}

// 示例进程行：图标占位块 + 名称 + 右对齐速度（accent）+ 分隔线，返回下一行 y
static int DrawSampleRow(HDC dc, int y, const wchar_t* name, const wchar_t* speed,
                         COLORREF acc, COLORREF text, COLORREF sep, COLORREF icon_c,
                         HFONT font) {
    int x_name = PADDING + 2 + ICON_SIZE + 6;
    HBRUSH hIcoBrush = CreateSolidBrush(icon_c);
    HPEN hIcoPen = CreatePen(PS_SOLID, 0, icon_c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, hIcoBrush);
    HPEN op = (HPEN)SelectObject(dc, hIcoPen);
    RoundRect(dc, PADDING + 2, y + (ROW_H - ICON_SIZE) / 2,
              PADDING + 2 + ICON_SIZE, y + (ROW_H + ICON_SIZE) / 2, 4, 4);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(hIcoBrush);
    DeleteObject(hIcoPen);

    SetTextColor(dc, text);
    RECT rc_name = { x_name, y, PREV_W - PADDING - SPEED_AREA_W + 10, y + ROW_H };
    DrawTextW(dc, name, -1, &rc_name, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SetTextColor(dc, acc);
    RECT rc_spd = { PREV_W - PADDING - SPEED_AREA_W, y, PREV_W - PADDING, y + ROW_H };
    DrawTextW(dc, speed, -1, &rc_spd, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

    HPEN hSep = CreatePen(PS_SOLID, 1, sep);
    HPEN op2 = (HPEN)SelectObject(dc, hSep);
    MoveToEx(dc, x_name, y + ROW_H - 1, NULL);
    LineTo(dc, PREV_W - PADDING, y + ROW_H - 1);
    SelectObject(dc, op2);
    DeleteObject(hSep);
    return y + ROW_H;
}

// 示例画面（画在调用方铺好的底色上；颜色与悬浮窗同源，无运行时反色判定）。
// 结构复刻真实悬浮窗（2026-10-21 咩咩要求「和正常一样」，首版只画下载
// 区被否）：总速条 → ▲实时上传（橙，2 行）→ ▼实时下载（绿，3 行）→ 按钮
void COpacityPreview::DrawContent(HDC dc) {
    COLORREF text     = m_dark ? RGB(240, 240, 240) : RGB(30, 30, 30);
    COLORREF acc_up   = m_dark ? RGB(230, 150, 40)  : RGB(230, 126, 34);   // 上传=橙
    COLORREF acc_down = m_dark ? RGB(85, 208, 125)  : RGB(70, 175, 95);    // 下载=绿
    COLORREF sep      = m_dark ? RGB(45, 45, 45)    : RGB(235, 235, 235);
    COLORREF icon_c   = m_dark ? RGB(70, 70, 74)    : RGB(215, 215, 218);

    SetBkMode(dc, TRANSPARENT);
    HFONT hOld = (HFONT)SelectObject(dc, m_font_normal);

    // 总速条
    SetTextColor(dc, text);
    RECT rc_header = { PADDING, PADDING, PREV_W - PADDING, PADDING + HEADER_H };
    DrawTextW(dc, L"\u2191 3.1 MB/s   \u2193 146.3 MB/s", -1, &rc_header,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    int y = PADDING + HEADER_H;

    // ▲ 实时上传（与悬浮窗分区标题同款：accent 色 + 常规字重）
    SetTextColor(dc, acc_up);
    RECT rc_sec_up = { PADDING + 2, y, PREV_W - PADDING, y + SECTION_H };
    DrawTextW(dc, TR(L"\u25B2 \u5B9E\u65F6\u4E0A\u4F20"), -1, &rc_sec_up,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += SECTION_H;
    static const wchar_t* up_names[5] = {
        L"OneDrive", L"Chrome", L"Steam", L"WeChat", L"Edge" };
    static const wchar_t* up_speeds[5] = {
        L"2.1 MB/s", L"0.4 MB/s", L"0.3 MB/s", L"0.2 MB/s", L"0.1 MB/s" };
    for (int i = 0; i < 5; i++)
        y = DrawSampleRow(dc, y, up_names[i], up_speeds[i],
                          acc_up, text, sep, icon_c, m_font_normal);
    y += GAP_H;

    // ▼ 实时下载
    SetTextColor(dc, acc_down);
    RECT rc_sec_dn = { PADDING + 2, y, PREV_W - PADDING, y + SECTION_H };
    DrawTextW(dc, TR(L"\u25BC \u5B9E\u65F6\u4E0B\u8F7D"), -1, &rc_sec_dn,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    y += SECTION_H;
    static const wchar_t* dn_names[5] = {
        L"Chrome", L"Steam", L"Edge", L"OneDrive", L"Firefox" };
    static const wchar_t* dn_speeds[5] = {
        L"87.2 MB/s", L"41.3 MB/s", L"12.6 MB/s", L"3.4 MB/s", L"1.8 MB/s" };
    for (int i = 0; i < 5; i++)
        y = DrawSampleRow(dc, y, dn_names[i], dn_speeds[i],
                          acc_down, text, sep, icon_c, m_font_normal);
    y += 4;   // BTN_AREA_H - BTN_H

    // 「查看详细」按钮（实心自绘，配色与悬浮窗同款）
    COLORREF btn_bg  = m_dark ? RGB(50, 50, 54)  : RGB(235, 235, 235);
    COLORREF btn_pen = m_dark ? RGB(70, 70, 74)  : RGB(210, 210, 210);
    COLORREF btn_txt = m_dark ? RGB(255, 165, 0) : RGB(230, 126, 34);
    RECT rc_btn = { PADDING, y, PREV_W - PADDING, y + 24 };
    HBRUSH hBtnBrush = CreateSolidBrush(btn_bg);
    HPEN hBtnPen = CreatePen(PS_SOLID, 1, btn_pen);
    HBRUSH ob2 = (HBRUSH)SelectObject(dc, hBtnBrush);
    HPEN op3 = (HPEN)SelectObject(dc, hBtnPen);
    RoundRect(dc, rc_btn.left, rc_btn.top, rc_btn.right, rc_btn.bottom, 6, 6);
    SelectObject(dc, ob2);
    SelectObject(dc, op3);
    DeleteObject(hBtnBrush);
    DeleteObject(hBtnPen);
    HFONT hOldSmall = (HFONT)SelectObject(dc, m_font_small);
    SetTextColor(dc, btn_txt);
    DrawTextW(dc, TR(L"\u67E5\u770B\u8BE6\u7EC6"), -1, &rc_btn,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, hOldSmall);

    SelectObject(dc, hOld);
}

// coverage 双底解算 + 预乘合成 + ULW 提交（公式与悬浮窗 ComposeLayeredSurface
// 同源：黑底 pass 是内容色×通道覆盖率的预乘，逐通道除回；死点邻像素回填；
// 亚克力噪声确定性哈希防逐帧闪）
void COpacityPreview::Compose() {
    if (!m_ulw_bits || !m_cov_a_bits || !m_cov_c_bits) return;

    // 1. 双底内容
    RECT full = { 0, 0, PREV_W, PREV_H };
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(m_cov_a_dc, &full, wb);
    DeleteObject(wb);
    HBRUSH bb = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(m_cov_c_dc, &full, bb);
    DeleteObject(bb);
    DrawContent(m_cov_a_dc);
    DrawContent(m_cov_c_dc);

    // 2. 合成
    unsigned char* pa = (unsigned char*)m_cov_a_bits;
    unsigned char* pc = (unsigned char*)m_cov_c_bits;
    unsigned char* pw = (unsigned char*)m_ulw_bits;

    int panel_r = m_dark ? 32 : 249, panel_g = m_dark ? 32 : 249, panel_b = m_dark ? 32 : 249;
    int bg_a = m_opacity * 255 / 100;
    int r = CORNER_RADIUS;
    int w = PREV_W, h = PREV_H;

    std::vector<int> frow((size_t)w * 3);
    std::vector<int> crow((size_t)w);
    std::vector<float> grow((size_t)w);
    std::vector<unsigned char> drow((size_t)w);

    for (int y = 0; y < h; y++) {
        float cy = y + 0.5f;
        for (int x = 0; x < w; x++) {
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
            grow[x] = cov;
            size_t i = ((size_t)y * w + x) * 4;
            if (cov <= 0.0f) {
                crow[x] = 0;
                frow[x * 3] = frow[x * 3 + 1] = frow[x * 3 + 2] = 0;
                drow[x] = 0;
                pw[i] = pw[i + 1] = pw[i + 2] = pw[i + 3] = 0;
                continue;
            }
            int ab = 255 - pa[i] + pc[i];
            int ag = 255 - pa[i + 1] + pc[i + 1];
            int ar = 255 - pa[i + 2] + pc[i + 2];
            int cov255 = (ab + ag + ar) / 3;
            crow[x] = cov255;
            int fs = (pc[i] + pc[i + 1] + pc[i + 2]) * 255 / (3 * (cov255 > 0 ? cov255 : 1));
            frow[x * 3 + 0] = ab > 1 ? pc[i] * 255 / ab : fs;
            frow[x * 3 + 1] = ag > 1 ? pc[i + 1] * 255 / ag : fs;
            frow[x * 3 + 2] = ar > 1 ? pc[i + 2] * 255 / ar : fs;
            drow[x] = (unsigned char)((ab <= 1 ? 1 : 0) | (ag <= 1 ? 2 : 0) | (ar <= 1 ? 4 : 0));
        }
        for (int rep = 0; rep < 2; rep++) {
            for (int x = 0; x < w; x++) {
                if (!drow[x] || crow[x] <= 0) continue;
                for (int c = 0; c < 3; c++) {
                    int bit = 1 << c;
                    if (!(drow[x] & bit)) continue;
                    bool got = false;
                    int v = 0;
                    if (x > 0 && crow[x - 1] > 0 && !(drow[x - 1] & bit)) {
                        v = frow[(x - 1) * 3 + c]; got = true;
                    }
                    if (x + 1 < w && crow[x + 1] > 0 && !(drow[x + 1] & bit)) {
                        int rv = frow[(x + 1) * 3 + c];
                        v = got ? max(v, rv) : rv; got = true;
                    }
                    if (got) {
                        frow[x * 3 + c] = v;
                        drow[x] = (unsigned char)(drow[x] & ~bit);
                    }
                }
            }
        }
        for (int x = 0; x < w; x++) {
            size_t i = ((size_t)y * w + x) * 4;
            float cov = grow[x];
            int cov255 = crow[x];
            if (cov <= 0.0f) continue;   // 圆角外已清零

            int nz = 0;
            if (m_noise) {
                nz = ((x * 73856093) ^ (y * 19349663)) & 7;   // 0..7
                nz = nz - 3;                                   // -3..+3
            }

            int a = cov255 + (bg_a * (255 - cov255) + 127) / 255;
            a = (int)(a * cov + 0.5f);
            if (a <= 0) {
                pw[i] = pw[i + 1] = pw[i + 2] = pw[i + 3] = 0;
                continue;
            }
            if (cov255 >= 255) {
                pw[i]     = (unsigned char)min(255, (int)(frow[x * 3 + 0] * cov + 0.5f));
                pw[i + 1] = (unsigned char)min(255, (int)(frow[x * 3 + 1] * cov + 0.5f));
                pw[i + 2] = (unsigned char)min(255, (int)(frow[x * 3 + 2] * cov + 0.5f));
            } else if (cov255 <= 0) {
                int pb = max(0, min(255, panel_b + nz)) * bg_a / 255;
                int pg = max(0, min(255, panel_g + nz)) * bg_a / 255;
                int pr = max(0, min(255, panel_r + nz)) * bg_a / 255;
                pw[i]     = (unsigned char)min(255, (int)(pb * cov + 0.5f));
                pw[i + 1] = (unsigned char)min(255, (int)(pg * cov + 0.5f));
                pw[i + 2] = (unsigned char)min(255, (int)(pr * cov + 0.5f));
            } else {
                int inv = 255 - cov255;
                int fr = frow[x * 3 + 2], fg = frow[x * 3 + 1], fb = frow[x * 3 + 0];
                int pnr = max(0, panel_r + nz), png = max(0, panel_g + nz), pnb = max(0, panel_b + nz);
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

    // 3. ULW 提交
    SIZE size = { PREV_W, PREV_H };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(NULL);
    UpdateLayeredWindow(m_hwnd, screen, NULL, &size, m_ulw_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

void COpacityPreview::Show(HWND hDlg, int effect, int opacity) {
    if (!hDlg || effect < 0 || effect > 2) return;
    if (opacity < 10) opacity = 10;
    if (opacity > 100) opacity = 100;
    m_effect = effect;
    m_opacity = opacity;
    m_dark = IsDarkTheme();
    m_noise = (effect == 0);

    if (!EnsureWindow()) return;
    EnsureSurfaces();
    if (!m_ulw_bits || !m_cov_a_bits || !m_cov_c_bits) return;

    // 定位：对话框右侧、与滑条行同水平线；右侧放不下退左侧，工作区 clamp
    RECT rd = {}, rt = {};
    GetWindowRect(hDlg, &rd);
    HWND hTrack = GetDlgItem(hDlg, 1024);
    int line_cy = rd.top + (rd.bottom - rd.top) / 2;
    if (hTrack && GetWindowRect(hTrack, &rt))
        line_cy = (rt.top + rt.bottom) / 2;
    int x = rd.right + 12;
    int y = line_cy - PREV_H / 2;
    HMONITOR mon = MonitorFromWindow(hDlg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi)) {
        const RECT& wa = mi.rcWork;
        if (x + PREV_W > wa.right) x = rd.left - 12 - PREV_W;
        if (y + PREV_H > wa.bottom) y = wa.bottom - PREV_H;
        if (y < wa.top) y = wa.top;
        if (x < wa.left) x = wa.left;
    }

    // blur 层先就位（亚克力/模糊档），内容层随后压上（同悬浮窗 Z 序手法）
    if ((effect == 0 || effect == 2) && m_blur_hwnd) {
        SetWindowPos(m_blur_hwnd, HWND_TOPMOST, x, y, PREV_W, PREV_H,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (m_blur_hwnd) {
        ShowWindow(m_blur_hwnd, SW_HIDE);   // 透明档：无模糊层
    }
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, PREV_W, PREV_H,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    Compose();
    SetTimer(m_hwnd, TIMER_AUTOHIDE, 2000, NULL);   // 重置即每次滑动续 2s
}

void COpacityPreview::Hide() {
    DestroyWindowpair();
    ReleaseSurfaces();
}
