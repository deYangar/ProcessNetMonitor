#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include "hourly_history.h"

namespace pnm {
enum class HistoryLoad { Missing, Loaded, Recovered, Blocked };

inline HistoryLoad LoadHistoryFile(const std::filesystem::path& path, HourlyHistory& history) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error == std::errc::no_such_file_or_directory) return HistoryLoad::Missing;
    if (!error && size <= HourlyHistory::MaxFile) {
        std::ifstream input(path, std::ios::binary);
        if (input && history.Read(input)) return HistoryLoad::Loaded;
    }
    // Never rename a directory or replace an earlier backup.
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return HistoryLoad::Blocked;
    const auto suffix = L".bak." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    for (unsigned i = 0; i < 100; ++i) {
        auto backup = path;
        backup += suffix + L"." + std::to_wstring(i);
        if (MoveFileExW(path.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
            history = {};
            return HistoryLoad::Recovered;
        }
        const auto code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) break;
    }
    return HistoryLoad::Blocked;
}
}