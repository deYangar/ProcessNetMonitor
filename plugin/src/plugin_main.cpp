#include "plugin_main.h"
#include "utils.h"
#include "ip_geo.h"
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
                swprintf_s(buf, 512, L"\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u7591\u4f3c\u88ab %s \u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed\u540e\u91cd\u542f TM", owner);
            } else {
                swprintf_s(buf, 512, L"\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u88ab\u5176\u4ed6\u7a0b\u5e8f\u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed AppNetworkCounter \u540e\u91cd\u542f TM");
            }
            return buf;
        }
    } else if (cap->IsRunning()) {
        return L"\u26a0 ETW \u4e0d\u53ef\u7528\uff0c\u5df2\u56de\u9000\u65e7\u91c7\u96c6\uff0c\u6570\u636e\u53ef\u80fd\u51c6\u786e\u964d\u4f4e";
    }
    return nullptr;
}

CProcessNetPlugin CProcessNetPlugin::s_instance;
wchar_t CProcessNetItem::s_value_buf[2][256] = { L"starting...", L"starting..." };
int CProcessNetItem::s_transparent_width = 100;  // default 100px

static void FmtSpeed(double bps, wchar_t* buf, int n) {
    FormatSpeed(bps, buf, n);
}

const wchar_t* CProcessNetItem::GetItemName() const {
    if (m_dir == DIR_TRANSPARENT) return L"\u900F\u660E\u533A\u57DF";
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
    if (m_dir == DIR_TRANSPARENT) return L"\u900F\u660E\u533A\u57DF";
    return m_dir == DIR_UPLOAD ? L"U:chrome.exe 5.6KB/s" : L"D:mihomo 1.4KB/s";
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
        FmtSpeed(spd, proc_str, 32);
    } else {
        FmtSpeed(0, proc_str, 32);
    }

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
            swprintf_s(m_tooltip, 2048, L"Process Net Monitor\n\u26a0 \u542f\u52a8\u5931\u8d25\uff1a%s\n\n%s",
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
                swprintf_s(warn, 600, L"Process Net Monitor (ETW-attach)\n\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u7591\u4f3c\u88ab %s \u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed\u540e\u91cd\u542f TM\n", owner);
            else
                swprintf_s(warn, 600, L"Process Net Monitor (ETW-attach)\n\u26a0 \u9644\u52a0\u6a21\u5f0f\uff1aNT Kernel Logger \u88ab\u5176\u4ed6\u7a0b\u5e8f\u5360\u7528\uff0c\u5927\u6d41\u91cf\u65f6\u53ef\u80fd\u4e22\u4e8b\u4ef6\uff0c\u5efa\u8bae\u5173\u95ed AppNetworkCounter \u540e\u91cd\u542f TM\n");
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
    case TMI_VERSION: return L"1.12.0";
    case TMI_URL: return L"https://github.com/deYangar/ProcessNetMonitor";
    default: return L"";
    }
}

const wchar_t* CProcessNetPlugin::GetTooltipInfo() { return m_tooltip; }

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
    m_detail_created = m_detail.Initialize(hInst);
    
    // Set PacketCapture pointer for connection details
    m_detail.SetCapture(&m_capture);

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
    m_detail.LoadSettings();
    // Sync transparent width from settings to static member
    CProcessNetItem::s_transparent_width = m_detail.GetTransparentWidth();

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
            m_popup.UpdateAndShow(procs, m_cached_up, m_cached_down, anchor, EtwPopupStatus(&m_etw_cap));
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

static LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Transparent area width label
        CreateWindowW(L"STATIC", L"\x900F\x660E\x533A\x57DF\x5BBD\x5EA6\xFF08px\xFF09:",
            WS_CHILD | WS_VISIBLE, 10, 12, 200, 20, hwnd, (HMENU)1002, nullptr, nullptr);
        // Width edit
        wchar_t width_buf[16];
        swprintf_s(width_buf, L"%d", CProcessNetItem::s_transparent_width);
        CreateWindowW(L"EDIT", width_buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            260, 10, 110, 24, hwnd, (HMENU)1003, nullptr, nullptr);

        // Refresh interval label + combo (100/250/500/1000 ms)
        CreateWindowW(L"STATIC", L"\x5237\x65B0\x95F4\x9694\xFF08ms\xFF09:",
            WS_CHILD | WS_VISIBLE, 10, 44, 130, 20, hwnd, (HMENU)1005, nullptr, nullptr);
        HWND hCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            150, 42, 100, 120, hwnd, (HMENU)1004, nullptr, nullptr);
        {
            static const wchar_t* items[] = { L"100", L"250", L"500", L"1000" };
            for (int i = 0; i < 4; i++) SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)items[i]);
            int cur = CProcessNetPlugin::Instance().m_detail.GetRefreshMs();
            int idx = 1;
            if (cur <= 100) idx = 0; else if (cur <= 250) idx = 1; else if (cur <= 500) idx = 2; else idx = 3;
            SendMessageW(hCombo, CB_SETCURSEL, idx, 0);
        }

        // 启用归属地显示 checkbox (第3行)
        CreateWindowW(L"BUTTON", L"\u542F\u7528\u8FDE\u63A5\u5F52\u5C5E\u5730\u663E\u793A\uFF08\u5173\u95ED\u540E\u4E0D\u4E0B\u8F7D\u5E93\uFF09",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 70, 340, 22, hwnd, (HMENU)1012, nullptr, nullptr);
        if (IpGeo::Instance().IsEnabled())
            SendMessageW(GetDlgItem(hwnd, 1012), BM_SETCHECK, BST_CHECKED, 0);

        // ---- IP 库设置 ----
        // 代理服务器 label + edit (placeholder 提示)
        CreateWindowW(L"STATIC", L"IP\u5E93\u4E0B\u8F7D\u8D70\u4EE3\u7406\u5730\u5740\uFF08\u652F\u6301 http/socks5\uFF0C\u7559\u7A7A=\u76F4\u8FDE\uFF09",
            WS_CHILD | WS_VISIBLE, 10, 100, 470, 20, hwnd, (HMENU)1010, nullptr, nullptr);
        HWND hProxyEdit = CreateWindowW(L"EDIT", IpGeo::Instance().GetProxy().c_str(),
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 122, 460, 24, hwnd, (HMENU)1007, nullptr, nullptr);
        // 占位提示 (EM_SETCUEBANNER 由 comctl32 处理, 无需额外样式; 注意 0x2000 是 ES_NUMBER 不能加!)
        SendMessageW(hProxyEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"http://127.0.0.1:7890 \u6216 socks5://127.0.0.1:1080 \u7B49");

        // 更新间隔 label + edit
        CreateWindowW(L"STATIC", L"IP\u5E93\u81EA\u52A8\u66F4\u65B0\u95F4\u9694\uFF08\u5929\uFF0C\u9ED8\u8BA47\uFF09",
            WS_CHILD | WS_VISIBLE, 10, 154, 250, 20, hwnd, (HMENU)1011, nullptr, nullptr);
        wchar_t days_buf[16];
        swprintf_s(days_buf, L"%d", IpGeo::Instance().GetUpdateDays());
        CreateWindowW(L"EDIT", days_buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            265, 152, 50, 24, hwnd, (HMENU)1008, nullptr, nullptr);

        // 立即更新按钮
        CreateWindowW(L"BUTTON", L"\u7ACB\u5373\u66F4\u65B0",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 325, 152, 95, 24, hwnd, (HMENU)1009, nullptr, nullptr);

        // Debug-log checkbox (default OFF, 最后一行)
        CreateWindowW(L"BUTTON",
            L"\u8C03\u8BD5\u65E5\u5FD7\uFF08\u5199\u5165\u63D2\u4EF6\u76EE\u5F55 debug\\\uFF09",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 186, 460, 22, hwnd, (HMENU)1006, nullptr, nullptr);
        if (CProcessNetPlugin::Instance().m_detail.GetDebugLogs())
            SendMessageW(GetDlgItem(hwnd, 1006), BM_SETCHECK, BST_CHECKED, 0);

        // OK button
        CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 230, 216, 65, 24, hwnd, (HMENU)IDOK, nullptr, nullptr);
        // Cancel button
        CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 305, 216, 65, 24, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        // Use the proper UI font (Segoe UI 9pt) on every child control -
        // default is the bitmap 'System' font (jagged, ugly).
        EnumChildWindows(hwnd, OptionsSetFontProc, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
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
        // 立即更新 IP 库
        if (LOWORD(wp) == 1009) {
            IpGeo::Instance().ForceUpdate();
            EnableWindow(GetDlgItem(hwnd, 1009), FALSE);
            SetWindowTextW(GetDlgItem(hwnd, 1009), L"\u66F4\u65B0\u4E2D...");  // 更新中...
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
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
        L"\x63D2\x4EF6\x8BBE\x7F6E",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        0, 0, 500, 330,
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
