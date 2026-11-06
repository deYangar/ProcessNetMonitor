#pragma once
// ============================================================
// Production ETW per-process network byte counter.
// L1 (Win8+): private system logger session "PnmNetTrace"
//   (EVENT_TRACE_SYSTEM_LOGGER_MODE) - coexists with other tools'
// NT Kernel Logger usage. L2 (Win7, or L1 failure): the classic
// NT Kernel Logger, only when FREE (never stop/attach a foreign
// session). L3: legacy EStats/raw-socket capture upstream.
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
    // Thread-safe snapshots: the consumer thread writes these while the
    // TM/timer threads read them (unsynchronized wchar_t array access across
    // threads was a torn-read / heap-corruption risk).
    void GetConnState(wchar_t* out, size_t cap) const;

    // Same contract as PacketCapture::GetStats: per-process speeds.
    // conn_count is left 0 (merge with PacketCapture data upstream).
    std::vector<ProcTraffic> GetStats(double interval_sec);

    // Debug-log master switch (default OFF, user setting in settings.json /
    // detail-window context menu). Controls etw_capture.log / capture.log
    // generation. Crash diagnostics (crash.log / crash.dmp / werdumps) are
    // ALWAYS enabled so crashes stay diagnosable even with logging off.
    static void SetDebugLogs(bool on) { s_debug_logs.store(on); }
    static bool DebugLogs() { return s_debug_logs.load(); }
    static std::atomic<bool> s_debug_logs;

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
        bool seen = false;      // first GetStats snapshot only sets the baseline
        std::wstring name;      // resolved process name - kept after process exits
        // PID 复用防护（2026-11-06）：进程身份 = PID + 创建时间。
        // owner_ct = 当前主人的进程创建时间（FILETIME 转 ULONGLONG，0=未知，
        // 首次核对时记录）。GetStats 周期比对：不一致 = PID 已换主，条目重置。
        ULONGLONG owner_ct = 0;
        ULONGLONG ct_check_tick = 0;  // 上次 CreationTime 核对时间（节流用）
    };

    void ConsumerLoop();
    // One layer-start attempt. consumer valid + own=true: we started and
    // own the session (Stop() must ControlTrace-stop it). All invalid:
    // caller falls to the next layer.
    struct SessionResult {
        TRACEHANDLE consumer;   // OpenTraceW handle (INVALID... on failure)
        bool own;               // we created the session
        TRACEHANDLE session;    // StartTraceW handle when own
    };
    SessionResult TryStartSession(bool system_logger);
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
    // guards m_conn_state / m_error (consumer thread writes,
    // TM + timer threads read via EtwPopupStatus/BuildTooltip)
    mutable std::mutex m_state_mutex;
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
    // m_cum size cap: evict stalest-idle entries down to kCumLow once kCumMax
    // is exceeded (never the pid of the event triggering the eviction).
    static const size_t kCumMax = 4096;
    static const size_t kCumLow = 3500;
    // PID 复用核对参数（两路线一致）：活跃窗 60s（死条目不占核对名额）、
    // 核对节流 10s、每轮核对上限 32 条（放锁段执行，防大批量卡顿）。
    static const ULONGLONG kCtActiveWindowMs = 60000;
    static const ULONGLONG kCtCheckIntervalMs = 10000;
    static const size_t kCtCheckBatch = 32;

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

    // PID 换主时重置累计条目（caller 持 m_mutex）。new_path 非空时顺带刷新
    // 名字/路径缓存（权威值来自核对句柄）。src 仅用于日志（procstart/periodic）。
    void ResetCumLocked(DWORD pid, ULONGLONG new_ct, const wchar_t* src, const std::wstring& new_path);
    void LogLine(const wchar_t* fmt, ...);
    void LogPeriodicLocked();

    // Best-effort: find which process(es) hold ETW session/consumer handles
    std::wstring FindSessionOwners();

    std::mutex m_name_mutex;
    std::map<DWORD, std::wstring> m_name_cache;
    std::map<DWORD, std::wstring> m_path_cache;
    // Failed name lookups ("<pid>") are re-tried with backoff: a dead
    // process can never resolve, and retrying it every refresh tick would
    // burn a full Toolhelp snapshot per dead pid per tick. 30s cooldown
    // keeps the cost negligible while still catching late-resolvable ones.
    std::map<DWORD, ULONGLONG> m_name_retry_ts;

    wchar_t m_error[256] = L"";
    void SetError(const wchar_t* fmt, ...);
    void SetConnState(const wchar_t* s);   // locked write (consumer thread)
};
