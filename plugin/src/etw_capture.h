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
#include <set>
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
    // "has live data": true only if we received events within the last 15s.
    // Once the ETW session drops, upstream falls back to legacy capture.
    bool HasData() const {
        ULONGLONG t = m_last_event_tick.load(std::memory_order_acquire);
        return t != 0 && (GetTickCount64() - t) < 15000;
    }
    const wchar_t* GetLastError() const { return m_error; }
    const wchar_t* ConnState() const { return m_conn_state; }  // init/starting/self-started/attached/failed
    const wchar_t* OwnerText() const { return m_owner.empty() ? nullptr : m_owner.c_str(); }

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
        uint32_t daddr_off = 0, daddr_len = 0;   // 0 = unknown
        uint32_t saddr_off = 0, saddr_len = 0;
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
    std::atomic<ULONGLONG> m_last_event_tick{0};   // set on every kept event
    std::thread m_thread;

    // diagnostics counters (guarded by m_mutex)
    uint64_t m_ev_total = 0;       // all events reaching OnEvent with data
    uint64_t m_ev_net = 0;         // classified send/recv with valid offsets
    uint64_t m_ev_filt_addr = 0;   // skipped by virtual-IP blacklist
    uint64_t m_ev_kept = 0;        // actually counted
    ULONGLONG m_last_log_tick = 0;
    ULONGLONG m_last_shape_tick = 0;
    wchar_t m_conn_state[64] = L"init";   // attaching/attached/self/failed
    std::set<uint32_t> m_samp_kept;  // IPv4 local addrs of kept events (since last log)
    std::set<uint32_t> m_samp_filt;  // IPv4 local addrs of filtered events
    // protocol split (diagnostics; guard by m_mutex)
    uint64_t m_tcp_send_ev = 0, m_tcp_send_b = 0;
    uint64_t m_tcp_recv_ev = 0, m_tcp_recv_b = 0;
    uint64_t m_udp_send_ev = 0, m_udp_send_b = 0;
    uint64_t m_udp_recv_ev = 0, m_udp_recv_b = 0;

    // current consumer state (for Stop() unblock)
    std::atomic<TRACEHANDLE> m_cur_consumer{INVALID_PROCESSTRACE_HANDLE};
    std::atomic<bool> m_own_session{false};
    TRACEHANDLE m_own_session_handle = 0;

    std::mutex m_mutex;                       // protects m_cum + shape cache
    std::map<ShapeKey, ShapeInfo> m_shapes;
    std::map<DWORD, Cum> m_cum;

    // Adapter unicast IPs, split physical vs virtual (TUN/TAP/VPN...) by name.
    // Blacklist semantics: only events whose LOCAL address belongs to a virtual
    // adapter are skipped (dedup against the physical-side copy of the same
    // bytes). Unknown addresses are ALWAYS kept - no false positives.
    struct IfaceIps {
        std::vector<uint32_t> v4;                 // network byte order (memcmp-able)
        std::vector<std::vector<uint8_t>> v6;     // 16 bytes each
        std::vector<std::pair<uint32_t, uint8_t>> subnets_v4;  // (network addr, prefix) - virtual adapters only
        ULONGLONG refreshed = 0;
    };
    IfaceIps m_phys;  // guarded by m_mutex
    IfaceIps m_virt;
    void RefreshIfaceIpsLocked();
    bool SkipByLocalAddr(const BYTE* ud, USHORT ulen, const ShapeInfo& si);

    // diagnostics: shapes whose kept local addr looks like a remote/public IP
    struct WeirdShape {
        uint32_t p1; uint16_t task; uint8_t opcode; uint16_t id; uint8_t version;
        int dir; DWORD pid; uint32_t addr;   // addr network byte order
        bool operator<(const WeirdShape& o) const {
            if (p1 != o.p1) return p1 < o.p1;
            if (task != o.task) return task < o.task;
            if (opcode != o.opcode) return opcode < o.opcode;
            if (id != o.id) return id < o.id;
            if (version != o.version) return version < o.version;
            return false;
        }
    };
    std::set<WeirdShape> m_samp_weird;

    void LogLine(const wchar_t* fmt, ...);
    void LogPeriodicLocked();

    // Best-effort: find which process(es) hold ETW session/consumer handles
    std::wstring FindSessionOwners();
    std::wstring m_owner;   // filled when attaching to someone else's session

    std::mutex m_name_mutex;
    std::map<DWORD, std::wstring> m_name_cache;
    std::map<DWORD, std::wstring> m_path_cache;

    wchar_t m_error[256] = L"";
    void SetError(const wchar_t* fmt, ...);
};
