#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "capture.h"

// Set during DLL_PROCESS_DETACH. Window destructors skip DestroyWindow when
// this is set: destroying windows during DLL unload fires synchronous
// window-proc/IME callbacks that can crash the host (issue #7).
extern volatile bool g_shutting_down;

// Destroy a window with IME detached and SEH-guarded. Kept as a standalone
// function because C2712 forbids __try inside destructors (object unwinding).
void SafeDestroyWindow(HWND hwnd);

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
    // status: optional one-line status/warning shown under the total speed (may be nullptr)
    // anchor_hwnd: the TrafficMonitor window this popup is anchored to, used to locate
    //               its native tooltip window so we can avoid overlapping it.
    void UpdateAndShow(const std::vector<ProcDisplayInfo>& procs,
                       double total_up, double total_down,
                       const RECT& anchor_rect,
                       const wchar_t* status = nullptr,
                       HWND anchor_hwnd = nullptr);
    // Update data only (no reposition); used while popup stays visible
    void UpdateData(const std::vector<ProcDisplayInfo>& procs,
                    double total_up, double total_down,
                    const wchar_t* status = nullptr);

    // Re-run tooltip avoidance if the native tooltip has appeared/moved.
    // Returns true if the popup was repositioned.
    bool RepositionIfTooltipsChanged();
    void Hide();

    bool IsVisible() const { return m_visible; }
    bool IsHovering() const { return m_hovering; }
    HWND GetHwnd() const { return m_hwnd; }

    // Backdrop effect for the popup (popup_effect setting):
    //   0 = acrylic  (自绘模糊管线 + 噪声)
    //   1 = transparent (纯 ULW 真半透明，无模糊)
    //   2 = blur     (自绘模糊管线，无噪声)
    //   3 = opaque   (plain window)
    // opacity: 10-100 = 面板色混入模糊背景的比例（浓度），文字始终不透明；
    // 仅不透明模式禁用。Idempotent; call before showing.
    void ApplyBackdrop(int effect, int opacity);
    bool IsAcrylicActive() const { return m_acrylic_active; }

    // Set the TrafficMonitor window handle so the popup can detect its native
    // tooltip window and avoid overlapping it.
    void SetAnchorHwnd(HWND hwnd) { m_anchor_hwnd = hwnd; }

    // ---- 锚窗事件驱动跟随（拖动零延迟，2026-10-18 方案 A 咩咩拍板）----
    // 旧跟随靠 100ms WM_TIMER 轮询：WM_TIMER 是最低优先级合成消息，拖动
    // 模态循环里鼠标流不断，tick 被挤得时跑时不跑——悬浮窗掉队数百 ms 的
    // 根因。改为子类化 TM 主窗拦 WM_WINDOWPOSCHANGED：位置事件以窗口过程
    // 直接调用同步直达（不走队列、不受输入流挤压），模态循环每挪一像素
    // 都当场跟随。纯位移走轻量 delta 路径（不算布局、不做避开扫描），
    // 反色判定挂同一事件流 150ms 节流；松手（EXITSIZEMOVE）全量重定位 +
    // 强制终判，免掉静止判定 300ms 稳定期。
    // anchor=null 卸载（Hide 内部即时卸载；固定/换目标由 HoverTick 每 tick
    // reconcile 收敛）。子类化被第三方注入器剥掉时静默退回轮询兜底
    // （HoverTick 仍在，行为不劣于旧版），不自动重挂：无法分辨「被剥」与
    // 「他人合法叠子类化」，误重挂会造调用环；下个显示会话重挂即自愈
    void SetAnchorFollow(HWND anchor);
    // 事件路径同步回写 plugin 侧锚矩形（m_popup_anchor）：HoverTick 的轮询
    // 比对因此常态相等，不再触发全量 UpdateAndShow（轮询保留作兜底）
    void SetAnchorMovedCallback(std::function<void(const RECT&)> cb) {
        m_on_anchor_moved = std::move(cb);
    }

    std::wstring m_status;   // status/warning line (empty = none)

    // Public for standalone WndProc access
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

private:
    // Window
    void OnPaint();
    void OnMouseLeave();

    // Subclass watchdog. The hover state machine runs on the popup's own
    // 100ms WM_TIMER, so a third-party injector that strips GWLP_WNDPROC
    // (GamePP's re-hook after sleep+display-switch, the 2026-10-07 RDP
    // takeover corpse) kills HoverTick silently and the hover popup never
    // comes back. A message-only window whose proc is Natively registered
    // (not a subclass) ticks once a second and re-installs StaticWndProc +
    // the hover timer from the owning thread when it finds them replaced.
    static LRESULT CALLBACK WatchdogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    HWND m_watchdog_hwnd = nullptr;

    // 锚窗子类化跟踪（见 SetAnchorFollow 注释）
    static LRESULT CALLBACK AnchorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void DetachAnchorTracker();
    // 锚窗位置事件：纯位移轻量 delta 跟随；尺寸/跨屏转节流全量
    void OnAnchorPosChanged(const RECT& new_anchor, bool sized);
    // 全量重定位（DPI+公式+避开扫描）；force_judge 清节流钟强制反色重判
    void AnchorFullPass(const RECT& anchor_rect, bool force_judge);
    // 反色判定节流（事件流驱动；判定变化才重绘）
    void AnchorJudgeThrottled(const RECT& rc, ULONGLONG min_interval_ms);
    HWND m_tracked_hwnd = nullptr;        // 被子类化跟踪的锚窗（TM 主窗）
    WNDPROC m_tracked_orig_proc = nullptr;
    bool m_anchor_in_sizemove = false;    // 锚窗拖动/缩放会话进行中
    bool m_have_last_seen_anchor = false; // delta 基准是否已建立
    RECT m_last_seen_anchor = {};         // 上一个锚矩形（delta 基准）
    HMONITOR m_last_seen_mon = nullptr;   // 上一个锚所在显示器（跨屏检测）
    ULONGLONG m_last_full_pass_tick = 0;  // 尺寸/跨屏全量重定位节流
    std::function<void(const RECT&)> m_on_anchor_moved;

    // Layout & rendering
    void CalcLayout(HDC hdc, int& out_w, int& out_h, int& out_up_count, int& out_down_count);
    void DrawBackground(HDC hdc, int w, int h, bool content_only);
    // 内容绘制主体（所有呈现路径共用，画到任意 memory DC）。
    // content_only=true 时跳过背景填充（coverage 双底色 pass 用：内容
    // 画在白底/黑底上解覆盖率）
    void DrawContent(HDC dc, int w, int h, bool content_only);

    // ---- ULW per-pixel 呈现（透明系三档的唯一实现） ----
    // 实测（2026-10-16 harness+探针矩阵）本机 DWM 对「含内容像素的 SWCA
    // 窗口」忽略 tint alpha（一律全浓度）——滑条在 SWCA 上必然无效。
    // UpdateLayeredWindow 是唯一语义稳定的 per-pixel 通道。
    // 背景磨砂（方案 A'，2026-10-16 咩咩拍板）= 系统实时模糊双层架构：
    //   底层 blur 效果层：纯黑窗 + SWCA BLURBEHIND（26300 实测真模糊
    //   lap_var≈24，探针 tools/systembackdrop_probe），DWM 硬件逐帧合成
    //   ——背后视频/滚动实时跟随（实测 50-100ms 内），零 CPU 永不闪
    //   上层内容层（本窗 ULW）：背景=面板色×浓度预乘半透明盖在 blur 上
    //   （浓度公式探针实测精确吻合 73 vs 理论 74），内容不透明，圆角走
    //   ULW alpha；亚克力档面板叠确定性噪声（与模糊档可辨）
    //   透明档 = 不挂 blur 层，真半透明透实时桌面
    //   文字反色：affinity 无闪截屏低频判定（300ms，blur 层画面天然平滑）
    bool EnsureLayeredSurface(int w, int h);
    void ComposeLayeredSurface(int w, int h);
    // blur 效果层（亚克力/模糊档挂）：无内容纯黑窗 + SWCA 只取系统模糊
    void EnsureBlurLayer();
    void DestroyBlurLayer();
    // 背景亮度采样 + 文字反色判定：affinity 无闪截屏 popup 区域 → 均值
    // （截到的已是 blur 层模糊画面，天然平滑）→ 亮度公式承自旧
    // UpdateAdaptiveTextTheme
    void RefreshBackdrop(const RECT& popup_rect);
    // affinity 无闪截屏：WDA_EXCLUDEFROMCAPTURE 期间窗口从一切屏幕捕获
    // 中剔除、用户肉眼显示不受影响——BitBlt 拿到窗口脚下真实背景，全程
    // 窗口可见零闪。写 m_snap 并返回 true；设置失败（Win10 2004 前的
    // 老系统）返回 false，调用方回退清空法
    bool CaptureSnapNoFlash(const RECT& popup_rect, int w, int h);
    // 兜底自截规避（affinity 不可用的回退路径）：清空 ULW 表面后截屏
    void ClearLayeredSurfaceForCapture();
    void DrawProcRow(HDC hdc, int y, const ProcDisplayInfo& proc, bool is_upload);
    void DrawSectionTitle(HDC hdc, int y, const wchar_t* title, bool is_upload, bool hover);
    void PositionWindow(const RECT& anchor_rect);

    // DPI handling (per-monitor, same approach as TM's DPIFromRect)
    void UpdateDpiScale(const RECT& anchor_rect);
    void RecreateScaledResources();

    // Helpers
    bool IsDarkMode();
    // 面板观感主题（背景/section 条/分隔线/按钮等面板元素用）：不透明档
    // 与高浓度档（op≥80 不做判定）跟系统主题；半透明档与文字判定统一
    // （2026-10-18 主修）——判定翻深字时面板整套翻亮。此前面板独跟系统
    // 主题，白桌面+低浓度+深主题时背景停在 ~198 的不亮不暗灰（blur 只
    // 压暗 6%），而彩字变体对亮度带只有 15/28，深浅两头都够不着高对比，
    // 速度列发灰；面板跟随判定后白桌面呈现亮玻璃+深字+深变体彩字，暗
    // 桌面维持现状
    bool UseDarkTheme() const { return UseTextTheme(); }
    // 文字主题：默认跟随真实主题；ULW 半透明背景下背景观感≈桌面时，
    // RefreshBackdrop 会按对比度自动翻转文字深浅（撞色保护）
    bool UseTextTheme() const { return m_text_adaptive ? m_text_dark : m_dark_mode; }
    COLORREF GetBgColor();
    COLORREF GetTextColor();
    // 强调色（橙=上传 绿=下载），速度数字与区块标题共用：深浅跟文字主题
    // 走并随右列独立翻转保对比。标题条实心自绘底时代曾有 on_panel 分支
    // （跟面板主题不翻转），2026-10-18 实心底退役后统一（咩咩拍板）。
    // 顺修（2026-10-18）：变体对亮度带宽只有 15（橙）/28（绿），背景落在
    // 变体带附近时（中灰过渡带）所选变体与背景差 <45 彩字发灰——按
    // m_speed_bg_lum 等比缩放 RGB（保色相）推离背景方向到差 ≥45；亮方
    // 向受色相天花板限制（单通道 clamp 后如实到顶为止）
    COLORREF GetAccentColor(bool is_upload);
    COLORREF GetSectionBgColor();

    // True while the cursor is inside/near the popup rect (~32px margin):
    // the user is moving toward the "查看详细" button - the popup must not
    // move under the cursor (button ends up missed, 2026-09-21).
    bool IsCursorNear() const;

    // Process icon, frame-split extraction（review P2）：绘制路径只查缓存
    // （LookupProcessIcon，miss 记入 m_icon_pending），同步提取
    // （ExtractProcessIcon，含磁盘 IO）由 PumpIconExtraction 分帧驱动
    HICON LookupProcessIcon(const std::wstring& exe_path);
    HICON ExtractProcessIcon(const std::wstring& exe_path);
    bool PumpIconExtraction(int count);

    // State
    HWND m_hwnd = nullptr;
    HINSTANCE m_hinst = nullptr;
    HWND m_anchor_hwnd = nullptr;  // TrafficMonitor window we are anchored to
    bool m_visible = false;
    bool m_hovering = false;  // mouse is over the popup itself
    bool m_dark_mode = true;

    // Backdrop state (ApplyBackdrop): mode 0 = plain window, 1 = ULW
    // layered（透明系三档共用）+ 底下挂 blur 效果层（亚克力/模糊档）
    int m_backdrop_mode = 0;
    int m_backdrop_effect = 0;      // 0 acrylic / 1 transparent / 2 blur / 3 opaque
    int m_backdrop_opacity = -1;
    bool m_acrylic_active = false;  // == (m_backdrop_mode == 1), read by OnPaint
    bool m_acrylic_dark = true;     // theme the panel color was built for
    bool m_noise_acrylic = false;   // 亚克力档：面板叠加 Fluent 细噪声（与模糊档可辨）
    HWND m_blur_hwnd = nullptr;     // blur 效果层（SWCA 系统模糊，点击穿透）
    // 文字自适应反色：ULW 半透明背景下文字深浅可脱离主题自动适配
    bool m_text_dark = false;       // true=浅色文字（深背景观感），false=深色文字
    bool m_text_adaptive = false;   // 本显示会话是否翻转过主题文字色
    // 速度列 accent 亮/深变体独立判定：反色判定只采左侧进程名列（咩咩
    // 2026-10-16 指定），左右背景亮度分裂时（左暗右亮）亮变体叠亮背景
    // 对比塌——彩字真彩化后暴露（土灰时代的均值恰好提供了假对比）。
    // 对右侧速度列区域单独取对比更大的变体（上/下载橙绿分别判；中点
    // 判定对固定两点数学上即是取差更大者）
    bool m_speed_flip_up = false;
    bool m_speed_flip_down = false;
    // 速度列区域背景亮度（RefreshBackdrop 的 sbg 公式值，随判定刷新）；
    // -1=未判定（高浓度 op≥80 / 不透明档 / 首帧判定前），GetAccentColor
    // 的对比保障只在有效时生效
    double m_speed_bg_lum = -1.0;

    // ULW 呈现资源：32bpp DIB 表面 + coverage 双底 pass + 采样表面
    HDC m_ulw_dc = nullptr;
    HBITMAP m_ulw_bmp = nullptr;
    void* m_ulw_bits = nullptr;
    int m_ulw_w = 0, m_ulw_h = 0;
    HDC m_cov_a_dc = nullptr;  HBITMAP m_cov_a_bmp = nullptr;  void* m_cov_a_bits = nullptr;
    HDC m_cov_c_dc = nullptr;  HBITMAP m_cov_c_bmp = nullptr;  void* m_cov_c_bits = nullptr;
    // 背景采样表面（反色判定）：affinity 截屏原样（w×h）
    HDC m_snap_dc = nullptr;   HBITMAP m_snap_bmp = nullptr;   void* m_snap_bits = nullptr;
    RECT m_last_snap_rect = {};     // 上次采样的矩形（节流比对基准）
    // 静止期反色判定（2026-10-16）：blur 层由 DWM 逐帧实时合成无需刷
    // 新，只有文字深浅需要低频跟随背景亮度。affinity 不可用（老系统）
    // 时不做静止判定（首显判定仍在——那时窗口未上屏，截屏天然干净）
    ULONGLONG m_last_refresh_tick = 0;
    bool m_affinity_ok = true;
    // 静止判定双档节奏（2026-10-19）：矩形刚变过（移动停止/重定位）置
    // true，停稳 300ms 快判一轮落定；之后转 false 走 1s 巡检——判定输入
    // 已与首显同源（CaptureSnapNoFlash 双层剔除），巡检职责只剩脚下背
    // 真变了（开视频/切深色窗）的检测，500ms tick 全轮跑是浪费
    bool m_steady_probe_fast = false;

    // Data
    std::vector<ProcDisplayInfo> m_procs;
    double m_total_up = 0;
    double m_total_down = 0;
    RECT m_last_anchor = {};  // anchor rect used for the current show session
    // Tooltip-avoidance state: TM's native tooltip follows the cursor and is
    // rebuilt every second (text width changes), so its rect jitters
    // constantly. Reposition only on real overlap, freeze while the cursor
    // is near, and return to the anchor position only after the tooltip has
    // stayed clear for 500ms.
    bool m_avoided = false;           // nudge away from a tooltip happened
    ULONGLONG m_no_overlap_tick = 0;  // first tick with no overlap (0 = reset)

    // Icon cache: exe_path -> HICON
    std::unordered_map<std::wstring, HICON> m_icon_cache;
    // 待提取图标队列（LookupProcessIcon miss 入队，PumpIconExtraction 消费）
    std::vector<std::wstring> m_icon_pending;

    // Cached GDI objects (created once, reused in OnPaint)
    HFONT m_font_normal = nullptr;   // Microsoft YaHei -14 (process name + header + speed + section title)
    HFONT m_font_small = nullptr;    // Microsoft YaHei -13 (status line + detail button)
    HPEN m_pen_separator = nullptr;  // row separator
    bool m_pen_dark = true;          // theme the cached separator pen was built for

    // IsDarkMode cache
    bool m_dark_mode_cached = true;
    ULONGLONG m_dark_mode_tick = 0;

    // Layout constants (DPI-scaled; BASE_ values are for 96 DPI / 100%)
    int PADDING = 12;
    int ROW_HEIGHT = 32;
    int ICON_SIZE = 22;
    int CORNER_RADIUS = 8;
    int MIN_WIDTH = 260;
    int HEADER_H = 34;      // total speed header band
    int SECTION_H = 24;     // section title step
    int GAP_H = 4;          // gap between upload/download sections
    int BTN_AREA_H = 28;    // "查看详细" button band
    int BTN_H = 24;         // button inner height
    int SPEED_AREA_W = 85;  // right-aligned speed column width（110→85，2026-10-16
                            // 咩咩反馈进程名-流速间留白大；"10.8 KB/s" ~65px 仍宽裕）
    int STATUS_H = 18;      // status/warning line height (0 = hidden)

    static const int BASE_PADDING = 12;
    static const int BASE_ROW_HEIGHT = 32;
    static const int BASE_ICON_SIZE = 22;
    static const int BASE_CORNER_RADIUS = 8;
    static const int BASE_MIN_WIDTH = 260;
    static const int BASE_HEADER_H = 34;
    static const int BASE_SECTION_H = 24;
    static const int BASE_GAP_H = 4;
    static const int BASE_BTN_AREA_H = 28;
    static const int BASE_BTN_H = 24;
    static const int BASE_SPEED_AREA_W = 85;
    static const int BASE_STATUS_H = 18;

    float m_dpi_scale = 1.0f;

public:
    static const int MAX_SHOW = 5;  // max processes per section
    static const UINT_PTR TIMER_HOVER = 1;    // 100ms hover state machine timer
    static const UINT_PTR TIMER_WATCHDOG = 2; // 1s subclass watchdog timer

    // Mouse tracking
    bool m_tracking = false;

    // "查看详细" button rect (updated in OnPaint)
    RECT m_rcDetailBtn = {};

    // Hover 反馈（2026-10-16 咩咩反馈按钮无接入效果突兀）：两个 section
    // 标题条 + 查看详细按钮。矩形由 DrawContent 记录（与 m_rcDetailBtn
    // 同模式），WM_MOUSEMOVE 命中测试，状态翻转才整窗重绘（ULW 三表面
    // 全量重绘成本与每秒数据刷新同级，进出区域是低频事件）
    bool m_hover_up = false;
    bool m_hover_down = false;
    bool m_hover_btn = false;
    RECT m_rcSectionUp = {};
    RECT m_rcSectionDown = {};

    static CTooltipPopup* s_instance;
};
