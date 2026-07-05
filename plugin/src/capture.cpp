#include "capture.h"
#include <mstcpip.h>
#include <tlhelp32.h>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

PacketCapture::PacketCapture() {}
PacketCapture::~PacketCapture() { Stop(); }

// ============================================================
// Adapter enumeration + TM config
// ============================================================

struct AdapterInfo {
    std::wstring friendly_name;
    std::string ip;
    bool is_loopback = false;
};

static std::vector<AdapterInfo> EnumAdapters() {
    std::vector<AdapterInfo> result;
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &bufLen);
    if (!bufLen) return result;
    std::vector<uint8_t> buf(bufLen);
    auto* addrs = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &bufLen) != NO_ERROR) return result;
    for (auto* a = addrs; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        AdapterInfo info;
        info.friendly_name = a->FriendlyName ? a->FriendlyName : L"";
        info.is_loopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
        if (a->FirstUnicastAddress) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &((sockaddr_in*)a->FirstUnicastAddress->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
            info.ip = ip;
        }
        if (!info.ip.empty()) result.push_back(std::move(info));
    }
    return result;
}

static std::string FindAdapterIP() {
    auto adapters = EnumAdapters();
    if (adapters.empty()) return "";

    // Read TM config
    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t* slash = wcsrchr(exe_dir, L'\\');
    if (slash) *slash = 0;
    wchar_t ini_path[MAX_PATH];
    swprintf_s(ini_path, MAX_PATH, L"%s\\global_cfg.ini", exe_dir);

    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"connection", L"select_all", L"0", buf, 512, ini_path);
    bool select_all = (wcscmp(buf, L"1") == 0 || _wcsicmp(buf, L"true") == 0);
    GetPrivateProfileStringW(L"connection", L"auto_select", L"1", buf, 512, ini_path);
    bool auto_select = (wcscmp(buf, L"1") == 0 || _wcsicmp(buf, L"true") == 0);
    GetPrivateProfileStringW(L"connection", L"connection_name", L"", buf, 512, ini_path);
    std::string conn_name;
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    if (len > 1) { conn_name.resize(len - 1); WideCharToMultiByte(CP_UTF8, 0, buf, -1, &conn_name[0], len, NULL, NULL); }

    // auto/select_all: prefer proxy/virtual for per-process mapping
    if (auto_select || select_all) {
        for (auto& a : adapters) {
            if (a.is_loopback || a.ip == "127.0.0.1") continue;
            std::wstring lower = a.friendly_name;
            for (auto& ch : lower) ch = towlower(ch);
            if (lower.find(L"mihomo") != std::wstring::npos || lower.find(L"clash") != std::wstring::npos ||
                lower.find(L"tun") != std::wstring::npos || lower.find(L"tap") != std::wstring::npos ||
                lower.find(L"vpn") != std::wstring::npos || lower.find(L"wireguard") != std::wstring::npos)
                return a.ip;
        }
        for (auto& a : adapters) { if (!a.is_loopback && a.ip != "127.0.0.1") return a.ip; }
        return adapters[0].ip;
    }

    // specific connection
    if (!conn_name.empty()) {
        for (auto& a : adapters) {
            std::wstring wf(a.friendly_name.begin(), a.friendly_name.end());
            std::string fname(wf.begin(), wf.end());
            if (fname.find(conn_name) != std::string::npos || conn_name.find(fname) != std::string::npos)
                return a.ip;
        }
    }

    for (auto& a : adapters) { if (!a.is_loopback && a.ip != "127.0.0.1") return a.ip; }
    return adapters.empty() ? "" : adapters[0].ip;
}

// ============================================================
// Process name
// ============================================================

std::wstring PacketCapture::GetProcessName(DWORD pid) {
    auto it = m_name_cache.find(pid);
    if (it != m_name_cache.end()) return it->second;
    std::wstring name = L"[" + std::to_wstring(pid) + L"]";
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do { if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; } } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    m_name_cache[pid] = name;
    return name;
}

std::wstring PacketCapture::GetProcessPath(DWORD pid) {
    auto it = m_path_cache.find(pid);
    if (it != m_path_cache.end()) return it->second;

    std::wstring path;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t buf[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
            path = buf;
        }
        CloseHandle(hProc);
    }
    m_path_cache[pid] = path;
    return path;
}

// ============================================================
// Start / Stop
// ============================================================

bool PacketCapture::Start() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        wcscpy_s(m_error, L"WSAStartup failed"); return false;
    }

    std::string ip = FindAdapterIP();
    if (ip.empty()) { wcscpy_s(m_error, L"No adapter found"); WSACleanup(); return false; }

    m_sock = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
    if (m_sock == INVALID_SOCKET) {
        swprintf_s(m_error, L"Raw socket failed (err=%d). Run as Admin?", WSAGetLastError());
        WSACleanup(); return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (bind(m_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        swprintf_s(m_error, L"Bind to %S failed (err=%d)", ip.c_str(), WSAGetLastError());
        closesocket(m_sock); m_sock = INVALID_SOCKET; WSACleanup(); return false;
    }

    int optval = 1;
    DWORD bytesReturned = 0;
    if (WSAIoctl(m_sock, SIO_RCVALL, &optval, sizeof(optval), NULL, 0, &bytesReturned, NULL, NULL) == SOCKET_ERROR) {
        swprintf_s(m_error, L"SIO_RCVALL failed (err=%d). Run as Admin?", WSAGetLastError());
        closesocket(m_sock); m_sock = INVALID_SOCKET; WSACleanup(); return false;
    }

    int timeout = 500;
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    m_running = true;
    m_capture_thread = std::thread(&PacketCapture::CaptureLoop, this);
    m_conn_thread = std::thread(&PacketCapture::ConnRefreshLoop, this);
    wcscpy_s(m_error, L"OK");
    return true;
}

void PacketCapture::Stop() {
    m_running = false;
    if (m_sock != INVALID_SOCKET) { closesocket(m_sock); m_sock = INVALID_SOCKET; }
    if (m_capture_thread.joinable()) m_capture_thread.join();
    if (m_conn_thread.joinable()) m_conn_thread.join();
    WSACleanup();
}

// ============================================================
// Loops
// ============================================================

void PacketCapture::CaptureLoop() {
    uint8_t buf[65536];
    while (m_running) {
        int n = recv(m_sock, (char*)buf, sizeof(buf), 0);
        if (n > 20) ProcessPacket(buf, n);
    }
}

void PacketCapture::ConnRefreshLoop() {
    while (m_running) {
        std::map<uint16_t, DWORD> tcp_port_pid;
        std::map<uint16_t, DWORD> udp_port_pid;
        std::map<ConnKey, DWORD> tcp_conns;

        ULONG tcpSize = 0;
        GetExtendedTcpTable(NULL, &tcpSize, FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_ALL, 0);
        if (tcpSize > 0) {
            std::vector<uint8_t> tcpBuf(tcpSize);
            if (GetExtendedTcpTable(tcpBuf.data(), &tcpSize, FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_ALL, 0) == NO_ERROR) {
                auto* table = (MIB_TCPTABLE_OWNER_MODULE*)tcpBuf.data();
                for (DWORD i = 0; i < table->dwNumEntries; i++) {
                    auto& row = table->table[i];
                    if (!row.dwOwningPid) continue;
                    uint16_t lp = ntohs((u_short)row.dwLocalPort);
                    uint16_t rp = ntohs((u_short)row.dwRemotePort);
                    tcp_port_pid[lp] = row.dwOwningPid;
                    tcp_conns[{lp, row.dwRemoteAddr, rp}] = row.dwOwningPid;
                }
            }
        }

        ULONG udpSize = 0;
        GetExtendedUdpTable(NULL, &udpSize, FALSE, AF_INET, UDP_TABLE_OWNER_MODULE, 0);
        if (udpSize > 0) {
            std::vector<uint8_t> udpBuf(udpSize);
            if (GetExtendedUdpTable(udpBuf.data(), &udpSize, FALSE, AF_INET, UDP_TABLE_OWNER_MODULE, 0) == NO_ERROR) {
                auto* table = (MIB_UDPTABLE_OWNER_MODULE*)udpBuf.data();
                for (DWORD i = 0; i < table->dwNumEntries; i++) {
                    if (!table->table[i].dwOwningPid) continue;
                    udp_port_pid[ntohs((u_short)table->table[i].dwLocalPort)] = table->table[i].dwOwningPid;
                }
            }
        }

        { std::lock_guard<std::mutex> lk(m_mutex); m_tcp_port_pid = std::move(tcp_port_pid); m_udp_port_pid = std::move(udp_port_pid); m_tcp_conns = std::move(tcp_conns); }
        for (int i = 0; i < 30 && m_running; i++) Sleep(100);
    }
}

void PacketCapture::ProcessPacket(const uint8_t* data, int len) {
    if (len < 20) return;
    uint8_t ihl = (data[0] & 0x0F) * 4;
    uint8_t proto = data[9];
    uint16_t total_len = (data[2] << 8) | data[3];
    uint32_t src_addr = *(uint32_t*)(data + 12);
    uint32_t dst_addr = *(uint32_t*)(data + 16);

    DWORD pid = 0; bool is_out = false;

    if (proto == 6 && len >= ihl + 4) {
        uint16_t sp = (data[ihl] << 8) | data[ihl + 1];
        uint16_t dp = (data[ihl + 2] << 8) | data[ihl + 3];
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_tcp_conns.find({sp, dst_addr, dp});
        if (it != m_tcp_conns.end()) { pid = it->second; is_out = true; }
        else { it = m_tcp_conns.find({dp, src_addr, sp}); if (it != m_tcp_conns.end()) { pid = it->second; } }
        if (!pid) {
            auto pit = m_tcp_port_pid.find(sp);
            if (pit != m_tcp_port_pid.end()) { pid = pit->second; is_out = true; }
            else { pit = m_tcp_port_pid.find(dp); if (pit != m_tcp_port_pid.end()) { pid = pit->second; } }
        }
    } else if (proto == 17 && len >= ihl + 4) {
        uint16_t sp = (data[ihl] << 8) | data[ihl + 1];
        uint16_t dp = (data[ihl + 2] << 8) | data[ihl + 3];
        std::lock_guard<std::mutex> lk(m_mutex);
        auto pit = m_udp_port_pid.find(sp);
        if (pit != m_udp_port_pid.end()) { pid = pit->second; is_out = true; }
        else { pit = m_udp_port_pid.find(dp); if (pit != m_udp_port_pid.end()) { pid = pit->second; } }
    }

    if (pid > 0) {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto& st = m_stats[pid];
        if (st.pid == 0) {
            // First time seeing this process - initialize to avoid bogus speed
            st.pid = pid;
            st.name = GetProcessName(pid);
            st.exe_path = GetProcessPath(pid);
            st.bytes_sent = total_len;
            st.bytes_recv = 0;
            st.prev_sent = total_len;
            st.prev_recv = 0;
        } else {
            st.pid = pid;
            if (st.name.empty()) st.name = GetProcessName(pid);
            if (st.exe_path.empty()) st.exe_path = GetProcessPath(pid);
            if (is_out) st.bytes_sent += total_len; else st.bytes_recv += total_len;
        }
    }
}

std::vector<ProcTraffic> PacketCapture::GetStats(double dt) {
    std::vector<ProcTraffic> result;
    std::lock_guard<std::mutex> lk(m_mutex);

    // Debug: write raw values to temp file
    FILE* dbg = fopen("C:\\Users\\Yang\\AppData\\Local\\Temp\\procnet_debug.txt", "w");
    if (dbg) fprintf(dbg, "dt=%.3f count=%zu\n", dt, m_stats.size());

    // Count connections per process
    std::map<DWORD, int> conn_counts;
    for (auto& [key, pid] : m_tcp_conns) {
        conn_counts[pid]++;
    }

    for (auto& [pid, st] : m_stats) {
        if (dbg) fprintf(dbg, "PID=%u sent=%llu prev=%llu recv=%llu prev=%llu\n",
            pid, st.bytes_sent, st.prev_sent, st.bytes_recv, st.prev_recv);

        st.speed_up = (double)((int64_t)(st.bytes_sent - st.prev_sent)) / dt;
        st.speed_down = (double)((int64_t)(st.bytes_recv - st.prev_recv)) / dt;
        st.prev_sent = st.bytes_sent;
        st.prev_recv = st.bytes_recv;
        if (st.name.empty() || st.name[0] == L'[') st.name = GetProcessName(pid);
        if (st.exe_path.empty()) st.exe_path = GetProcessPath(pid);
        auto cit = conn_counts.find(pid);
        st.conn_count = (cit != conn_counts.end()) ? cit->second : 0;
        result.push_back(st);
    }
    if (dbg) fclose(dbg);

    std::sort(result.begin(), result.end(), [](auto& a, auto& b) { return (a.speed_up + a.speed_down) > (b.speed_up + b.speed_down); });
    // Do NOT reset byte counters - just let them accumulate
    // prev_sent/prev_recv track the last snapshot for delta calculation
    return result;
}
