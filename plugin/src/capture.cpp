#include "capture.h"
#include <mstcpip.h>
#include <tlhelp32.h>
#include <algorithm>
#include <set>

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



// ============================================================
// TM config reading
// ============================================================
std::vector<std::string> PacketCapture::ReadTMAdapterConfig() {
    auto adapters = EnumAdapters();
    std::vector<std::string> result;
    if (adapters.empty()) return result;

    // Determine config file path
    std::wstring config_path = m_tm_config_dir;
    if (config_path.empty()) {
        // Fallback: try AppData\Roaming\TrafficMonitor\config.ini
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
            config_path = std::wstring(appdata) + L"\\TrafficMonitor\\config.ini";
        }
    } else {
        config_path += L"\\config.ini";
    }

    // Read connection settings
    wchar_t buf[512] = {};
    GetPrivateProfileStringW(L"connection", L"select_all", L"false", buf, 512, config_path.c_str());
    bool select_all = (_wcsicmp(buf, L"true") == 0 || wcscmp(buf, L"1") == 0);
    GetPrivateProfileStringW(L"connection", L"auto_select", L"true", buf, 512, config_path.c_str());
    bool auto_select = (_wcsicmp(buf, L"true") == 0 || wcscmp(buf, L"1") == 0);
    GetPrivateProfileStringW(L"connection", L"connection_name", L"", buf, 512, config_path.c_str());
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    std::string conn_name;
    if (len > 1) { conn_name.resize(len - 1); WideCharToMultiByte(CP_UTF8, 0, buf, -1, &conn_name[0], len, NULL, NULL); }

    // Log
    std::string log_text;
    auto log_adapter = [&](const char* mode, const std::string& ip, const std::wstring& fname) {
        int nlen = WideCharToMultiByte(CP_UTF8, 0, fname.c_str(), -1, NULL, 0, NULL, NULL);
        std::string fn; if (nlen > 1) { fn.resize(nlen-1); WideCharToMultiByte(CP_UTF8, 0, fname.c_str(), -1, &fn[0], nlen, NULL, NULL); }
        log_text += std::string(mode) + ": " + ip + " (" + fn + ")\n";
    };

    if (select_all) {
        for (auto& a : adapters) {
            if (a.is_loopback || a.ip == "127.0.0.1" || a.ip.empty()) continue;
            result.push_back(a.ip);
            log_adapter("all", a.ip, a.friendly_name);
        }
    } else if (!conn_name.empty() && !auto_select) {
        for (auto& a : adapters) {
            int nlen = WideCharToMultiByte(CP_UTF8, 0, a.friendly_name.c_str(), -1, NULL, 0, NULL, NULL);
            std::string fn; if (nlen > 1) { fn.resize(nlen-1); WideCharToMultiByte(CP_UTF8, 0, a.friendly_name.c_str(), -1, &fn[0], nlen, NULL, NULL); }
            if (fn.find(conn_name) != std::string::npos || conn_name.find(fn) != std::string::npos) {
                result.push_back(a.ip);
                log_adapter("specific", a.ip, a.friendly_name);
                break;
            }
        }
    }
    // auto_select or fallback: prefer physical adapters, skip virtual/TUN
    if (result.empty()) {
        // First pass: skip virtual adapters
        for (auto& a : adapters) {
            if (a.is_loopback || a.ip == "127.0.0.1" || a.ip.empty()) continue;
            std::wstring lower = a.friendly_name;
            for (auto& ch : lower) ch = towlower(ch);
            if (lower.find(L"tun") != std::wstring::npos || lower.find(L"tap") != std::wstring::npos ||
                lower.find(L"mihomo") != std::wstring::npos || lower.find(L"clash") != std::wstring::npos ||
                lower.find(L"vpn") != std::wstring::npos || lower.find(L"wireguard") != std::wstring::npos ||
                lower.find(L"virtual") != std::wstring::npos || lower.find(L"loopback") != std::wstring::npos)
                continue;
            result.push_back(a.ip);
            log_adapter("auto", a.ip, a.friendly_name);
            break;
        }
        // Second pass: any non-loopback
        if (result.empty()) {
            for (auto& a : adapters) {
                if (!a.is_loopback && a.ip != "127.0.0.1" && !a.ip.empty()) {
                    result.push_back(a.ip);
                    log_adapter("auto-fallback", a.ip, a.friendly_name);
                    break;
                }
            }
        }
    }

    return result;
}

void PacketCapture::RebindSockets(const std::vector<std::string>& new_ips) {
    // Build bind key to detect changes
    std::string new_key;
    for (auto& ip : new_ips) new_key += ip + ";";
    if (new_key == m_last_bind_key) return;  // no change
    m_last_bind_key = new_key;

    // Store local IPs for packet direction detection
    m_local_ips.clear();
    for (auto& ip : new_ips) {
        uint32_t addr;
        inet_pton(AF_INET, ip.c_str(), &addr);
        m_local_ips.push_back(addr);
    }

    // Close old sockets
    for (auto& s : m_socks) { if (s != INVALID_SOCKET) closesocket(s); }
    m_socks.clear();

    // Create new sockets
    for (auto& ip : new_ips) {
        SOCKET s = socket(AF_INET, SOCK_RAW, IPPROTO_IP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(s); continue;
        }
        int optval = 1;
        DWORD bytesReturned = 0;
        if (WSAIoctl(s, SIO_RCVALL, &optval, sizeof(optval), NULL, 0, &bytesReturned, NULL, NULL) == SOCKET_ERROR) {
            closesocket(s); continue;
        }
        int timeout = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        m_socks.push_back(s);
    }
}

void PacketCapture::CheckConfigChanged() {
    time_t now = time(NULL);
    if (now - m_last_config_check < 5) return;  // check every 5 seconds
    m_last_config_check = now;
    auto new_ips = ReadTMAdapterConfig();
    if (!new_ips.empty()) RebindSockets(new_ips);
}

// ============================================================
// Start / Stop
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

    auto bind_ips = ReadTMAdapterConfig();
    if (bind_ips.empty()) { wcscpy_s(m_error, L"No adapter found"); WSACleanup(); return false; }

    RebindSockets(bind_ips);
    if (m_socks.empty()) {
        swprintf_s(m_error, L"Raw socket failed. Run as Admin?");
        WSACleanup(); return false;
    }

    m_running = true;
    m_capture_thread = std::thread(&PacketCapture::CaptureLoop, this);
    m_conn_thread = std::thread(&PacketCapture::ConnRefreshLoop, this);
    wcscpy_s(m_error, L"OK");
    return true;
}

void PacketCapture::Stop() {
    m_running = false;
    for (auto& s : m_socks) { if (s != INVALID_SOCKET) closesocket(s); }
    m_socks.clear();
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
        if (m_socks.empty()) break;
        // Use select() to wait on all sockets
        fd_set readset;
        FD_ZERO(&readset);
        for (auto& s : m_socks) FD_SET(s, &readset);
        timeval tv = { 0, 500000 }; // 500ms timeout
        int ret = select(0, &readset, NULL, NULL, &tv);
        if (ret <= 0) continue; // timeout or error
        for (auto& s : m_socks) {
            if (FD_ISSET(s, &readset)) {
                int n = recv(s, (char*)buf, sizeof(buf), 0);
                if (n > 20) ProcessPacket(buf, n);
            }
        }
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
        
        // 刷新连接详情缓存
        RefreshConnectionDetails();
        
        // Check if TM config changed (adapter selection)
        CheckConfigChanged();
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
        // TCP traffic is tracked via GetPerTcpConnectionEStats (more accurate, works on WLAN)
        // Raw socket TCP matching only used for UDP fallback and connection display
        return;
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
        // Ensure process exists in m_stats for name/path lookup
        auto& st = m_stats[pid];
        if (st.pid == 0) {
            st.pid = pid;
            st.name = GetProcessName(pid);
            st.exe_path = GetProcessPath(pid);
        }
        // Track UDP bytes separately (TCP is tracked via GetPerTcpConnectionEStats)
        if (is_out) m_udp_cum[pid].second += total_len;
        else        m_udp_cum[pid].first += total_len;
    }
}

// ============================================================
// TCP byte stats via GetPerTcpConnectionEStats
// ============================================================
void PacketCapture::QueryTcpStats() {
    // Query all TCP connections and get per-connection byte counters
    ULONG tcpSize = 0;
    GetExtendedTcpTable(NULL, &tcpSize, FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_ALL, 0);
    if (tcpSize == 0) return;
    std::vector<uint8_t> tcpBuf(tcpSize);
    if (GetExtendedTcpTable(tcpBuf.data(), &tcpSize, FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_ALL, 0) != NO_ERROR) return;
    
    auto* table = (MIB_TCPTABLE_OWNER_MODULE*)tcpBuf.data();
    std::map<DWORD, std::pair<uint64_t, uint64_t>> cum;  // pid -> {bytes_in, bytes_out}
    
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        auto& row = table->table[i];
        DWORD pid = row.dwOwningPid;
        if (!pid) continue;
        
        // Only query ESTABLISHED connections (state = 5)
        if (row.dwState != 5) continue;
        
        // Skip loopback connections (127.0.0.1) to avoid double-counting proxy traffic
        // e.g. Chrome -> 127.0.0.1:7890 (mihomo) should not count as Chrome's internet traffic
        uint32_t loopback = htonl(INADDR_LOOPBACK);  // 127.0.0.1 in network byte order
        if (row.dwLocalAddr == loopback || row.dwRemoteAddr == loopback) continue;
        // Also skip ::1 (IPv6 loopback) - dwLocalAddr would be 0x0100007F in some representations
        if (row.dwLocalAddr == 0x0100007F || row.dwRemoteAddr == 0x0100007F) continue;
        
        // Skip TUN adapter connections to avoid double-counting in TUN proxy mode
        // In TUN mode: Edge -> 198.18.x.x (TUN) -> mihomo -> physical adapter -> remote
        // Both Edge's TUN connection and mihomo's physical connection carry the same data.
        // Skipping TUN-side connections ensures each byte is counted only once (via mihomo's outbound).
        uint32_t local_ip = ntohl(row.dwLocalAddr);
        uint32_t remote_ip = ntohl(row.dwRemoteAddr);
        bool is_tun = false;
        for (auto& cidr : m_tun_cidrs) {
            if ((local_ip & cidr.second) == (cidr.first & cidr.second) ||
                (remote_ip & cidr.second) == (cidr.first & cidr.second)) {
                is_tun = true;
                break;
            }
        }
        if (is_tun) continue;
        
        // Build MIB_TCPROW for GetPerTcpConnectionEStats
        MIB_TCPROW tcpRow;
        tcpRow.dwState = row.dwState;
        tcpRow.dwLocalAddr = row.dwLocalAddr;
        tcpRow.dwLocalPort = row.dwLocalPort;
        tcpRow.dwRemoteAddr = row.dwRemoteAddr;
        tcpRow.dwRemotePort = row.dwRemotePort;
        
        // Enable stats collection (once per connection)
        uint64_t conn_hash = ((uint64_t)row.dwLocalPort << 48) | ((uint64_t)row.dwRemotePort << 32) | row.dwRemoteAddr;
        if (m_tcp_stats_enabled.find(conn_hash) == m_tcp_stats_enabled.end()) {
            TCP_ESTATS_DATA_RW_v0 rw;
            rw.EnableCollection = TRUE;
            SetPerTcpConnectionEStats(&tcpRow, TcpConnectionEstatsData, (PUCHAR)&rw, 0, sizeof(rw), 0);
            m_tcp_stats_enabled.insert(conn_hash);
        }
        
        // Query byte counters
        TCP_ESTATS_DATA_ROD_v0 rod;
        memset(&rod, 0, sizeof(rod));
        DWORD ret = GetPerTcpConnectionEStats(&tcpRow, TcpConnectionEstatsData,
            NULL, 0, 0,   // Rw
            NULL, 0, 0,   // Ros
            (PUCHAR)&rod, 0, sizeof(rod));  // Rod
        if (ret == NO_ERROR) {
            cum[pid].first += rod.DataBytesIn;
            cum[pid].second += rod.DataBytesOut;
        }
    }
    
    // Clean up enabled set for closed connections
    std::set<uint64_t> active_hashes;
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        auto& row = table->table[i];
        if (row.dwState != 5) continue;
        uint64_t h = ((uint64_t)row.dwLocalPort << 48) | ((uint64_t)row.dwRemotePort << 32) | row.dwRemoteAddr;
        active_hashes.insert(h);
    }
    for (auto it = m_tcp_stats_enabled.begin(); it != m_tcp_stats_enabled.end();) {
        if (active_hashes.find(*it) == active_hashes.end()) it = m_tcp_stats_enabled.erase(it);
        else ++it;
    }
    
    std::lock_guard<std::mutex> lk(m_mutex);
    m_tcp_cum = std::move(cum);
}

std::vector<ProcTraffic> PacketCapture::GetStats(double dt) {
    // Query TCP byte stats from kernel
    QueryTcpStats();
    
    std::vector<ProcTraffic> result;
    std::lock_guard<std::mutex> lk(m_mutex);

    // Count connections per process (TCP + UDP)
    std::map<DWORD, int> conn_counts;
    for (auto& [key, pid] : m_tcp_conns) {
        conn_counts[pid]++;
    }
    for (auto& [port, pid] : m_udp_port_pid) {
        if (conn_counts.find(pid) == conn_counts.end())
            conn_counts[pid]++;
    }
    
    // Merge: ensure all PIDs from tcp_cum and raw socket stats are present
    for (auto& [pid, vals] : m_tcp_cum) {
        if (m_stats.find(pid) == m_stats.end()) {
            ProcTraffic st;
            st.pid = pid;
            st.name = GetProcessName(pid);
            st.exe_path = GetProcessPath(pid);
            m_stats[pid] = st;
        }
    }
    // Also add PIDs that only have UDP traffic
    for (auto& [pid, vals] : m_udp_cum) {
        if (m_stats.find(pid) == m_stats.end()) {
            ProcTraffic st;
            st.pid = pid;
            st.name = GetProcessName(pid);
            st.exe_path = GetProcessPath(pid);
            m_stats[pid] = st;
        }
    }

    for (auto& [pid, st] : m_stats) {
        // TCP bytes from kernel
        auto tcp_it = m_tcp_cum.find(pid);
        uint64_t tcp_in = 0, tcp_out = 0;
        if (tcp_it != m_tcp_cum.end()) {
            tcp_in = tcp_it->second.first;
            tcp_out = tcp_it->second.second;
        }
        
        // UDP bytes from raw socket
        auto udp_it = m_udp_cum.find(pid);
        uint64_t udp_in = 0, udp_out = 0;
        if (udp_it != m_udp_cum.end()) {
            udp_in = udp_it->second.first;
            udp_out = udp_it->second.second;
        }
        
        // Total = TCP + UDP
        st.bytes_recv = tcp_in + udp_in;
        st.bytes_sent = tcp_out + udp_out;
        
        // Calculate speed (delta from previous snapshot)
        auto prev_it = m_tcp_prev.find(pid);
        auto uprev_it = m_udp_prev.find(pid);
        uint64_t prev_in = 0, prev_out = 0;
        if (prev_it != m_tcp_prev.end()) { prev_in += prev_it->second.first; prev_out += prev_it->second.second; }
        if (uprev_it != m_udp_prev.end()) { prev_in += uprev_it->second.first; prev_out += uprev_it->second.second; }
        
        if (prev_it != m_tcp_prev.end() || uprev_it != m_udp_prev.end()) {
            int64_t d_in = (int64_t)st.bytes_recv - (int64_t)prev_in;
            int64_t d_out = (int64_t)st.bytes_sent - (int64_t)prev_out;
            st.speed_up = (d_out > 0) ? (double)d_out / dt : 0;
            st.speed_down = (d_in > 0) ? (double)d_in / dt : 0;
        } else {
            st.speed_up = 0;
            st.speed_down = 0;
        }
        m_tcp_prev[pid] = {tcp_in, tcp_out};
        m_udp_prev[pid] = {udp_in, udp_out};
        
        if (st.name.empty() || st.name[0] == L'[') st.name = GetProcessName(pid);
        if (st.exe_path.empty()) st.exe_path = GetProcessPath(pid);
        auto cit = conn_counts.find(pid);
        st.conn_count = (cit != conn_counts.end()) ? cit->second : 0;
        result.push_back(st);
    }

    // Notify listener (for history recording) before sorting
    if (m_on_stats) m_on_stats(result);
    
    // Debug log disabled for release
    // DumpDebugLog(result, dt);

    std::sort(result.begin(), result.end(), [](auto& a, auto& b) { return (a.speed_up + a.speed_down) > (b.speed_up + b.speed_down); });
    // Do NOT reset byte counters - just let them accumulate
    // prev_sent/prev_recv track the last snapshot for delta calculation
    return result;
}

// ============================================================
// Connection Details
// ============================================================

void PacketCapture::RefreshConnectionDetails() {
    std::map<DWORD, std::vector<ConnDetail>> new_cache;
    
    // 1. Get TCP connections
    ULONG tcpSize = 0;
    GetExtendedTcpTable(NULL, &tcpSize, FALSE, AF_INET, TCP_TABLE_OWNER_MODULE_ALL, 0);
    if (tcpSize > 0) {
        std::vector<uint8_t> tcpBuf(tcpSize);
        if (GetExtendedTcpTable(tcpBuf.data(), &tcpSize, FALSE, AF_INET, 
                                TCP_TABLE_OWNER_MODULE_ALL, 0) == NO_ERROR) {
            auto* table = (MIB_TCPTABLE_OWNER_MODULE*)tcpBuf.data();
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                auto& row = table->table[i];
                if (!row.dwOwningPid) continue;
                
                ConnDetail conn;
                conn.protocol = ConnDetail::TCP;
                conn.pid = row.dwOwningPid;
                
                // Local address
                wchar_t local[64];
                struct in_addr local_addr;
                local_addr.s_addr = row.dwLocalAddr;
                swprintf_s(local, L"%S:%u", inet_ntoa(local_addr), 
                          ntohs((u_short)row.dwLocalPort));
                conn.local_addr = local;
                
                // Remote address
                wchar_t remote[64];
                struct in_addr remote_addr;
                remote_addr.s_addr = row.dwRemoteAddr;
                if (row.dwRemoteAddr == 0) {
                    swprintf_s(remote, L"*:*");
                } else {
                    swprintf_s(remote, L"%S:%u", inet_ntoa(remote_addr), 
                              ntohs((u_short)row.dwRemotePort));
                }
                conn.remote_addr = remote;
                
                // TCP state
                switch (row.dwState) {
                case MIB_TCP_STATE_CLOSED:     conn.state = L"CLOSED"; break;
                case MIB_TCP_STATE_LISTEN:     conn.state = L"LISTENING"; break;
                case MIB_TCP_STATE_SYN_SENT:   conn.state = L"SYN_SENT"; break;
                case MIB_TCP_STATE_SYN_RCVD:   conn.state = L"SYN_RCVD"; break;
                case MIB_TCP_STATE_ESTAB:      conn.state = L"ESTABLISHED"; break;
                case MIB_TCP_STATE_FIN_WAIT1:  conn.state = L"FIN_WAIT1"; break;
                case MIB_TCP_STATE_FIN_WAIT2:  conn.state = L"FIN_WAIT2"; break;
                case MIB_TCP_STATE_CLOSE_WAIT: conn.state = L"CLOSE_WAIT"; break;
                case MIB_TCP_STATE_CLOSING:    conn.state = L"CLOSING"; break;
                case MIB_TCP_STATE_LAST_ACK:   conn.state = L"LAST_ACK"; break;
                case MIB_TCP_STATE_TIME_WAIT:  conn.state = L"TIME_WAIT"; break;
                case MIB_TCP_STATE_DELETE_TCB: conn.state = L"DELETE_TCB"; break;
                default:                       conn.state = L"UNKNOWN"; break;
                }
                
                new_cache[row.dwOwningPid].push_back(conn);
            }
        }
    }
    
    // 2. Get UDP connections
    ULONG udpSize = 0;
    GetExtendedUdpTable(NULL, &udpSize, FALSE, AF_INET, UDP_TABLE_OWNER_MODULE, 0);
    if (udpSize > 0) {
        std::vector<uint8_t> udpBuf(udpSize);
        if (GetExtendedUdpTable(udpBuf.data(), &udpSize, FALSE, AF_INET, 
                                UDP_TABLE_OWNER_MODULE, 0) == NO_ERROR) {
            auto* table = (MIB_UDPTABLE_OWNER_MODULE*)udpBuf.data();
            for (DWORD i = 0; i < table->dwNumEntries; i++) {
                if (!table->table[i].dwOwningPid) continue;
                
                ConnDetail conn;
                conn.protocol = ConnDetail::UDP;
                conn.pid = table->table[i].dwOwningPid;
                
                // Local address
                wchar_t local[64];
                struct in_addr local_addr;
                local_addr.s_addr = table->table[i].dwLocalAddr;
                swprintf_s(local, L"%S:%u", inet_ntoa(local_addr), 
                          ntohs((u_short)table->table[i].dwLocalPort));
                conn.local_addr = local;
                
                // UDP has no remote address
                conn.remote_addr = L"*:*";
                conn.state = L"-";
                
                new_cache[table->table[i].dwOwningPid].push_back(conn);
            }
        }
    }
    
    // 3. Update cache
    {
        std::lock_guard<std::mutex> lk(m_conn_mutex);
        m_conn_details_cache = std::move(new_cache);
    }
}

std::vector<ConnDetail> PacketCapture::GetProcessConnections(DWORD pid) {
    std::lock_guard<std::mutex> lk(m_conn_mutex);
    auto it = m_conn_details_cache.find(pid);
    if (it != m_conn_details_cache.end()) {
        return it->second;
    }
    return {};
}

// Parse CIDR notation (e.g. "198.18.0.0/15") and store as {network, mask} in host byte order
void PacketCapture::SetTunRanges(const std::vector<std::wstring>& ranges) {
    m_tun_cidrs.clear();
    for (auto& cidr : ranges) {
        // Convert wstring to string
        char str[64];
        WideCharToMultiByte(CP_UTF8, 0, cidr.c_str(), -1, str, 64, NULL, NULL);
        // Parse "a.b.c.d/prefix"
        unsigned int a, b, c, d, prefix;
        if (sscanf(str, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &prefix) == 5 && prefix <= 32) {
            uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
            uint32_t mask = prefix == 0 ? 0 : (0xFFFFFFFF << (32 - prefix));
            m_tun_cidrs.push_back({ip, mask});
        }
    }
}
