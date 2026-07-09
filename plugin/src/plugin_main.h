#pragma once
#include <winsock2.h>
#include <windows.h>
#include "PluginInterface.h"
#include "capture.h"
#include "tooltip_popup.h"
#include "detail_window.h"
#include <unordered_map>

struct RecentProc {
    std::wstring name;
    double speed_up = 0;
    double speed_down = 0;
    double ema_up = 0;      // exponential moving average - recent upload speed
    double ema_down = 0;    // exponential moving average - recent download speed
    int idle_rounds = 0;
};

class CProcessNetItem : public IPluginItem {
public:
    enum Direction { DIR_UPLOAD = 0, DIR_DOWNLOAD = 1 };

    void Init(Direction dir) { m_dir = dir; }
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

    void Update(const std::vector<ProcTraffic>& stats, double sys_up, double sys_down);

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

private:
    CProcessNetItem m_items[2];
    ITrafficMonitor* m_app = nullptr;
    ULONGLONG m_last_time = 0;
    bool m_started = false;
    wchar_t m_tooltip[2048] = L"";
    static CProcessNetPlugin s_instance;

    // TM config directory (received via OnExtenedInfo)
    std::wstring m_tm_config_dir;

    // Rich tooltip popup
    CTooltipPopup m_popup;
    bool m_popup_created = false;

    // Detail window access (for CProcessNetItem::OnMouseEvent)
public:
    void ToggleDetailWindow(HWND parent_wnd);
    CDetailWindow m_detail;
    PacketCapture m_capture;
    bool m_detail_created = false;

private:
    ULONGLONG m_last_hover_check = 0;
    bool m_was_hovering = false;
    void CheckHoverAndShowPopup();
    void GetProcessDisplayInfo(std::vector<CTooltipPopup::ProcDisplayInfo>& out,
                               const std::vector<ProcTraffic>& stats);
    // Cached stats for popup (updated each DataRequired cycle)
    std::vector<ProcTraffic> m_cached_stats;
    double m_cached_up = 0;
    double m_cached_down = 0;
};
