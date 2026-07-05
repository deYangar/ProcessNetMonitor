#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
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
    // hovered_item_rect: screen rect of the hovered plugin item
    void UpdateAndShow(const std::vector<ProcDisplayInfo>& procs,
                       double total_up, double total_down,
                       const RECT& item_rect, bool is_taskbar);
    void Hide();

    // Call this in DataRequired() to check if we should show/hide
    // Returns true if popup state changed
    bool TickCheck(HWND taskbar_wnd);

    bool IsVisible() const { return m_visible; }
    bool IsHovering() const { return m_hovering; }
    HWND GetHwnd() const { return m_hwnd; }

    // Process icon cache
    HICON GetProcessIcon(const std::wstring& exe_path);

    // Public for standalone WndProc access
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    // Window
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    void OnMouseLeave();

    // Layout & rendering
    void CalcLayout(HDC hdc, int& out_w, int& out_h, int& out_up_count, int& out_down_count);
    void DrawBackground(HDC hdc, int w, int h);
    void DrawProcRow(HDC hdc, int y, const ProcDisplayInfo& proc, bool is_upload);
    void DrawSectionTitle(HDC hdc, int y, const wchar_t* title, bool is_upload);
    void PositionWindow(const RECT& anchor_rect);

    // Helpers
    bool IsDarkMode();
    void FormatSpeed(double bps, wchar_t* buf, int n);
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

    // Icon cache: exe_path -> HICON
    std::map<std::wstring, HICON> m_icon_cache;

    // Layout constants
    static const int PADDING = 12;
    static const int ROW_HEIGHT = 32;
    static const int ICON_SIZE = 22;
    static const int CORNER_RADIUS = 8;
    static const int MIN_WIDTH = 300;

public:
    static const int MAX_SHOW = 5;  // max processes per section

    // Mouse tracking
    bool m_tracking = false;

    static CTooltipPopup* s_instance;
};
