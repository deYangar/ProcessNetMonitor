// ============================================================
// Production ETW per-process network byte counter - etw_capture.h
// Proven on Win11 25H2 (build 26200) + mihomo TUN: classic
// kernel network events DO carry per-process byte sizes; parse
// via TDH schema for robustness.
// ============================================================
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "etw_capture.h"
#include "utils.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tdh.h>
#include <evntprov.h>
#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <share.h>
#include <set>
#include <tlhelp32.h>

#pragma comment(lib, "tdh.lib")

EtwCapture* EtwCapture::s_self = nullptr;
std::atomic<bool> EtwCapture::s_debug_logs{ false };

// ---- best-effort ETW session owner detection ----
// There is no public API mapping an ETW session to its creator process.
// We enumerate all system handles and find processes holding handles of
// type EtwSession (the session controller) / EtwConsumer (consumers).
// Visibility is limited by token rights: handles of SYSTEM/admin processes
// may be invisible - in that case we just fall back to the generic hint.

typedef LONG NTSTATUS;
typedef NTSTATUS(NTAPI* pNtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI* pNtQueryObject)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef NTSTATUS(NTAPI* pNtDuplicateObject)(HANDLE, HANDLE, HANDLE, PHANDLE, ULONG, ULONG, ULONG);

struct SysHandleEntry {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};
struct SysHandleInfo {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SysHandleEntry Handles[1];
};
// UNICODE_STRING is not defined by windows.h - define our own layout
struct EtwUsStr {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t* Buffer;
};
struct ObjTypeInfo {
    EtwUsStr TypeName;
    ULONG TotalNumberOfObjects;
    ULONG TotalNumberOfHandles;
    ULONG TotalPagedPoolUsage;
    ULONG TotalNonPagedPoolUsage;
    ULONG TotalNamePoolUsage;
    ULONG TotalHandleTableUsage;
    ULONG HighWaterNumberOfObjects;
    ULONG HighWaterNumberOfHandles;
    ULONG HighWaterPagedPoolUsage;
    ULONG HighWaterNonPagedPoolUsage;
    ULONG HighWaterNamePoolUsage;
    ULONG HighWaterHandleTableUsage;
    ULONG InvalidAttributes;
    GENERIC_MAPPING GenericMapping;
    ULONG ValidAccess;
    BOOLEAN SecurityRequired;
    BOOLEAN MaintainHandleCount;
    BOOLEAN TypeListLock;
    BOOLEAN SecurityDescriptor;
};

std::wstring EtwCapture::FindSessionOwners() {
    std::wstring out;

    // 1) Handle enumeration: precise when the process can see the owner's
    // handles (works reliably when TM runs elevated).
    {
        auto ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            auto qsi = (pNtQuerySystemInformation)GetProcAddress(ntdll, "NtQuerySystemInformation");
            auto qo = (pNtQueryObject)GetProcAddress(ntdll, "NtQueryObject");
            auto ndup = (pNtDuplicateObject)GetProcAddress(ntdll, "NtDuplicateObject");
            if (qsi && qo && ndup) {
                ULONG len = 0;
                std::vector<BYTE> buf;
                for (int attempt = 0; attempt < 3; attempt++) {
                    buf.resize(len ? len : (1 << 20));
                    ULONG needed = 0;
                    NTSTATUS st = qsi(64 /*SystemExtendedHandleInformation*/, buf.data(), (ULONG)buf.size(), &needed);
                    if (st == 0) break;
                    if (needed > buf.size()) { len = needed; continue; }
                    buf.clear();
                    break;
                }
                if (!buf.empty()) {
                    DWORD self_pid = GetCurrentProcessId();
                    std::set<DWORD> pids;
                    auto* info = (SysHandleInfo*)buf.data();
                    for (ULONG_PTR i = 0; i < info->NumberOfHandles; i++) {
                        auto& h = info->Handles[i];
                        if (h.UniqueProcessId == self_pid || h.UniqueProcessId == 0 || h.UniqueProcessId == 4) continue;
                        HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)h.UniqueProcessId);
                        if (!hProc) continue;
                        HANDLE dup = nullptr;
                        if (ndup(hProc, (HANDLE)h.HandleValue, GetCurrentProcess(), &dup, 0x02000000, 0, 0) != 0) {
                            CloseHandle(hProc);
                            continue;
                        }
                        BYTE tbuf[512] = {};
                        ULONG ret = 0;
                        if (qo(dup, 2 /*ObjectTypeInformation*/, tbuf, sizeof(tbuf), &ret) == 0) {
                            auto* t = (ObjTypeInfo*)tbuf;
                            if (t->TypeName.Buffer && t->TypeName.Length > 0) {
                                std::wstring type(t->TypeName.Buffer, t->TypeName.Length / 2);
                                if (type == L"EtwSession" || type == L"EtwConsumer")
                                    pids.insert((DWORD)h.UniqueProcessId);
                            }
                        }
                        CloseHandle(dup);
                        CloseHandle(hProc);
                    }
                    std::set<std::wstring> names;
                    for (DWORD pid : pids) {
                        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                        if (hp) {
                            wchar_t path[512]; DWORD n = 512;
                            if (QueryFullProcessImageNameW(hp, 0, path, &n)) {
                                std::wstring full = path;
                                size_t p = full.find_last_of(L"\\/");
                                names.insert((p != std::wstring::npos) ? full.substr(p + 1) : full);
                            }
                            CloseHandle(hp);
                        }
                    }
                    for (auto& nm : names) {
                        if (!out.empty()) out += L", ";
                        out += nm;
                    }
                }
            }
        }
    }
    if (!out.empty()) return out;

    // 2) Fallback heuristic: known tools that consume the NT Kernel Logger.
    static const wchar_t* tools[] = {
        L"appnetworkcounter", L"netlimiter", L"glasswire", L"dumpcap",
        L"wireshark", L"procmon", L"procmon64", L"perfmon", L"wpr",
        L"xperf", L"resmon", nullptr
    };
    std::set<std::wstring> names;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(snap, &pe)) {
            do {
                std::wstring n = pe.szExeFile;
                for (auto& c : n) c = towlower(c);
                for (int i = 0; tools[i]; i++) {
                    if (n.find(tools[i]) != std::wstring::npos) {
                        names.insert(pe.szExeFile);
                        break;
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    for (auto& nm : names) {
        if (!out.empty()) out += L", ";
        out += nm;
    }
    return out;
}

// ---- diagnostics log (shared read: TM can keep running) ----
static FILE* EtwLogOpen() {
    wchar_t path[MAX_PATH] = L"";
    if (!PNM_GetDebugDir(path, MAX_PATH)) return nullptr;
    wcscat_s(path, L"\\etw_capture.log");
    FILE* f = _wfsopen(path, L"a", _SH_DENYNO);
    return f;
}

void EtwCapture::LogLine(const wchar_t* fmt, ...) {
    if (!s_debug_logs.load()) return;  // debug logging switch (default off)
    FILE* f = EtwLogOpen();
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(f, L"[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

// dir: 0=ignore 1=send 2=recv
static int ClassifyOpcodeName(const std::wstring& nm) {
    if (nm.empty()) return 0;
    // exclude retransmit/copy (duplicate bytes already counted by send/recv)
    if (nm.find(L"etransmit") != std::wstring::npos) return 0;
    if (nm.find(L"Copy") != std::wstring::npos || nm.find(L"copied") != std::wstring::npos) return 0;
    if (nm.find(L"Send") != std::wstring::npos || nm.find(L"send") != std::wstring::npos ||
        nm.find(L"sent") != std::wstring::npos || nm.find(L"Datasent") != std::wstring::npos)
        return 1;
    if (nm.find(L"Recv") != std::wstring::npos || nm.find(L"recv") != std::wstring::npos ||
        nm.find(L"received") != std::wstring::npos || nm.find(L"Receive") != std::wstring::npos)
        return 2;
    return 0;
}

static uint32_t IntypeSize2(uint16_t intype) {
    uint16_t base = intype & 0x0FFF;
    if (intype & 0x1000) return (uint32_t)sizeof(void*);
    switch (base) {
    case 3: case 4: case 13: return 1;
    case 5: case 6: return 2;
    case 7: case 8: case 11: return 4;              // int32, uint32, float
    case 9: case 10: case 12: case 16: case 17: return 8;  // int64, uint64, double, pointer, filetime
    case 20: return 4;                              // hexint32
    case 21: return 8;                              // hexint64 (was wrongly 4)
    default: return 0;                              // binary/counted/... handled by p.length
    }
}

EtwCapture::~EtwCapture() { Stop(); }

bool EtwCapture::Start() {
    if (m_running) return true;
    s_self = this;
    m_stop = false;
    m_last_event_tick.store(0);
    m_ev_total = 0; m_ev_net = 0; m_ev_filt_addr = 0; m_ev_kept = 0;
    wcscpy_s(m_conn_state, L"starting");
    try { m_thread = std::thread([this] { ConsumerLoop(); }); }
    catch (...) { return false; }
    return true;
}

void EtwCapture::Stop() {
    if (!m_running && !m_thread.joinable()) return;
    m_stop = true;

    EVENT_TRACE_PROPERTIES sp;
    memset(&sp, 0, sizeof(sp));
    sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
    if (m_own_session && m_own_session_handle) {
        ControlTraceW(m_own_session_handle, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);
    } else {
        TRACEHANDLE h = m_cur_consumer.load();
        if (h != INVALID_PROCESSTRACE_HANDLE) CloseTrace(h);
    }
    if (m_thread.joinable()) {
        for (int i = 0; i < 30 && m_running; i++) Sleep(100);
        if (m_thread.joinable()) {
            try { m_thread.join(); } catch (...) { m_thread.detach(); }
        }
    }
    m_running = false;
}

void EtwCapture::SetError(const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(m_error, 256, _TRUNCATE, fmt, ap);
    va_end(ap);
}

void EtwCapture::ConsumerLoop() {
    m_running = true;

    while (!m_stop) {
        TRACEHANDLE consumer = INVALID_PROCESSTRACE_HANDLE;
        bool started_own = false;

        EVENT_TRACE_LOGFILEW lf;
        memset(&lf, 0, sizeof(lf));
        lf.LoggerName = const_cast<LPWSTR>(KERNEL_LOGGER_NAMEW);
        lf.ProcessTraceMode = EVENT_TRACE_REAL_TIME_MODE | PROCESS_TRACE_MODE_EVENT_RECORD;
        lf.EventRecordCallback = &EtwCapture::Callback;

        // 1) Prefer our OWN session: we control buffer sizing so heavy
        // traffic doesn't overflow and drop events (attaching to another
        // app's NT Kernel Logger inherits ITS small buffers).
        DWORD nameBytes = (DWORD)((wcslen(KERNEL_LOGGER_NAMEW) + 1) * sizeof(wchar_t));
        DWORD bufSize = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
        std::vector<BYTE> buf(bufSize, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = bufSize;
        props->Wnode.ClientContext = 1;
        props->BufferSize = 256;
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 120;
        props->FlushTimer = 1;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;

        TRACEHANDLE session = 0;
        ULONG sr = StartTraceW(&session, KERNEL_LOGGER_NAMEW, props);
        if (sr == ERROR_SUCCESS) {
            started_own = true;
            m_own_session_handle = session;
            wcscpy_s(m_conn_state, L"self-started");
            LogLine(L"StartTrace OK (own session, maxbuf=%u)", props->MaximumBuffers);
            consumer = OpenTraceW(&lf);
            if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
                sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
                ControlTraceW(session, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);
                started_own = false;
                SetError(L"ETW OpenTrace failed err=%lu", GetLastError());
                LogLine(L"OpenTrace after StartTrace FAILED err=%lu", GetLastError());
            }
        } else if (sr == ERROR_ALREADY_EXISTS) {
            // Someone else owns "NT Kernel Logger" - usually a stale session
            // left by a killed app (e.g. ANC). Try to stop it and take over;
            // if that fails, attach to theirs.
            EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
            sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
            ULONG st = ControlTraceW(0, KERNEL_LOGGER_NAMEW, &sp, EVENT_TRACE_CONTROL_STOP);
            LogLine(L"session exists - stop attempt rc=%lu", st);
            if (st == ERROR_SUCCESS) {
                ULONG sr2 = StartTraceW(&session, KERNEL_LOGGER_NAMEW, props);
                if (sr2 == ERROR_SUCCESS) {
                    started_own = true;
                    m_own_session_handle = session;
                    wcscpy_s(m_conn_state, L"self-started");
                    LogLine(L"took over stale session - StartTrace OK (own, maxbuf=%u)", props->MaximumBuffers);
                    consumer = OpenTraceW(&lf);
                    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                        EVENT_TRACE_PROPERTIES sp2; memset(&sp2, 0, sizeof(sp2));
                        sp2.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
                        ControlTraceW(session, nullptr, &sp2, EVENT_TRACE_CONTROL_STOP);
                        started_own = false;
                        SetError(L"ETW OpenTrace failed err=%lu", GetLastError());
                        LogLine(L"OpenTrace after takeover FAILED err=%lu", GetLastError());
                    }
                } else {
                    LogLine(L"StartTrace after stop failed err=%lu", sr2);
                }
            }
            if (!started_own && consumer == INVALID_PROCESSTRACE_HANDLE) {
                // fall back: attach to whatever is there
                wcscpy_s(m_conn_state, L"attached");
                LogLine(L"attaching to existing session instead (buffers NOT ours)");
                m_owner = FindSessionOwners();
                LogLine(L"ETW session owner(s): %s", m_owner.empty() ? L"(unknown)" : m_owner.c_str());
                consumer = OpenTraceW(&lf);
                if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                    LogLine(L"attach failed err=%lu", GetLastError());
                    SetError(L"ETW attach failed err=%lu", GetLastError());
                }
            }
        } else {
            SetError(L"ETW StartTrace failed err=%lu (need admin?)", sr);
            LogLine(L"StartTrace failed err=%lu", sr);
        }

        if (consumer == INVALID_PROCESSTRACE_HANDLE) {
            wcscpy_s(m_conn_state, L"failed");
            if (m_stop) break;
            Sleep(5000);
            continue;
        }

        m_own_session = started_own;
        m_cur_consumer = consumer;
        ULONG rc = ProcessTrace(&consumer, 1, nullptr, nullptr);
        m_cur_consumer = INVALID_PROCESSTRACE_HANDLE;
        m_own_session = false;
        CloseTrace(consumer);
        LogLine(L"ProcessTrace returned rc=%lu - session ended, reconnecting in 2s", rc);

        if (started_own) {
            EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
            sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
            ControlTraceW(m_own_session_handle, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);
            m_own_session_handle = 0;
        }

        if (m_stop) break;
        // Session ended (owner exited) - try to re-attach shortly
        (void)rc;
        Sleep(2000);
    }

    m_running = false;
}

void EtwCapture::Callback(PEVENT_RECORD rec) {
    if (s_self) s_self->OnEvent(rec);
}

EtwCapture::ShapeInfo& EtwCapture::ResolveShape(PEVENT_RECORD rec, const ShapeKey& key) {
    // called with m_mutex held
    auto it = m_shapes.find(key);
    if (it != m_shapes.end()) return it->second;
    ShapeInfo si;

    ULONG need = 0;
    TRACE_EVENT_INFO* info = nullptr;
    ULONG st = TdhGetEventInformation(rec, 0, nullptr, nullptr, &need);
    if (st == ERROR_INSUFFICIENT_BUFFER && need > 0 && need < 1024 * 1024) {
        info = (TRACE_EVENT_INFO*)malloc(need);
        if (info) st = TdhGetEventInformation(rec, 0, nullptr, info, &need);
    }
    if (st == ERROR_SUCCESS && info) {
        const wchar_t* opName = info->OpcodeNameOffset ?
            (const wchar_t*)((BYTE*)info + info->OpcodeNameOffset) : L"";
        si.dir = ClassifyOpcodeName(opName);

        uint32_t off = 0;
        bool valid = true;
        int pid_idx = -1, size_idx = -1;
        for (ULONG i = 0; i < info->TopLevelPropertyCount; i++) {
            auto& p = info->EventPropertyInfoArray[i];
            uint32_t psz = IntypeSize2(p.nonStructType.InType);
            if (psz == 0) {
                // fixed-size binary (IPv4/IPv6 address blobs)
                uint16_t base = p.nonStructType.InType & 0x0FFF;
                if (base == 14 && (p.length == 4 || p.length == 16)) psz = p.length;
            }
            const wchar_t* nm = p.NameOffset ? (const wchar_t*)((BYTE*)info + p.NameOffset) : L"";
            if (valid) {
                if (psz == 0) {
                    valid = false;  // later offsets unknown; earlier ones stay valid
                } else {
                    if (pid_idx < 0 && (_wcsicmp(nm, L"PID") == 0 || _wcsicmp(nm, L"ProcessId") == 0)) {
                        pid_idx = (int)i; si.pid_off = off; si.pid_len = psz;
                    }
                    if (size_idx < 0 && _wcsicmp(nm, L"size") == 0) {
                        size_idx = (int)i; si.size_off = off; si.size_len = psz;
                    }
                    if (si.daddr_len == 0 && _wcsicmp(nm, L"daddr") == 0) { si.daddr_off = off; si.daddr_len = psz; }
                    if (si.saddr_len == 0 && _wcsicmp(nm, L"saddr") == 0) { si.saddr_off = off; si.saddr_len = psz; }
                    off += psz;
                }
            }
        }
        si.offsets_ok = (pid_idx >= 0 && size_idx >= 0);
        si.resolved = true;
        free(info);
    } else {
        if (info) free(info);
        // Fallback: classic kernel layout PID@0 size@4, only trust opcodes 10/11 (send/recv v4)
        si.resolved = true;
        si.offsets_ok = true;
        si.pid_off = 0; si.pid_len = 4;
        si.size_off = 4; si.size_len = 4;
        UCHAR op = rec->EventHeader.EventDescriptor.Opcode;
        if (op == 10) si.dir = 1;
        else if (op == 11) si.dir = 2;
        else si.dir = 0;
    }
    auto res = m_shapes.emplace(key, si);
    return res.first->second;
}

void EtwCapture::OnEvent(PEVENT_RECORD rec) {
    if (m_stop) return;

    ShapeKey key;
    key.p1 = rec->EventHeader.ProviderId.Data1;
    key.task = rec->EventHeader.EventDescriptor.Task;
    key.opcode = rec->EventHeader.EventDescriptor.Opcode;
    key.id = rec->EventHeader.EventDescriptor.Id;
    key.version = rec->EventHeader.EventDescriptor.Version;

    USHORT ulen = rec->UserDataLength;
    const BYTE* ud = (const BYTE*)rec->UserData;
    if (!ud || ulen < 8) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    m_ev_total++;
    ShapeInfo& si = ResolveShape(rec, key);
    if (si.dir == 0 || !si.offsets_ok) return;
    if (ulen < si.pid_off + si.pid_len || ulen < si.size_off + si.size_len) return;
    m_ev_net++;

    // Periodic refresh of adapter IP lists
    {
        ULONGLONG tnow = GetTickCount64();
        if (tnow - m_phys.refreshed > 15000) RefreshIfaceIpsLocked();
    }
    // Blacklist semantics: skip ONLY events whose local address belongs to a
    // virtual/TUN adapter (dedup - the same bytes also appear once on the
    // physical side). Unknown addresses are always kept.
    bool skip = SkipByLocalAddr(ud, ulen, si);
    // sample local addresses for diagnostics
    {
        uint32_t sample_pid = si.pid_len == 4 ? *(const uint32_t*)(ud + si.pid_off) : 0;
        if (sample_pid == 0 || sample_pid == (uint32_t)-1) sample_pid = rec->EventHeader.ProcessId;
        uint32_t loff = (si.dir == 1) ? si.saddr_off : si.daddr_off;
        uint32_t llen = (si.dir == 1) ? si.saddr_len : si.daddr_len;
        if (llen == 4 && ulen >= (USHORT)(loff + 4)) {
            uint32_t x; memcpy(&x, ud + loff, 4);
            (skip ? m_samp_filt : m_samp_kept).insert(x);
            if (!skip) {
                // flag shapes whose "local" looks like a public/remote address
                const uint8_t* b = (const uint8_t*)&x;
                bool priv = (b[0] == 10) || (b[0] == 127) ||
                    (b[0] == 169 && b[1] == 254) ||
                    (b[0] == 172 && b[1] >= 16 && b[1] <= 31) ||
                    (b[0] == 192 && b[1] == 168) ||
                    (b[0] == 198 && (b[1] == 18 || b[1] == 19)) ||
                    (b[0] == 100 && b[1] >= 64 && b[1] <= 127) ||
                    (b[0] == 0) || (b[0] >= 224);
                if (!priv) {
                    WeirdShape ws{ key.p1, key.task, key.opcode, key.id, key.version, si.dir, sample_pid, x };
                    m_samp_weird.insert(ws);
                }
            }
        }
    }
    if (skip) {
        m_ev_filt_addr++;
        return;
    }

    uint32_t pid = si.pid_len == 4 ? *(const uint32_t*)(ud + si.pid_off) : 0;
    uint64_t bytes = 0;
    if (si.size_len == 4) bytes = *(const uint32_t*)(ud + si.size_off);
    else if (si.size_len == 8) bytes = *(const uint64_t*)(ud + si.size_off);
    else if (si.size_len == 2) bytes = *(const uint16_t*)(ud + si.size_off);

    if (pid == 0 || pid == (uint32_t)-1) pid = rec->EventHeader.ProcessId;
    if (pid == 0 || pid == (uint32_t)-1) return;

    Cum& c = m_cum[pid];
    if (si.dir == 1) c.sent += bytes; else c.recv += bytes;
    c.last_seen = GetTickCount64();
    c.idle_rounds = 0;
    m_ev_kept++;
    m_last_event_tick.store(GetTickCount64(), std::memory_order_release);

    // protocol split (diagnostics)
    bool is_udp = false;
    if (key.p1 == 0xbf3a50c5) is_udp = true;                       // classic UdpIp
    else if (key.p1 == 0x7dd42a49) is_udp = (key.task == 11);      // manifest UDPIP
    else is_udp = (key.opcode == 26 || key.opcode == 27 || key.opcode == 28 || key.opcode == 29);
    if (is_udp) {
        if (si.dir == 1) { m_udp_send_ev++; m_udp_send_b += bytes; }
        else             { m_udp_recv_ev++; m_udp_recv_b += bytes; }
    } else {
        if (si.dir == 1) { m_tcp_send_ev++; m_tcp_send_b += bytes; }
        else             { m_tcp_recv_ev++; m_tcp_recv_b += bytes; }
    }
}

std::vector<ProcTraffic> EtwCapture::GetStats(double interval_sec) {
    std::vector<ProcTraffic> out;
    if (interval_sec < 0.05) interval_sec = 0.05;

    std::lock_guard<std::mutex> lk(m_mutex);
    ULONGLONG now = GetTickCount64();
    for (auto& [pid, c] : m_cum) {
        c.idle_rounds++;

        ProcTraffic pt;
        pt.pid = pid;
        pt.bytes_sent = c.sent;
        pt.bytes_recv = c.recv;
        uint64_t ds = c.sent - c.prev_sent;
        uint64_t dr = c.recv - c.prev_recv;
        c.prev_sent = c.sent;
        c.prev_recv = c.recv;
        // First snapshot: only set the baseline - never report the cumulative
        // bytes as a delta (that produced a huge bogus speed on plugin start).
        if (!c.seen) {
            c.seen = true;
            pt.speed_up = 0;
            pt.speed_down = 0;
        } else {
            pt.speed_up = (double)ds / interval_sec;
            pt.speed_down = (double)dr / interval_sec;
        }
        pt.conn_count = 0;
        pt.name = ProcName(pid);
        pt.exe_path = ProcPath(pid);
        // Keep every process that has ever had traffic - never evict
        // (user request 2026-08-04: drop the idle-removal entirely)
        out.push_back(std::move(pt));
    }
    LogPeriodicLocked();
    (void)now;
    return out;
}

std::wstring EtwCapture::ProcName(DWORD pid) {
    std::lock_guard<std::mutex> lk(m_name_mutex);
    auto it = m_name_cache.find(pid);
    if (it != m_name_cache.end()) return it->second;
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[512]; DWORD n = 512;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            std::wstring full = buf;
            size_t p = full.find_last_of(L"\\/");
            name = (p != std::wstring::npos) ? full.substr(p + 1) : full;
            m_path_cache[pid] = full;
        }
        CloseHandle(h);
    }
    if (name.empty()) {
        // Generic fallback via Toolhelp snapshot: covers System (pid 4),
        // Registry, protected/system processes that have no accessible image
        // path - no hardcoded PID special-casing needed.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }
    if (name.empty()) name = L"<" + std::to_wstring(pid) + L">";
    m_name_cache[pid] = name;
    return name;
}

std::wstring EtwCapture::ProcPath(DWORD pid) {
    std::lock_guard<std::mutex> lk(m_name_mutex);
    auto it = m_path_cache.find(pid);
    if (it != m_path_cache.end()) return it->second;
    return L"";
}

// ------------------------------------------------------------
// Physical-adapter filtering (call with m_mutex held)
// ------------------------------------------------------------

static bool NameHasVirtualKeyword(const std::wstring& lower) {
    static const wchar_t* kw[] = {
        L"tun", L"tap", L"mihomo", L"clash", L"vpn", L"wireguard",
        L"virtual", L"loopback", L"wintun", L"tailscale", L"zerotier", nullptr
    };
    for (int i = 0; kw[i]; i++) {
        if (lower.find(kw[i]) != std::wstring::npos) return true;
    }
    return false;
}

void EtwCapture::RefreshIfaceIpsLocked() {
    m_phys.v4.clear(); m_phys.v6.clear();
    m_virt.v4.clear(); m_virt.v6.clear();

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG bufsize = 0;
    GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &bufsize);
    if (bufsize == 0 || bufsize > 4 * 1024 * 1024) {
        m_phys.refreshed = GetTickCount64();
        LogLine(L"GetAdaptersAddresses size probe failed (buf=%lu)", bufsize);
        return;
    }
    std::vector<BYTE> buf(bufsize);
    auto* head = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, head, &bufsize) != ERROR_SUCCESS) {
        m_phys.refreshed = GetTickCount64();
        LogLine(L"GetAdaptersAddresses failed err=%lu", GetLastError());
        return;
    }
    for (IP_ADAPTER_ADDRESSES* ad = head; ad; ad = ad->Next) {
        if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring nm;
        if (ad->FriendlyName) nm += ad->FriendlyName;
        if (ad->Description) { nm += L" "; nm += ad->Description; }
        for (auto& c : nm) c = (wchar_t)towlower(c);
        bool virt = NameHasVirtualKeyword(nm);
        for (IP_ADAPTER_UNICAST_ADDRESS* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr) continue;
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                uint32_t x;
                memcpy(&x, &((sockaddr_in*)ua->Address.lpSockaddr)->sin_addr, 4);
                (virt ? m_virt.v4 : m_phys.v4).push_back(x);
                if (virt) {
                    // also record the subnet (OnLinkPrefixLength) so TUN-side
                    // fake-ip addresses in the same range get filtered too
                    uint8_t pre = (uint8_t)ua->OnLinkPrefixLength;
                    if (pre == 0 || pre > 32) pre = 32;
                    uint8_t net[4]; memcpy(net, &x, 4);
                    uint32_t full = pre / 8, rem = pre % 8;
                    for (uint32_t i = full; i < 4; i++) net[i] = 0;
                    if (rem) net[full] = (uint8_t)(net[full] & (0xFF << (8 - rem)));
                    uint32_t nv; memcpy(&nv, net, 4);
                    m_virt.subnets_v4.push_back({ nv, pre });
                }
            } else if (ua->Address.lpSockaddr->sa_family == AF_INET6) {
                std::vector<uint8_t> b(16);
                memcpy(b.data(), &((sockaddr_in6*)ua->Address.lpSockaddr)->sin6_addr, 16);
                (virt ? m_virt.v6 : m_phys.v6).push_back(std::move(b));
            }
        }
    }
    m_phys.refreshed = GetTickCount64();
}

void EtwCapture::LogPeriodicLocked() {
    ULONGLONG tnow = GetTickCount64();
    if (tnow - m_last_log_tick < 5000) return;
    m_last_log_tick = tnow;

    wchar_t phys[256] = L"", virt[256] = L"";
    auto fmt4 = [](wchar_t* dst, size_t cap, const std::vector<uint32_t>& v) {
        for (size_t i = 0; i < v.size() && i < 6; i++) {
            const uint8_t* b = (const uint8_t*)&v[i];
            wchar_t t[32];
            swprintf_s(t, 32, i ? L",%u.%u.%u.%u" : L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (wcslen(dst) + wcslen(t) >= cap - 8) break;
            wcscat_s(dst, cap, t);
        }
    };
    fmt4(phys, 256, m_phys.v4);
    fmt4(virt, 256, m_virt.v4);
    if (m_phys.v4.empty()) wcscat_s(phys, 256, L"(none)");
    if (m_virt.v4.empty()) wcscat_s(virt, 256, L"(none)");

    wchar_t sk[512] = L"", sf[512] = L"";
    auto fmt_set = [](wchar_t* dst, size_t cap, const std::set<uint32_t>& s) {
        int n = 0;
        for (auto v : s) {
            if (n >= 8) { if (wcslen(dst) + 4 < cap) wcscat_s(dst, cap, L"..."); break; }
            const uint8_t* b = (const uint8_t*)&v;
            wchar_t t[32];
            swprintf_s(t, 32, n ? L",%u.%u.%u.%u" : L"%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (wcslen(dst) + wcslen(t) >= cap - 8) break;
            wcscat_s(dst, cap, t); n++;
        }
    };
    fmt_set(sk, 512, m_samp_kept);
    fmt_set(sf, 512, m_samp_filt);
    m_samp_kept.clear();
    m_samp_filt.clear();

    wchar_t pids[512] = L"";
    for (auto& [pid, c] : m_cum) {
        wchar_t t[128];
        swprintf_s(t, 128, L"%.96ls(%lu) ", ProcName(pid).c_str(), pid);
        if (wcslen(pids) + wcslen(t) >= 480) break;
        wcscat_s(pids, 512, t);
    }

    // dump resolved shapes every 30s (offsets tell us which layout is real)
    if (tnow - m_last_shape_tick >= 30000) {
        m_last_shape_tick = tnow;
        wchar_t sd[1600] = L"";
        for (auto& [k, si] : m_shapes) {
            if (!si.offsets_ok) continue;
            if (k.p1 != 0x9a280ac0 && k.p1 != 0x7dd42a49) continue;
            if (k.opcode != 10 && k.opcode != 11 && k.opcode != 42 && k.opcode != 43) continue;
            wchar_t t[200];
            swprintf_s(t, 200, L"{p1=%08x op=%u id=%u dir=%d po=%u so=%u do=%u sl=%u dl=%u} ",
                       k.p1, k.opcode, k.id, si.dir, si.pid_off, si.saddr_off,
                       si.daddr_off, si.saddr_len, si.daddr_len);
            if (wcslen(sd) + wcslen(t) >= 1500) break;
            wcscat_s(sd, 1600, t);
        }
        LogLine(L"SHAPES: %s", sd);
    }

    wchar_t wd[1600] = L"";
    for (auto& w : m_samp_weird) {
        const uint8_t* b = (const uint8_t*)&w.addr;
        wchar_t t[200];
        swprintf_s(t, 200, L"{p1=%08x t=%u op=%u id=%u v=%u dir=%d pid=%lu addr=%u.%u.%u.%u} ",
                   w.p1, w.task, w.opcode, w.id, w.version, w.dir, w.pid,
                   b[0], b[1], b[2], b[3]);
        if (wcslen(wd) + wcslen(t) >= 1500) break;
        wcscat_s(wd, 1600, t);
    }
    m_samp_weird.clear();

    LogLine(L"conn=%s total=%llu net=%llu filt=%llu kept=%llu cum=%zu phys=[%s] virt=[%s] keptAddrs=[%s] filtAddrs=[%s] procs=%s weird=%s tcpU=%lluB/%lluev tcpD=%lluB/%lluev udpU=%lluB/%lluev udpD=%lluB/%lluev",
            m_conn_state, m_ev_total, m_ev_net, m_ev_filt_addr, m_ev_kept,
            m_cum.size(), phys, virt, sk, sf, pids, wd,
            m_tcp_send_b, m_tcp_send_ev, m_tcp_recv_b, m_tcp_recv_ev,
            m_udp_send_b, m_udp_send_ev, m_udp_recv_b, m_udp_recv_ev);
    m_tcp_send_ev = 0; m_tcp_send_b = 0;
    m_tcp_recv_ev = 0; m_tcp_recv_b = 0;
    m_udp_send_ev = 0; m_udp_send_b = 0;
    m_udp_recv_ev = 0; m_udp_recv_b = 0;
}

bool EtwCapture::SkipByLocalAddr(const BYTE* ud, USHORT ulen, const ShapeInfo& si) {
    // Does a 4-byte address (network byte order) belong to a virtual/TUN subnet?
    auto in_virt = [this](uint32_t x) -> bool {
        for (auto p : m_virt.v4) if (p == x) return true;
        const uint8_t* a4 = (const uint8_t*)&x;
        for (auto& [nv, pre] : m_virt.subnets_v4) {
            if (pre == 0 || pre >= 32) continue;
            const uint8_t* nb = (const uint8_t*)&nv;
            uint32_t full = pre / 8, rem = pre % 8;
            bool in = true;
            for (uint32_t i = 0; i < full && in; i++) if (a4[i] != nb[i]) in = false;
            if (in && rem) {
                uint8_t mask = (uint8_t)(0xFF << (8 - rem));
                if ((a4[full] & mask) != (nb[full] & mask)) in = false;
            }
            if (in) return true;
        }
        return false;
    };

    // Primary path: BOTH endpoints parsed. Skip when EITHER endpoint is in a
    // virtual/TUN subnet. TUN-side events always have one endpoint in the
    // virtual range; physical-side events never do. This is layout-agnostic:
    // the classic provider's saddr/daddr byte order contradicts its TDH
    // schema, so we cannot trust a single "local" field.
    if (si.saddr_len == 4 && si.daddr_len == 4 &&
        ulen >= (USHORT)(si.saddr_off + 4) && ulen >= (USHORT)(si.daddr_off + 4)) {
        uint32_t a, b;
        memcpy(&a, ud + si.saddr_off, 4);
        memcpy(&b, ud + si.daddr_off, 4);
        const uint8_t* pa = (const uint8_t*)&a;
        const uint8_t* pb = (const uint8_t*)&b;
        if (pa[0] == 127 || pb[0] == 127) return true;   // loopback
        return in_virt(a) || in_virt(b);
    }

    // Fallback: only one endpoint parsed - keep old local-addr semantics.
    uint32_t off, len;
    if (si.dir == 1) { off = si.saddr_off; len = si.saddr_len; }
    else             { off = si.daddr_off; len = si.daddr_len; }
    if (len == 0 || ulen < (USHORT)(off + len)) return false;      // unknown -> keep
    const BYTE* a = ud + off;
    if (len == 4 && a[0] == 127) return true;                      // loopback
    if (len == 16) {
        bool is_loop = true;
        for (int i = 0; i < 15; i++) if (a[i] != 0) { is_loop = false; break; }
        if (is_loop && a[15] == 1) return true;
        return false;   // IPv6 not subnetted yet - keep
    }
    if (len == 4) {
        uint32_t x; memcpy(&x, a, 4);
        return in_virt(x);
    }
    return false;
}
