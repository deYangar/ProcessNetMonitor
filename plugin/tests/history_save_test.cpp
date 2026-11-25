#include "hourly_history.h"
#include <windows.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#ifndef PNM_BASELINE
#include "history_save.h"
#include "history_load.h"
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

static void TestRecovery(const fs::path& dir) {
    using pnm::HistoryLoad;
    for (const auto& bytes : {std::string("PNH1"), std::string("corrupt data"),
                             std::string("PNH1\xff\0\0\0\0\0\0\0", 12)}) {
        const auto path = dir / L"recover.dat";
        const auto legacy = dir / L"history.dat";
        auto read = [](const fs::path& file) {
            std::ifstream stream(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(stream), {});
        };
        History history;
        assert(pnm::LoadHistoryFile(path, history) == HistoryLoad::Missing);
        std::ofstream(legacy, std::ios::binary) << "legacy sentinel";
        std::ofstream(path, std::ios::binary).write(bytes.data(), bytes.size());
        const auto lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(lock != INVALID_HANDLE_VALUE);
        assert(pnm::LoadHistoryFile(path, history) == HistoryLoad::Blocked);
        assert(read(path) == bytes);
        CloseHandle(lock);
        assert(pnm::LoadHistoryFile(path, history) == HistoryLoad::Recovered);
        bool backedUp = false;
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().wstring().find(L"recover.dat.bak.") == 0 && read(entry.path()) == bytes)
                backedUp = true;
        assert(backedUp && !fs::exists(path));
        {
            // 文件由更新版本的插件写出（版本 483）：不是损坏——原文件必须原样
            // 保留、不产生备份、报告 NewerVersion（review B2）
            const auto newer_path = dir / L"newer.dat";
            const std::string newer_bytes = std::string("PNH1\xe3\x01\0\0", 8) + std::string(20, '\0');
            std::ofstream(newer_path, std::ios::binary).write(newer_bytes.data(), newer_bytes.size());
            History fresh;
            assert(pnm::LoadHistoryFile(newer_path, fresh) == HistoryLoad::NewerVersion);
            assert(read(newer_path) == newer_bytes);
            for (const auto& entry : fs::directory_iterator(dir))
                assert(entry.path().filename().wstring().find(L"newer.dat.bak.") != 0);
            fs::remove(newer_path);
        }
        pnm::HistorySave save;
        assert(save.Flush(path, history));
        assert(pnm::LoadHistoryFile(path, history) == HistoryLoad::Loaded);
        history = {};
        assert(save.Flush(path, history));
        assert(history.Add(1800000000000ULL, u"fresh", 2, 9));
        assert(save.Flush(path, history));
        assert(Load(path).buckets.begin()->second->at(u"fresh").up == 9);
        assert(read(legacy) == "legacy sentinel");
        fs::remove(path);
    }
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
        assert(restored.buckets.size() == 1);
        assert(restored.buckets.begin()->second->at(u"fresh").up == 8);
    }
    TestRecovery(dir);
#endif
}