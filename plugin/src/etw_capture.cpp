// ============================================================
// Production ETW per-process network byte counter - etw_capture.h
// Proven on Win11 25H2 (build 26200) + mihomo TUN: classic
// kernel network events DO carry per-process byte sizes; parse
// via TDH schema for robustness.
// ============================================================
#include "etw_capture.h"
#include <tdh.h>
#include <evntprov.h>
#include <algorithm>
#include <cstdarg>
#include <cwchar>

#pragma comment(lib, "tdh.lib")

EtwCapture* EtwCapture::s_self = nullptr;

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
    m_got_events = false;
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
                consumer = OpenTraceW(&lf);
                if (consumer == INVALID_PROCESSTRACE_HANDLE) {
                    EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
                    sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
                    ControlTraceW(session, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);
                    started_own = false;
                    SetError(L"ETW OpenTrace failed err=%lu", GetLastError());
                }
            } else {
                SetError(L"ETW attach+start failed: Open err=%lu Start=%lu (need admin?)", openErr, sr);
            }
        }

        if (consumer == INVALID_PROCESSTRACE_HANDLE) {
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

        uint32_t off = 0; bool ok = true;
        int pid_idx = -1, size_idx = -1;
        for (ULONG i = 0; i < info->TopLevelPropertyCount; i++) {
            auto& p = info->EventPropertyInfoArray[i];
            uint32_t psz = IntypeSize2(p.nonStructType.InType);
            if (psz == 0) { ok = false; break; }
            const wchar_t* nm = p.NameOffset ? (const wchar_t*)((BYTE*)info + p.NameOffset) : L"";
            if (pid_idx < 0 && (wcscmp(nm, L"PID") == 0 || wcscmp(nm, L"Pid") == 0 || wcscmp(nm, L"ProcessId") == 0)) {
                pid_idx = (int)i; si.pid_off = off; si.pid_len = psz;
            }
            if (size_idx < 0 && (_wcsicmp(nm, L"size") == 0)) {
                size_idx = (int)i; si.size_off = off; si.size_len = psz;
            }
            off += psz;
        }
        si.offsets_ok = ok && pid_idx >= 0 && size_idx >= 0;
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
    ShapeInfo& si = ResolveShape(rec, key);
    if (si.dir == 0 || !si.offsets_ok) return;
    if (ulen < si.pid_off + si.pid_len || ulen < si.size_off + si.size_len) return;

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
    m_got_events = true;
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
        if (pt.speed_up > 0.001 || pt.speed_down > 0.001 || c.idle_rounds <= 2)
            out.push_back(std::move(pt));
        ++it;
    }
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
