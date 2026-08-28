// i18n.h - 轻量本地化模块
// 设计：以中文原文为 key，TR() 宏包装所有用户可见字符串。
// 加载语言文件（与 TrafficMonitor 的 language\*.ini 同格式：
// UTF-8 BOM + [text] section + KEY = "value"）。
// 未命中 key 时返回 key 本身（中文原文兜底，天然无故障降级）。
#pragma once
#include <string>
#include <map>

namespace I18n {

// 加载语言文件（UTF-8/UTF-16，带/不带 BOM）。成功返回 true。
// 加载失败不影响使用：Get() 会退回中文原文。
bool Load(const std::wstring& file_path);

// 按 key 查当前语言文本；未命中返回 key 本身。
// 返回的指针在后续 Load() 之前一直有效（内部存储只增不减）。
const wchar_t* Get(const wchar_t* key);

// 由 BCP-47 语言标签映射到语言文件名（如 "en-US" -> L"English.ini"）。
// 未知语言一律返回 English.ini（国际用户默认英文）。
std::wstring LangFileFromBcp47(const std::wstring& bcp47);

} // namespace I18n

// TR = Translate：包装用户可见字符串。
// 中文环境（无语言文件）时 TR 返回原文，行为与改动前完全一致。
#define TR(str) I18n::Get(str)
