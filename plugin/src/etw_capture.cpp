// ============================================================
// Production ETW per-process network byte counter - etw_capture.h
// Proven on Win11 25H2 (build 26200) + mihomo TUN: classic
// kernel network events DO carry per-process byte sizes; parse
// via TDH schema for robustness.
// ============================================================
#include "etw_capture.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <tdh.h>
#include <evntprov.h>
#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <share.h>

#pragma comment(lib, "tdh.lib")

EtwCapture* EtwCapture::s_self = nullptr;

// ---- diagnostics log (shared read: TM can keep running) ----
static FILE* EtwLogOpen() {
    wchar_t path[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH)) return nullptr;
    wcscat_s(path, L"\\TrafficMonitor\\plugins\\ProcessNetMonitor\\etw_capture.log");
    FILE* f = _wfsopen(path, L"a", _SH_DENYNO);
    return f;
}

void EtwCapture::LogLine(const wchar_t* fmt, ...) {
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
    case 7: case 8: case 11: case 21: case 22: return 4;
    case 9: case 10: case 12: case 16: return 8;
    case 17: return 8;
    default: return 0;
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

        // 1) attach to an already-running NT Kernel Logger (e.g. AppNetworkCounter's)
        consumer = OpenTraceW(&lf);
        if (consumer == INVALID_PROCESSTRACE_HANDLE) {
            DWORD openErr = ::GetLastError();
            // 2) start our own
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
                LogLine(L"StartTrace OK (own session)");
                consumer = OpenTraceW(&lf);
                if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                    EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
                    sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
                    ControlTraceW(session, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);
                    started_own = false;
                    SetError(L"ETW OpenTrace failed err=%lu", GetLastError());
                    LogLine(L"OpenTrace after StartTrace FAILED err=%lu", GetLastError());
                }
            } else {
                SetError(L"ETW attach+start failed: Open err=%lu Start=%lu (need admin?)", openErr, sr);
                LogLine(L"attach err=%lu, StartTrace err=%lu", openErr, sr);
            }
        } else {
            wcscpy_s(m_conn_state, L"attached");
            LogLine(L"attached to existing NT Kernel Logger");
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
    if (SkipByLocalAddr(ud, ulen, si)) {
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
}

std::vector<ProcTraffic> EtwCapture::GetStats(double interval_sec) {
    std::vector<ProcTraffic> out;
    if (interval_sec < 0.05) interval_sec = 0.05;

    std::lock_guard<std::mutex> lk(m_mutex);
    ULONGLONG now = GetTickCount64();
    for (auto it = m_cum.begin(); it != m_cum.end(); ) {
        Cum& c = it->second;
        c.idle_rounds++;
        if (c.idle_rounds > 60) { it = m_cum.erase(it); continue; }  // ~1min idle at 1s poll

        ProcTraffic pt;
        pt.pid = it->first;
        pt.bytes_sent = c.sent;
        pt.bytes_recv = c.recv;
        uint64_t ds = c.sent - c.prev_sent;
        uint64_t dr = c.recv - c.prev_recv;
        c.prev_sent = c.sent;
        c.prev_recv = c.recv;
        pt.speed_up = (double)ds / interval_sec;
        pt.speed_down = (double)dr / interval_sec;
        pt.conn_count = 0;
        pt.name = ProcName(it->first);
        pt.exe_path = ProcPath(it->first);
        if (pt.speed_up > 0.001 || pt.speed_down > 0.001 || c.idle_rounds <= 8)
            out.push_back(std::move(pt));
        ++it;
    }
    LogPeriodicLocked();
    (void)now;
    return out;
}

std::wstring EtwCapture::ProcName(DWORD pid) {
    std::lock_guard<std::mutex> lk(m_name_mutex);
    auto it = m_name_cache.find(pid);
    if (it != m_name_cache.end()) return it->second;
    std::wstring name = L"<" + std::to_wstring(pid) + L">";
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
            wcscat_s(dst, cap, t);
        }
    };
    fmt4(phys, 256, m_phys.v4);
    fmt4(virt, 256, m_virt.v4);
    if (m_phys.v4.empty()) wcscat_s(phys, 256, L"(none)");
    if (m_virt.v4.empty()) wcscat_s(virt, 256, L"(none)");

    LogLine(L"conn=%s total=%llu net=%llu filt=%llu kept=%llu cum=%zu phys=[%s] virt=[%s]",
            m_conn_state, m_ev_total, m_ev_net, m_ev_filt_addr, m_ev_kept,
            m_cum.size(), phys, virt);
}

bool EtwCapture::SkipByLocalAddr(const BYTE* ud, USHORT ulen, const ShapeInfo& si) {
    uint32_t off, len;
    if (si.dir == 1) { off = si.saddr_off; len = si.saddr_len; }   // send: local = saddr
    else             { off = si.daddr_off; len = si.daddr_len; }   // recv: local = daddr
    if (len == 0 || ulen < (USHORT)(off + len)) return false;      // unknown -> keep
    const BYTE* a = ud + off;

    // Loopback: 127.0.0.0/8 and ::1
    if (len == 4 && a[0] == 127) return true;
    if (len == 16) {
        bool is_loop = true;
        for (int i = 0; i < 15; i++) if (a[i] != 0) { is_loop = false; break; }
        if (is_loop && a[15] == 1) return true;
    }

    // Virtual-adapter blacklist: skip only known virtual/TUN local addresses.
    // Everything else (physical, 0.0.0.0, unrecognized) is kept.
    if (len == 4) {
        uint32_t x; memcpy(&x, a, 4);
        for (auto p : m_virt.v4) if (p == x) return true;
        return false;
    }
    if (len == 16) {
        for (auto& v : m_virt.v6) {
            if (v.size() == 16 && memcmp(v.data(), a, 16) == 0) return true;
        }
        return false;
    }
    return false;
}
