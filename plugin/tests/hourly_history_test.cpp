#include "hourly_history.h"
#include <cassert>
#include <sstream>

using History = pnm::HourlyHistory;

static void ClearThenRestartTwice() {
    constexpr auto tick = 1800000000000ULL;
    History history;
    assert(history.Add(tick, u"deleted", 10, 5));
    history = {};
    history.legacyMerged = true;
    assert(history.Add(tick, u"new", 70, 20));
    for (int restart = 0; restart < 2; ++restart) {
        std::stringstream disk;
        assert(history.Write(disk));
        History loaded;
        assert(loaded.Read(disk));
        assert(loaded.legacyMerged);
        const auto& apps = loaded.buckets.begin()->second;
        assert(!apps.count(u"deleted"));
        assert(apps.at(u"new").down == 70 && apps.at(u"new").up == 20);
        history = std::move(loaded);
    }
}

static void ChartTotals() {
    const auto day = History::DayStart(1800000000000ULL);
    History history;
    assert(history.Add(day, u"one", 11, 2));
    assert(history.Add(day, u"two", 3, 10));
    assert(history.Add(day + History::Hour, u"one", 20, 7));
    const auto all = History::TotalUsage(history.SummarizeDay(day));
    const auto selected = History::TotalUsage(history.buckets.at(day));
    assert(all.down == 34 && all.up == 19);
    assert(selected.down == 14 && selected.up == 12);
}

static void IdleSamplesDoNotUseCapacity() {
    History history;
    const auto day = History::DayStart(1800000000000ULL);
    for (int hour = 0; hour < 24 * 30; ++hour) {
        for (int app = 0; app < 500; ++app) {
            const std::u16string name{char16_t(u'a' + app / 26), char16_t(u'a' + app % 26)};
            assert(!history.Add(day + hour * History::Hour, name, 0, 0));
        }
    }
    assert(history.buckets.empty());
    assert(history.Add(day, u"down", 30, 0));
    assert(history.Add(day, u"up", 0, 40));
    auto total = History::TotalUsage(history.buckets.at(day));
    assert(total.down == 30 && total.up == 40);

    History old;
    assert(old.Add(day, u"idle", 1, 0));
    std::stringstream output;
    assert(old.Write(output));
    auto bytes = output.str();
    bytes.at(13 + 1 + 8) = 0; // v482 row's download count, little endian
    std::stringstream input(bytes);
    assert(old.Read(input));
    assert(old.buckets.at(day).at(u"idle").down == 0);
}

int main() {
    ClearThenRestartTwice();
    ChartTotals();
    IdleSamplesDoNotUseCapacity();
}