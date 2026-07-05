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
};

class PacketCapture {
public:
    PacketCapture();
    ~PacketCapture();
    bool Start();
    void Stop();
    bool IsRunning() const { return m_running; }
    const wchar_t* GetLastError() const { return m_error; }
    std::vector<ProcTraffic> GetStats(double interval_sec);

private:
    void CaptureLoop();
    void ConnRefreshLoop();
    void ProcessPacket(const uint8_t* data, int len);
    std::wstring GetProcessName(DWORD pid);
    std::wstring GetProcessPath(DWORD pid);

    SOCKET m_sock = INVALID_SOCKET;
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
};
