#pragma once
#include <windows.h>

// 透明度滑条实时预览窗（2026-10-21）：选项对话框拖动 trackbar 时在对话框
// 旁弹出的迷你悬浮窗，让「浓度」滑动的观感与真实悬浮窗一致。
//
// 为什么必须是独立窗口而不是对话框内的一块预览区：背景呈现走的是
// CTooltipPopup 的双层架构——底层 blur 效果层（纯黑窗 + SWCA
// ACRYLICBLURBEHIND，DWM 逐帧实时模糊窗口脚下的一切）+ 上层 ULW 内容层
// （面板色×浓度预乘半透明盖在模糊上）。SWCA 是窗口级效果，糊不了对话框
// 客户区里的一小块；ULW 也只能整窗接管 alpha。预览必须自立门户。
//
// 呈现管线从 tooltip_popup.cpp 复刻（coverage 双底解算/死点回填/确定性
// 噪声/圆角几何 coverage/预乘合成公式全部同款），内容是固定的示例画面
// （总速条 + 下载区两行示例 + 查看详细按钮），颜色跟插件「颜色模式」与
// 系统主题（预览不做运行时的背景反色判定——那是真实悬浮窗的自适应行为）。
//
// 生命周期：滑条一动就 Show（选项对话框线程内），2 秒无操作自动收起并
// 销毁（下次 Show 重建，代码路径单一）；对话框关闭走 WM_DESTROY 兜底
// Hide。全程不抢焦点（WS_EX_NOACTIVATE），键盘流始终留在对话框里。
class COpacityPreview {
public:
    static COpacityPreview& Instance();

    // effect: 0 亚克力 / 1 透明 / 2 模糊（3=不透明不会到这——trackbar 在
    //   该档被禁用，滑条无输入；低系统降级由调用方先做 OptionsClampEffectForOS）
    // opacity: 10-100 浓度
    // hDlg: 选项对话框（定位预览窗用：贴对话框旁、与滑条行同水平线）
    // 重复调用 = 更新画面并重置 2s 自动收起定时
    void Show(HWND hDlg, int effect, int opacity);

    // 立即销毁预览窗（对话框关闭 / 切到不透明档）。幂等。
    void Hide();

private:
    COpacityPreview() = default;
    ~COpacityPreview();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    bool EnsureWindow();            // 内容层 + blur 层懒创建
    void EnsureSurfaces();          // ULW + coverage 双底 DIB（固定尺寸一次建齐）
    void DestroyWindowpair();       // 两窗口销毁（g_shutting_down 保护）
    void ReleaseSurfaces();         // DIB/DC/字体回收
    void DrawContent(HDC dc);       // 示例画面（画在任意底色上，背景不画）
    void Compose();                 // coverage 解算 + 预乘合成 + ULW 提交
    bool IsDarkTheme();

    HWND m_hwnd = nullptr;          // ULW 内容层
    HWND m_blur_hwnd = nullptr;     // SWCA blur 效果层（亚克力/模糊档）
    HINSTANCE m_hinst = nullptr;
    int m_effect = 0;               // 0/1/2
    int m_opacity = 100;
    bool m_dark = true;             // Show 时判一次的主题
    bool m_noise = false;           // 亚克力档叠噪声

    HDC m_ulw_dc = nullptr;
    HBITMAP m_ulw_bmp = nullptr;
    void* m_ulw_bits = nullptr;
    HDC m_cov_a_dc = nullptr;       // coverage 白底 pass
    HBITMAP m_cov_a_bmp = nullptr;
    void* m_cov_a_bits = nullptr;
    HDC m_cov_c_dc = nullptr;       // coverage 黑底 pass
    HBITMAP m_cov_c_bmp = nullptr;
    void* m_cov_c_bits = nullptr;
    HFONT m_font_normal = nullptr;  // 雅黑 -14（与悬浮窗同款）
    HFONT m_font_small = nullptr;   // 雅黑 -13
};
