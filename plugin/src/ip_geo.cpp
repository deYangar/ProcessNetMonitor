// ip_geo.cpp - 离线 IP 归属地查询 (ip2region xdb v3) + 自动下载
#include "ip_geo.h"
#include "detail_window.h"
#include "i18n.h"
#include <ws2tcpip.h>
#include <cstring>

// ============ ISO 3166-1 alpha-2 -> 中文国家名 ============
static const struct IsoEntry { const char* code; const wchar_t* name; } kIsoCn[] = {
    { "CN", L"\u4E2D\u56FD" }, { "US", L"\u7F8E\u56FD" }, { "JP", L"\u65E5\u672C" },
    { "KR", L"\u97E9\u56FD" }, { "KP", L"\u671D\u9C9C" }, { "GB", L"\u82F1\u56FD" },
    { "DE", L"\u5FB7\u56FD" }, { "FR", L"\u6CD5\u56FD" }, { "RU", L"\u4FC4\u7F57\u65AF" },
    { "AU", L"\u6FB3\u5927\u5229\u4E9A" }, { "CA", L"\u52A0\u62FF\u5927" }, { "SG", L"\u65B0\u52A0\u5761" },
    { "IN", L"\u5370\u5EA6" }, { "NL", L"\u8377\u5170" }, { "SE", L"\u745E\u5178" },
    { "NO", L"\u632A\u5A01" }, { "FI", L"\u82AC\u5170" }, { "DK", L"\u4E39\u9EA6" },
    { "CH", L"\u745E\u58EB" }, { "IT", L"\u610F\u5927\u5229" }, { "ES", L"\u897F\u73ED\u7259" },
    { "PT", L"\u8461\u8404\u7259" }, { "BE", L"\u6BD4\u5229\u65F6" }, { "AT", L"\u5965\u5730\u5229" },
    { "IE", L"\u7231\u5C14\u5170" }, { "PL", L"\u6CE2\u5170" }, { "CZ", L"\u6377\u514B" },
    { "SK", L"\u65AF\u6D1B\u4F10\u514B" }, { "HU", L"\u5308\u7259\u5229" }, { "RO", L"\u7F57\u9A6C\u5C3C\u4E9A" },
    { "BG", L"\u4FDD\u52A0\u5229\u4E9A" }, { "GR", L"\u5E0C\u814A" }, { "TR", L"\u571F\u8033\u5176" },
    { "UA", L"\u4E4C\u514B\u5170" }, { "BY", L"\u767D\u4FC4\u7F57\u65AF" }, { "KZ", L"\u54C8\u8428\u514B\u65AF\u5766" },
    { "UZ", L"\u4E4C\u5179\u522B\u514B\u65AF\u5766" }, { "AE", L"\u963F\u8054\u914B" }, { "SA", L"\u6C99\u7279\u963F\u62C9\u4F2F" },
    { "IL", L"\u4EE5\u8272\u5217" }, { "IR", L"\u4F0A\u6717" }, { "PK", L"\u5DF4\u57FA\u65AF\u5766" },
    { "BD", L"\u5B5F\u52A0\u62C9\u56FD" }, { "TH", L"\u6CF0\u56FD" }, { "VN", L"\u8D8A\u5357" },
    { "MY", L"\u9A6C\u6765\u897F\u4E9A" }, { "ID", L"\u5370\u5C3C" }, { "PH", L"\u83F2\u5F8B\u5BBE" },
    { "NZ", L"\u65B0\u897F\u5170" }, { "MX", L"\u58A8\u897F\u54E5" }, { "BR", L"\u5DF4\u897F" },
    { "AR", L"\u963F\u6839\u5EF7" }, { "CL", L"\u667A\u5229" }, { "CO", L"\u54E5\u4F26\u6BD4\u4E9A" },
    { "PE", L"\u79D8\u9C81" }, { "ZA", L"\u5357\u975E" }, { "EG", L"\u57C3\u53CA" },
    { "NG", L"\u5C3C\u65E5\u5229\u4E9A" }, { "KE", L"\u80AF\u5C3C\u4E9A" }, { "MA", L"\u6469\u6D1B\u54E5" },
    { "HK", L"\u4E2D\u56FD\u9999\u6E2F" }, { "MO", L"\u4E2D\u56FD\u6FB3\u95E8" }, { "TW", L"\u4E2D\u56FD\u53F0\u6E7E" },
    { "CY", L"\u585E\u6D66\u8DEF\u65AF" }, { "IS", L"\u51B0\u5C9B" }, { "LT", L"\u7ACB\u9676\u5B9B" },
    { "LV", L"\u62C9\u8131\u7EF4\u4E9A" }, { "EE", L"\u7231\u6C99\u5C3C\u4E9A" }, { "SI", L"\u65AF\u6D1B\u6587\u5C3C\u4E9A" },
    { "HR", L"\u514B\u7F57\u5730\u4E9A" }, { "RS", L"\u585E\u5C14\u7EF4\u4E9A" }, { "GE", L"\u683C\u9C81\u5409\u4E9A" },
    { "AM", L"\u4E9A\u7F8E\u5C3C\u4E9A" }, { "AZ", L"\u963F\u585E\u62DC\u7586" }, { "MN", L"\u8499\u53E4" },
    { "CU", L"\u53E4\u5DF4" }, { "VE", L"\u59D4\u5185\u745E\u62C9" }, { "QA", L"\u5361\u5854\u5C14" },
    { "KW", L"\u79D1\u5A01\u7279" }, { "OM", L"\u963F\u66FC" }, { "LK", L"\u65AF\u91CC\u5170\u5361" },
    { "NP", L"\u5C3C\u6CCA\u5C14" }, { "MM", L"\u7F05\u7538" }, { "KH", L"\u67EC\u57D4\u5BE8" },
    { "LA", L"\u8001\u631D" }, { "TJ", L"\u5854\u5409\u514B\u65AF\u5766" }, { "KG", L"\u5409\u5C14\u5409\u65AF\u65AF\u5766" },
    { "TM", L"\u571F\u5E93\u66FC\u65AF\u5766" }, { "MD", L"\u6469\u5C14\u591A\u74E6" }, { "AL", L"\u963F\u5C14\u5DF4\u5C3C\u4E9A" },
    { "MK", L"\u5317\u9A6C\u5176\u987F" }, { "BA", L"\u6CE2\u9ED1" }, { "LU", L"\u5362\u68EE\u5821" },
    { "MC", L"\u6469\u7EB3\u54E5" }, { "MT", L"\u9A6C\u8033\u4ED6" }, { "AD", L"\u5B89\u9053\u5C14" },
    { "LI", L"\u5217\u652F\u6566\u58EB\u767B" }, { "FJ", L"\u6590\u6D4E" }, { "PG", L"\u5DF4\u5E03\u4E9A\u65B0\u51E0\u5185\u4E9A" },
    { "TT", L"\u7279\u7ACB\u5C3C\u8FBE\u548C\u591A\u5DF4\u54E5" }, { "JM", L"\u7259\u4E70\u52A0" },
    { "DO", L"\u591A\u7C73\u5C3C\u52A0" }, { "GT", L"\u5371\u5730\u9A6C\u62C9" }, { "SV", L"\u8428\u5C14\u74E6\u591A" },
    { "HN", L"\u6D2A\u90FD\u62C9\u65AF" }, { "NI", L"\u5C3C\u52A0\u62C9\u74DC" }, { "CR", L"\u54E5\u65AF\u8FBE\u9ECE\u52A0" },
    { "PA", L"\u5DF4\u62FF\u9A6C" }, { "EC", L"\u5384\u74DC\u591A\u5C14" }, { "BO", L"\u73BB\u5229\u7EF4\u4E9A" },
    { "PY", L"\u5DF4\u62C9\u572D" }, { "UY", L"\u4E4C\u62C9\u572D" }, { "TN", L"\u7A81\u5C3C\u65AF" },
    { "DZ", L"\u963F\u5C14\u53CA\u5229\u4E9A" }, { "LY", L"\u5229\u6BD4\u4E9A" }, { "SD", L"\u82CF\u4E39" },
    { "ET", L"\u57C3\u585E\u4FC4\u6BD4\u4E9A" }, { "TZ", L"\u5766\u6851\u5C3C\u4E9A" }, { "UG", L"\u4E4C\u5E72\u8FBE" },
    { "GH", L"\u52A0\u7EB3" }, { "CI", L"\u79D1\u7279\u8FEA\u74E6" }, { "SN", L"\u585E\u5185\u52A0\u5C14" },
    { "CM", L"\u5580\u9EA6\u9686" }, { "ZW", L"\u6D25\u5DF4\u5E03\u97E6" }, { "MZ", L"\u83AB\u6851\u6BD4\u514B" },
    { "AO", L"\u5B89\u54E5\u62C9" }, { "NA", L"\u7EB3\u7C73\u6BD4\u4E9A" }, { "BW", L"\u535A\u8328\u74E6\u7EB3" },
    { "ZM", L"\u8D5E\u6BD4\u4E9A" }, { "MW", L"\u9A6C\u62C9\u7EF4" }, { "MG", L"\u9A6C\u8FBE\u52A0\u65AF\u52A0" },
    { "MU", L"\u6BDB\u91CC\u6C42\u65AF" }, { "SC", L"\u585E\u820C\u5C14" }, { "CV", L"\u4F5B\u5F97\u89D2" },
    { "HT", L"\u6D77\u5730" }, { "PR", L"\u6CE2\u591A\u9ECE\u5404" }, { "GU", L"\u5173\u5C9B" },
    { "PF", L"\u6CD5\u5C5E\u6CE2\u5229\u5C3C\u897F\u4E9A" }, { "IO", L"\u82F1\u5C5E\u5370\u5EA6\u6D0B\u9886\u5730" },
};

std::wstring IpGeo::GetCountryName(const std::string& iso) {
    for (const auto& e : kIsoCn) {
        if (iso == e.code) return e.name;
    }
    return L"";
}

// ============ xdb 查询 ============
// 头部: [2B ver][2B algo][4B ts][4B seg_start][4B seg_end][2B ipver][2B ptr_size] = 16B, 头部总长 256B
// vIndex: 256B 起, 256x256 条 x 8B = 512KiB, 条目 [4B seg_start][4B seg_end]
// 段索引(IPv4): 14B = [4B start_ip][4B end_ip][2B data_len][4B data_ptr]
std::string IpGeo::XdbSearch(const std::vector<uint8_t>& buf, uint32_t ip) {
    if (buf.size() < 256 + 512 * 1024) return "";
    uint32_t seg_start = 0, seg_end = 0;
    memcpy(&seg_start, buf.data() + 8, 4);
    memcpy(&seg_end, buf.data() + 12, 4);
    if (seg_start == 0 || seg_end > buf.size() || seg_start >= seg_end) return "";

    uint32_t idx = ((ip >> 16) & 0xFFFF) * 8;
    if (256 + idx + 8 > buf.size()) return "";
    uint32_t s_ptr = 0, e_ptr = 0;
    memcpy(&s_ptr, buf.data() + 256 + idx, 4);
    memcpy(&e_ptr, buf.data() + 256 + idx + 4, 4);
    if (s_ptr == 0 || e_ptr == 0 || e_ptr > seg_end || s_ptr < seg_start) return "";

    const uint32_t SEG = 14;
    uint32_t lo = 0, hi = (e_ptr - s_ptr) / SEG;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        uint32_t p = s_ptr + m * SEG;
        if (p + 14 > buf.size()) break;
        uint32_t s = 0, e = 0, doff = 0;
        uint16_t len = 0;
        memcpy(&s, buf.data() + p, 4);
        memcpy(&e, buf.data() + p + 4, 4);
        memcpy(&len, buf.data() + p + 8, 2);
        memcpy(&doff, buf.data() + p + 10, 4);
        if (ip < s) {
            hi = m;
        } else if (ip > e) {
            lo = m + 1;
        } else {
            if ((uint64_t)doff + len <= buf.size() && len > 0) {
                return std::string((const char*)buf.data() + doff, len);
            }
            return "";
        }
    }
    return "";
}

// 段索引(IPv6): 38B = [16B start_ip][16B end_ip][2B data_len][4B data_ptr]
std::string IpGeo::XdbSearchV6(const std::vector<uint8_t>& buf, const uint8_t ip[16]) {
    if (buf.size() < 256 + 512 * 1024) return "";
    uint32_t seg_start = 0, seg_end = 0;
    memcpy(&seg_start, buf.data() + 8, 4);
    memcpy(&seg_end, buf.data() + 12, 4);
    if (seg_start == 0 || seg_end > buf.size() || seg_start >= seg_end) return "";

    uint32_t idx = ((uint32_t)ip[0] * 256 + ip[1]) * 8;
    if (256 + idx + 8 > buf.size()) return "";
    uint32_t s_ptr = 0, e_ptr = 0;
    memcpy(&s_ptr, buf.data() + 256 + idx, 4);
    memcpy(&e_ptr, buf.data() + 256 + idx + 4, 4);
    if (s_ptr == 0 || e_ptr == 0 || e_ptr > seg_end || s_ptr < seg_start) return "";

    const uint32_t SEG = 38;
    uint32_t lo = 0, hi = (e_ptr - s_ptr) / SEG;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        uint32_t p = s_ptr + m * SEG;
        if (p + 38 > buf.size()) break;
        const uint8_t* s = buf.data() + p;
        const uint8_t* e = buf.data() + p + 16;
        int cs = memcmp(ip, s, 16);
        if (cs < 0) {
            hi = m;
        } else {
            int ce = memcmp(ip, e, 16);
            if (ce > 0) {
                lo = m + 1;
            } else {
                uint16_t len = 0;
                uint32_t doff = 0;
                memcpy(&len, buf.data() + p + 32, 2);
                memcpy(&doff, buf.data() + p + 34, 4);
                if ((uint64_t)doff + len <= buf.size() && len > 0) {
                    return std::string((const char*)buf.data() + doff, len);
                }
                return "";
            }
        }
    }
    return "";
}
std::wstring IpGeo::FormatRegion(const std::string& region) {
    if (region.empty()) return L"";
    // 拆 '|'
    std::string parts[5];
    int pi = 0;
    size_t start = 0;
    for (size_t i = 0; i <= region.size() && pi < 5; i++) {
        if (i == region.size() || region[i] == '|') {
            parts[pi++] = region.substr(start, i - start);
            start = i + 1;
        }
    }
    if (pi < 5) return L"";
    const std::string& country = parts[0];
    const std::string& prov = parts[1];
    const std::string& city = parts[2];
    const std::string& isp = parts[3];
    const std::string& iso = parts[4];

    std::wstring out;
    auto utf8_to_wide = [](const std::string& s) -> std::wstring {
        if (s.empty() || s == "0") return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
        if (n <= 0) return L"";
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    };

    if (I18n::IsChinese()) {
        if (iso == "CN") {
            out = L"\u4E2D\u56FD";  // 中国
            std::wstring wp = utf8_to_wide(prov), wc = utf8_to_wide(city), wi = utf8_to_wide(isp);
            if (!wp.empty()) out += L" " + wp;
            if (!wc.empty()) out += L" " + wc;
            if (!wi.empty()) out += L" " + wi;
        } else {
            std::wstring cn = GetCountryName(iso);
            if (!cn.empty()) out = cn; else out = utf8_to_wide(country);
            std::wstring wp = utf8_to_wide(prov), wc = utf8_to_wide(city), wi = utf8_to_wide(isp);
            if (!wp.empty()) out += L" " + wp;
            if (!wc.empty()) out += L" " + wc;
            if (!wi.empty()) out += L" " + wi;
        }
    } else {
        // 非中文界面：直接用 xdb 数据原文（ip2region_v4 数据国外字段为英文国家/省/市）
        if (iso == "CN") {
            out = TR(L"\u4E2D\u56FD");  // 国内段数据是中文，非中文界面只显示国家名
        } else {
            out = utf8_to_wide(country);
            std::wstring wp = utf8_to_wide(prov), wc = utf8_to_wide(city), wi = utf8_to_wide(isp);
            if (!wp.empty()) out += L" " + wp;
            if (!wc.empty()) out += L" " + wc;
            if (!wi.empty()) out += L" " + wi;
        }
    }
    return out;
}

// ============ 实例 ============
IpGeo& IpGeo::Instance() {
    static IpGeo s_inst;
    return s_inst;
}

IpGeo::~IpGeo() {
    if (m_dl_thread.joinable()) m_dl_thread.join();
}

const wchar_t* IpGeo::StateText() const {
    switch (m_state.load()) {
    case DbState::Downloading: return TR(L"\u4E0B\u8F7DIP\u5E93\u2026");  // 下载IP库…
    case DbState::Failed:      return TR(L"IP\u5E93\u5931\u8D25");        // IP库失败
    case DbState::NoDb:        return TR(L"IP\u5E93\u672A\u5C31\u7EEA");  // IP库未就绪
    default:                   return L"";
    }
}

void IpGeo::EnsureDatabase(const std::wstring& dll_dir) {
    bool start = false, force = false, need_load = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_dll_dir = dll_dir;
        m_db_path = dll_dir + L"\\ip2region.xdb";
        m_db6_path = dll_dir + L"\\ip2region_v6.xdb";
        // 清理上次崩溃/中断遗留的下载临时文件
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW((dll_dir + L"\\*.tmp").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                DeleteFileW((dll_dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        // 功能关闭: 不加载不下载
        if (!m_enabled) return;
        // 已有文件则标记加载 (IO 在锁外做)
        if (m_state.load() != DbState::Ready) {
            DWORD attr = GetFileAttributesW(m_db_path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                need_load = true;
            }
        }
        // 下载条件: v4 未就绪 或 v6 缺失 或 任一过期
        DWORD attr6 = GetFileAttributesW(m_db6_path.c_str());
        bool v6_ok = (attr6 != INVALID_FILE_ATTRIBUTES && !(attr6 & FILE_ATTRIBUTE_DIRECTORY));
        bool stale4 = !m_buf.empty() && IsFileStale(m_db_path, m_update_days);
        bool stale6 = !m_buf6.empty() && IsFileStale(m_db6_path, m_update_days);
        bool need_dl = (m_state.load() != DbState::Ready) || !v6_ok || stale4 || stale6;
        force = stale4 || stale6;
        if (need_dl && !m_download_started.exchange(true)) {
            start = true;
        }
    }
    if (need_load) LoadDb();  // 锁外加载 (内部自加锁)
    if (start) {
        if (m_state.load() != DbState::Ready) m_state.store(DbState::Downloading);
        // 旧线程可能正在收尾 (m_download_started 已复位但线程未完全退出):
        // 不 join 直接赋值 std::thread 会触发 std::terminate 崩溃!
        if (m_dl_thread.joinable()) m_dl_thread.join();
        m_dl_thread = std::thread(&IpGeo::DownloadThread, this, dll_dir, force);
    }
}

void IpGeo::SetEnabled(bool on) {
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_enabled = on;
        m_fail_notified.store(false);
        if (!on) {
            m_buf.clear();
            m_buf6.clear();
            m_state.store(DbState::NoDb);
        }
    }
    if (on && !m_dll_dir.empty()) {
        EnsureDatabase(m_dll_dir);
    }
}

void IpGeo::ForceUpdate() {
    if (!m_enabled) return;
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_download_started.exchange(true)) start = true;
    }
    if (start) {
        if (m_state.load() != DbState::Ready) m_state.store(DbState::Downloading);
        if (m_dl_thread.joinable()) m_dl_thread.join();
        m_dl_thread = std::thread(&IpGeo::DownloadThread, this, m_dll_dir, true);
    }
}

// 文件修改时间距今是否超过 days 天
bool IpGeo::IsFileStale(const std::wstring& path, int days) {
    if (days <= 0) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_u{ now_ft.dwLowDateTime, now_ft.dwHighDateTime };
    ULARGE_INTEGER mtime{ fad.ftLastWriteTime.dwLowDateTime, fad.ftLastWriteTime.dwHighDateTime };
    ULONGLONG age = now_u.QuadPart - mtime.QuadPart;
    ULONGLONG limit = (ULONGLONG)days * 24 * 3600 * 10000000ULL;
    return age > limit;
}

// 读取并校验单个 xdb 文件 (纯 IO, 无锁; 调用方保证路径稳定)
static std::vector<uint8_t> ReadXdbFile(const std::wstring& path, bool is_v6) {
    std::vector<uint8_t> out;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz{};
    GetFileSizeEx(hFile, &sz);
    if (sz.QuadPart > 8 && sz.QuadPart < (1LL << 30)) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0, total = 0;
        while (total < out.size()) {
            if (!ReadFile(hFile, out.data() + total, (DWORD)(out.size() - total), &rd, NULL) || rd == 0) break;
            total += rd;
        }
        out.resize(total);
    }
    CloseHandle(hFile);
    if (out.size() < 256 + 512 * 1024) { out.clear(); return out; }
    uint16_t ver = 0;
    uint32_t seg_start = 0, seg_end = 0;
    memcpy(&ver, out.data(), 2);
    memcpy(&seg_start, out.data() + 8, 4);
    memcpy(&seg_end, out.data() + 12, 4);
    bool ok = (ver == 2 || ver == 3) && seg_start >= 256 + 512 * 1024 &&
              seg_end <= out.size() && seg_start < seg_end;
    if (is_v6) {
        uint16_t ipver = 0;
        memcpy(&ipver, out.data() + 16, 2);
        ok = ok && (ipver == 6);
    }
    if (!ok) out.clear();
    return out;
}

// 加载 v4/v6 库: 文件 IO 在锁外 (不阻塞查询线程), 短锁换入内存
void IpGeo::LoadDb() {
    std::wstring db_path, db6_path;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        db_path = m_db_path;
        db6_path = m_db6_path;
    }
    std::vector<uint8_t> buf = ReadXdbFile(db_path, false);
    std::vector<uint8_t> buf6 = ReadXdbFile(db6_path, true);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!buf.empty()) m_buf = std::move(buf);
        if (!buf6.empty()) m_buf6 = std::move(buf6);
        if (!m_buf.empty()) m_state.store(DbState::Ready);
        else m_state.store(DbState::Failed);
    }
}

// ============ 下载 ============
// curl 子进程纳入 Job Object: 父进程(TM)退出时自动杀掉所有 curl (KILL_ON_JOB_CLOSE),
// 防止孤儿下载进程残留
static HANDLE g_curl_job = nullptr;
static HANDLE GetCurlJob() {
    if (!g_curl_job) {
        g_curl_job = CreateJobObjectW(NULL, NULL);
        if (g_curl_job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_curl_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
    }
    return g_curl_job;
}

// curl.exe 子进程下载 (Win10 1803+ 自带; schannel 吊销检查 best-effort, 大陆直连可用)
// 返回 true 且退出码 0 = 成功写盘
static bool DownloadWithCurl(const std::wstring& url, const std::wstring& dest, const std::wstring& proxy) {
    // 连接超时 60s, 单源总限时 300s (v6 37MB 大文件需要充足传输时间)
    std::wstring cmd = L"curl.exe -4 -sL --connect-timeout 60 --max-time 300";
    if (!proxy.empty()) {
        cmd += L" -x \"" + proxy + L"\"";
    }
    cmd += L" -o \"" + dest + L"\" \"" + url + L"\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(NULL, cmdline.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return false;
    // 加入 Job: TM 退出时自动终止 (失败可忽略, 仅尽力而为)
    HANDLE job = GetCurlJob();
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    // 等待最多 305s
    DWORD wait = WaitForSingleObject(pi.hProcess, 305000);
    if (wait == WAIT_TIMEOUT) {
        // curl 卡死: 强制结束, 防止残留 tmp / 僵尸进程
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// 下载任务: 主源优先(CDN/国内站), GitHub raw 兜底 (大陆部分地区直连不通)
struct DlTask {
    const wchar_t* dest_name;   // 目标文件名
    const wchar_t* sources[5];  // 候选源
};
void IpGeo::DownloadThread(std::wstring dll_dir, bool force) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    int update_days = 7;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        update_days = m_update_days;
    }

    static const DlTask kTasks[] = {
        {
            L"ip2region.xdb",
            {
                L"https://cdn.jsdelivr.net/gh/lionsoul2014/ip2region@master/data/ip2region_v4.xdb",
                L"https://ip.adysec.com/ip2region/ip2region.xdb",
                L"https://fastly.jsdelivr.net/gh/lionsoul2014/ip2region@master/data/ip2region_v4.xdb",
                L"https://gcore.jsdelivr.net/gh/lionsoul2014/ip2region@master/data/ip2region_v4.xdb",
                L"https://raw.githubusercontent.com/lionsoul2014/ip2region/master/data/ip2region_v4.xdb",
            }
        },
        {
            L"ip2region_v6.xdb",
            {
                L"https://raw.githubusercontent.com/lionsoul2014/ip2region/master/data/ip2region_v6.xdb",
                L"https://cdn.staticaly.com/gh/lionsoul2014/ip2region@master/data/ip2region_v6.xdb",
                L"https://fastly.jsdelivr.net/gh/lionsoul2014/ip2region@master/data/ip2region_v6.xdb",
                L"https://gcore.jsdelivr.net/gh/lionsoul2014/ip2region@master/data/ip2region_v6.xdb",
                nullptr,
            }
        },
    };

    for (const auto& task : kTasks) {
        std::wstring dest = dll_dir + L"\\" + task.dest_name;
        // 已存在且有效且未过期则跳过; force(立即更新/过期)时重新下载
        WIN32_FILE_ATTRIBUTE_DATA chk{};
        if (!force && GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &chk) &&
            chk.nFileSizeHigh == 0 && chk.nFileSizeLow > 256 + 512 * 1024) {
            continue;
        }
        std::wstring tmp = dest + L".tmp";
        DeleteFileW(tmp.c_str());
        bool ok = false;
        // 每个源开始前重新读取代理设置 (设置中改代理后, 下一个源即生效)
        for (int si = 0; si < 5 && task.sources[si]; si++) {
            std::wstring cur_proxy;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                cur_proxy = m_proxy;
            }
            if (DownloadWithCurl(task.sources[si], tmp, cur_proxy)) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(tmp.c_str(), GetFileExInfoStandard, &fad) &&
                    fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 256 + 512 * 1024) {
                    ok = true;
                    break;
                }
            }
            DeleteFileW(tmp.c_str());
        }
        if (ok) {
            MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING);
        } else {
            DeleteFileW(tmp.c_str());
        }
    }

    LoadDb();  // 锁外加载 (内部自加锁, 不阻塞查询线程)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_download_started.store(false);
        // 下载成功则复位失败提示标志 (下次失败再提示)
        if (m_state.load() == DbState::Ready) m_fail_notified.store(false);
    }
    // 首次下载失败: 提示用户可配代理或关闭功能 (PostMessage 到 UI 线程弹窗)
    if (m_state.load() != DbState::Ready && !m_fail_notified.exchange(true)) {
        if (CDetailWindow::s_instance && CDetailWindow::s_instance->GetHwnd()) {
            PostMessageW(CDetailWindow::s_instance->GetHwnd(), WM_APP + 5, 0, 0);
        } else {
            // 兜底: 详情窗口不可用时直接弹 (后台线程弹窗不阻塞 UI)
            MessageBoxW(NULL,
                L"IP \u5F52\u5C5E\u5730\u6570\u636E\u5E93\u4E0B\u8F7D\u5931\u8D25\u3002\n\u53EF\u5728 \u63D2\u4EF6\u8BBE\u7F6E \u4E2D\u914D\u7F6E\u4EE3\u7406\u670D\u52A1\u5668\uFF0C\u6216\u5173\u95ED\u201C\u542F\u7528\u8FDE\u63A5\u5F52\u5C5E\u5730\u663E\u793A\u201D\u3002",
                L"ProcessNetMonitor", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
    }
}

// ============ 查询 ============
std::wstring IpGeo::Query(const std::wstring& remote_addr) {
    if (!m_enabled || remote_addr.empty()) return L"";
    // 提取 IP 部分
    std::wstring ip = remote_addr;
    if (ip[0] == L'[') {  // IPv6 "[::1]:443"
        size_t close = ip.find(L']');
        if (close == std::wstring::npos) return L"";
        ip = ip.substr(1, close - 1);
    } else {
        size_t colon = ip.find(L':');
        if (colon != std::wstring::npos) ip = ip.substr(0, colon);
    }
    if (ip.empty() || ip == L"*") return L"";

    // 缓存
    {
        std::lock_guard<std::mutex> lk(m_cache_mutex);
        auto it = m_cache.find(ip);
        if (it != m_cache.end()) return it->second;
    }

    DbState st = m_state.load();
    if (st != DbState::Ready) return StateText();

    std::wstring result;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        result = Lookup(ip);
    }
    if (result.empty()) result = L"-";

    {
        std::lock_guard<std::mutex> lk(m_cache_mutex);
        m_cache[ip] = result;
        m_cache_order.push_back(ip);
        while (m_cache_order.size() > MAX_CACHE) {
            m_cache.erase(m_cache_order.front());
            m_cache_order.erase(m_cache_order.begin());
        }
    }
    return result;
}

std::wstring IpGeo::Lookup(const std::wstring& ip) {
    std::string a;
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, ip.c_str(), (int)ip.size(), NULL, 0, NULL, NULL);
        if (n > 0) {
            a.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, ip.c_str(), (int)ip.size(), &a[0], n, NULL, NULL);
        }
    }
    // ---- IPv4 ----
    uint32_t v4 = 0;
    if (inet_pton(AF_INET, a.c_str(), &v4) == 1) {
        v4 = ntohl(v4);
        // 内网/本机/链路地址: 不查, 显示 "-"
        if ((v4 & 0xFF000000) == 0x00000000) return L"";         // 0.0.0.0/8 本机
        if ((v4 & 0xFF000000) == 0x0A000000) return L"";         // 10.0.0.0/8 专用网络
        if ((v4 & 0xFF000000) == 0x7F000000) return L"";         // 127.0.0.0/8 环回
        if ((v4 & 0xFFFF0000) == 0xA9FE0000) return L"";         // 169.254.0.0/16 链路本地
        if ((v4 & 0xFFF00000) == 0xAC100000) return L"";         // 172.16.0.0/12 专用网络
        if ((v4 & 0xFFFF0000) == 0xC0A80000) return L"";         // 192.168.0.0/16 专用网络
        if ((v4 & 0xFFC00000) == 0x64400000) return L"";         // 100.64.0.0/10 电信级NAT
        if ((v4 & 0xF0000000) == 0xE0000000) return L"";         // 224.0.0.0/4 组播
        // 特殊保留段: 显示 "保留地址"
        if ((v4 & 0xFFFFFF00) == 0xC0000000) return TR(L"\u4FDD\u7559\u5730\u5740");  // 192.0.0.0/24 IANA
        if ((v4 & 0xFFFFFF00) == 0xC0000200) return TR(L"\u4FDD\u7559\u5730\u5740");  // 192.0.2.0/24 TEST-NET
        if ((v4 & 0xFFFFFF00) == 0xC0586300) return TR(L"\u4FDD\u7559\u5730\u5740");  // 192.88.99.0/24 6to4
        if ((v4 & 0xFFFE0000) == 0xC6120000) return TR(L"\u4FDD\u7559\u5730\u5740");  // 198.18.0.0/15 基准测试
        if ((v4 & 0xFFFFFF00) == 0xC6336400) return TR(L"\u4FDD\u7559\u5730\u5740");  // 198.51.100.0/24 TEST-NET-2
        if ((v4 & 0xFFFFFF00) == 0xCB007100) return TR(L"\u4FDD\u7559\u5730\u5740");  // 203.0.113.0/24 TEST-NET-3
        if ((v4 & 0xFFFFFF00) == 0xE9FC0000) return TR(L"\u4FDD\u7559\u5730\u5740");  // 233.252.0.0/24 MCAST-TEST-NET
        if ((v4 & 0xF0000000) == 0xF0000000) return TR(L"\u4FDD\u7559\u5730\u5740");  // 240.0.0.0/4 保留

        std::string region = XdbSearch(m_buf, v4);
        if (region.empty()) return L"";
        return FormatRegion(region);
    }

    // ---- IPv6 ----
    uint8_t b6[16] = {};
    if (inet_pton(AF_INET6, a.c_str(), b6) != 1) return L"";
    // ::ffff:0:0/96 IPv4 映射地址 -> 转 IPv4 查询
    {
        bool v4mapped = true;
        for (int i = 0; i < 10; i++) if (b6[i] != 0) { v4mapped = false; break; }
        if (v4mapped && b6[10] == 0xFF && b6[11] == 0xFF) {
            char v4buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, b6 + 12, v4buf, sizeof(v4buf))) {
                return Lookup(std::wstring(v4buf, v4buf + strlen(v4buf)));
            }
        }
    }
    // 保留/特殊 IPv6: 显示 "保留地址" 或 "-"
    {
        bool all_zero = true;
        for (int i = 0; i < 16; i++) if (b6[i] != 0) { all_zero = false; break; }
        if (all_zero) return TR(L"\u4FDD\u7559\u5730\u5740");                    // ::/128 未指定
        bool loopback = true;
        for (int i = 0; i < 15; i++) if (b6[i] != 0) { loopback = false; break; }
        if (loopback && b6[15] == 1) return L"";                             // ::1/128 环回
        if ((b6[0] & 0xFE) == 0xFC) return L"";                              // fc00::/7 专用网络
        if (b6[0] == 0xFE && (b6[1] & 0xC0) == 0x80) return L"";             // fe80::/10 链路本地
        if (b6[0] == 0xFF) return TR(L"\u4FDD\u7559\u5730\u5740");             // ff00::/8 组播
        if (b6[0] == 0x01 && b6[1] == 0x00 && b6[2] == 0x00 && b6[3] == 0x00) return TR(L"\u4FDD\u7559\u5730\u5740");  // 100::/64 黑洞
        if (b6[0] == 0x64 && b6[1] == 0xFF && b6[2] == 0x9B && b6[3] == 0x00) return TR(L"\u4FDD\u7559\u5730\u5740");  // 64:ff9b::/96 NAT64
        if (b6[0] == 0x20 && b6[1] == 0x01 && b6[2] == 0x0D && b6[3] == 0xB8) return TR(L"\u4FDD\u7559\u5730\u5740");  // 2001:db8::/32 文档
        if (b6[0] == 0x20 && b6[1] == 0x01 && b6[2] == 0x00 && b6[3] == 0x00) return TR(L"\u4FDD\u7559\u5730\u5740");  // 2001::/32 Teredo
        if (b6[0] == 0x20 && b6[1] == 0x02) return TR(L"\u4FDD\u7559\u5730\u5740");  // 2002::/16 6to4
    }
    if (m_buf6.empty()) return L"";
    std::string region = XdbSearchV6(m_buf6, b6);
    if (region.empty()) return L"";
    return FormatRegion(region);
}
