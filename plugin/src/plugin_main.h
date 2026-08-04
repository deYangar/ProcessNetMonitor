#pragma once
#include <winsock2.h>
#include <windows.h>
#include "PluginInterface.h"
#include "capture.h"
#include "etw_capture.h"
#include "tooltip_popup.h"
#include "detail_window.h"
#include <unordered_map>
#include <deque>
#include <map>

struct RecentProc {
    std::wstring name;
    double speed_up = 0;
    double speed_down = 0;
    double ema_up = 0;      // exponential moving average - recent upload speed
    double ema_down = 0;    // exponential moving average - recent download speed
    int idle_rounds = 0;
};

// One sample point for the rolling 1-second speed window
struct WinSample {
    ULONGLONG tick = 0;
    uint64_t sent = 0;
    uint64_t recv = 0;
};

class CProcessNetItem : public IPluginItem {
public:
    enum Direction { DIR_UPLOAD = 0, DIR_DOWNLOAD = 1, DIR_TRANSPARENT = 2 };

    void Init(Direction dir) { m_dir = dir; }
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    void Update(const std::vector<ProcTraffic>& stats, double sys_up, double sys_down);

    // Custom draw: transparent area only (invisible on taskbar, but still clickable)
    bool IsCustomDraw() const override { return m_dir == DIR_TRANSPARENT; }
    int GetItemWidth() const override { return m_dir == DIR_TRANSPARENT ? s_transparent_width : 0; }
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override { /* intentionally empty for transparent area */ }

    static int s_transparent_width;  // configurable width for transparent area

    static wchar_t s_value_buf[2][256];
    std::unordered_map<DWORD, RecentProc> m_recent;
    static const int MAX_SHOW = 5;
    static const int MAX_IDLE_ROUNDS = 30;  // keep historical processes longer (~30 seconds)

private:
    Direction m_dir = DIR_UPLOAD;
};

class CProcessNetPlugin : public ITMPlugin {
public:
    static CProcessNetPlugin& Instance();
    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    void OnInitialize(ITrafficMonitor* pApp) override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    OptionReturn ShowOptionsDialog(void* hParent) override;
    ~CProcessNetPlugin();

private:
    CProcessNetItem m_items[3];
    ITrafficMonitor* m_app = nullptr;
    ULONGLONG m_last_time = 0;
    bool m_started = false;
    ULONGLONG m_last_start_attempt = 0;  // throttle capture Start() retries
    wchar_t m_tooltip[2048] = L"";
    static CProcessNetPlugin s_instance;

    // TM config directory (received via OnExtenedInfo)
    std::wstring m_tm_config_dir;

    // Detail window access (for CProcessNetItem::OnMouseEvent)
public:
    void ToggleDetailWindow(HWND parent_wnd);
    CTooltipPopup m_popup;
    bool m_popup_created = false;
    bool m_popup_pinned = false;   // click-pinned: stays visible after mouse leaves
    RECT m_popup_anchor = {};      // anchor rect of the currently shown popup
    void ShowPopupAt(const RECT& anchor);
    CDetailWindow m_detail;
    PacketCapture m_capture;
    EtwCapture m_etw_cap;   // production ETW per-process byte counter (primary when active)
    bool m_detail_created = false;
    void StopEtwCapture() { m_etw_cap.Stop(); }

    // Cached stats for popup (updated each refresh cycle)
    std::vector<ProcTraffic> m_cached_stats;
    double m_cached_up = 0;
    double m_cached_down = 0;

    // High-frequency refresh timer (independent of TM's 1s DataRequired tick)
    HANDLE m_refresh_timer = nullptr;
    bool m_refresh_timer_ok = false;
    CRITICAL_SECTION m_data_lock;         // guards cached stats + item updates
    bool m_lock_inited = false;
    std::map<DWORD, std::deque<WinSample>> m_win;   // rolling 1s window samples
    static void CALLBACK RefreshTimerProc(void* param, BOOLEAN timer_or_wait);
    void RefreshTick();
    void StartRefreshTimer();
    void StopRefreshTimer();
    void ComputeWindowSpeeds(std::vector<ProcTraffic>& stats);
    void BuildTooltip(bool etw_active, const std::vector<ProcTraffic>& stats, double su, double sd);
    // Run on the window's own (UI) thread via WM_PNM_REFRESH
    void DetailRefreshFromSnapshot();
    void PopupRefreshFromSnapshot();
    std::vector<CTooltipPopup::ProcDisplayInfo> GetCachedProcDisplayInfo();

private:
    // Hover state machine (driven by the popup's 100ms WM_TIMER)
    ULONGLONG m_hover_start_tick = 0;   // when cursor entered the current TM window
    ULONGLONG m_hover_leave_tick = 0;   // when cursor left everything (hide grace)
    HWND m_hover_target = nullptr;      // TM window currently hovered
    bool m_hover_is_taskbar = false;
public:
    void HoverTick();
    void GetProcessDisplayInfo(std::vector<CTooltipPopup::ProcDisplayInfo>& out,
                               const std::vector<ProcTraffic>& stats);
};
