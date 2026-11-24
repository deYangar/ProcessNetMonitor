#include "hourly_history.h"
#include <windows.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifndef PNM_BASELINE
#include "history_save.h"
#endif

using History = pnm::HourlyHistory;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

static double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

#ifndef PNM_BASELINE
static pnm::HistorySave::Result Wait(const pnm::HistorySave& save) {
    const auto start = Clock::now();
    while (save.Status() == pnm::HistorySave::Pending && Milliseconds(start) < 10000) Sleep(1);
    assert(save.Status() != pnm::HistorySave::Pending);
    return save.Status();
}

static History Load(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    History loaded;
    assert(loaded.Read(input));
    return loaded;
}
#endif

int wmain(int argc, wchar_t** argv) {
    assert(argc == 2);
    const fs::path dir(argv[1]), path = dir / L"history-hourly-v1.dat";
    fs::create_directories(dir);
    History history;
    constexpr History::Number tick = 1800000000000ULL;
    for (int hour = 0; hour < 400; ++hour)
        for (int app = 0; app < 500; ++app) {
            std::u16string name = u"synthetic-program-";
            name += char16_t(u'a' + app / 26);
            name += char16_t(u'a' + app % 26);
            assert(history.Add(tick + hour * History::Hour, name, 71, 49));
        }
#ifndef PNM_BASELINE
    pnm::HistorySave save;
#endif
    for (int run = 0; run < 5; ++run) {
        const auto start = Clock::now();
#ifdef PNM_BASELINE
        const auto temp = dir / L"baseline.tmp";
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        assert(history.Write(output));
        output.close();
        assert(output && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
        std::cout << "baseline synchronous ms=" << Milliseconds(start) << '\n';
#else
        assert(save.Queue(path, history, false));
        const auto submit = Milliseconds(start);
        assert(Wait(save) == pnm::HistorySave::Saved);
        std::cout << "submit ms=" << submit << "; complete ms=" << Milliseconds(start) << '\n';
#endif
    }
    std::cout << "bytes=" << fs::file_size(path) << '\n';
#ifndef PNM_BASELINE
    fs::copy_file(path, dir / L"benchmark.dat");
    const auto snapshot = history;
    assert(history.Add(tick, u"synthetic-program-aa", 1, 0));
    assert(snapshot.buckets.begin()->second->at(u"synthetic-program-aa").down == 71);
    assert(save.Queue(path, history, false));
    History cleared;
    cleared.legacyMerged = true;
    assert(save.Flush(path, cleared));
    Wait(save);
    assert(Load(path).buckets.empty());
    assert(cleared.Add(tick, u"fresh", 3, 8));
    const auto locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(save.Queue(path, cleared, false));
    assert(Wait(save) == pnm::HistorySave::Failed);
    CloseHandle(locked);
    assert(Load(path).buckets.empty());
    assert(save.Queue(path, cleared, false));
    assert(Wait(save) == pnm::HistorySave::Saved);
    for (int restart = 0; restart < 2; ++restart) {
        const auto restored = Load(path);
        assert(restored.legacyMerged && restored.buckets.size() == 1);
        assert(restored.buckets.begin()->second->at(u"fresh").up == 8);
    }
#endif
}