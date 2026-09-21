// i18n.cpp - 轻量本地化模块实现
// 语言文件格式与 TrafficMonitor 的 language\*.ini 一致：
//   [general]
//   BCP_47 = "zh-CN"
//   DISPLAY_NAME = "简体中文"
//   [text]
//   KEY = "***"
// 支持 UTF-8（带/不带 BOM）与 UTF-16 LE/BE。值内支持 \" \\ \n \r \t 转义。
#include "i18n.h"
#include <windows.h>
#include <vector>
#include <memory>

namespace {

std::map<std::wstring, std::wstring> g_table;
SRWLOCK g_lock = SRWLOCK_INIT;  // 保护 g_table + 下面的全局语言状态
// （定时器线程 CheckAndReload/Reload/ScanLangFiles vs UI 线程 TR/Get/选项对话框
// 并发读写曾导致堆损坏 0xc0000374——所有全局状态必须持锁访问）
// Reload 时旧表不立即析构：保留若干份，避免 Get() 返回的指针在调用方使用期间失效
std::vector<std::shared_ptr<std::map<std::wstring, std::wstring>>> g_old_tables;
std::vector<I18n::LangInfo> g_lang_list;   // 扫描到的语言列表
std::wstring g_lang_dir;                   // 语言文件目录
std::wstring g_mode = L"auto";             // auto 或具体 BCP-47
std::wstring g_tm_lang;                    // TM 主程序当前语言（auto 模式匹配用）

// 读取文件全部字节
bool ReadAllBytes(const std::wstring& path, std::vector<BYTE>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(h); return false; }
    out.resize(size);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), size, &read, nullptr);
    CloseHandle(h);
    return ok && read == size;
}

// 字节流 -> 宽字符串（自动识别 UTF-8 / UTF-16 LE / UTF-16 BE，兼容 BOM）
std::wstring BytesToWstring(const std::vector<BYTE>& bytes) {
    const BYTE* p = bytes.data();
    size_t n = bytes.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }  // UTF-8 BOM
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {  // UTF-16 LE BOM
        return std::wstring((const wchar_t*)(p + 2), (n - 2) / 2);
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {  // UTF-16 BE BOM
        std::wstring out;
        out.resize((n - 2) / 2);
        for (size_t i = 0; i < out.size(); i++) {
            out[i] = (wchar_t)(((wchar_t)p[i * 2 + 2] << 8) | p[i * 2 + 3]);
        }
        return out;
    }
    // 默认 UTF-8
    int len = MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, &out[0], len);
    return out;
}

// 去掉首尾空白（含全角空格）
std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\u3000')) b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\u3000')) e--;
    return s.substr(b, e - b);
}

// 解析 KEY = "***" 的值（去引号 + 转义）
std::wstring ParseValue(const std::wstring& raw) {
    std::wstring v = Trim(raw);
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') {
        v = v.substr(1, v.size() - 2);
    }
    std::wstring out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == L'\\' && i + 1 < v.size()) {
            wchar_t c = v[++i];
            switch (c) {
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'"': out += L'"'; break;
            case L'\\': out += L'\\'; break;
            default: out += L'\\'; out += c; break;
            }
        } else {
            out += v[i];
        }
    }
    return out;
}

// 读取 ini 的 [general] 与 [text] section；返回是否解析到内容
bool ParseIni(const std::wstring& content, std::map<std::wstring, std::wstring>& table) {
    std::wstring section;
    size_t pos = 0;
    bool any = false;
    while (pos < content.size()) {
        size_t eol = content.find(L'\n', pos);
        std::wstring line = content.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = (eol == std::wstring::npos) ? content.size() : eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;

        if (line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        // Find ' = ' (with spaces) first; fall back to first '=' if not found.
        // Keys can contain '=' without spaces (e.g. "留空=直连"), so bare '='
        // would split inside the key.  The real separator always has spaces.
        size_t eq = line.find(L" = ");
        size_t val_start;
        if (eq != std::wstring::npos) {
            val_start = eq + 3;  // skip past " = "
        } else {
            eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            val_start = eq + 1;
        }
        std::wstring key = ParseValue(Trim(line.substr(0, eq)));  // key 同样支持 \n 等转义
        std::wstring value = ParseValue(line.substr(val_start));
        if (key.empty()) continue;
        if (section.empty() || section == L"text") {
            table[key] = value;
            any = true;
        }
    }
    return any;
}

// 小写化
std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = (wchar_t)towlower(c);
    return out;
}

// BCP-47 主语言（- 之前的小写部分）
std::wstring MainLang(const std::wstring& bcp47) {
    size_t dash = bcp47.find(L'-');
    return ToLower(bcp47.substr(0, dash));
}

} // namespace

namespace I18n {

bool Load(const std::wstring& file_path) {
    std::vector<BYTE> bytes;
    if (!ReadAllBytes(file_path, bytes)) return false;
    std::wstring content = BytesToWstring(bytes);
    if (content.empty()) return false;

    std::map<std::wstring, std::wstring> new_table;
    if (!ParseIni(content, new_table)) return false;
    AcquireSRWLockExclusive(&g_lock);
    // 旧表保留（延迟析构），防止其他线程正在使用 Get() 返回的指针
    g_old_tables.push_back(std::make_shared<std::map<std::wstring, std::wstring>>(std::move(g_table)));
    if (g_old_tables.size() > 8) g_old_tables.erase(g_old_tables.begin());
    g_table = std::move(new_table);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool ScanLangFiles(const std::wstring& dir) {
    std::vector<LangInfo> list;   // 本地构建，IO 全程无锁；最后短锁换入
    std::wstring pattern = dir + L"\\*.ini";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file_name = fd.cFileName;
        std::wstring file_path = dir + L"\\" + file_name;
        std::vector<BYTE> bytes;
        if (!ReadAllBytes(file_path, bytes)) continue;
        std::wstring content = BytesToWstring(bytes);
        if (content.empty()) continue;
        // 只解析 [general] section 拿 BCP_47 / DISPLAY_NAME
        std::wstring section;
        std::wstring bcp47, display;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t eol = content.find(L'\n', pos);
            std::wstring line = content.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
            pos = (eol == std::wstring::npos) ? content.size() : eol + 1;
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            line = Trim(line);
            if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
            if (line.front() == L'[' && line.back() == L']') {
                section = Trim(line.substr(1, line.size() - 2));
                continue;
            }
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = Trim(line.substr(0, eq));
            if (section == L"general") {
                std::wstring value = ParseValue(line.substr(eq + 1));
                if (key == L"BCP_47") bcp47 = value;
                else if (key == L"DISPLAY_NAME") display = value;
            }
        }
        if (bcp47.empty()) continue;
        LangInfo info;
        info.bcp47 = bcp47;
        info.display_name = display.empty() ? file_name : display;
        info.file_name = file_name;
        // 文件修改时间（100ns 间隔，自 1601-01-01）
        ULARGE_INTEGER li;
        li.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        li.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        info.mtime = li.QuadPart;
        list.push_back(info);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    if (list.empty()) return false;
    AcquireSRWLockExclusive(&g_lock);
    g_lang_dir = dir;
    g_lang_list = std::move(list);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

void SetLang(const std::wstring& bcp47_or_auto) {
    AcquireSRWLockExclusive(&g_lock);
    g_mode = bcp47_or_auto.empty() ? L"auto" : bcp47_or_auto;
    ReleaseSRWLockExclusive(&g_lock);
}

std::wstring GetLang() {
    AcquireSRWLockShared(&g_lock);
    std::wstring m = g_mode;
    ReleaseSRWLockShared(&g_lock);
    return m;
}

bool IsChinese() {
    AcquireSRWLockShared(&g_lock);
    std::wstring mode = g_mode, tm = g_tm_lang;
    ReleaseSRWLockShared(&g_lock);
    std::wstring lang = (mode == L"auto") ? tm : mode;
    if (lang.empty()) lang = L"zh-CN";
    return MainLang(lang) == L"zh";
}

void SetTmLang(const std::wstring& bcp47) {
    AcquireSRWLockExclusive(&g_lock);
    g_tm_lang = bcp47;
    ReleaseSRWLockExclusive(&g_lock);
}

bool Reload() {
    // 快照模式/语言/目录后解锁：Load() 内部再取排他锁（不可嵌套持锁调用）。
    std::wstring mode, tm, dir;
    std::vector<LangInfo> langs;
    AcquireSRWLockShared(&g_lock);
    mode = g_mode; tm = g_tm_lang; dir = g_lang_dir; langs = g_lang_list;
    ReleaseSRWLockShared(&g_lock);
    // 确定目标 BCP-47
    std::wstring target = mode;
    if (target == L"auto") {
        target = tm;
        if (target.empty()) {
            // 未知 TM 语言 -> 中文兜底（清空当前表）。
            AcquireSRWLockExclusive(&g_lock);
            g_old_tables.push_back(std::make_shared<std::map<std::wstring, std::wstring>>(std::move(g_table)));
            if (g_old_tables.size() > 8) g_old_tables.erase(g_old_tables.begin());
            g_table.clear();
            ReleaseSRWLockExclusive(&g_lock);
            return false;
        }
    }
    std::wstring target_main = MainLang(target);
    // 1) 在扫描到的语言列表里精确匹配 BCP-47，其次匹配主语言
    const LangInfo* match = nullptr;
    for (const auto& lang : langs) {
        if (ToLower(lang.bcp47) == ToLower(target)) { match = &lang; break; }
    }
    if (!match) {
        for (const auto& lang : langs) {
            if (MainLang(lang.bcp47) == target_main) { match = &lang; break; }
        }
    }
    if (match) {
        return Load(dir + L"\\" + match->file_name);
    }
    // 2) 回退：按文件名映射（English.ini / Indonesian.ini 等内置约定）
    if (!dir.empty()) {
        std::wstring fallback = LangFileFromBcp47(target);
        if (Load(dir + L"\\" + fallback)) return true;
    }
    // 3) 中文兜底：不加载任何文件，TR 返回中文原文
    return false;
}

std::vector<LangInfo> GetLangList() {
    AcquireSRWLockShared(&g_lock);
    std::vector<LangInfo> copy = g_lang_list;
    ReleaseSRWLockShared(&g_lock);
    return copy;
}

const wchar_t* Get(const wchar_t* key) {
    AcquireSRWLockShared(&g_lock);
    auto it = g_table.find(key);
    const wchar_t* r = (it != g_table.end()) ? it->second.c_str() : key;  // 中文原文兜底
    ReleaseSRWLockShared(&g_lock);
    return r;
}

std::wstring LangFileFromBcp47(const std::wstring& bcp47) {
    std::wstring main = MainLang(bcp47);
    if (main == L"zh") {
        std::wstring region = ToLower(bcp47);
        if (region.find(L"tw") != std::wstring::npos || region.find(L"hk") != std::wstring::npos ||
            region.find(L"mo") != std::wstring::npos || region.find(L"hant") != std::wstring::npos)
            return L"Traditional_Chinese.ini";
        return L"Simplified_Chinese.ini";
    }
    if (main == L"en") return L"English.ini";
    if (main == L"id" || main == L"ms") return L"Indonesian.ini";
    // 其他语言：默认英文（非中文用户大概率不读中文；找不到文件时 TR 兜底中文）
    return L"English.ini";
}

bool CheckAndReload(const std::wstring& tm_lang) {
    // 快照旧状态（共享锁）→ 目录 rescan（无锁 IO）→ 有变化才换入（排他锁）。
    // 旧版直接 ScanLangFiles 重建 g_lang_list：定时器线程与 UI 线程的
    // GetLangList()/TR() 无锁并发即堆损坏 0xc0000374。
    std::vector<LangInfo> old_list;
    std::wstring old_tm, dir;
    AcquireSRWLockShared(&g_lock);
    old_list = g_lang_list; old_tm = g_tm_lang; dir = g_lang_dir;
    ReleaseSRWLockShared(&g_lock);
    if (dir.empty()) return false;   // 尚未 InitOnce：无语言目录可扫描
    if (!ScanLangFiles(dir)) {
        if (old_list.empty()) return false;
        // 目录不可读但旧表非空：保持旧表，仅检查 TM 语言变化
        if (ToLower(tm_lang) == ToLower(old_tm)) return false;
        AcquireSRWLockExclusive(&g_lock);
        g_tm_lang = tm_lang;
        ReleaseSRWLockExclusive(&g_lock);
        return Reload();
    }
    AcquireSRWLockShared(&g_lock);
    std::vector<LangInfo> new_list = g_lang_list;   // ScanLangFiles 已换入
    ReleaseSRWLockShared(&g_lock);
    bool list_changed = (old_list != new_list);
    bool tm_changed = (ToLower(tm_lang) != ToLower(old_tm));
    if (!list_changed && !tm_changed) return false;
    AcquireSRWLockExclusive(&g_lock);
    g_tm_lang = tm_lang;
    ReleaseSRWLockExclusive(&g_lock);
    return Reload();
}

} // namespace I18n
