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

int main() {
    ClearThenRestartTwice();
    ChartTotals();
}