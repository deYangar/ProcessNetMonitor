#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
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

    // Persistence (called from plugin_main)
    void SaveHistory();
    void LoadHistory();
    void SetConfigDir(const std::wstring& dir) { m_config_dir = dir; }
    void RecordHistory(const std::vector<ProcTraffic>& stats);
    void SaveSettings();
    void LoadSettings();
    std::vector<std::wstring> GetTunRanges() const { return m_tun_ranges; }
    
    // 设置 PacketCapture 指针（用于获取连接详情）
    void SetCapture(PacketCapture* capture) { m_capture = capture; }


    // History time range
    enum TimeRange { TR_24H = 0, TR_3D, TR_7D, TR_30D };

    // History data types (public for persistence helpers)
    struct HistorySnapshot {
        ULONGLONG tick;
        uint64_t cum_recv;
        uint64_t cum_sent;
        bool loaded = false;  // true = loaded from disk (old session)
    };
    struct ProcessHistory {
        std::wstring name;
        std::wstring exe_path;
        std::deque<HistorySnapshot> raw;
        std::deque<HistorySnapshot> min;
        std::deque<HistorySnapshot> hour;
        std::deque<HistorySnapshot> day;
    };

private:
    // Window
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    void OnPaint();
    void OnSize(int w, int h);
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
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
    bool HitTestScrollbar(int x, int y) const;
    void GetScrollbarThumbRect(RECT* rc, int w, int h) const;
    int HitTestColumn(int x) const;
    void SortByColumn(int col);
    void ResortRows();
    void RebuildRows();
    void ShowContextMenu(int row, int x, int y);
    void ToggleExpand(int row);
    void ScrollTo(int pos);

    // History
    void BuildHistoryRows();

    std::wstring m_config_dir;

    // Helpers
    bool IsDarkMode();
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
    struct SubProcess {
        DWORD pid = 0;
        std::wstring exe_path;
        double speed_up = 0;
        double speed_down = 0;
        int conn_count = 0;
        std::vector<ConnDetail> connections;
        bool connections_loaded = false;
        bool conn_expanded = false;
    };
    struct DisplayRow {
        DWORD pid = 0;              // first PID for icon
        std::wstring name;          // exe name
        std::wstring display_name;  // friendly name / description
        std::wstring category;      // 程序类别
        std::wstring exe_path;      // first process path for icon
        double speed_up = 0;        // aggregated
        double speed_down = 0;      // aggregated
        int conn_count = 0;         // aggregated
        bool expanded = false;
        // History fields
        uint64_t hist_recv = 0;
        uint64_t hist_sent = 0;
        double hist_avg_down = 0;
        double hist_avg_up = 0;
        
        // For single-process rows (backward compat)
        std::vector<ConnDetail> connections;
        bool connections_loaded = false;
        bool conn_expanded = false;
        
        // Multi-process support
        std::vector<SubProcess> sub_processes;  // when multiple PIDs share same name
        
        bool IsMultiProcess() const { return sub_processes.size() > 1; }
    };

    void CompressHistory();
    void GetMergedSnapshots(const ProcessHistory& h, std::vector<const HistorySnapshot*>& out, ULONGLONG cutoff) const;
    uint64_t m_hist_total_recv = 0;  // summary for current time range
    uint64_t m_hist_total_sent = 0;

    // State
    HWND m_hwnd = nullptr;
    HWND m_parent_wnd = nullptr;
    HINSTANCE m_hinst = nullptr;
    bool m_visible = false;
    bool m_dark_mode = true;
    bool m_tracking_mouse = false;
    bool m_context_menu_open = false;
    bool m_history_dirty = true;  // only rebuild history rows when new data arrives
    DWORD m_context_menu_pid = 0;
    std::wstring m_context_menu_path;

    // Data
    std::vector<DisplayRow> m_rows;
    double m_total_up = 0;
    double m_total_down = 0;

    // Cached raw stats for tab switching
    std::vector<ProcTraffic> m_cached_stats;

    // History: process name -> history
    std::unordered_map<std::wstring, ProcessHistory> m_history;
    // Last known cumulative bytes per process (for delta calculation in RecordHistory)
    struct CumBytes { uint64_t recv; uint64_t sent; };
    std::unordered_map<std::wstring, CumBytes> m_last_cum;
    ULONGLONG m_session_start_tick = 0;  // GetTickCount64 when TM started
    ULONGLONG m_history_start_tick = 0;  // wall-clock ms when plugin started
    ULONGLONG m_last_save_tick = 0;
    // Tier limits & time constants
    static const int MAX_RAW = 3600;      // ~1 hour at 1/sec
    static const int MAX_MIN = 1440;      // ~24 hours at 1/min
    static const int MAX_HOUR = 168;      // ~7 days at 1/hour
    static const int MAX_DAY = 365;       // ~1 year at 1/day
    static constexpr ULONGLONG MS_PER_HOUR  = 3600000ULL;
    static constexpr ULONGLONG MS_PER_DAY   = 86400000ULL;
    static constexpr ULONGLONG MS_PER_WEEK  = 604800000ULL;
    static constexpr ULONGLONG MS_PER_YEAR  = 365ULL * 24 * 3600 * 1000;

    // Time range filter (history tab)
    TimeRange m_time_range = TR_24H;

    // Table state (per-tab sort)
    int m_sort_col[2] = { 3, 3 };     // default sort by download/total_down
    bool m_sort_asc[2] = { false, false };  // false=descending(big first), true=ascending(small first)
    int m_scroll_pos = 0;  // pixel offset
    bool m_dragging_scrollbar = false;
    int m_drag_start_y = 0;
    int m_drag_start_scroll = 0;
    int m_hovered_row = -1;
    int m_hovered_col = -1;
    float m_dpi_scale = 1.0f;
    void UpdateDpiScale(HWND hwnd = nullptr);
    void CreateFonts();
    void RecreateGdiObjects();
    bool m_hovering_close = false;
    bool m_hovering_min = false;
    int m_active_tab = 0;      // 0=实时流量, 1=历史流量
    
    // TUN address ranges (user configurable)
    std::vector<std::wstring> m_tun_ranges = { L"198.18.0.0/15" };

    // Columns - real-time
    enum ColIndex { COL_ICON, COL_NAME, COL_CATEGORY, COL_DOWN, COL_UP, COL_CONN, COL_ACTION };
    static const int NUM_COLS = 7;
    Column m_rt_cols[NUM_COLS] = {
        { L"",            42,  42,  Column::CENTER },
        { L"\u7A0B\u5E8F\u540D\u79F0",    180, 120, Column::LEFT   },
        { L"\u7A0B\u5E8F\u7C7B\u522B",    90,  70,  Column::LEFT   },
        { L"\u4E0B\u8F7D\u901F\u5EA6",    100, 80,  Column::RIGHT  },
        { L"\u4E0A\u4F20\u901F\u5EA6",    100, 80,  Column::RIGHT  },
        { L"\u8FDE\u63A5\u6570",          60,  50,  Column::RIGHT  },
        { L"\u64CD\u4F5C",              80,  60,  Column::CENTER },
    };
    // Columns - history
    Column m_hist_cols[NUM_COLS] = {
        { L"",            42,  42,  Column::CENTER },
        { L"\u7A0B\u5E8F\u540D\u79F0",    140, 80,  Column::LEFT   },
        { L"\u7A0B\u5E8F\u7C7B\u522B",    70,  50,  Column::LEFT   },
        { L"\u603B\u4E0B\u8F7D",          90,  60,  Column::RIGHT  },
        { L"\u603B\u4E0A\u4F20",          90,  60,  Column::RIGHT  },
        { L"\u5E73\u5747\u4E0B\u8F7D",    85,  60,  Column::RIGHT  },
        { L"\u5E73\u5747\u4E0A\u4F20",    85,  60,  Column::RIGHT  },
    };
    Column* GetActiveCols() { return (m_active_tab == 0) ? m_rt_cols : m_hist_cols; }
    const Column* GetActiveCols() const { return (m_active_tab == 0) ? m_rt_cols : m_hist_cols; }

    // Layout constants (DPI-scaled at init)
    int TITLE_BAR_H = 36;
    int TAB_BAR_H = 32;
    int TIME_RANGE_H = 30;
    int SUMMARY_H = 28;
    int TABLE_HEADER_H = 28;
    int ROW_H = 36;
    int CHILD_ROW_H = 28;
    int PADDING = 10;
    int CORNER_RADIUS = 8;
    int ICON_SIZE = 20;
    int SCROLL_W = 8;
    int MIN_WIDTH = 680;
    int MIN_HEIGHT = 400;
    static const UINT_PTR TIMER_DEFER_REBUILD = 1;
    
    // Connection table constants (DPI-scaled)
    int CONN_HEADER_H = 24;
    int CONN_ROW_H = 22;
    static const int MAX_CONN_ROWS = 10;
    int CONN_TABLE_PADDING = 12;
    int SUBPROC_HEADER_H = 24;
    int SUBPROC_INDENT = 20;
    
    // Base layout values (before DPI scaling)
    static const int BASE_TITLE_BAR_H = 36;
    static const int BASE_TAB_BAR_H = 32;
    static const int BASE_TIME_RANGE_H = 30;
    static const int BASE_SUMMARY_H = 28;
    static const int BASE_TABLE_HEADER_H = 28;
    static const int BASE_ROW_H = 36;
    static const int BASE_CHILD_ROW_H = 28;
    static const int BASE_PADDING = 10;
    static const int BASE_CORNER_RADIUS = 8;
    static const int BASE_ICON_SIZE = 20;
    static const int BASE_SCROLL_W = 8;
    static const int BASE_CONN_HEADER_H = 26;
    static const int BASE_CONN_ROW_H = 26;
    static const int BASE_CONN_TABLE_PADDING = 12;
    static const int BASE_SUBPROC_HEADER_H = 24;
    static const int BASE_SUBPROC_INDENT = 20;
    static const int BASE_MIN_WIDTH = 680;
    static const int BASE_MIN_HEIGHT = 400;
    
    // Connection table columns
    struct ConnColumn {
        const wchar_t* title;
        int width;
    };
    static const int NUM_CONN_COLS = 4;
    ConnColumn m_conn_cols[NUM_CONN_COLS] = {
        { L"\u534F\u8BAE",    50  },
        { L"\u672C\u5730\u5730\u5740", 180 },
        { L"\u8FDC\u7A0B\u5730\u5740", 180 },
        { L"\u72B6\u6001",    100 },
    };

    // Icon cache
    std::unordered_map<std::wstring, HICON> m_icon_cache;
    
    // PacketCapture pointer for connection details
    PacketCapture* m_capture = nullptr;
    
    // Get expanded row height
    int GetExpandedRowHeight(const DisplayRow& row) const;
    int GetTotalHeight() const;
    int GetVisibleHeight() const;
    int GetScrollMax() const;

    // Cached GDI objects (created once, reused in OnPaint)
    HFONT m_font_title = nullptr;     // Microsoft YaHei -14 semibold
    HFONT m_font_row = nullptr;       // Microsoft YaHei -14 normal
    HFONT m_font_header = nullptr;    // Microsoft YaHei -13 semibold
    HFONT m_font_small = nullptr;     // Microsoft YaHei -11 normal (expand arrow)
    HFONT m_font_time = nullptr;      // Microsoft YaHei -12 normal (time range buttons)
    HPEN m_pen_border = nullptr;      // row bottom border
    HPEN m_pen_border_exp = nullptr;  // expanded row border
    HBRUSH m_br_row[2] = {};          // row bg: [0]=even, [1]=odd
    HBRUSH m_br_hover = nullptr;      // hovered row bg
    HBRUSH m_br_child = nullptr;      // expanded child row bg

    // IsDarkMode cache
    bool m_dark_mode_cached = true;
    ULONGLONG m_dark_mode_tick = 0;

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
