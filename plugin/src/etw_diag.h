#pragma once
// ============================================================
// ETW diagnostic consumer for per-process network traffic.
// Attaches to the classic "NT Kernel Logger" session AND starts
// a private session for the Microsoft-Windows-Kernel-Network
// manifest provider, then parses events via TDH (name-based)
// while also recording manual-offset reads for comparison.
// Writes etw_diag.log + etw_diag_stats.log for offline analysis.
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
#include <cstdio>

class EtwDiag {
public:
    EtwDiag() = default;
    ~EtwDiag();

    void SetLogDir(const std::wstring& dir) { m_log_dir = dir; }
    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }

private:
    struct ShapeKey {
        uint8_t tag;          // 0=classic kernel trace, 1=manifest kernel-network, 2=other
        uint16_t task;
        uint8_t opcode;
        uint16_t id;
        uint8_t version;
        bool operator<(const ShapeKey& o) const {
            if (tag != o.tag) return tag < o.tag;
            if (task != o.task) return task < o.task;
            if (opcode != o.opcode) return opcode < o.opcode;
            if (id != o.id) return id < o.id;
            return version < o.version;
        }
    };

    struct PropInfo {
        std::wstring name;
        uint32_t offset = 0;
        uint32_t size = 0;      // 0 = unknown/variable
        uint16_t intype = 0;
    };

    struct ShapeInfo {
        ShapeKey key{};
        bool schema_logged = false;
        bool tdh_ok = false;
        ULONG tdh_status = 0;
        int pid_idx = -1;       // index into props
        int size_idx = -1;
        uint32_t pid_off = 0, pid_len = 0;
        uint32_t size_off = 0, size_len = 0;
        bool offsets_valid = false;
        std::vector<PropInfo> props;

        uint64_t count = 0;
        uint64_t ulen_min = UINT64_MAX, ulen_max = 0;
        uint64_t tdh_size_sum = 0;      // sum of TDH-parsed "size"
        uint64_t manual_dw1_sum = 0;    // sum of DWORD at offset 4
        uint64_t tdh_zero = 0, dw1_zero = 0, mismatch = 0;
        uint64_t size_max_tdh = 0, size_max_dw1 = 0;
        uint64_t hdr_pid_valid = 0;     // Header.ProcessId != 0xFFFFFFFF
        int dumps_done = 0;

        // offset scan: DWORD-aligned, offsets 0..60
        uint64_t off_nonzero[16] = {};
        uint64_t off_max[16] = {};
    };

    struct PidAgg {
        uint64_t tdh_bytes = 0;
        uint64_t dw1_bytes = 0;
        uint64_t events = 0;
        uint64_t last_seen = 0;
    };

    void ThreadMain();
    void ConsumeOne(TRACEHANDLE h, const char* name);
    void OnEvent(PEVENT_RECORD rec);
    void EnsureSchema(PEVENT_RECORD rec, ShapeInfo& si);
    void FlushStats(bool final);
    void DumpEvent(PEVENT_RECORD rec, ShapeInfo& si, const char* why);
    void LogHeaderInfo();
    std::wstring ProcName(DWORD pid);

    static void WINAPI StaticCallback(PEVENT_RECORD rec);
    static EtwDiag* s_self;   // for static callback

    std::wstring m_log_dir;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::vector<std::thread> m_workers;
    std::atomic<int> m_workers_done{0};
    std::mutex m_mutex;  // protects maps + log files (two consumer threads)

    TRACEHANDLE m_session_a = 0;    // NT Kernel Logger (attach or start)
    TRACEHANDLE m_session_b = 0;    // private manifest-provider session
    TRACEHANDLE m_consumer_a = INVALID_PROCESSTRACE_HANDLE;
    TRACEHANDLE m_consumer_b = INVALID_PROCESSTRACE_HANDLE;
    bool m_started_a = false;
    bool m_started_b = false;

    FILE* m_log = nullptr;
    FILE* m_stats = nullptr;

    std::map<ShapeKey, ShapeInfo> m_shapes;
    std::map<DWORD, PidAgg> m_pids;
    std::map<DWORD, std::wstring> m_name_cache;

    uint64_t m_total_events = 0;
    uint64_t m_last_flush_tick = 0;
    uint64_t m_start_tick = 0;
    int m_big_dumps = 0;
    static const int MAX_BIG_DUMPS = 800;
    static const int MAX_SHAPE_DUMPS = 20;
};
