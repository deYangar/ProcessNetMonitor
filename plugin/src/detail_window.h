#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "capture.h"

// Huorong-style detail window with sortable table, tabs, context menu
class CDetailWindow {
public:
    struct Column {
        const wchar_t* title;
        int width;
        int min_width;
        enum Align { LEFT, RIGHT, CENTER } align;
    };

    CDetailWindow();
    ~CDetailWindow();

    bool Initialize(HINSTANCE hInst);
    void Show(HWND parent_wnd);
    void Hide();
    void UpdateData(const std::vector<ProcTraffic>& stats, double total_up, double total_down);
    bool IsVisible() const { return m_visible; }
    HWND GetHwnd() const { return m_hwnd; }

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    // Window
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    void OnSize(int w, int h);
    void OnLButtonDown(int x, int y);
    void OnRButtonDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnMouseWheel(int delta);
    void OnVScroll(int code);
    void OnLButtonDblClk(int x, int y);

    // Drawing
    void DrawTitleBar(HDC hdc, int w);
    void DrawTabs(HDC hdc, int w, int y);
    void DrawSpeedSummary(HDC hdc, int w, int y);
    void DrawTableHeader(HDC hdc, int w, int y);
    void DrawTableRows(HDC hdc, int w, int y, int h);
    void DrawScrollbar(HDC hdc, int w, int h);

    // Layout
    int GetHeaderHeight() const;
    int GetTabsHeight() const;
    int GetSummaryHeight() const;
    int GetTableHeaderHeight() const;
    int GetRowHeight() const;
    int GetTableAreaTop() const;
    int GetVisibleRows(int client_h) const;

    // Table interaction
    int HitTestRow(int y) const;
    int HitTestColumn(int x) const;
    void SortByColumn(int col);
    void ResortRows();
    void RebuildRows();
    void ShowContextMenu(int row, int x, int y);
    void ToggleExpand(int row);
    void ScrollTo(int pos);

    // Helpers
    bool IsDarkMode();
    void FormatSpeed(double bps, wchar_t* buf, int n);
    HICON GetProcessIcon(const std::wstring& exe_path);

    // Colors
    COLORREF GetBgColor();
    COLORREF GetTextColor();
    COLORREF GetSecondaryTextColor();
    COLORREF GetAccentColor(bool is_upload);
    COLORREF GetHeaderBgColor();
    COLORREF GetRowBgColor(int row, bool hovered);
    COLORREF GetBorderColor();
    COLORREF GetTitleBarColor();

    // Data row for display
    struct DisplayRow {
        DWORD pid = 0;
        std::wstring name;          // exe name
        std::wstring display_name;  // friendly name / description
        std::wstring category;      // 程序类别
        std::wstring exe_path;
        double speed_up = 0;
        double speed_down = 0;
        int conn_count = 0;
        bool expanded = false;
        // Children (for tree view)
        struct ChildRow {
            std::wstring name;
            std::wstring remote_addr;
            uint16_t remote_port = 0;
            double speed = 0;
        };
        std::vector<ChildRow> children;
    };

    // State
    HWND m_hwnd = nullptr;
    HWND m_parent_wnd = nullptr;
    HINSTANCE m_hinst = nullptr;
    bool m_visible = false;
    bool m_dark_mode = true;
    bool m_tracking_mouse = false;

    // Data
    std::vector<DisplayRow> m_rows;
    double m_total_up = 0;
    double m_total_down = 0;

    // Cached raw stats for tab switching
    std::vector<ProcTraffic> m_cached_stats;

    // Table state
    int m_sort_col = 3;        // default sort by download speed
    bool m_sort_desc = true;
    int m_scroll_pos = 0;      // first visible row index
    int m_hovered_row = -1;
    int m_hovered_col = -1;
    bool m_hovering_title = false;
    bool m_hovering_close = false;
    bool m_hovering_min = false;
    bool m_hovering_tab0 = false;
    bool m_hovering_tab1 = false;
    int m_active_tab = 0;      // 0=实时流量, 1=历史流量

    // Columns
    enum ColIndex { COL_ICON, COL_NAME, COL_CATEGORY, COL_DOWN, COL_UP, COL_CONN, COL_ACTION };
    Column m_columns[7] = {
        { L"",            32,  32,  Column::CENTER },  // icon
        { L"\u7A0B\u5E8F\u540D\u79F0",    180, 120, Column::LEFT   },
        { L"\u7A0B\u5E8F\u7C7B\u522B",    90,  70,  Column::LEFT   },
        { L"\u4E0B\u8F7D\u901F\u5EA6",    100, 80,  Column::RIGHT  },
        { L"\u4E0A\u4F20\u901F\u5EA6",    100, 80,  Column::RIGHT  },
        { L"\u8FDE\u63A5\u6570",          60,  50,  Column::RIGHT  },
        { L"\u64CD\u4F5C",              80,  60,  Column::CENTER },
    };

    // Layout constants
    static const int TITLE_BAR_H = 36;
    static const int TAB_BAR_H = 32;
    static const int SUMMARY_H = 28;
    static const int TABLE_HEADER_H = 28;
    static const int ROW_H = 36;
    static const int CHILD_ROW_H = 28;
    static const int PADDING = 10;
    static const int CORNER_RADIUS = 8;
    static const int ICON_SIZE = 20;
    static const int SCROLL_W = 8;
    static const int MIN_WIDTH = 620;
    static const int MIN_HEIGHT = 400;

    // Icon cache
    std::map<std::wstring, HICON> m_icon_cache;

    // Dragging
    bool m_dragging = false;
    POINT m_drag_offset = {};
    int m_drag_col = -1;       // column resize drag
    int m_drag_col_x = 0;

    // Title bar buttons
    RECT m_rcTitle = {};
    RECT m_rcClose = {};
    RECT m_rcMin = {};
    RECT m_rcRefresh = {};
    RECT m_rcTab0 = {};
    RECT m_rcTab1 = {};
    RECT m_rcTableHeader = {};
    RECT m_rcScrollbar = {};
    RECT m_rcScrollbarThumb = {};

public:
    static CDetailWindow* s_instance;
};
