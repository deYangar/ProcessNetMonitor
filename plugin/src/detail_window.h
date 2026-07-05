#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <set>
#include <functional>
#include "capture.h"

// Huorong-style detail window with sortable table, tabs, history, context menu
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

    // History time range
    enum TimeRange { TR_24H = 0, TR_3D, TR_7D, TR_30D };

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
    void DrawTimeRangeButtons(HDC hdc, int w, int y);
    void DrawSpeedSummary(HDC hdc, int w, int y);
    void DrawTableHeader(HDC hdc, int w, int y);
    void DrawTableRows(HDC hdc, int w, int y, int h);
    void DrawScrollbar(HDC hdc, int w, int h);

    // Layout
    int GetHeaderHeight() const;
    int GetTabsHeight() const;
    int GetTimeRangeHeight() const;
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

    // History
    void RecordHistory(const std::vector<ProcTraffic>& stats);
    void BuildHistoryRows();

    // Helpers
    bool IsDarkMode();
    void FormatSpeed(double bps, wchar_t* buf, int n);
    void FormatBytes(uint64_t bytes, wchar_t* buf, int n);
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
        // History fields
        uint64_t hist_recv = 0;     // total bytes received in time range
        uint64_t hist_sent = 0;     // total bytes sent in time range
        double hist_avg_down = 0;   // average download speed
        double hist_avg_up = 0;     // average upload speed
    };

    // History snapshot per process
    struct HistorySnapshot {
        ULONGLONG tick;         // GetTickCount64
        uint64_t cum_recv;      // cumulative bytes received
        uint64_t cum_sent;      // cumulative bytes sent
    };

    struct ProcessHistory {
        std::wstring name;
        std::wstring exe_path;
        std::deque<HistorySnapshot> snapshots;  // 1 per second, max 3600
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

    // History: process name -> history
    std::map<std::wstring, ProcessHistory> m_history;
    ULONGLONG m_history_start_tick = 0;  // when TM plugin started
    static const int MAX_HISTORY_SNAPSHOTS = 3600;  // 1 hour at 1/sec

    // Time range filter (history tab)
    TimeRange m_time_range = TR_24H;

    // Table state (per-tab sort)
    int m_sort_col[2] = { 3, 3 };     // default sort by download/total_down
    bool m_sort_desc[2] = { true, true };
    int m_scroll_pos = 0;
    int m_hovered_row = -1;
    int m_hovered_col = -1;
    bool m_hovering_close = false;
    bool m_hovering_min = false;
    int m_active_tab = 0;      // 0=实时流量, 1=历史流量

    // Columns - real-time
    enum ColIndex { COL_ICON, COL_NAME, COL_CATEGORY, COL_DOWN, COL_UP, COL_CONN, COL_ACTION };
    static const int NUM_COLS = 7;
    Column m_rt_cols[NUM_COLS] = {
        { L"",            32,  32,  Column::CENTER },
        { L"\u7A0B\u5E8F\u540D\u79F0",    180, 120, Column::LEFT   },
        { L"\u7A0B\u5E8F\u7C7B\u522B",    90,  70,  Column::LEFT   },
        { L"\u4E0B\u8F7D\u901F\u5EA6",    100, 80,  Column::RIGHT  },
        { L"\u4E0A\u4F20\u901F\u5EA6",    100, 80,  Column::RIGHT  },
        { L"\u8FDE\u63A5\u6570",          60,  50,  Column::RIGHT  },
        { L"\u64CD\u4F5C",              80,  60,  Column::CENTER },
    };
    // Columns - history
    Column m_hist_cols[NUM_COLS] = {
        { L"",            32,  32,  Column::CENTER },
        { L"\u7A0B\u5E8F\u540D\u79F0",    180, 120, Column::LEFT   },
        { L"\u7A0B\u5E8F\u7C7B\u522B",    90,  70,  Column::LEFT   },
        { L"\u603B\u4E0B\u8F7D",          100, 80,  Column::RIGHT  },
        { L"\u603B\u4E0A\u4F20",          100, 80,  Column::RIGHT  },
        { L"\u5E73\u5747\u4E0B\u8F7D",    100, 80,  Column::RIGHT  },
        { L"\u5E73\u5747\u4E0A\u4F20",    100, 80,  Column::RIGHT  },
    };
    Column* GetActiveCols() const { return (Column*)(m_active_tab == 0 ? m_rt_cols : m_hist_cols); }

    // Layout constants
    static const int TITLE_BAR_H = 36;
    static const int TAB_BAR_H = 32;
    static const int TIME_RANGE_H = 30;  // only shown in history tab
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

    // Title bar / UI rects
    RECT m_rcClose = {};
    RECT m_rcMin = {};
    RECT m_rcTab0 = {};
    RECT m_rcTab1 = {};
    RECT m_rcTRButtons[4] = {};  // time range buttons
    RECT m_rcTableHeader = {};
    RECT m_rcScrollbar = {};

public:
    static CDetailWindow* s_instance;
};
