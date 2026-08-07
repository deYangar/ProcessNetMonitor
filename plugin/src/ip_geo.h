#pragma once
// ip_geo.h - 离线 IP 归属地查询 (ip2region xdb v3)
// - 首次加载时若 DLL 同目录无 ip2region.xdb, 后台线程自动下载 (adysec 主源 + GitHub 备用)
// - 查询: vIndex(256B头+512KiB) + 14B 段索引二分, 全内存, 微秒级
// - 国外国家名用 ISO 3166 中文映射表本地化 (方案A)
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>

class IpGeo {
public:
    enum class DbState { NoDb, Downloading, Ready, Failed };

    static IpGeo& Instance();

    // 检查 DLL 同目录数据库, 不存在则启动后台下载 (幂等, 不阻塞 UI)
    void EnsureDatabase(const std::wstring& dll_dir);

    // 查询归属地; remote_addr 支持 "1.2.3.4:443" / "[::1]:443" / "1.2.3.4"
    // 本地地址返回 L""; 库未就绪返回 L"\u4E0B\u8F7D\u4E2D\u2026"(下载中…)/失败文案
    std::wstring Query(const std::wstring& remote_addr);

    DbState State() const { return m_state.load(); }
    const wchar_t* StateText() const;

    // ---- 设置 (由 detail_window 持久化到 settings.json) ----
    const std::wstring& GetProxy() const { return m_proxy; }
    void SetProxy(const std::wstring& v) { m_proxy = v; }
    int GetUpdateDays() const { return m_update_days; }
    void SetUpdateDays(int v) { m_update_days = v; }
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool on);
    // 立即后台更新 IP 库 (失败保留旧文件)
    void ForceUpdate();

private:
    IpGeo() = default;
    ~IpGeo();
    IpGeo(const IpGeo&) = delete;
    IpGeo& operator=(const IpGeo&) = delete;

    void LoadDb();
    void DownloadThread(std::wstring dll_dir, bool force);
    std::wstring Lookup(const std::wstring& ip);
    static std::string XdbSearch(const std::vector<uint8_t>& buf, uint32_t ip);
    static std::string XdbSearchV6(const std::vector<uint8_t>& buf, const uint8_t ip[16]);
    static std::wstring FormatRegion(const std::string& region);
    static std::wstring GetCountryName(const std::string& iso);
    static bool IsFileStale(const std::wstring& path, int days);

    // 数据库
    std::mutex m_mutex;                 // 保护 m_buf / m_buf6 / m_state
    std::vector<uint8_t> m_buf;         // IPv4 xdb
    std::vector<uint8_t> m_buf6;        // IPv6 xdb
    std::atomic<DbState> m_state{ DbState::NoDb };
    std::atomic<bool> m_download_started{ false };
    std::thread m_dl_thread;
    std::wstring m_db_path;             // IPv4 xdb 路径
    std::wstring m_db6_path;            // IPv6 xdb 路径
    std::wstring m_dll_dir;             // DLL 目录

    // 设置 (内存态, detail_window 负责持久化到 settings.json)
    std::wstring m_proxy;               // 代理 (空=直连)
    int m_update_days = 7;              // 自动更新间隔(天)
    bool m_enabled = true;              // 启用归属地显示 (关闭则不下载)
    std::atomic<bool> m_fail_notified{ false };  // 首次下载失败已提示

    // 查询缓存 (FIFO 淘汰)
    std::mutex m_cache_mutex;
    std::unordered_map<std::wstring, std::wstring> m_cache;
    std::vector<std::wstring> m_cache_order;
    static const size_t MAX_CACHE = 8192;
};
