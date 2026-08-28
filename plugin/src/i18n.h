// i18n.h - 轻量本地化模块
// 设计：以中文原文为 key，TR() 宏包装所有用户可见字符串。
// 语言文件与 TrafficMonitor 的 language\*.ini 同格式：
// UTF-8 BOM + [general] BCP_47/DISPLAY_NAME + [text] KEY = "***"。
// 未命中 key 时返回 key 本身（中文原文兜底，天然无故障降级）。
#pragma once
#include <string>
#include <map>
#include <vector>

namespace I18n {

struct LangInfo {
    std::wstring bcp47;        // BCP-47 标签，如 en-US / zh-CN / ja-JP
    std::wstring display_name; // 显示名，如 English / 简体中文 / 日本語
    std::wstring file_name;    // 文件名，如 English.ini
};

// 扫描语言目录（dir\*.ini），建立语言列表。成功返回 true。
// 文件名不限，以 ini 内 [general] 的 BCP_47 / DISPLAY_NAME 为准。
bool ScanLangFiles(const std::wstring& dir);

// 设置语言模式："auto" = 跟随 TM 主程序语言；其他值 = 具体 BCP-47 标签（手动选择）。
void SetLang(const std::wstring& bcp47_or_auto);

// 当前语言模式（"auto" 或具体 BCP-47）。
const std::wstring& GetLang();

// 按当前模式重新加载语言文件：
//   auto  -> 需要先用 SetTmLang() 告知 TM 的语言，再 Reload()；
//   手动  -> 直接匹配用户选择的 BCP-47。
// 找不到匹配文件时回退中文（TR 返回原文）。返回是否加载了语言文件。
bool Reload();

// 设置 TM 主程序当前语言（auto 模式匹配用），由 plugin_main 调用。
void SetTmLang(const std::wstring& bcp47);

// 扫描到的语言列表（含 display_name）。
const std::vector<LangInfo>& GetLangList();

// 加载指定语言文件（UTF-8/UTF-16，带/不带 BOM）。成功返回 true。
bool Load(const std::wstring& file_path);

// 按 key 查当前语言文本；未命中返回 key 本身。
const wchar_t* Get(const wchar_t* key);

// 由 BCP-47 语言标签映射到语言文件名（如 "en-US" -> L"English.ini"）。
// 未知语言一律返回 English.ini（国际用户默认英文）。
std::wstring LangFileFromBcp47(const std::wstring& bcp47);

} // namespace I18n

// TR = Translate：包装用户可见字符串。
// 中文环境（无语言文件）时 TR 返回原文，行为与改动前完全一致。
#define TR(str) I18n::Get(str)
