#pragma once
// ============================================================
// Production ETW per-process network byte counter.
// Consumes the classic "NT Kernel Logger" session (attaches if
// already running - e.g. started by AppNetworkCounter - otherwise
// starts its own with EVENT_TRACE_FLAG_NETWORK_TCPIP).
// Parses events via TDH schema (name-based offsets), counts
// TCP+UDP send/recv bytes per process. Works under TUN proxies.
// Same output contract as PacketCapture::GetStats (ProcTraffic).
// ============================================================
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include "capture.h"

class EtwCapture {
public:
    EtwCapture() = default;
    ~EtwCapture();

    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }
    bool HasData() const { return m_got_events; }
    const wchar_t* GetLastError() const { return m_error; }

    // Same contract as PacketCapture::GetStats: per-process speeds.
    // conn_count is left 0 (merge with PacketCapture data upstream).
    std::vector<ProcTraffic> GetStats(double interval_sec);

private:
    struct ShapeKey {
        uint32_t p1;      // provider GUID Data1 (enough to distinguish our 3 providers)
        uint16_t task;
        uint8_t opcode;
        uint16_t id;
        uint8_t version;
        bool operator<(const ShapeKey& o) const {
            if (p1 != o.p1) return p1 < o.p1;
            if (task != o.task) return task < o.task;
            if (opcode != o.opcode) return opcode < o.opcode;
            if (id != o.id) return id < o.id;
            return version < o.version;
        }
    };

    // dir: 0=ignore, 1=send, 2=recv
    struct ShapeInfo {
        int dir = 0;
        bool resolved = false;   // TDH attempted
        bool offsets_ok = false;
        uint32_t pid_off = 0, pid_len = 0;
        uint32_t size_off = 0, size_len = 0;
    };

    struct Cum {
        uint64_t sent = 0;
        uint64_t recv = 0;
        uint64_t prev_sent = 0;
        uint64_t prev_recv = 0;
        ULONGLONG last_seen = 0;
        int idle_rounds = 0;
    };

    void ConsumerLoop();
    static void WINAPI Callback(PEVENT_RECORD rec);
    static EtwCapture* s_self;
    void OnEvent(PEVENT_RECORD rec);
    ShapeInfo& ResolveShape(PEVENT_RECORD rec, const ShapeKey& key);
    std::wstring ProcName(DWORD pid);
    std::wstring ProcPath(DWORD pid);

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_got_events{false};
    std::thread m_thread;

    // current consumer state (for Stop() unblock)
    std::atomic<TRACEHANDLE> m_cur_consumer{INVALID_PROCESSTRACE_HANDLE};
    std::atomic<bool> m_own_session{false};
    TRACEHANDLE m_own_session_handle = 0;

    std::mutex m_mutex;                       // protects m_cum + shape cache
    std::map<ShapeKey, ShapeInfo> m_shapes;
    std::map<DWORD, Cum> m_cum;

    std::mutex m_name_mutex;
    std::map<DWORD, std::wstring> m_name_cache;
    std::map<DWORD, std::wstring> m_path_cache;

    wchar_t m_error[256] = L"";
    void SetError(const wchar_t* fmt, ...);
};
