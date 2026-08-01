#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "capture.h"

// Rich tooltip popup window - shows process icons + speed info
// Replaces the plain-text TM tooltip with a Huorong-style popup
class CTooltipPopup {
public:
    struct ProcDisplayInfo {
        std::wstring name;
        std::wstring exe_path;    // full path for icon extraction
        double speed_up = 0;
        double speed_down = 0;
    };

    CTooltipPopup();
    ~CTooltipPopup();

    // Call once during plugin init
    bool Initialize(HINSTANCE hInst);

    // Update data and show/hide popup
    // anchor_rect: screen rect the popup should attach to (TM window or click area)
    void UpdateAndShow(const std::vector<ProcDisplayInfo>& procs,
                       double total_up, double total_down,
                       const RECT& anchor_rect);
    // Update data only (no reposition); used while popup stays visible
    void UpdateData(const std::vector<ProcDisplayInfo>& procs,
                    double total_up, double total_down);
    void Hide();

    bool IsVisible() const { return m_visible; }
    bool IsHovering() const { return m_hovering; }
    HWND GetHwnd() const { return m_hwnd; }

    // Process icon cache
    HICON GetProcessIcon(const std::wstring& exe_path);

    // Public for standalone WndProc access
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    // Window
    void OnPaint();
    void OnMouseLeave();

    // Layout & rendering
    void CalcLayout(HDC hdc, int& out_w, int& out_h, int& out_up_count, int& out_down_count);
    void DrawBackground(HDC hdc, int w, int h);
    void DrawProcRow(HDC hdc, int y, const ProcDisplayInfo& proc, bool is_upload);
    void DrawSectionTitle(HDC hdc, int y, const wchar_t* title, bool is_upload);
    void PositionWindow(const RECT& anchor_rect);

    // DPI handling (per-monitor, same approach as TM's DPIFromRect)
    void UpdateDpiScale(const RECT& anchor_rect);
    void RecreateScaledResources();

    // Helpers
    bool IsDarkMode();
    COLORREF GetBgColor();
    COLORREF GetTextColor();
    COLORREF GetAccentColor(bool is_upload);
    COLORREF GetSectionBgColor();

    // State
    HWND m_hwnd = nullptr;
    HINSTANCE m_hinst = nullptr;
    bool m_visible = false;
    bool m_hovering = false;  // mouse is over the popup itself
    bool m_dark_mode = true;

    // Data
    std::vector<ProcDisplayInfo> m_procs;
    double m_total_up = 0;
    double m_total_down = 0;
    RECT m_last_anchor = {};  // anchor rect used for the current show session

    // Icon cache: exe_path -> HICON
    std::unordered_map<std::wstring, HICON> m_icon_cache;

    // Cached GDI objects (created once, reused in OnPaint)
    HFONT m_font_normal = nullptr;   // Segoe UI -15
    HFONT m_font_small = nullptr;    // Microsoft YaHei -13 (section title + detail button)
    HPEN m_pen_separator = nullptr;  // row separator

    // IsDarkMode cache
    bool m_dark_mode_cached = true;
    ULONGLONG m_dark_mode_tick = 0;

    // Layout constants (DPI-scaled; BASE_ values are for 96 DPI / 100%)
    int PADDING = 12;
    int ROW_HEIGHT = 32;
    int ICON_SIZE = 22;
    int CORNER_RADIUS = 8;
    int MIN_WIDTH = 300;
    int HEADER_H = 34;      // total speed header band
    int SECTION_H = 24;     // section title step
    int GAP_H = 4;          // gap between upload/download sections
    int BTN_AREA_H = 28;    // "查看详细" button band
    int BTN_H = 24;         // button inner height
    int SPEED_AREA_W = 110; // right-aligned speed column width

    static const int BASE_PADDING = 12;
    static const int BASE_ROW_HEIGHT = 32;
    static const int BASE_ICON_SIZE = 22;
    static const int BASE_CORNER_RADIUS = 8;
    static const int BASE_MIN_WIDTH = 300;
    static const int BASE_HEADER_H = 34;
    static const int BASE_SECTION_H = 24;
    static const int BASE_GAP_H = 4;
    static const int BASE_BTN_AREA_H = 28;
    static const int BASE_BTN_H = 24;
    static const int BASE_SPEED_AREA_W = 110;

    float m_dpi_scale = 1.0f;

public:
    static const int MAX_SHOW = 5;  // max processes per section
    static const UINT_PTR TIMER_HOVER = 1;  // 100ms hover state machine timer

    // Mouse tracking
    bool m_tracking = false;

    // "查看详细" button rect (updated in OnPaint)
    RECT m_rcDetailBtn = {};

    static CTooltipPopup* s_instance;
};
