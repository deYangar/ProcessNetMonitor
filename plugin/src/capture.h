#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>
#include <set>

struct ProcTraffic {
    DWORD pid = 0;
    std::wstring name;
    std::wstring exe_path;  // full path for icon extraction
    uint64_t bytes_sent = 0;
    uint64_t bytes_recv = 0;
    uint64_t prev_sent = 0;
    uint64_t prev_recv = 0;
    double speed_up = 0.0;
    double speed_down = 0.0;
    int conn_count = 0;    // active TCP connections
};

// 连接详情
struct ConnDetail {
    enum Protocol { TCP, UDP } protocol;
    std::wstring local_addr;    // "192.168.1.100:51234"
    std::wstring remote_addr;   // "142.250.80.46:443" or "*:*" for UDP
    std::wstring state;         // "ESTABLISHED", "LISTENING", "TIME_WAIT", etc.
    DWORD pid = 0;
};

class PacketCapture {
public:
    PacketCapture();
    ~PacketCapture();
    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }
    const wchar_t* GetLastError() const { return m_error; }
    // Detailed error description (for tooltip / diagnostics)
    const wchar_t* GetErrorDetail() const { return m_error_detail; }
    std::vector<ProcTraffic> GetStats(double interval_sec);
    void SetOnStats(std::function<void(const std::vector<ProcTraffic>&)> cb) { m_on_stats = cb; }
    
    // Check if TM config changed and rebind sockets if needed
    void CheckConfigChanged();
    // Set TM config directory
    void SetTMConfigDir(const std::wstring& dir) { m_tm_config_dir = dir; }
    // Set directory for capture.log (diagnostics)
    void SetLogDir(const std::wstring& dir) { m_log_dir = dir; }
    
    // 获取指定进程的连接详情
    std::vector<ConnDetail> GetProcessConnections(DWORD pid);
    
    // Set TUN address ranges to skip (CIDR notation like "198.18.0.0/15")
    void SetTunRanges(const std::vector<std::wstring>& ranges);

private:
    void CaptureLoop();
    void ConnRefreshLoop();
    void ProcessPacket(const uint8_t* data, int len);
    std::wstring GetProcessName(DWORD pid);
    std::wstring GetProcessPath(DWORD pid);
    
    // 刷新连接详情缓存
    void RefreshConnectionDetails();

    std::vector<SOCKET> m_socks;  // multiple sockets for 'select all' mode
    std::atomic<bool> m_running{false};
    std::thread m_capture_thread;
    std::thread m_conn_thread;
    std::mutex m_mutex;
    std::map<DWORD, ProcTraffic> m_stats;
    std::map<uint16_t, DWORD> m_tcp_port_pid;
    std::map<uint16_t, DWORD> m_udp_port_pid;
    struct ConnKey {
        uint16_t local_port; uint32_t remote_addr; uint16_t remote_port;
        bool operator<(const ConnKey& o) const {
            if (local_port != o.local_port) return local_port < o.local_port;
            if (remote_addr != o.remote_addr) return remote_addr < o.remote_addr;
            return remote_port < o.remote_port;
        }
    };
    std::map<ConnKey, DWORD> m_tcp_conns;
    std::map<DWORD, std::wstring> m_name_cache;
    std::map<DWORD, std::wstring> m_path_cache;
    wchar_t m_error[128] = L"";
    wchar_t m_error_detail[512] = L"";  // detailed explanation for tooltip
    std::function<void(const std::vector<ProcTraffic>&)> m_on_stats;
    
    // 连接详情缓存
    std::mutex m_conn_mutex;
    std::map<DWORD, std::vector<ConnDetail>> m_conn_details_cache;
    
    // TM config directory and adapter rebind
    std::wstring m_tm_config_dir;
    std::string m_last_bind_key;  // hash of last adapter selection
    time_t m_last_config_check = 0;
    
    // Last socket creation failure info (for specific error messages)
    int m_fail_stage = 0;       // 1=socket() 2=bind() 3=WSAIoctl(SIO_RCVALL)
    int m_last_wsa_error = 0;   // WSAGetLastError() at failure
    
    // Diagnostics log
    std::wstring m_log_dir;
    std::string m_last_logged_selection;
    std::wstring m_last_logged_error;
    void WriteLog(const std::string& text);
    void LogStartupFailure();
    void SetError(const wchar_t* short_msg, const wchar_t* detail_fmt, ...);
    
    // Read TM connection config and return adapter IPs to bind
    std::vector<std::string> ReadTMAdapterConfig();
    // Rebind sockets (called when config changes)
    void RebindSockets(const std::vector<std::string>& new_ips);
    
    // Local adapter IPs (for packet direction detection)
    std::vector<uint32_t> m_local_ips;  // in network byte order
    
    // TCP per-connection byte stats (from GetPerTcpConnectionEStats)
    // Key: PID, Value: {DataBytesIn, DataBytesOut}
    std::map<DWORD, std::pair<uint64_t, uint64_t>> m_tcp_cum;   // current cumulative
    std::map<DWORD, std::pair<uint64_t, uint64_t>> m_tcp_prev;  // previous cumulative
    std::set<uint64_t> m_tcp_stats_enabled;  // track enabled connections (hashed by local_port + remote_addr + remote_port)

    // UDP per-process byte stats (from raw socket)
    std::map<DWORD, std::pair<uint64_t, uint64_t>> m_udp_cum;   // pid -> {bytes_recv, bytes_sent}
    std::map<DWORD, std::pair<uint64_t, uint64_t>> m_udp_prev;  // for delta calc
    
    // Query TCP byte stats from the kernel
    void QueryTcpStats();
    std::vector<std::pair<uint32_t, uint32_t>> m_tun_cidrs;  // {network, mask} in host byte order
    
    // Debug log (disabled for release)
    // void DumpDebugLog(const std::vector<ProcTraffic>& result, double dt);
    // std::wstring m_debug_path;
    // time_t m_last_debug = 0;
};
