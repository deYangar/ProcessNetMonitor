#include "plugin_main.h"
#include "utils.h"
#include "ip_geo.h"
#include "i18n.h"
#include <algorithm>
#include <shellapi.h>
#include <dbghelp.h>
#include <commctrl.h>

#pragma comment(lib, "dbghelp.lib")

// From tooltip_popup.cpp DllMain - DLL's HINSTANCE
extern HINSTANCE s_dll_hinst;

// ---- crash diagnostics: write minidump + crash.log on unhandled exception ----
static LONG WINAPI PnmCrashHandler(EXCEPTION_POINTERS* ep) {
    // Only dump real crashes - ignore benign first-chance/debug exceptions
    // such as 0x40010006 (DBG_PRINTEXCEPTION) that VEH sees on every debug print.
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    static const DWORD serious[] = {
        0xC0000005,  // access violation
        0xC000001D,  // illegal instruction
        0xC000008C,  // array bounds exceeded
        0xC0000094,  // integer divide by zero
        0xC00000FD,  // stack overflow
        0xC0000374,  // heap corruption
        0xC0000409,  // stack buffer overrun / failfast
        0xC0000139,  // entry point not found
        0xC0000135,  // dll not found
        0xC0000142,  // dll init failed
    };
    bool serious_crash = false;
    for (DWORD c : serious) if (code == c) { serious_crash = true; break; }
    if (!serious_crash) return EXCEPTION_CONTINUE_SEARCH;

    // De-dupe: same code+address within 10s (VEH fires first-chance too)
    static DWORD s_last_code = 0;
    static void* s_last_addr = nullptr;
    static ULONGLONG s_last_tick = 0;
    ULONGLONG now = GetTickCount64();
    if (code == s_last_code && ep->ExceptionRecord->ExceptionAddress == s_last_addr &&
        now - s_last_tick < 10000)
        return EXCEPTION_CONTINUE_SEARCH;
    s_last_code = code;
    s_last_addr = ep->ExceptionRecord->ExceptionAddress;
    s_last_tick = now;

    // ---- 1) crash.log FIRST (pure Win32, zero CRT) ----
    // The ORIGINAL exception must be captured before MiniDumpWriteDump:
    // dbghelp re-enters its own CRT during failfast and can failfast again,
    // losing everything (2026-08-05 dev-machine: WER only saw the 2nd
    // failfast inside this handler - crash.log/crash.dmp never landed).
    void* stack[24] = {};
    USHORT nstack = CaptureStackBackTrace(2, 24, stack, nullptr);
    // zero-CRT hex/decimal helpers (pure arithmetic)
    auto hexw = [](wchar_t* out, unsigned long long v, int nd) {
        for (int i = nd - 1; i >= 0; i--) { out[i] = L"0123456789abcdef"[v & 0xF]; v >>= 4; }
        out[nd] = 0;
    };
    auto decw = [](wchar_t* out, unsigned long long v) {
        wchar_t tmp[24]; int i = 0;
        if (v == 0) tmp[i++] = L'0';
        while (v && i < 23) { tmp[i++] = L"0123456789"[v % 10]; v /= 10; }
        for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
        out[i] = 0;
    };
    wchar_t dir[MAX_PATH] = L"";
    if (PNM_GetDebugDir(dir, MAX_PATH)) {
        size_t dlen = wcslen(dir);
        // <dir>\crash.log (manual concat - no CRT)
        static const wchar_t lsuf[] = L"\\crash.log";
        size_t llen = (sizeof(lsuf) / sizeof(wchar_t)) - 1;
        if (dlen + llen < MAX_PATH) {
            wchar_t logpath[MAX_PATH];
            memcpy(logpath, dir, dlen * sizeof(wchar_t));
            memcpy(logpath + dlen, lsuf, (llen + 1) * sizeof(wchar_t));
            HANDLE hLog = CreateFileW(logpath, FILE_APPEND_DATA, FILE_SHARE_READ,
                                      nullptr, OPEN_ALWAYS, 0, nullptr);
            if (hLog != INVALID_HANDLE_VALUE) {
                auto wr = [&](const wchar_t* s) {
                    DWORD w = 0;
                    WriteFile(hLog, s, (DWORD)wcslen(s) * sizeof(wchar_t), &w, nullptr);
                };
                SYSTEMTIME st; GetLocalTime(&st);
                wchar_t hh[4], mm[4], ss[4], cx[9], ax[17], th[12];
                decw(hh, st.wHour); decw(mm, st.wMinute); decw(ss, st.wSecond);
                hexw(cx, code, 8);
                hexw(ax, (unsigned long long)ep->ExceptionRecord->ExceptionAddress, 16);
                decw(th, GetCurrentThreadId());
                wr(L"["); wr(hh); wr(L":"); wr(mm); wr(L":"); wr(ss);
                wr(L"] CRASH code=0x"); wr(cx);
                wr(L" addr=0x"); wr(ax);
                wr(L" thread="); wr(th);
                for (USHORT si = 0; si < nstack; si++) {
                    wr(L" st=");
                    wchar_t sx[17];
                    hexw(sx, (unsigned long long)stack[si], 16);
                    wr(sx);
                }
                wr(L"\n");
                CloseHandle(hLog);
            }
        }
        // ---- 2) crash.dmp (best effort - may fail during failfast) ----
        static const wchar_t dsuf[] = L"\\crash.dmp";
        size_t dlen2 = (sizeof(dsuf) / sizeof(wchar_t)) - 1;
        if (dlen + dlen2 < MAX_PATH) {
            wchar_t dmppath[MAX_PATH];
            memcpy(dmppath, dir, dlen * sizeof(wchar_t));
            memcpy(dmppath + dlen, dsuf, (dlen2 + 1) * sizeof(wchar_t));
            HANDLE hFile = CreateFileW(dmppath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                                  MiniDumpNormal, &mei, nullptr, nullptr);
                CloseHandle(hFile);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// CRT invalid-parameter handler: a *_s safe function received an illegal
// argument (e.g. NULL src or wrong buffer size). The default CRT behaviour is
// to failfast (0xc0000409) with no diagnostics. Log the offending function /
// source line plus a stack backtrace (resolvable via the .map file), then
// failfast exactly like the CRT would.
static void PnmInvalidParamHandler(const wchar_t* expr, const wchar_t* func,
                                   const wchar_t* file, unsigned line, uintptr_t reserved) {
    // Re-entrancy guard: the CRT itself may fail again while we log
    static volatile LONG s_in_handler = 0;
    if (InterlockedIncrement(&s_in_handler) > 1)
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);

    // Grab a stack backtrace (Win32 API - no CRT involved) so we can
    // resolve the caller via the .map file even though CRT passes null
    // expr/func/file in release builds.
    void* stack[24] = {};
    USHORT nstack = CaptureStackBackTrace(2, 24, stack, nullptr);

    // ZERO CRT usage here: even swprintf_s/_wfopen re-trigger invalid_parameter
    // in this broken state (recursion -> failfast, log never lands). Only
    // Win32 calls + hand-rolled helpers.
    wchar_t path[MAX_PATH] = L"";
    if (PNM_GetDebugDir(path, MAX_PATH) && path[0]) {
        static const wchar_t suffix[] = L"\\crash.log";
        size_t plen = wcslen(path);
        size_t slen = (sizeof(suffix) / sizeof(wchar_t)) - 1;
        if (plen + slen < MAX_PATH) {
            wchar_t full[MAX_PATH];
            memcpy(full, path, plen * sizeof(wchar_t));
            memcpy(full + plen, suffix, (slen + 1) * sizeof(wchar_t));
            HANDLE hFile = CreateFileW(full, FILE_APPEND_DATA, FILE_SHARE_READ,
                                       nullptr, OPEN_ALWAYS, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                auto wr = [&](const wchar_t* s) {
                    DWORD w = 0;
                    WriteFile(hFile, s, (DWORD)wcslen(s) * sizeof(wchar_t), &w, nullptr);
                };
                wr(L"INVALID_PARAM expr=");
                wr(expr ? expr : L"(null)");
                wr(L" func=");
                wr(func ? func : L"(null)");
                wr(L" file=");
                wr(file ? file : L"(null)");
                // line as hex, hand-rolled (no CRT itoa)
                wchar_t lb[16];
                unsigned v = line;
                int i = 0;
                if (v == 0) lb[i++] = L'0';
                while (v && i < 15) { lb[i++] = L"0123456789abcdef"[v & 0xF]; v >>= 4; }
                wchar_t rb[16]; int j = 0;
                while (i > 0) rb[j++] = lb[--i];
                rb[j] = 0;
                wr(L" line=0x");
                wr(rb);
                // stack addresses, hand-rolled hex (resolve via .map file)
                for (USHORT si = 0; si < nstack; si++) {
                    wr(L" st=");
                    uintptr_t a = (uintptr_t)stack[si];
                    wchar_t hx[20];
                    int hi = 0;
                    do { hx[hi++] = L"0123456789abcdef"[a & 0xF]; a >>= 4; } while (a && hi < 18);
                    wchar_t hr[20]; int hj = 0;
                    while (hi > 0) hr[hj++] = hx[--hi];
                    hr[hj] = 0;
                    wr(hr);
                }
                wr(L"\n");
                CloseHandle(hFile);
            }
        }
    }
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

// WER LocalDumps: kernel-level crash dumps that do NOT depend on in-process
// handlers. failfast termination (0xc0000409) bypasses SEH/VEH entirely, so
// only WER can reliably capture it. Requires admin (HKLM); best effort.
static void SetupWerLocalDumps() {
    HKEY hk = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\TrafficMonitor.exe",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr);
    if (st != ERROR_SUCCESS) return;
    wchar_t dump_dir[MAX_PATH] = L"";
    if (PNM_GetDebugDir(dump_dir, MAX_PATH) && dump_dir[0]) {
        wcscat_s(dump_dir, L"\\werdumps");
        CreateDirectoryW(dump_dir, nullptr);
        RegSetValueExW(hk, L"DumpFolder", 0, REG_SZ,
                       (const BYTE*)dump_dir, (DWORD)((wcslen(dump_dir) + 1) * sizeof(wchar_t)));
        DWORD type = 2;  // full dump
        RegSetValueExW(hk, L"DumpType", 0, REG_DWORD, (const BYTE*)&type, sizeof(type));
    }
    RegCloseKey(hk);
}

// One-line status for popup/tooltip when ETW cannot own the kernel session
static const wchar_t* EtwPopupStatus(const EtwCapture* cap) {
    thread_local wchar_t buf[512];
    if (!cap) return nullptr;
    if (cap->HasData()) {
        const wchar_t* st = cap->ConnState();
        if (st && wcsstr(st, L"attach") != nullptr) {
            const wchar_t* owner = cap->OwnerText();
            if (owner && owner[0]) {
                swprintf_s(buf, 512, TR(L"\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u7591\u4f3c\u88ab %s \u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed\u540e\u91cd\u542f TM"), owner);
            } else {
                swprintf_s(buf, 512, TR(L"\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u88ab\u5176\u4ed6\u7a0b\u5e8f\u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed AppNetworkCounter \u540e\u91cd\u542f TM"));
            }
            return buf;
        }
    } else if (cap->IsRunning()) {
        return TR(L"\u26a0 ETW \u4e0d\u53ef\u7528\uff0c\u5df2\u56de\u9000\u65e7\u91c7\u96c6\uff0c\u6570\u636e\u53ef\u80fd\u51c6\u786e\u964d\u4f4e");
    }
    return nullptr;
}

CProcessNetPlugin::CProcessNetPlugin() {
    // Initialize item roles at construction: TM enumerates the items and
    // reads GetItemId/IsCustomDraw/GetItemWidth at LOAD time (before the
    // first DataRequired). If Init were deferred, all three items would
    // report the same ID ("SpdUp01") during load, which corrupts TM's
    // display-item config (issue #7).
    m_items[0].Init(CProcessNetItem::DIR_UPLOAD);
    m_items[1].Init(CProcessNetItem::DIR_DOWNLOAD);
    m_items[2].Init(CProcessNetItem::DIR_TRANSPARENT);
}

CProcessNetPlugin CProcessNetPlugin::s_instance;
wchar_t CProcessNetItem::s_value_buf[2][256] = { L"starting...", L"starting..." };
int CProcessNetItem::s_transparent_width = 100;  // default 100px
wchar_t CProcessNetItem::s_top_name[2][48] = { L"-", L"-" };
wchar_t CProcessNetItem::s_top_speed[2][32] = { L"0 B/s", L"0 B/s" };
COLORREF CProcessNetItem::s_value_color = 0;
bool CProcessNetItem::s_has_value_color = false;
bool CProcessNetItem::s_show_speed_items = true;

static void FmtSpeed(double bps, wchar_t* buf, int n) {
    FormatSpeed(bps, buf, n);
}

const wchar_t* CProcessNetItem::GetItemName() const {
    if (m_dir == DIR_TRANSPARENT) return TR(L"\u900F\u660E\u533A\u57DF");
    return m_dir == DIR_UPLOAD ? L"Up" : L"Down";
}
const wchar_t* CProcessNetItem::GetItemId() const {
    if (m_dir == DIR_TRANSPARENT) return L"TransparentArea";
    return m_dir == DIR_UPLOAD ? L"SpdUp01" : L"SpdDn01";
}
const wchar_t* CProcessNetItem::GetItemLableText() const {
    return L"";
}
const wchar_t* CProcessNetItem::GetItemValueText() const {
    if (m_dir == DIR_TRANSPARENT) return L"";
    return s_value_buf[m_dir];
}
const wchar_t* CProcessNetItem::GetItemValueSampleText() const {
    if (m_dir == DIR_TRANSPARENT) return TR(L"\u900F\u660E\u533A\u57DF");
    return m_dir == DIR_UPLOAD ? L"U:chrome.exe 5.6KB/s" : L"D:mihomo 1.4KB/s";
}

int CProcessNetItem::GetItemWidth() const {
    if (m_dir == DIR_TRANSPARENT) return s_transparent_width;
    return 120;  // 96 DPI fallback width; TM scales it by DPI
}

int CProcessNetItem::GetItemWidthEx(void* hDC) const {
    if (m_dir == DIR_TRANSPARENT) return 0;  // fall back to GetItemWidth (TM scales by DPI)
    HDC hdc = (HDC)hDC;
    // Width benchmark: prefix + 12-char name + ellipsis + max speed,
    // so a normal-length name and the speed both fit without clipping.
    const wchar_t* sample = L"U:abcdefghijkl.. 999.9 MB/s";
    SIZE sz{};
    GetTextExtentPoint32W(hdc, sample, (int)wcslen(sample), &sz);
    return sz.cx;
}

void CProcessNetItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) {
    if (m_dir == DIR_TRANSPARENT) return;  // invisible area, draw nothing
    HDC hdc = (HDC)hDC;

    COLORREF color = s_has_value_color ? s_value_color : (dark_mode ? RGB(255, 255, 255) : RGB(0, 0, 0));
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);

    // Local snapshot: same torn-read safety level as the old s_value_buf pattern
    wchar_t name[48];
    wchar_t speed[32];
    wcsncpy_s(name, 48, s_top_name[m_dir], _TRUNCATE);
    wcsncpy_s(speed, 32, s_top_speed[m_dir], _TRUNCATE);

    // Right column: speed, right-aligned
    SIZE sz_spd{};
    GetTextExtentPoint32W(hdc, speed, (int)wcslen(speed), &sz_spd);
    RECT rc_spd = { x + w - sz_spd.cx, y, x + w, y + h };
    DrawTextW(hdc, speed, -1, &rc_spd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Left column: U:/D: + process name, left-aligned at the leftmost edge;
    // dynamically truncated with ".." when it would overlap the speed column
    std::wstring left = (m_dir == DIR_UPLOAD) ? L"U:" : L"D:";
    left += name;
    int avail = (x + w - sz_spd.cx - 3) - x;  // 3px gap between columns
    SIZE sz_left{};
    GetTextExtentPoint32W(hdc, left.c_str(), (int)left.size(), &sz_left);
    if (sz_left.cx > avail && avail > 0) {
        SIZE sz_dot{};
        GetTextExtentPoint32W(hdc, L"..", 2, &sz_dot);
        while (left.size() > 3) {
            left.pop_back();
            GetTextExtentPoint32W(hdc, left.c_str(), (int)left.size(), &sz_left);
            if (sz_left.cx + sz_dot.cx <= avail) break;
        }
        left += L"..";
    }
    RECT rc_left = { x, y, x + w, y + h };
    DrawTextW(hdc, left.c_str(), (int)left.size(), &rc_left, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

int CProcessNetItem::OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) {
    auto& plugin = CProcessNetPlugin::Instance();
    bool is_taskbar = (flag & MF_TASKBAR_WND) != 0;

    if (type == MT_LCLICKED) {
        if (is_taskbar) {
            // Taskbar click = pin/unpin the popup.
            // Pinned popup survives mouse leave; clicking again dismisses it.
            if (plugin.m_popup.IsVisible() && plugin.m_popup_pinned) {
                plugin.m_popup_pinned = false;
                plugin.m_popup.Hide();
            } else {
                POINT pt = { x, y };
                ClientToScreen((HWND)hWnd, &pt);
                RECT wr;
                GetWindowRect((HWND)hWnd, &wr);
                // Anchor band around the click point, spanning the taskbar window height
                int half = 60;
                RECT anchor = { pt.x - half, wr.top, pt.x + half, wr.bottom };
                if (anchor.left < wr.left) { anchor.left = wr.left; anchor.right = wr.left + 2 * half; }
                if (anchor.right > wr.right) { anchor.right = wr.right; anchor.left = wr.right - 2 * half; }
                plugin.m_popup_pinned = true;
                plugin.ShowPopupAt(anchor);
            }
        } else {
            // Main window click: toggle detail window (existing behavior), dismiss popup
            plugin.m_popup_pinned = false;
            plugin.m_popup.Hide();
            plugin.ToggleDetailWindow((HWND)hWnd);
        }
        return 1;
    }
    if (type == MT_RCLICKED) {
        // Right-click dismisses the popup; only consume the event when we actually
        // hid something, otherwise let TM show its own context menu.
        if (plugin.m_popup.IsVisible()) {
            plugin.m_popup_pinned = false;
            plugin.m_popup.Hide();
            return 1;
        }
        return 0;
    }
    if (type == MT_DBCLICKED) {
        // Double-click always opens detail window
        plugin.ToggleDetailWindow((HWND)hWnd);
        return 1;
    }
    return 0;
}

void CProcessNetItem::Update(const std::vector<ProcTraffic>& stats, double sys_up, double sys_down) {
    const double EMA_ALPHA = 0.3;

    for (auto& [pid, rp] : m_recent) rp.idle_rounds++;
    for (const auto& st : stats) {
        auto& rp = m_recent[st.pid];
        rp.name = st.name;
        if (rp.idle_rounds == 0 || rp.ema_up == 0) {
            rp.ema_up = st.speed_up;
            rp.ema_down = st.speed_down;
        } else {
            rp.ema_up = EMA_ALPHA * st.speed_up + (1.0 - EMA_ALPHA) * rp.ema_up;
            rp.ema_down = EMA_ALPHA * st.speed_down + (1.0 - EMA_ALPHA) * rp.ema_down;
        }
        rp.speed_up = st.speed_up;
        rp.speed_down = st.speed_down;
        rp.idle_rounds = 0;
    }
    for (auto& [pid, rp] : m_recent) {
        if (rp.idle_rounds > 0) {
            rp.ema_up *= (1.0 - EMA_ALPHA);
            rp.ema_down *= (1.0 - EMA_ALPHA);
        }
    }
    for (auto it = m_recent.begin(); it != m_recent.end(); ) {
        if (it->second.idle_rounds > MAX_IDLE_ROUNDS) it = m_recent.erase(it); else ++it;
    }

    std::vector<RecentProc*> list;
    for (auto& [pid, rp] : m_recent) {
        list.push_back(&rp);
    }
    if (m_dir == DIR_UPLOAD)
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) {
            double sa = (a->speed_up > 0.01) ? a->speed_up : a->ema_up * 0.5;
            double sb = (b->speed_up > 0.01) ? b->speed_up : b->ema_up * 0.5;
            return sa > sb;
        });
    else
        std::sort(list.begin(), list.end(), [](auto* a, auto* b) {
            double sa = (a->speed_down > 0.01) ? a->speed_down : a->ema_down * 0.5;
            double sb = (b->speed_down > 0.01) ? b->speed_down : b->ema_down * 0.5;
            return sa > sb;
        });

    double sys_spd = (m_dir == DIR_UPLOAD) ? sys_up : sys_down;
    wchar_t proc_name[16] = L"-";
    wchar_t proc_str[32];
    if (!list.empty()) {
        double spd = (m_dir == DIR_UPLOAD) ? list[0]->speed_up : list[0]->speed_down;
        const auto& n = list[0]->name;
        wcsncpy_s(proc_name, 16, n.c_str(), _TRUNCATE);
        if (n.size() > 12) { proc_name[10] = L'.'; proc_name[11] = L'.'; proc_name[12] = 0; }
        wcsncpy_s(s_top_name[m_dir], 48, n.c_str(), _TRUNCATE);
        FmtSpeed(spd, proc_str, 32);
    } else {
        wcsncpy_s(s_top_name[m_dir], 48, L"-", _TRUNCATE);
        FmtSpeed(0, proc_str, 32);
    }
    wcsncpy_s(s_top_speed[m_dir], 32, proc_str, _TRUNCATE);

    wchar_t prefix = (m_dir == DIR_UPLOAD) ? L'U' : L'D';
    swprintf_s(s_value_buf[m_dir], 256, L"%c:%s %s", prefix, proc_name, proc_str);
}

CProcessNetPlugin& CProcessNetPlugin::Instance() { return s_instance; }
IPluginItem* CProcessNetPlugin::GetItem(int i) {
    if (i >= 0 && i <= 2) return &m_items[i];
    return nullptr;
}

void CProcessNetPlugin::DataRequired() {
    if (!m_started) {
        m_items[0].Init(CProcessNetItem::DIR_UPLOAD);
        m_items[1].Init(CProcessNetItem::DIR_DOWNLOAD);
        m_items[2].Init(CProcessNetItem::DIR_TRANSPARENT);
        // Retry Start() at most every 10 seconds (not every tick)
        ULONGLONG now = GetTickCount64();
        if (m_last_start_attempt == 0 || now - m_last_start_attempt >= 10000) {
            m_last_start_attempt = now;
            m_started = m_capture.Start();
            m_last_time = now;
        }
        if (!m_started) {
            if (m_etw_cap.HasData()) {
                // ETW-only mode: legacy capture unavailable but ETW works
                ULONGLONG n2 = GetTickCount64();
                double dt2 = (double)(n2 - m_last_time) / 1000.0;
                if (dt2 < 0.1) dt2 = 0.1;
                m_last_time = n2;
                auto stats = m_etw_cap.GetStats(dt2);
                double su = 0, sd = 0;
                for (auto& s : stats) { su += s.speed_up; sd += s.speed_down; }
                m_items[0].Update(stats, su, sd);
                m_items[1].Update(stats, su, sd);
                m_cached_stats = stats;
                m_cached_up = su;
                m_cached_down = sd;
                if (m_detail_created) PostMessage(m_detail.GetHwnd(), WM_PNM_REFRESH, 0, 0);
                swprintf_s(m_tooltip, 2048, L"Process Net Monitor (ETW)\nTotal: U:%.1fKB/s D:%.1fKB/s",
                           su / 1024.0, sd / 1024.0);
                return;
            }
            swprintf_s(CProcessNetItem::s_value_buf[0], 256, L"ERR: %s", m_capture.GetLastError());
            swprintf_s(CProcessNetItem::s_value_buf[1], 256, L"ERR: %s", m_capture.GetLastError());
            swprintf_s(m_tooltip, 2048, TR(L"Process Net Monitor\n\u26a0 \u542f\u52a8\u5931\u8d25\uff1a%s\n\n%s"),
                       m_capture.GetLastError(), m_capture.GetErrorDetail());
        }
        return;
    }

    // Fast path: the refresh timer owns data collection (250ms default).
    // TM's tick just pushes the latest snapshot to the taskbar items and
    // rebuilds the tooltip - no duplicate ETW polling.
    if (m_refresh_timer_ok) {
        std::vector<ProcTraffic> snap;
        double su = 0, sd = 0;
        {
            EnterCriticalSection(&m_data_lock);
            snap = m_cached_stats;
            su = m_cached_up;
            sd = m_cached_down;
            m_items[0].Update(snap, su, sd);
            m_items[1].Update(snap, su, sd);
            LeaveCriticalSection(&m_data_lock);
        }
        BuildTooltip(m_etw_cap.HasData(), snap, su, sd);
        return;
    }

    ULONGLONG now = GetTickCount64();
    double dt = (double)(now - m_last_time) / 1000.0;
    if (dt < 0.1) dt = 0.1;
    m_last_time = now;

    auto stats = m_capture.GetStats(dt);

    // ETW backend takes priority: more accurate per-process bytes (TCP+UDP,
    // kernel attribution, works under TUN). Legacy stays warm as fallback
    // and supplies conn_count. If ETW currently yields no rows (session
    // hiccup / all filtered), keep legacy data so the UI never blanks out.
    bool etw_active = m_etw_cap.HasData();
    if (etw_active) {
        auto es = m_etw_cap.GetStats(dt);
        if (!es.empty()) {
            std::map<DWORD, int> conn_by_pid;
            for (auto& l : stats) conn_by_pid[l.pid] = l.conn_count;
            for (auto& e : es) {
                auto it = conn_by_pid.find(e.pid);
                if (it != conn_by_pid.end()) e.conn_count = it->second;
            }
            stats = std::move(es);
        }
    }
    double su = 0, sd = 0;
    for (auto& s : stats) { su += s.speed_up; sd += s.speed_down; }

    m_items[0].Update(stats, su, sd);
    m_items[1].Update(stats, su, sd);

    m_cached_stats = stats;
    m_cached_up = su;
    m_cached_down = sd;

    // Update detail window via UI thread (history recording keeps working)
    if (m_detail_created) {
        PostMessage(m_detail.GetHwnd(), WM_PNM_REFRESH, 0, 0);
    }

    BuildTooltip(etw_active, stats, su, sd);
}

void CProcessNetPlugin::BuildTooltip(bool etw_active, const std::vector<ProcTraffic>& stats, double su, double sd) {
    wchar_t line[256];
    if (etw_active) {
        const wchar_t* st = m_etw_cap.ConnState();
        if (st && wcsstr(st, L"attach") != nullptr) {
            wchar_t warn[600];
            const wchar_t* owner = m_etw_cap.OwnerText();
            if (owner && owner[0])
                swprintf_s(warn, 600, TR(L"Process Net Monitor (ETW-attach)\n\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u7591\u4f3c\u88ab %s \u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed\u540e\u91cd\u542f TM\n"), owner);
            else
                swprintf_s(warn, 600, TR(L"Process Net Monitor (ETW-attach)\n\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u88ab\u5176\u4ed6\u7a0b\u5e8f\u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed AppNetworkCounter \u540e\u91cd\u542f TM\n"));
            wcscpy_s(m_tooltip, 2048, warn);
        } else {
            wcscpy_s(m_tooltip, 2048, L"Process Net Monitor (ETW)\n");
        }
    } else {
        wcscpy_s(m_tooltip, 2048, L"Process Net Monitor\n");
    }
    swprintf_s(line, 256, L"Total: U:%.1fKB/s D:%.1fKB/s\n", su/1024.0, sd/1024.0);
    wcscat_s(m_tooltip, line);

    wcscat_s(m_tooltip, L"\n--- Upload ---\n");
    std::vector<RecentProc*> up_list, down_list;
    {
        EnterCriticalSection(&m_data_lock);
        for (auto& [pid, rp] : m_items[0].m_recent) {
            if (rp.speed_up > 0.01 || rp.idle_rounds == 0) up_list.push_back(&rp);
        }
        for (auto& [pid, rp] : m_items[1].m_recent) {
            if (rp.speed_down > 0.01 || rp.idle_rounds == 0) down_list.push_back(&rp);
        }
        LeaveCriticalSection(&m_data_lock);
    }
    std::sort(up_list.begin(), up_list.end(), [](auto* a, auto* b) { return a->speed_up > b->speed_up; });
    std::sort(down_list.begin(), down_list.end(), [](auto* a, auto* b) { return a->speed_down > b->speed_down; });

    int count = 0;
    for (auto* rp : up_list) {
        if (count >= 5) break;
        wchar_t spd[32]; FmtSpeed(rp->speed_up, spd, 32);
        swprintf_s(line, 256, L"  %-14s %s\n", rp->name.c_str(), spd);
        wcscat_s(m_tooltip, line); count++;
    }
    while (count < 5) { wcscat_s(m_tooltip, L"  -\n"); count++; }

    wcscat_s(m_tooltip, L"\n--- Download ---\n");
    count = 0;
    for (auto* rp : down_list) {
        if (count >= 5) break;
        wchar_t spd[32]; FmtSpeed(rp->speed_down, spd, 32);
        swprintf_s(line, 256, L"  %-14s %s\n", rp->name.c_str(), spd);
        wcscat_s(m_tooltip, line); count++;
    }
    while (count < 5) { wcscat_s(m_tooltip, L"  -\n"); count++; }
}

// High-frequency refresh: runs on a TimerQueueTimer independent of TM's tick.
void CProcessNetPlugin::RefreshTick() {
    if (!m_started) return;

    // 运行期语言检测：TM 语言或 lang\ 目录变化 → 重载语言表并通知各窗口刷新
    CheckLanguageChange();

    ULONGLONG now = GetTickCount64();
    double dt = (double)(now - m_last_time) / 1000.0;
    if (dt < 0.05) dt = 0.05;
    m_last_time = now;

    auto stats = m_capture.GetStats(dt);
    bool etw_active = m_etw_cap.HasData();
    if (etw_active) {
        auto es = m_etw_cap.GetStats(dt);
        if (!es.empty()) {
            std::map<DWORD, int> conn_by_pid;
            for (auto& l : stats) conn_by_pid[l.pid] = l.conn_count;
            for (auto& e : es) {
                auto it = conn_by_pid.find(e.pid);
                if (it != conn_by_pid.end()) e.conn_count = it->second;
            }
            stats = std::move(es);
        }
    }

    // Rolling ~1s speed window (aligned with TM's default 1000ms monitor span)
    ComputeWindowSpeeds(stats);

    double su = 0, sd = 0;
    for (auto& s : stats) { su += s.speed_up; sd += s.speed_down; }

    {
        EnterCriticalSection(&m_data_lock);
        m_cached_stats = stats;
        m_cached_up = su;
        m_cached_down = sd;
        m_items[0].Update(stats, su, sd);
        m_items[1].Update(stats, su, sd);
        LeaveCriticalSection(&m_data_lock);
    }

    BuildTooltip(etw_active, stats, su, sd);

    // Never touch popup/detail window members from this timer thread - post
    // a refresh request; the window updates itself on its own (UI) thread.
    if (m_detail_created) {
        PostMessage(m_detail.GetHwnd(), WM_PNM_REFRESH, 0, 0);
    }
    if (m_popup_created && m_popup.IsVisible()) {
        PostMessage(m_popup.GetHwnd(), WM_PNM_REFRESH, 0, 0);
    }
}

// Runs on the detail window's UI thread (WM_PNM_REFRESH handler)
void CProcessNetPlugin::DetailRefreshFromSnapshot() {
    std::vector<ProcTraffic> snap;
    double su = 0, sd = 0;
    {
        EnterCriticalSection(&m_data_lock);
        snap = m_cached_stats;
        su = m_cached_up;
        sd = m_cached_down;
        LeaveCriticalSection(&m_data_lock);
    }
    m_detail.UpdateData(snap, su, sd);
}

// Runs on the popup's UI thread (WM_PNM_REFRESH handler)
void CProcessNetPlugin::PopupRefreshFromSnapshot() {
    if (!m_popup.IsVisible()) return;
    std::vector<ProcTraffic> snap;
    double su = 0, sd = 0;
    {
        EnterCriticalSection(&m_data_lock);
        snap = m_cached_stats;
        su = m_cached_up;
        sd = m_cached_down;
        LeaveCriticalSection(&m_data_lock);
    }
    std::vector<CTooltipPopup::ProcDisplayInfo> procs;
    GetProcessDisplayInfo(procs, snap);
    m_popup.UpdateData(procs, su, sd, EtwPopupStatus(&m_etw_cap));
}

void CProcessNetPlugin::ComputeWindowSpeeds(std::vector<ProcTraffic>& stats) {
    ULONGLONG now = GetTickCount64();
    for (auto& st : stats) {
        auto& q = m_win[st.pid];
        q.push_back(WinSample{ now, st.bytes_sent, st.bytes_recv });
        if (q.size() > 6) q.pop_front();
        while (q.size() >= 2 && now - q.front().tick > 1500) q.pop_front();
        if (q.size() >= 2) {
            const auto& f = q.front();
            const auto& b = q.back();
            double span = (double)(b.tick - f.tick) / 1000.0;
            if (span > 0.05) {
                // Guard against counter resets / source switches (legacy<->ETW
                // cumulative values differ): a backward jump must not become a
                // huge uint64 underflow speed.
                uint64_t ds = (b.sent >= f.sent) ? (b.sent - f.sent) : 0;
                uint64_t dr = (b.recv >= f.recv) ? (b.recv - f.recv) : 0;
                st.speed_up = (double)ds / span;
                st.speed_down = (double)dr / span;
            }
        } else {
            st.speed_up = 0;
            st.speed_down = 0;
        }
    }
    // Prune pids that stopped producing events
    for (auto it = m_win.begin(); it != m_win.end(); ) {
        if (it->second.empty() || now - it->second.back().tick > 3000)
            it = m_win.erase(it);
        else
            ++it;
    }
}

void CALLBACK CProcessNetPlugin::RefreshTimerProc(void* param, BOOLEAN) {
    ((CProcessNetPlugin*)param)->RefreshTick();
}

void CProcessNetPlugin::CheckLanguageChange() {
    const wchar_t* bcp = (m_app ? m_app->GetStringRes(L"BCP_47", L"general") : nullptr);
    std::wstring tm = bcp && bcp[0] ? bcp : L"zh-CN";
    bool changed = I18n::CheckAndReload(tm);
    if (changed) {
        // 通知 UI 线程刷新（本函数运行在刷新定时器线程，不直接碰窗口）
        if (m_detail_created && m_detail.GetHwnd())
            PostMessage(m_detail.GetHwnd(), WM_PNM_LANG_CHANGED, 0, 0);
        if (m_popup_created && m_popup.GetHwnd())
            PostMessage(m_popup.GetHwnd(), WM_PNM_LANG_CHANGED, 0, 0);
        HWND opt = FindWindowW(L"ProcessNetMonitorOptionsDlg", nullptr);
        if (opt) PostMessage(opt, WM_PNM_LANG_CHANGED, 0, 0);
    }
}

void CProcessNetPlugin::StartRefreshTimer() {
    StopRefreshTimer();
    int ms = m_detail.GetRefreshMs();
    if (ms < 100) ms = 100;
    if (ms > 2000) ms = 2000;
    if (CreateTimerQueueTimer(&m_refresh_timer, nullptr, RefreshTimerProc, this, ms, ms, WT_EXECUTELONGFUNCTION))
        m_refresh_timer_ok = true;
}

void CProcessNetPlugin::StopRefreshTimer() {
    if (m_refresh_timer) {
        DeleteTimerQueueTimer(nullptr, m_refresh_timer, INVALID_HANDLE_VALUE);
        m_refresh_timer = nullptr;
    }
    m_refresh_timer_ok = false;
}
const wchar_t* CProcessNetPlugin::GetInfo(PluginInfoIndex i) {
    switch (i) {
    case TMI_NAME: return L"ProcessNetMonitor";
    case TMI_DESCRIPTION: return L"Per-process network speed";
    case TMI_AUTHOR: return L"Aemeath";
    case TMI_COPYRIGHT: return L"MIT";
    case TMI_VERSION: return L"1.14.0";
    case TMI_URL: return L"https://github.com/deYangar/ProcessNetMonitor";
    default: return L"";
    }
}

const wchar_t* CProcessNetPlugin::GetTooltipInfo() {
    // When the Up/Down items are hidden, also stay out of TM's native
    // hover tooltip - the plugin's own popup already shows the process
    // list (issue #9).
    if (!CProcessNetItem::s_show_speed_items) return L"";
    return m_tooltip;
}

void CProcessNetPlugin::OnInitialize(ITrafficMonitor* p) {
    m_app = p;

    // Crash diagnostics: the filter catches normal unhandled exceptions;
    // the vectored handler also has a chance to catch failfast termination
    // (e.g. heap corruption 0xc0000374), which bypasses the filter.
    SetUnhandledExceptionFilter(PnmCrashHandler);
    AddVectoredExceptionHandler(1, PnmCrashHandler);
    _set_invalid_parameter_handler(PnmInvalidParamHandler);
    SetupWerLocalDumps();
    // Use DLL's HINSTANCE (not EXE's) so resource loading (icons, etc.) works
    HINSTANCE hInst = s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(NULL);
    m_popup_created = m_popup.Initialize(hInst);

    // Use TM's plugin config dir + plugin name (GetPluginConfigDir returns the plugins/ folder)
    const wchar_t* cfg_base = p->GetPluginConfigDir();
    if (cfg_base && cfg_base[0]) {
        std::wstring cfg_dir = std::wstring(cfg_base) + L"\\ProcessNetMonitor";
        m_detail.SetConfigDir(cfg_dir.c_str());
        // capture.log / etw_capture.log follow the "debug_logs" setting
        // (default OFF), synced inside LoadSettings. Crash diagnostics
        // (crash.log / crash.dmp / werdumps) are always enabled.
        // Production ETW per-process counter (attaches/starts NT Kernel Logger)
        m_etw_cap.Start();
    }
    m_detail.LoadHistory();
    m_detail.LoadSettings();   // 读取界面语言等设置（lang 字段）

    // ---- 本地化：扫描 lang\ 目录，按设置加载语言文件（DLL 同目录） ----
    // 语言文件与 TM 的 language\*.ini 同格式（UTF-8 BOM + [text] section）。
    // 模式：auto=跟随 TM 主程序语言；手动=用户在下拉框选择的 BCP-47。
    // 加载失败/无对应文件时 TR() 返回中文原文兜底，不影响使用。
    {
        wchar_t dll_path[MAX_PATH] = {};
        GetModuleFileNameW(s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(NULL), dll_path, MAX_PATH);
        std::wstring dll_dir(dll_path);
        size_t slash = dll_dir.find_last_of(L"\\");
        if (slash != std::wstring::npos) {
            dll_dir = dll_dir.substr(0, slash);
            std::wstring lang_dir = dll_dir + L"\\lang";
            I18n::ScanLangFiles(lang_dir);
            // TM 主程序语言（auto 模式匹配用）
            const wchar_t* bcp = (m_app ? m_app->GetStringRes(L"BCP_47", L"general") : nullptr);
            I18n::SetTmLang(bcp && bcp[0] ? bcp : L"zh-CN");
            // 用户设置：auto 或具体 BCP-47
            I18n::SetLang(m_detail.GetLangSetting());
            I18n::Reload();
        }
    }

    m_detail_created = m_detail.Initialize(hInst);  // InitColumns 使用已加载的语言

    // Set PacketCapture pointer for connection details
    m_detail.SetCapture(&m_capture);
    // Sync transparent width from settings to static member
    CProcessNetItem::s_transparent_width = m_detail.GetTransparentWidth();
    // Sync Up/Down items visibility master switch (issue #9)
    CProcessNetItem::s_show_speed_items = m_detail.GetShowSpeedItems();

    // IP 归属地库: 检查 DLL 同目录, 缺失则后台自动下载 (不阻塞 UI)
    {
        wchar_t dll_path[MAX_PATH] = {};
        GetModuleFileNameW(s_dll_hinst ? s_dll_hinst : (HINSTANCE)GetModuleHandleW(NULL), dll_path, MAX_PATH);
        std::wstring dll_dir(dll_path);
        size_t slash = dll_dir.find_last_of(L"\\");
        if (slash != std::wstring::npos) {
            dll_dir = dll_dir.substr(0, slash);
            IpGeo::Instance().EnsureDatabase(dll_dir);
        }
    }

    // NOTE: history recording happens inside CDetailWindow::UpdateData (fed by
    // the merged ETW-first stats). We must NOT also record from the legacy
    // OnStats callback - two sources with different cumulative counters
    // (legacy double-counts under TUN) alternate and inflate history deltas.

    // High-frequency data refresh timer (independent of TM's 1s tick)
    if (!m_lock_inited) {
        InitializeCriticalSection(&m_data_lock);
        m_lock_inited = true;
    }
    StartRefreshTimer();
}

CProcessNetPlugin::~CProcessNetPlugin() {
    StopRefreshTimer();
    if (m_lock_inited) {
        DeleteCriticalSection(&m_data_lock);
        m_lock_inited = false;
    }
}

void CProcessNetPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {
    if (index == EI_CONFIG_DIR && data && data[0]) {
        m_tm_config_dir = data;
        m_capture.SetTMConfigDir(m_tm_config_dir);
    } else if (index == EI_VALUE_TEXT_COLOR && data && data[0]) {
        // TM passes the value text color right before DrawItem (taskbar & main window)
        CProcessNetItem::s_value_color = (COLORREF)wcstoul(data, nullptr, 10);
        CProcessNetItem::s_has_value_color = true;
    }
}

// ============================================================
// Hover detection & popup management
// ============================================================

// Check if a TrafficMonitor window is the floating main window (not on taskbar)
static bool IsTMMainWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return false;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (!state) return true;

    RECT tb = abd.rc;
    switch (abd.uEdge) {
    case ABE_BOTTOM: return rc.top < tb.top;
    case ABE_TOP:    return rc.bottom > tb.bottom;
    case ABE_LEFT:   return rc.right > tb.right;
    case ABE_RIGHT:  return rc.left < tb.left;
    }
    return true;
}

// Check if a TrafficMonitor window is embedded in the taskbar
static bool IsTMTaskbarWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return false;

    RECT rc;
    GetWindowRect(hwnd, &rc);

    APPBARDATA abd = { sizeof(abd) };
    UINT state = SHAppBarMessage(ABM_GETTASKBARPOS, &abd);
    if (!state) return false;  // can't determine taskbar position

    // Check if window overlaps with taskbar
    RECT tb = abd.rc;
    RECT inter;
    return IntersectRect(&inter, &rc, &tb) != 0;
}

// Walk the parent chain to find a TrafficMonitor window.
// Dialogs (class #32770) stop the chain: hovering a settings / connection
// dialog (owned by TM) must NOT trigger our popup. The taskbar dialog is
// the exception - its title contains "TrafficMonitorTaskbarWindow" and we
// keep walking up to its parent (the main TM window).
static HWND FindTMWindowInChain(HWND hwnd) {
    for (HWND cur = hwnd; cur; cur = GetParent(cur)) {
        wchar_t cls[64] = {};
        GetClassNameW(cur, cls, 64);
        if (wcsncmp(cls, L"TrafficMonitor", 14) == 0)
            return cur;
        // Popup menu (#32768): hovering any menu (incl. submenus) must NOT
        // trigger our popup - menus are owned by TM but are separate windows
        // (2026-08-05: mouse on a submenu re-showed the popup).
        if (wcscmp(cls, L"#32768") == 0)
            return nullptr;
        if (wcscmp(cls, L"#32770") == 0) {
            wchar_t txt[256] = {};
            GetWindowTextW(cur, txt, 256);
            // Taskbar dialog: keep walking up to its parent (main TM window)
            if (wcsstr(txt, L"TrafficMonitorTaskbarWindow") != nullptr ||
                wcsstr(txt, L"TrafficMonitor") != nullptr)
                continue;
            // Any other dialog (options, connection details, ...): stop
            return nullptr;
        }
    }
    return nullptr;
}

// EnumWindows callback to find all TM windows
struct FindTMWindowsCtx {
    HWND main_wnd = nullptr;   // floating
    HWND taskbar_wnd = nullptr; // in taskbar
};
static BOOL CALLBACK FindTMWindowsProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcsncmp(cls, L"TrafficMonitor", 14) != 0) return TRUE;
    auto* ctx = reinterpret_cast<FindTMWindowsCtx*>(lp);
    if (IsTMMainWindow(hwnd)) ctx->main_wnd = hwnd;
    else if (IsTMTaskbarWindow(hwnd)) ctx->taskbar_wnd = hwnd;
    return TRUE;
}

// Classify the TM window under the cursor: main floating window or taskbar window.
// Returns nullptr when the cursor is not over any TrafficMonitor window.
static HWND DetectTMWindow(HWND hover_wnd, bool& is_taskbar) {
    is_taskbar = false;
    if (!hover_wnd) return nullptr;

    // TM's taskbar window is an MFC dialog with class "#32770" and window text
    // "TrafficMonitorTaskbarWindow". Its parent is the main TM window.
    // We must check the hovered window itself BEFORE walking the parent chain,
    // otherwise FindTMWindowInChain returns the parent (main window) and we
    // lose the taskbar context.
    wchar_t wnd_text[256] = {};
    GetWindowTextW(hover_wnd, wnd_text, 256);
    wchar_t hover_cls[64] = {};
    GetClassNameW(hover_wnd, hover_cls, 64);

    if (wcsstr(wnd_text, L"TrafficMonitorTaskbarWindow") != nullptr ||
        (wcscmp(hover_cls, L"#32770") == 0 && wcsstr(wnd_text, L"TrafficMonitor") != nullptr)) {
        // This is the taskbar dialog. Its parent is the main TM window.
        HWND parent = GetParent(hover_wnd);
        if (parent) {
            wchar_t pcls[64] = {};
            GetClassNameW(parent, pcls, 64);
            if (wcsncmp(pcls, L"TrafficMonitor", 14) == 0) {
                is_taskbar = true;
                return parent;  // return the main TM window handle
            }
        }
    }

    // Fallback: walk parent chain for the main floating window
    HWND tm = FindTMWindowInChain(hover_wnd);
    if (!tm) return nullptr;
    if (IsTMTaskbarWindow(tm)) { is_taskbar = true; return tm; }
    if (IsTMMainWindow(tm)) return tm;
    return nullptr;
}

void CProcessNetPlugin::ToggleDetailWindow(HWND parent_wnd) {
    if (!m_detail_created) return;
    if (m_detail.IsVisible())
        m_detail.Hide();
    else
        m_detail.Show(parent_wnd);
}

void CProcessNetPlugin::GetProcessDisplayInfo(
        std::vector<CTooltipPopup::ProcDisplayInfo>& out,
        const std::vector<ProcTraffic>& stats) {
    std::unordered_map<DWORD, CTooltipPopup::ProcDisplayInfo> merged;

    for (auto& [pid, rp] : m_items[0].m_recent) {
        auto& d = merged[pid];
        d.name = rp.name;
        d.speed_up = rp.speed_up;
    }
    for (auto& [pid, rp] : m_items[1].m_recent) {
        auto& d = merged[pid];
        d.name = rp.name;
        d.speed_down = rp.speed_down;
    }
    for (auto& st : stats) {
        auto it = merged.find(st.pid);
        if (it != merged.end() && !st.exe_path.empty()) {
            it->second.exe_path = st.exe_path;
        }
    }

    out.clear();
    for (auto& [pid, d] : merged) {
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) {
        double sa = (a.speed_up + a.speed_down > 0.01) ? (a.speed_up + a.speed_down) : 0;
        double sb = (b.speed_up + b.speed_down > 0.01) ? (b.speed_up + b.speed_down) : 0;
        return sa > sb;
    });

    if (out.size() > CTooltipPopup::MAX_SHOW)
        out.resize(CTooltipPopup::MAX_SHOW);
}

// ============================================================
// Hover state machine (100ms WM_TIMER on the popup window)
// ============================================================
//
// Behavior:
//   - Hover a TM window (floating main window or taskbar window) for 400ms
//     -> popup shows, anchored to that window.
//   - Move onto the popup itself -> stays open (user can click "查看详细").
//   - Leave everything -> popup hides after a 300ms grace period.
//   - Click on the taskbar item -> pins the popup (survives mouse leave);
//     click again / right-click -> dismisses. See OnMouseEvent.

void CProcessNetPlugin::ShowPopupAt(const RECT& anchor) {
    std::vector<CTooltipPopup::ProcDisplayInfo> procs;
    GetProcessDisplayInfo(procs, m_cached_stats);
    m_popup_anchor = anchor;
    m_popup.UpdateAndShow(procs, m_cached_up, m_cached_down, anchor, EtwPopupStatus(&m_etw_cap));
}

void CProcessNetPlugin::HoverTick() {
    if (!m_popup_created) return;

    ULONGLONG now = GetTickCount64();

    POINT pt;
    GetCursorPos(&pt);
    HWND hover_wnd = WindowFromPoint(pt);
    bool over_popup = (hover_wnd == m_popup.GetHwnd()) || m_popup.IsHovering();

    bool is_taskbar = false;
    HWND tm_wnd = DetectTMWindow(hover_wnd, is_taskbar);

    // --- show / hide state machine ---
    if (tm_wnd) {
        // Cursor over a TrafficMonitor window
        m_hover_leave_tick = 0;
        if (tm_wnd != m_hover_target) {
            // Switched target window: restart the hover delay
            m_hover_target = tm_wnd;
            m_hover_is_taskbar = is_taskbar;
            m_hover_start_tick = now;
        }
        if (!m_popup.IsVisible() && now - m_hover_start_tick >= 400) {
            RECT anchor;
            if (is_taskbar) {
                // Use the actual taskbar dialog rect (hover_wnd), not the parent
                // main window rect (tm_wnd). The parent is the floating main window
                // which lives on the desktop, not in the taskbar.
                RECT wr;
                GetWindowRect(hover_wnd, &wr);
                int half = 60;
                anchor = { pt.x - half, wr.top, pt.x + half, wr.bottom };
                if (anchor.left < wr.left) { anchor.left = wr.left; anchor.right = wr.left + 2 * half; }
                if (anchor.right > wr.right) { anchor.right = wr.right; anchor.left = wr.right - 2 * half; }
            } else {
                GetWindowRect(tm_wnd, &anchor);
            }
            m_popup.SetAnchorHwnd(tm_wnd);
            ShowPopupAt(anchor);
        }
    } else if (!over_popup) {
        // Cursor elsewhere: start/continue the hide grace period.
        // Keep m_hover_target alive while the popup is visible so it can keep
        // following the main window during a drag - the cursor may briefly
        // leave the window while it is being moved (window lags behind).
        m_hover_start_tick = 0;
        if (m_popup.IsVisible() && !m_popup_pinned) {
            if (m_hover_leave_tick == 0) {
                m_hover_leave_tick = now;
            } else if (now - m_hover_leave_tick >= 300) {
                m_popup.Hide();
                m_hover_target = nullptr;
            }
        } else if (!m_popup.IsVisible()) {
            m_hover_target = nullptr;
        }
    } else {
        // Cursor on the popup itself: keep it open
        m_hover_leave_tick = 0;
    }

    // --- data refresh while visible (lightweight: no reposition unless anchor moved) ---
    if (m_popup.IsVisible()) {
        std::vector<CTooltipPopup::ProcDisplayInfo> procs;
        GetProcessDisplayInfo(procs, m_cached_stats);
        RECT anchor = m_popup_anchor;
        // Follow the floating main window if it moves; taskbar/pinned keep their anchor
        if (!m_popup_pinned && m_hover_target && !m_hover_is_taskbar && IsWindow(m_hover_target)) {
            GetWindowRect(m_hover_target, &anchor);
        }
        if (!EqualRect(&anchor, &m_popup_anchor)) {
            m_popup_anchor = anchor;
            m_popup.SetAnchorHwnd(m_hover_target);
            m_popup.UpdateAndShow(procs, m_cached_up, m_cached_down, anchor, EtwPopupStatus(&m_etw_cap), m_hover_target);
        } else if (m_popup.RepositionIfTooltipsChanged()) {
            // Native tooltip has appeared/moved; popup was already repositioned.
            // Just refresh data without another layout pass.
            m_popup.UpdateData(procs, m_cached_up, m_cached_down, EtwPopupStatus(&m_etw_cap));
        } else {
            m_popup.UpdateData(procs, m_cached_up, m_cached_down, EtwPopupStatus(&m_etw_cap));
        }
    }
}

// ============================================================
// Options dialog - TUN address range settings
// ============================================================

static std::vector<std::wstring> g_new_ranges;  // temp storage for dialog result
static bool g_option_changed = false;

static BOOL CALLBACK OptionsSetFontProc(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)lp, TRUE);
    return TRUE;
}

// Forward declaration: auto-resize controls to fit translated text
static void OptionsAdjustLayout(HWND hwnd);

// 语言切换后刷新选项对话框全部文本（控件文本是创建时用 TR() 固化的）
static void OptionsApplyLang(HWND hwnd) {
    if (!hwnd) return;
    SetWindowTextW(hwnd, TR(L"\x63D2\x4EF6\x8BBE\x7F6E"));
    SetWindowTextW(GetDlgItem(hwnd, 1002), TR(L"\x900F\x660E\x533A\x57DF\x5BBD\x5EA6\xFF08px\xFF09:"));
    SetWindowTextW(GetDlgItem(hwnd, 1005), TR(L"\x5237\x65B0\x95F4\x9694\xFF08ms\xFF09:"));
    SetWindowTextW(GetDlgItem(hwnd, 1012), TR(L"\u542F\u7528\u8FDE\u63A5\u5F52\u5C5E\u5730\u663E\u793A\uFF08\u5173\u95ED\u540E\u4E0D\u4E0B\u8F7D\u5E93\uFF09"));
    SetWindowTextW(GetDlgItem(hwnd, 1010), TR(L"IP\u5E93\u4E0B\u8F7D\u8D70\u4EE3\u7406\u5730\u5740\uFF08\u652F\u6301 http/socks5\uFF0C\u7559\u7A7A=\u76F4\u8FDE\uFF09"));
    SetWindowTextW(GetDlgItem(hwnd, 1011), TR(L"IP\u5E93\u81EA\u52A8\u66F4\u65B0\u95F4\u9694\uFF08\u5929\uFF0C\u9ED8\u8BA47\uFF09"));
    SetWindowTextW(GetDlgItem(hwnd, 1009), TR(L"\u7ACB\u5373\u66F4\u65B0"));
    SetWindowTextW(GetDlgItem(hwnd, 1006), TR(L"\u8C03\u8BD5\u65E5\u5FD7\uFF08\u5199\u5165\u63D2\u4EF6\u76EE\u5F55 debug\\\uFF09"));
    SetWindowTextW(GetDlgItem(hwnd, 1013), TR(L"\u5728 TrafficMonitor \u9F20\u6807\u60AC\u505C\u63D0\u793A\u4E2D\u663E\u793A\u8FDB\u7A0B\u7F51\u901F\u4FE1\u606F"));
    SetWindowTextW(GetDlgItem(hwnd, 1015), TR(L"\u754C\u9762\u8BED\u8A00"));
    // 重填语言下拉：item0 = 跟随系统，其余为扫描到的语言（保持当前选择）
    HWND hCombo = GetDlgItem(hwnd, 1014);
    if (hCombo) {
        int cur = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)TR(L"\u8DDF\u968F\u7CFB\u7EDF"));
        for (const auto& lang : I18n::GetLangList()) {
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)lang.display_name.c_str());
        }
        SendMessageW(hCombo, CB_SETCURSEL, cur < 0 ? 0 : cur, 0);
    }
    // Re-measure and resize controls/dialog for the new language
    OptionsAdjustLayout(hwnd);
}

// 语言选择变化：保存设置 + 重载语言表 + 刷新所有窗口
static void OptionsApplyLanguageChange(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, 1014);
    int sel = hCombo ? (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0) : 0;
    std::wstring mode = L"auto";
    const auto& langs = I18n::GetLangList();
    if (sel > 0 && sel - 1 < (int)langs.size()) {
        mode = langs[sel - 1].bcp47;
    }
    auto& plugin = CProcessNetPlugin::Instance();
    plugin.m_detail.SetLangSetting(mode);
    plugin.m_detail.SaveSettings();
    I18n::SetLang(mode);
    I18n::Reload();
    // 详情窗口：列头重取翻译 + 重绘
    plugin.m_detail.InitColumns();
    if (plugin.m_detail.GetHwnd()) InvalidateRect(plugin.m_detail.GetHwnd(), NULL, FALSE);
    // 悬浮提示窗重绘
    if (plugin.m_popup.GetHwnd()) InvalidateRect(plugin.m_popup.GetHwnd(), NULL, FALSE);
    // 选项对话框自身文本
    OptionsApplyLang(hwnd);
}

// Measure text width in pixels using the dialog's current font.
static int MeasureTextWidth(HWND hwnd, const wchar_t* text) {
    HDC hdc = GetDC(hwnd);
    HFONT hFont = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
    HFONT hOld = hFont ? (HFONT)SelectObject(hdc, hFont) : nullptr;
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    if (hOld) SelectObject(hdc, hOld);
    ReleaseDC(hwnd, hdc);
    return sz.cx;
}

// Auto-resize controls and dialog to fit translated text.
// Called after WM_CREATE and after runtime language changes.
static void OptionsAdjustLayout(HWND hwnd) {
    const int MARGIN = 20;
    const int CTRL_PAD = 8;
    const int CHK_PAD = 28;   // checkbox/radio internal offset
    const int BTN_PAD = 20;   // push button padding

    int max_right = 0;

    // Step 1: Walk ALL child controls, resize to fit text, track max_right
    HWND hChild = GetWindow(hwnd, GW_CHILD);
    while (hChild) {
        wchar_t buf[512] = {};
        GetWindowTextW(hChild, buf, 512);
        if (buf[0]) {
            DWORD style = (DWORD)GetWindowLongPtrW(hChild, GWL_STYLE);
            bool is_push = (style & BS_PUSHBUTTON) != 0;
            bool is_chk = (style & (BS_AUTOCHECKBOX | BS_AUTORADIOBUTTON)) != 0;
            bool is_static = (style & 0xFF) == SS_SIMPLE || (style & 0xFF) == SS_LEFT || (style & 0xFF) == 0;
            int pad = (is_push || is_chk) ? CHK_PAD : 0;
            int text_w = MeasureTextWidth(hwnd, buf);
            int needed = text_w + pad + CTRL_PAD;

            RECT rc;
            GetWindowRect(hChild, &rc);
            MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
            int cur_w = rc.right - rc.left;

            if (needed > cur_w) {
                SetWindowPos(hChild, nullptr, 0, 0, needed, rc.bottom - rc.top,
                             SWP_NOMOVE | SWP_NOZORDER);
            }
            int right = rc.left + max(cur_w, needed);
            if (right > max_right) max_right = right;
        }
        hChild = GetWindow(hChild, GW_HWNDNEXT);
    }

    // Step 2: Expand dialog if controls overflow
    RECT rcClient;
    GetClientRect(hwnd, &rcClient);
    int desired_cx = max(500, max_right + MARGIN);
    if (desired_cx > rcClient.right) {
        RECT rcWnd = { 0, 0, desired_cx, rcClient.bottom };
        DWORD dlgStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
        DWORD dlgEx = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        AdjustWindowRectEx(&rcWnd, dlgStyle, FALSE, dlgEx);
        int new_w = rcWnd.right - rcWnd.left;
        int new_h = rcWnd.bottom - rcWnd.top;

        HWND hParent = GetParent(hwnd);
        if (hParent) {
            RECT rcP;
            GetWindowRect(hParent, &rcP);
            int x = rcP.left + ((rcP.right - rcP.left) - new_w) / 2;
            int y = rcP.top + ((rcP.bottom - rcP.top) - new_h) / 2;
            SetWindowPos(hwnd, nullptr, x, y, new_w, new_h, SWP_NOZORDER);
        } else {
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            SetWindowPos(hwnd, nullptr, (sw - new_w) / 2, (sh - new_h) / 2,
                         new_w, new_h, SWP_NOZORDER);
        }
        GetClientRect(hwnd, &rcClient);
    }

    // Step 3: Anchor right-side controls to dialog width
    int dlg_w = rcClient.right;
    // Refresh combo (1004): anchor to right
    HWND hRefresh = GetDlgItem(hwnd, 1004);
    if (hRefresh) {
        RECT rc; GetWindowRect(hRefresh, &rc);
        MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
        int combo_w = rc.right - rc.left;
        int new_x = dlg_w - 15 - combo_w;
        SetWindowPos(hRefresh, nullptr, new_x, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    // Width edit (1003): anchor to right
    HWND hWidthEdit = GetDlgItem(hwnd, 1003);
    if (hWidthEdit) {
        RECT rc; GetWindowRect(hWidthEdit, &rc);
        MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
        int edit_w = rc.right - rc.left;
        int new_x = dlg_w - 15 - edit_w;
        SetWindowPos(hWidthEdit, nullptr, new_x, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    // Days edit (1008): anchor to right of label
    HWND hDaysEdit = GetDlgItem(hwnd, 1008);
    if (hDaysEdit) {
        RECT rc; GetWindowRect(hDaysEdit, &rc);
        MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
        int edit_w = rc.right - rc.left;
        int new_x = dlg_w - 15 - 110 - 10 - edit_w;  // space for Update btn + gap + edit
        SetWindowPos(hDaysEdit, nullptr, new_x, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    // Update Now button (1009): anchor to right
    HWND hUpdateBtn = GetDlgItem(hwnd, 1009);
    if (hUpdateBtn) {
        RECT rc; GetWindowRect(hUpdateBtn, &rc);
        MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
        int btn_w = rc.right - rc.left;
        int new_x = dlg_w - 15 - btn_w;
        SetWindowPos(hUpdateBtn, nullptr, new_x, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }

    // Step 4: Right-align OK/Cancel with max_right
    int btn_y_ok = 0, btn_y_cancel = 0;
    for (int btn_id : { IDOK, IDCANCEL }) {
        HWND hBtn = GetDlgItem(hwnd, btn_id);
        if (!hBtn) continue;
        RECT rc;
        GetWindowRect(hBtn, &rc);
        MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)&rc, 2);
        int btn_w = rc.right - rc.left;
        int btn_y = rc.top;
        int offset = (btn_id == IDOK) ? 80 : 8;
        int new_x = max_right - btn_w - offset;
        if (new_x < 10) new_x = 10;
        SetWindowPos(hBtn, nullptr, new_x, btn_y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}


static LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Transparent area width label
        CreateWindowW(L"STATIC", TR(L"\x900F\x660E\x533A\x57DF\x5BBD\x5EA6\xFF08px\xFF09:"),
            WS_CHILD | WS_VISIBLE, 10, 12, 260, 20, hwnd, (HMENU)1002, nullptr, nullptr);
        // Width edit
        wchar_t width_buf[16];
        swprintf_s(width_buf, L"%d", CProcessNetItem::s_transparent_width);
        CreateWindowW(L"EDIT", width_buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            280, 10, 130, 24, hwnd, (HMENU)1003, nullptr, nullptr);

        // Refresh interval label + combo (100/250/500/1000 ms)
        CreateWindowW(L"STATIC", TR(L"\x5237\x65B0\x95F4\x9694\xFF08ms\xFF09:"),
            WS_CHILD | WS_VISIBLE, 10, 44, 260, 20, hwnd, (HMENU)1005, nullptr, nullptr);
        HWND hCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            190, 42, 130, 120, hwnd, (HMENU)1004, nullptr, nullptr);
        {
            static const wchar_t* items[] = { L"100", L"250", L"500", L"1000" };
            for (int i = 0; i < 4; i++) SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)items[i]);
            int cur = CProcessNetPlugin::Instance().m_detail.GetRefreshMs();
            int idx = 1;
            if (cur <= 100) idx = 0; else if (cur <= 250) idx = 1; else if (cur <= 500) idx = 2; else idx = 3;
            SendMessageW(hCombo, CB_SETCURSEL, idx, 0);
        }

        // 启用归属地显示 checkbox (第3行)
        CreateWindowW(L"BUTTON", TR(L"\u542F\u7528\u8FDE\u63A5\u5F52\u5C5E\u5730\u663E\u793A\uFF08\u5173\u95ED\u540E\u4E0D\u4E0B\u8F7D\u5E93\uFF09"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 70, 580, 22, hwnd, (HMENU)1012, nullptr, nullptr);
        if (IpGeo::Instance().IsEnabled())
            SendMessageW(GetDlgItem(hwnd, 1012), BM_SETCHECK, BST_CHECKED, 0);

        // ---- IP 库设置 ----
        // 代理服务器 label + edit (placeholder 提示)
        CreateWindowW(L"STATIC", TR(L"IP\u5E93\u4E0B\u8F7D\u8D70\u4EE3\u7406\u5730\u5740\uFF08\u652F\u6301 http/socks5\uFF0C\u7559\u7A7A=\u76F4\u8FDE\uFF09"),
            WS_CHILD | WS_VISIBLE, 10, 100, 610, 20, hwnd, (HMENU)1010, nullptr, nullptr);
        HWND hProxyEdit = CreateWindowW(L"EDIT", IpGeo::Instance().GetProxy().c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 122, 610, 24, hwnd, (HMENU)1007, nullptr, nullptr);
        // 占位提示 (EM_SETCUEBANNER 由 comctl32 处理, 无需额外样式; 注意 0x2000 是 ES_NUMBER 不能加!)
        SendMessageW(hProxyEdit, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"http://127.0.0.1:7890 \u6216 socks5://127.0.0.1:1080 \u7B49"));

        // 更新间隔 label + edit
        CreateWindowW(L"STATIC", TR(L"IP\u5E93\u81EA\u52A8\u66F4\u65B0\u95F4\u9694\uFF08\u5929\uFF0C\u9ED8\u8BA47\uFF09"),
            WS_CHILD | WS_VISIBLE, 10, 154, 500, 20, hwnd, (HMENU)1011, nullptr, nullptr);
        wchar_t days_buf[16];
        swprintf_s(days_buf, L"%d", IpGeo::Instance().GetUpdateDays());
        CreateWindowW(L"EDIT", days_buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            265, 152, 50, 24, hwnd, (HMENU)1008, nullptr, nullptr);

        // 立即更新按钮
        CreateWindowW(L"BUTTON", TR(L"\u7ACB\u5373\u66F4\u65B0"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 460, 152, 110, 24, hwnd, (HMENU)1009, nullptr, nullptr);

        // Debug-log checkbox (default OFF)
        CreateWindowW(L"BUTTON",
            TR(L"\u8C03\u8BD5\u65E5\u5FD7\uFF08\u5199\u5165\u63D2\u4EF6\u76EE\u5F55 debug\\\uFF09"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 186, 610, 22, hwnd, (HMENU)1006, nullptr, nullptr);
        if (CProcessNetPlugin::Instance().m_detail.GetDebugLogs())
            SendMessageW(GetDlgItem(hwnd, 1006), BM_SETCHECK, BST_CHECKED, 0);

        // Show/hide the plugin's section in TM's native hover tooltip
        // (issue #9: the plugin already has its own popup/detail window).
        // Only controls GetTooltipInfo(); the resident Up/Down items are
        // unaffected.
        CreateWindowW(L"BUTTON",
            TR(L"\u5728 TrafficMonitor \u9F20\u6807\u60AC\u505C\u63D0\u793A\u4E2D\u663E\u793A\u8FDB\u7A0B\u7F51\u901F\u4FE1\u606F"),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 214, 610, 22, hwnd, (HMENU)1013, nullptr, nullptr);
        if (CProcessNetItem::s_show_speed_items)
            SendMessageW(GetDlgItem(hwnd, 1013), BM_SETCHECK, BST_CHECKED, 0);

        // 界面语言 label + combo (跟随系统 + 扫描到的语言)
        CreateWindowW(L"STATIC", TR(L"\u754C\u9762\u8BED\u8A00"),
            WS_CHILD | WS_VISIBLE, 10, 242, 90, 20, hwnd, (HMENU)1015, nullptr, nullptr);
        HWND hLangCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            90, 240, 190, 200, hwnd, (HMENU)1014, nullptr, nullptr);
        {
            SendMessageW(hLangCombo, CB_ADDSTRING, 0, (LPARAM)TR(L"\u8DDF\u968F\u7CFB\u7EDF"));
            int cur = 0;
            const auto& langs = I18n::GetLangList();
            std::wstring cur_lang = CProcessNetPlugin::Instance().m_detail.GetLangSetting();
            for (size_t i = 0; i < langs.size(); i++) {
                SendMessageW(hLangCombo, CB_ADDSTRING, 0, (LPARAM)langs[i].display_name.c_str());
                if (cur_lang != L"auto" &&
                    (langs[i].bcp47 == cur_lang ||
                     langs[i].bcp47.substr(0, langs[i].bcp47.find(L'-')) == cur_lang.substr(0, cur_lang.find(L'-')))) {
                    cur = (int)i + 1;
                }
            }
            SendMessageW(hLangCombo, CB_SETCURSEL, cur, 0);
        }

        // OK button
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 380, 276, 75, 24, hwnd, (HMENU)IDOK, nullptr, nullptr);
        // Cancel button
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 465, 276, 75, 24, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        // Use the proper UI font (Segoe UI 9pt) on every child control -
        // default is the bitmap 'System' font (jagged, ugly).
        EnumChildWindows(hwnd, OptionsSetFontProc, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
        // Auto-resize controls and dialog to fit the current language's text
        OptionsAdjustLayout(hwnd);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            // Save transparent width
            wchar_t wbuf[16] = {};
            GetDlgItemTextW(hwnd, 1003, wbuf, 16);
            int new_width = _wtoi(wbuf);
            if (new_width < 0) new_width = 0;
            if (new_width > 500) new_width = 500;
            CProcessNetItem::s_transparent_width = new_width;

            auto& plugin = CProcessNetPlugin::Instance();
            plugin.m_detail.SetTransparentWidth(new_width);

            // Save refresh interval from combo
            HWND hCombo2 = GetDlgItem(hwnd, 1004);
            int sel = (int)SendMessageW(hCombo2, CB_GETCURSEL, 0, 0);
            static const int vals[] = { 100, 250, 500, 1000 };
            if (sel < 0) sel = 1;
            if (sel > 3) sel = 3;
            plugin.m_detail.SetRefreshMs(vals[sel]);

            // Debug-log checkbox
            bool dbg_logs = (SendMessageW(GetDlgItem(hwnd, 1006), BM_GETCHECK, 0, 0) == BST_CHECKED);
            plugin.m_detail.SetDebugLogs(dbg_logs);

            // Show/hide Up/Down items master switch (issue #9)
            bool show_speed = (SendMessageW(GetDlgItem(hwnd, 1013), BM_GETCHECK, 0, 0) == BST_CHECKED);
            CProcessNetItem::s_show_speed_items = show_speed;
            plugin.m_detail.SetShowSpeedItems(show_speed);

            // IP 库代理
            wchar_t proxy_buf[512] = {};
            GetDlgItemTextW(hwnd, 1007, proxy_buf, 512);
            IpGeo::Instance().SetProxy(proxy_buf);

            // IP 库更新间隔 (天)
            wchar_t days_buf[32] = {};
            GetDlgItemTextW(hwnd, 1008, days_buf, 32);
            int days = _wtoi(days_buf);
            if (days >= 1 && days <= 365) IpGeo::Instance().SetUpdateDays(days);

            // 启用归属地显示
            bool geo_on = (SendMessageW(GetDlgItem(hwnd, 1012), BM_GETCHECK, 0, 0) == BST_CHECKED);
            IpGeo::Instance().SetEnabled(geo_on);

            plugin.m_detail.SaveSettings();
            plugin.StartRefreshTimer();   // apply immediately
            g_option_changed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            g_option_changed = false;
            DestroyWindow(hwnd);
            return 0;
        }
        // 界面语言下拉：切换即时生效（保存 + 重载 + 刷新）
        if (LOWORD(wp) == 1014 && HIWORD(wp) == CBN_SELCHANGE) {
            OptionsApplyLanguageChange(hwnd);
            return 0;
        }
        // 立即更新 IP 库
        if (LOWORD(wp) == 1009) {
            IpGeo::Instance().ForceUpdate();
            EnableWindow(GetDlgItem(hwnd, 1009), FALSE);
            SetWindowTextW(GetDlgItem(hwnd, 1009), TR(L"\u66F4\u65B0\u4E2D..."));  // 更新中...
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_PNM_LANG_CHANGED:
        // 运行期语言变化：刷新选项对话框文本（由定时器线程 PostMessage 而来）
        OptionsApplyLang(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ITMPlugin::OptionReturn CProcessNetPlugin::ShowOptionsDialog(void* hParent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = OptionsWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ProcessNetMonitorOptionsDlg";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    
    g_option_changed = false;
    
    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"ProcessNetMonitorOptionsDlg",
        TR(L"\x63D2\x4EF6\x8BBE\x7F6E"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 650, 370,
        (HWND)hParent, nullptr, GetModuleHandleW(NULL), nullptr
    );
    
    if (!hwnd) return OR_OPTION_NOT_PROVIDED;
    
    // Center on parent
    if (hParent) {
        RECT rcParent, rcDlg;
        GetWindowRect((HWND)hParent, &rcParent);
        GetWindowRect(hwnd, &rcDlg);
        int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
        int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    } else {
        // Center on screen
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        RECT rc;
        GetWindowRect(hwnd, &rc);
        SetWindowPos(hwnd, nullptr, (sw - (rc.right - rc.left)) / 2, (sh - (rc.bottom - rc.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    
    // Modal loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsWindow(hwnd)) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    
    return g_option_changed ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
}

extern "C" {
    __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
        return &CProcessNetPlugin::Instance();
    }
}
