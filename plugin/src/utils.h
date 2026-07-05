#pragma once
#include <cstdio>

// Shared speed/byte formatting utilities
inline void FormatSpeed(double bps, wchar_t* buf, int n) {
    if (bps < 0.01) wcsncpy_s(buf, n, L"0 B/s", _TRUNCATE);
    else if (bps < 1024) swprintf_s(buf, n, L"%.0f B/s", bps);
    else if (bps < 1048576) swprintf_s(buf, n, L"%.1f KB/s", bps / 1024.0);
    else swprintf_s(buf, n, L"%.2f MB/s", bps / 1048576.0);
}

inline void FormatBytes(uint64_t bytes, wchar_t* buf, int n) {
    if (bytes == 0) wcsncpy_s(buf, n, L"0 B", _TRUNCATE);
    else if (bytes < 1024ULL) swprintf_s(buf, n, L"%llu B", bytes);
    else if (bytes < 1048576ULL) swprintf_s(buf, n, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824ULL) swprintf_s(buf, n, L"%.2f MB", bytes / 1048576.0);
    else swprintf_s(buf, n, L"%.2f GB", bytes / 1073741824.0);
}
