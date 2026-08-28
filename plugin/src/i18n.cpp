// i18n.cpp - 轻量本地化模块实现
// 语言文件格式与 TrafficMonitor 的 language\*.ini 一致：
//   [general]
//   BCP_47 = "zh-CN"
//   [text]
//   KEY = "value"
// 支持 UTF-8（带/不带 BOM）与 UTF-16 LE/BE。值内支持 \" \\ \n \r \t 转义。
#include "i18n.h"
#include <windows.h>
#include <vector>

namespace {

std::map<std::wstring, std::wstring> g_table;

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

// 解析 KEY = "value" 的值（去引号 + 转义）
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

} // namespace

namespace I18n {

bool Load(const std::wstring& file_path) {
    std::vector<BYTE> bytes;
    if (!ReadAllBytes(file_path, bytes)) return false;
    std::wstring content = BytesToWstring(bytes);
    if (content.empty()) return false;

    std::map<std::wstring, std::wstring> new_table;
    std::wstring section;
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
        std::wstring key = ParseValue(Trim(line.substr(0, eq)));  // key 同样支持 \n 等转义
        std::wstring value = ParseValue(line.substr(eq + 1));
        if (key.empty()) continue;
        if (section.empty() || section == L"text") {
            new_table[key] = value;
        }
    }
    if (new_table.empty()) return false;
    g_table = std::move(new_table);
    return true;
}

const wchar_t* Get(const wchar_t* key) {
    auto it = g_table.find(key);
    if (it != g_table.end()) return it->second.c_str();
    return key;  // 中文原文兜底
}

std::wstring LangFileFromBcp47(const std::wstring& bcp47) {
    std::wstring lang = bcp47;
    // 取主语言（- 之前的小写部分）
    size_t dash = lang.find(L'-');
    std::wstring main = lang.substr(0, dash);
    for (auto& c : main) c = (wchar_t)towlower(c);
    if (main == L"zh") {
        std::wstring region = (dash == std::wstring::npos) ? L"" : lang.substr(dash + 1);
        for (auto& c : region) c = (wchar_t)towlower(c);
        if (region == L"tw" || region == L"hk" || region == L"mo" || region == L"hant")
            return L"Traditional_Chinese.ini";
        return L"Simplified_Chinese.ini";
    }
    if (main == L"en") return L"English.ini";
    if (main == L"id" || main == L"ms") return L"Indonesian.ini";
    // 其他语言：默认英文（非中文用户大概率不读中文；找不到文件时 TR 兜底中文）
    return L"English.ini";
}

} // namespace I18n
