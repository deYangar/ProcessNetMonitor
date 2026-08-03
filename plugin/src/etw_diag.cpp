// ============================================================
// ETW diagnostic consumer - see etw_diag.h
// Goal: find out why manual MofData+4 "size" reads return 0 on
// Win11 25H2 while AppNetworkCounter gets real byte counts.
// Strategy: consume classic NT Kernel Logger session AND a
// private Microsoft-Windows-Kernel-Network manifest session;
// parse via TDH schema (name-based) and compare with manual
// DWORD reads; dump raw bytes for offline analysis.
// ============================================================
#include "etw_diag.h"
#include <tdh.h>
#include <evntprov.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "tdh.lib")

// Classic NT Kernel Logger provider (Windows Kernel Trace)
static const GUID GUID_ClassicKernelNet =
    { 0x9E814AAD, 0x3204, 0x11D2, { 0x9A, 0x82, 0x00, 0x60, 0x08, 0xA8, 0x69, 0x39 } };
// Microsoft-Windows-Kernel-Network manifest provider
static const GUID GUID_ManifestKernelNet =
    { 0x7DD42A49, 0x5329, 0x4832, { 0x8D, 0xFD, 0x43, 0xD9, 0x79, 0x15, 0x3A, 0x88 } };
// Keywords for manifest provider: IPv4 | IPv6
static const ULONGLONG KEYWORD_IPV4 = 0x10;
static const ULONGLONG KEYWORD_IPV6 = 0x20;

EtwDiag* EtwDiag::s_self = nullptr;

static void GuidToStr(const GUID& g, char* out, int n) {
    snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static std::string W2A(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

EtwDiag::~EtwDiag() { Stop(); }

bool EtwDiag::Start() {
    if (m_running) return true;
    s_self = this;
    m_stop = false;
    try { m_thread = std::thread([this] { ThreadMain(); }); }
    catch (...) { return false; }
    return true;
}

void EtwDiag::Stop() {
    if (!m_running && !m_thread.joinable()) return;
    m_stop = true;

    EVENT_TRACE_PROPERTIES stopProps;
    memset(&stopProps, 0, sizeof(stopProps));
    stopProps.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);

    // Stop our own sessions; NEVER stop a session we merely attached to.
    if (m_started_b && m_session_b) {
        ControlTraceW(m_session_b, nullptr, &stopProps, EVENT_TRACE_CONTROL_STOP);
    }
    if (m_session_a) {
        if (m_started_a) {
            ControlTraceW(m_session_a, nullptr, &stopProps, EVENT_TRACE_CONTROL_STOP);
        } else {
            if (m_consumer_a != INVALID_PROCESSTRACE_HANDLE) CloseTrace(m_consumer_a);
        }
    }
    if (m_thread.joinable()) {
        // Give ProcessTrace up to 3s to unwind (called under loader lock at exit)
        for (int i = 0; i < 30 && m_running; i++) Sleep(100);
        if (m_thread.joinable()) {
            try { m_thread.join(); } catch (...) { m_thread.detach(); }
        }
    }
    m_running = false;
}

void EtwDiag::ThreadMain() {
    m_running = true;
    m_start_tick = GetTickCount64();

    // ---- open logs ----
    std::wstring dir = m_log_dir;
    if (dir.empty()) dir = L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    _wfopen_s(&m_log, (dir + L"\\etw_diag.log").c_str(), L"a");
    _wfopen_s(&m_stats, (dir + L"\\etw_diag_stats.log").c_str(), L"a");
    if (!m_log || !m_stats) {
        if (m_log) fclose(m_log);
        if (m_stats) fclose(m_stats);
        m_log = m_stats = nullptr;
        m_running = false;
        return;
    }
    setvbuf(m_log, nullptr, _IOFBF, 64 * 1024);
    setvbuf(m_stats, nullptr, _IOFBF, 64 * 1024);

    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(m_log, "\n================ ETW DIAG START %04d-%02d-%02d %02d:%02d:%02d ================\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    LogHeaderInfo();

    // ---- Session A: classic NT Kernel Logger (attach preferred) ----
    EVENT_TRACE_LOGFILEW lf;
    memset(&lf, 0, sizeof(lf));
    lf.LoggerName = const_cast<LPWSTR>(KERNEL_LOGGER_NAMEW);
    lf.ProcessTraceMode = EVENT_TRACE_REAL_TIME_MODE | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = &EtwDiag::StaticCallback;

    m_consumer_a = OpenTraceW(&lf);
    if (m_consumer_a != INVALID_PROCESSTRACE_HANDLE) {
        fprintf(m_log, "[A] OpenTrace(NT Kernel Logger) ATTACHED ok, handle=%llu\n",
            (unsigned long long)m_consumer_a);
        m_started_a = false;
    } else {
        DWORD err = GetLastError();
        fprintf(m_log, "[A] OpenTrace(NT Kernel Logger) failed err=%lu -> trying StartTrace\n", err);

        // start our own kernel logger session
        DWORD nameBytes = (DWORD)((wcslen(KERNEL_LOGGER_NAMEW) + 1) * sizeof(wchar_t));
        DWORD bufSize = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes;
        std::vector<BYTE> buf(bufSize, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        props->Wnode.BufferSize = bufSize;
        props->Wnode.ClientContext = 1; // QPC timestamps
        props->BufferSize = 256;        // KB
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 120;
        props->FlushTimer = 1;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->EnableFlags = EVENT_TRACE_FLAG_NETWORK_TCPIP;

        ULONG sr = StartTraceW(&m_session_a, KERNEL_LOGGER_NAMEW, props);
        if (sr == ERROR_SUCCESS) {
            m_started_a = true;
            fprintf(m_log, "[A] StartTrace(NT Kernel Logger) ok, session=%llu EnableFlags=0x%lX\n",
                (unsigned long long)m_session_a, (unsigned long)EVENT_TRACE_FLAG_NETWORK_TCPIP);
            m_consumer_a = OpenTraceW(&lf);
            if (m_consumer_a == INVALID_PROCESSTRACE_HANDLE)
                fprintf(m_log, "[A] OpenTrace after StartTrace failed err=%lu\n", GetLastError());
        } else {
            fprintf(m_log, "[A] StartTrace(NT Kernel Logger) FAILED status=%lu (5=access denied, 183=already exists)\n", sr);
        }
    }

    // ---- Session B: private session, manifest provider ----
    const wchar_t* SES_B = L"PnmKernelNetDiag";
    {
        DWORD nameBytes = (DWORD)((wcslen(SES_B) + 1) * sizeof(wchar_t));
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

        ULONG sr = StartTraceW(&m_session_b, SES_B, props);
        if (sr == ERROR_ALREADY_EXISTS) {
            // stale session from a crashed run: stop it and retry once
            EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
            sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
            ControlTraceW((TRACEHANDLE)0, SES_B, &sp, EVENT_TRACE_CONTROL_STOP);
            Sleep(300);
            memset(buf.data(), 0, bufSize);
            props->Wnode.BufferSize = bufSize;
            props->Wnode.ClientContext = 1;
            props->BufferSize = 256;
            props->MinimumBuffers = 4;
            props->MaximumBuffers = 120;
            props->FlushTimer = 1;
            props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
            sr = StartTraceW(&m_session_b, SES_B, props);
        }
        if (sr != ERROR_SUCCESS) {
            fprintf(m_log, "[B] StartTrace(PnmKernelNetDiag) FAILED status=%lu\n", sr);
        } else {
            m_started_b = true;
            ULONG er = EnableTraceEx2(m_session_b, &GUID_ManifestKernelNet,
                EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                KEYWORD_IPV4 | KEYWORD_IPV6, 0, 0, nullptr);
            fprintf(m_log, "[B] StartTrace ok session=%llu, EnableTraceEx2(Kernel-Network kw=0x30) status=%lu\n",
                (unsigned long long)m_session_b, er);

            EVENT_TRACE_LOGFILEW lfb;
            memset(&lfb, 0, sizeof(lfb));
            lfb.LoggerName = const_cast<LPWSTR>(SES_B);
            lfb.ProcessTraceMode = EVENT_TRACE_REAL_TIME_MODE | PROCESS_TRACE_MODE_EVENT_RECORD;
            lfb.EventRecordCallback = &EtwDiag::StaticCallback;
            m_consumer_b = OpenTraceW(&lfb);
            if (m_consumer_b == INVALID_PROCESSTRACE_HANDLE)
                fprintf(m_log, "[B] OpenTrace(PnmKernelNetDiag) failed err=%lu\n", GetLastError());
        }
    }

    fflush(m_log);

    m_last_flush_tick = GetTickCount64();
    bool any = false;
    if (m_consumer_a != INVALID_PROCESSTRACE_HANDLE) {
        m_workers.emplace_back([this] { ConsumeOne(m_consumer_a, "A"); });
        any = true;
    }
    if (m_consumer_b != INVALID_PROCESSTRACE_HANDLE) {
        m_workers.emplace_back([this] { ConsumeOne(m_consumer_b, "B"); });
        any = true;
    }
    if (!any) {
        fprintf(m_log, "[!] no consumer handles - nothing to do (likely missing admin rights)\n");
    } else {
        fprintf(m_log, "[*] consumer threads started\n");
        fflush(m_log);
        // wait until Stop() is called or all consumers exit on their own
        while (!m_stop && m_workers_done < (int)m_workers.size()) Sleep(500);
        if (m_workers_done < (int)m_workers.size())
            fprintf(m_log, "[*] stop requested, waiting for consumers...\n");
    }

    { std::lock_guard<std::mutex> lk(m_mutex); FlushStats(true); }

    for (auto& t : m_workers) if (t.joinable()) t.join();
    m_workers.clear();

    if (m_consumer_a != INVALID_PROCESSTRACE_HANDLE) CloseTrace(m_consumer_a);
    if (m_consumer_b != INVALID_PROCESSTRACE_HANDLE) CloseTrace(m_consumer_b);

    EVENT_TRACE_PROPERTIES sp; memset(&sp, 0, sizeof(sp));
    sp.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
    if (m_started_b && m_session_b) ControlTraceW(m_session_b, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);
    if (m_started_a && m_session_a) ControlTraceW(m_session_a, nullptr, &sp, EVENT_TRACE_CONTROL_STOP);

    fprintf(m_log, "================ ETW DIAG END ================\n");
    if (m_log) { fclose(m_log); m_log = nullptr; }
    if (m_stats) { fclose(m_stats); m_stats = nullptr; }
    m_running = false;
}

void EtwDiag::ConsumeOne(TRACEHANDLE h, const char* name) {
    ULONG pr = ProcessTrace(&h, 1, nullptr, nullptr);
    m_workers_done++;
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_log) {
        fprintf(m_log, "[*] ProcessTrace[%s] returned %lu\n", name, pr);
        fflush(m_log);
    }
}

void EtwDiag::LogHeaderInfo() {
    OSVERSIONINFOEXW os; memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    auto rtl = (RtlGetVersionPtr)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl) rtl((PRTL_OSVERSIONINFOW)&os);
    fprintf(m_log, "OS build: %lu.%lu.%lu  ptrsize=%zu\n",
        os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, sizeof(void*));
}

void EtwDiag::StaticCallback(PEVENT_RECORD rec) {
    if (s_self) s_self->OnEvent(rec);
}

static uint32_t IntypeSize(uint16_t intype) {
    uint16_t base = intype & 0x0FFF;
    if (intype & 0x1000) return (uint32_t)sizeof(void*); // pointer modifier
    switch (base) {
    case 3: case 4: case 13: return 1;                 // int8/uint8/boolean
    case 5: case 6: return 2;                          // int16/uint16
    case 7: case 8: case 11: case 21: case 22: return 4; // int32/uint32/float/hexint32/countedstring-approx
    case 9: case 10: case 12: case 16: return 8;       // int64/uint64/double/pointer(base)
    case 17: return 8;                                 // filetime
    default: return 0;                                 // unknown/variable
    }
}

void EtwDiag::EnsureSchema(PEVENT_RECORD rec, ShapeInfo& si) {
    if (si.schema_logged) return;
    si.schema_logged = true;

    char pg[64];
    GuidToStr(rec->EventHeader.ProviderId, pg, sizeof(pg));
    fprintf(m_log, "SHAPE tag=%d task=%u op=%u id=%u ver=%u provider=%s flags=0x%X eventprop=0x%X hpid=%lu\n",
        si.key.tag, si.key.task, si.key.opcode, si.key.id, si.key.version, pg,
        rec->EventHeader.Flags, rec->EventHeader.EventProperty,
        (unsigned long)rec->EventHeader.ProcessId);

    // TDH schema lookup
    ULONG need = 0;
    TRACE_EVENT_INFO* info = nullptr;
    ULONG st = TdhGetEventInformation(rec, 0, nullptr, nullptr, &need);
    if (st == ERROR_INSUFFICIENT_BUFFER && need > 0 && need < 1024 * 1024) {
        info = (TRACE_EVENT_INFO*)malloc(need);
        if (info) st = TdhGetEventInformation(rec, 0, nullptr, info, &need);
    }
    si.tdh_status = st;
    if (st != ERROR_SUCCESS || !info) {
        si.tdh_ok = false;
        fprintf(m_log, "  TDH: FAILED status=%lu (1168=not found)\n", st);
        if (info) free(info);
        return;
    }
    si.tdh_ok = true;
    fprintf(m_log, "  TDH: ok, decoding=%u props=%u taskNameOff=%u opcodeNameOff=%u\n",
        (unsigned)info->DecodingSource, (unsigned)info->TopLevelPropertyCount,
        (unsigned)info->TaskNameOffset, (unsigned)info->OpcodeNameOffset);
    const wchar_t* taskName = info->TaskNameOffset ? (const wchar_t*)((BYTE*)info + info->TaskNameOffset) : L"";
    const wchar_t* opName = info->OpcodeNameOffset ? (const wchar_t*)((BYTE*)info + info->OpcodeNameOffset) : L"";
    fprintf(m_log, "  TDH names: task='%s' opcode='%s'\n", W2A(taskName).c_str(), W2A(opName).c_str());

    uint32_t off = 0;
    bool ok = true;
    for (ULONG i = 0; i < info->TopLevelPropertyCount; i++) {
        auto& p = info->EventPropertyInfoArray[i];
        PropInfo pi;
        pi.name = p.NameOffset ? (const wchar_t*)((BYTE*)info + p.NameOffset) : L"";
        pi.intype = p.nonStructType.InType;
        pi.size = IntypeSize(pi.intype);
        pi.offset = off;
        if (pi.size == 0) ok = false;
        else off += pi.size;
        si.props.push_back(pi);

        fprintf(m_log, "  prop[%u] '%s' intype=%u size=%u offset=%u\n",
            (unsigned)i, W2A(pi.name).c_str(), pi.intype, pi.size, pi.offset);

        // match PID / size by name
        const std::wstring& nm = pi.name;
        if (si.pid_idx < 0 && (nm == L"PID" || nm == L"Pid" || nm == L"ProcessId")) {
            si.pid_idx = (int)i; si.pid_off = pi.offset; si.pid_len = pi.size;
        }
        if (si.size_idx < 0 && (nm == L"size" || nm == L"Size")) {
            si.size_idx = (int)i; si.size_off = pi.offset; si.size_len = pi.size;
        }
    }
    si.offsets_valid = ok;
    fprintf(m_log, "  offsets_valid=%d pid_idx=%d size_idx=%d\n",
        si.offsets_valid ? 1 : 0, si.pid_idx, si.size_idx);
    free(info);
}

void EtwDiag::OnEvent(PEVENT_RECORD rec) {
    if (m_stop) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_total_events++;

    ShapeKey key;
    const GUID& pg = rec->EventHeader.ProviderId;
    if (pg.Data1 == GUID_ClassicKernelNet.Data1 && pg.Data2 == GUID_ClassicKernelNet.Data2 &&
        pg.Data3 == GUID_ClassicKernelNet.Data3 &&
        memcmp(pg.Data4, GUID_ClassicKernelNet.Data4, 8) == 0) key.tag = 0;
    else if (pg.Data1 == GUID_ManifestKernelNet.Data1 && pg.Data2 == GUID_ManifestKernelNet.Data2 &&
        pg.Data3 == GUID_ManifestKernelNet.Data3 &&
        memcmp(pg.Data4, GUID_ManifestKernelNet.Data4, 8) == 0) key.tag = 1;
    else key.tag = 2;

    key.task = rec->EventHeader.EventDescriptor.Task;
    key.opcode = rec->EventHeader.EventDescriptor.Opcode;
    key.id = rec->EventHeader.EventDescriptor.Id;
    key.version = rec->EventHeader.EventDescriptor.Version;

    // Other providers: just count, don't analyze (shouldn't happen on our sessions)
    if (key.tag == 2) {
        // periodic flush still runs below
    }

    ShapeInfo& si = m_shapes[key];
    si.key = key;
    EnsureSchema(rec, si);

    si.count++;
    USHORT ulen = rec->UserDataLength;
    if (ulen < si.ulen_min) si.ulen_min = ulen;
    if (ulen > si.ulen_max) si.ulen_max = ulen;
    if (rec->EventHeader.ProcessId != 0 && rec->EventHeader.ProcessId != (DWORD)-1) si.hdr_pid_valid++;

    const BYTE* ud = (const BYTE*)rec->UserData;

    // manual DWORD reads
    uint32_t dw0 = (ulen >= 4 && ud) ? *(const uint32_t*)(ud + 0) : 0;
    uint32_t dw1 = (ulen >= 8 && ud) ? *(const uint32_t*)(ud + 4) : 0;
    si.manual_dw1_sum += dw1;
    if (dw1 == 0) si.dw1_zero++;
    if (dw1 > si.size_max_dw1) si.size_max_dw1 = dw1;

    // offset scan (first 16 DWORDs)
    for (int k = 0; k < 16; k++) {
        if (ulen >= (USHORT)((k + 1) * 4)) {
            uint32_t v = *(const uint32_t*)(ud + k * 4);
            if (v != 0) si.off_nonzero[k]++;
            if (v > si.off_max[k]) si.off_max[k] = v;
        }
    }

    // TDH-based read using cached offsets
    uint64_t tsize = 0;
    uint32_t tpid = 0;
    bool have_tsize = false;
    if (si.offsets_valid && si.size_idx >= 0 && ulen >= si.size_off + si.size_len) {
        if (si.size_len == 4) tsize = *(const uint32_t*)(ud + si.size_off);
        else if (si.size_len == 8) tsize = *(const uint64_t*)(ud + si.size_off);
        else if (si.size_len == 2) tsize = *(const uint16_t*)(ud + si.size_off);
        have_tsize = true;
    }
    if (si.offsets_valid && si.pid_idx >= 0 && ulen >= si.pid_off + si.pid_len) {
        if (si.pid_len == 4) tpid = *(const uint32_t*)(ud + si.pid_off);
    }
    if (have_tsize) {
        si.tdh_size_sum += tsize;
        if (tsize == 0) si.tdh_zero++;
        if (tsize > si.size_max_tdh) si.size_max_tdh = tsize;
        if (tsize != dw1) si.mismatch++;
    }

    // per-PID aggregation
    DWORD pid = tpid ? tpid : dw0;
    if (pid == 0 || pid == (DWORD)-1) pid = rec->EventHeader.ProcessId;
    if (pid != 0 && pid != (DWORD)-1) {
        PidAgg& pa = m_pids[pid];
        pa.tdh_bytes += have_tsize ? tsize : 0;
        pa.dw1_bytes += dw1;
        pa.events++;
        pa.last_seen = GetTickCount64();
    }

    // raw dumps: first N per shape + big packets
    if (key.tag != 2 && m_log) {
        bool dump = false;
        const char* why = "";
        if (si.dumps_done < MAX_SHAPE_DUMPS) { dump = true; why = "shape_head"; si.dumps_done++; }
        else if (ulen >= 500 && m_big_dumps < MAX_BIG_DUMPS) { dump = true; why = "big"; m_big_dumps++; }
        if (dump) DumpEvent(rec, si, why);
    }

    // periodic stats
    uint64_t now = GetTickCount64();
    if (now - m_last_flush_tick >= 5000) FlushStats(false);
}

void EtwDiag::DumpEvent(PEVENT_RECORD rec, ShapeInfo& si, const char* why) {
    USHORT ulen = rec->UserDataLength;
    const BYTE* ud = (const BYTE*)rec->UserData;
    fprintf(m_log, "EVT[%s] tag=%d task=%u op=%u id=%u v=%u hpid=%lu ulen=%u",
        why, si.key.tag, si.key.task, si.key.opcode, si.key.id, si.key.version,
        (unsigned long)rec->EventHeader.ProcessId, ulen);
    if (ulen >= 4 && ud) fprintf(m_log, " dw0=%u", *(const uint32_t*)ud);
    if (ulen >= 8 && ud) fprintf(m_log, " dw1=%u", *(const uint32_t*)(ud + 4));
    if (si.offsets_valid && si.size_idx >= 0 && ulen >= si.size_off + si.size_len) {
        uint64_t v = si.size_len == 4 ? *(const uint32_t*)(ud + si.size_off)
            : si.size_len == 8 ? *(const uint64_t*)(ud + si.size_off)
            : *(const uint16_t*)(ud + si.size_off);
        fprintf(m_log, " tdh_size=%llu", (unsigned long long)v);
    }
    int n = ulen < 96 ? ulen : 96;
    fprintf(m_log, " hex=");
    for (int i = 0; i < n; i++) fprintf(m_log, "%02X", ud[i]);
    fprintf(m_log, "\n");
}

void EtwDiag::FlushStats(bool final) {
    if (!m_stats) return;
    m_last_flush_tick = GetTickCount64();
    double elapsed = (double)(m_last_flush_tick - m_start_tick) / 1000.0;
    fprintf(m_stats, "=== elapsed=%.1fs%s total_events=%llu shapes=%zu pids=%zu ===\n",
        elapsed, final ? " [FINAL]" : "", (unsigned long long)m_total_events,
        m_shapes.size(), m_pids.size());

    for (auto& [k, s] : m_shapes) {
        if (s.count == 0) continue;
        fprintf(m_stats, "SHAPE tag=%d task=%u op=%u id=%u v=%u n=%llu ulen=%llu..%llu tdh=%s(st=%lu)%s\n",
            k.tag, k.task, k.opcode, k.id, k.version,
            (unsigned long long)s.count,
            (unsigned long long)(s.ulen_min == UINT64_MAX ? 0 : s.ulen_min),
            (unsigned long long)s.ulen_max,
            s.tdh_ok ? "OK" : "NO", s.tdh_status,
            s.offsets_valid ? " offs_ok" : " offs_var");
        if (s.tdh_ok && s.size_idx >= 0) {
            fprintf(m_stats, "  size: tdh_sum=%llu max=%llu zero=%llu | dw1_sum=%llu max=%llu zero=%llu | mismatch=%llu | size_off=%u len=%u pid_off=%u\n",
                (unsigned long long)s.tdh_size_sum, (unsigned long long)s.size_max_tdh, (unsigned long long)s.tdh_zero,
                (unsigned long long)s.manual_dw1_sum, (unsigned long long)s.size_max_dw1, (unsigned long long)s.dw1_zero,
                (unsigned long long)s.mismatch, s.size_off, s.size_len, s.pid_off);
        } else {
            fprintf(m_stats, "  dw1_sum=%llu max=%llu zero=%llu (no tdh size)\n",
                (unsigned long long)s.manual_dw1_sum, (unsigned long long)s.size_max_dw1, (unsigned long long)s.dw1_zero);
        }
        fprintf(m_stats, "  hdr_pid_valid=%llu/%llu\n", (unsigned long long)s.hdr_pid_valid, (unsigned long long)s.count);
        fprintf(m_stats, "  offscan nz:");
        for (int i = 0; i < 16; i++) fprintf(m_stats, " [%d]%llu/%llu", i,
            (unsigned long long)s.off_nonzero[i], (unsigned long long)s.off_max[i]);
        fprintf(m_stats, "\n");
    }

    // top PIDs by tdh bytes
    std::vector<std::pair<DWORD, PidAgg>> v(m_pids.begin(), m_pids.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.tdh_bytes > b.second.tdh_bytes; });
    int shown = 0;
    for (auto& [pid, pa] : v) {
        if (shown >= 25) break;
        if (pa.tdh_bytes == 0 && pa.dw1_bytes == 0) continue;
        fprintf(m_stats, "PID %lu (%s) tdh=%llu dw1=%llu ev=%llu\n",
            (unsigned long)pid, W2A(ProcName(pid)).c_str(),
            (unsigned long long)pa.tdh_bytes, (unsigned long long)pa.dw1_bytes,
            (unsigned long long)pa.events);
        shown++;
    }
    fflush(m_stats);
    if (m_log) fflush(m_log);
}

std::wstring EtwDiag::ProcName(DWORD pid) {
    auto it = m_name_cache.find(pid);
    if (it != m_name_cache.end()) return it->second;
    std::wstring name = L"<" + std::to_wstring(pid) + L">";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[512]; DWORD n = 512;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            name = buf;
            size_t p = name.find_last_of(L"\\/");
            if (p != std::wstring::npos) name = name.substr(p + 1);
        }
        CloseHandle(h);
    }
    m_name_cache[pid] = name;
    return name;
}
