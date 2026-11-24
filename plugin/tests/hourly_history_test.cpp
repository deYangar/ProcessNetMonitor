#include "hourly_history.h"
#include "history_calendar.h"
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
        const auto& apps = *loaded.buckets.begin()->second;
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
    const auto selected = History::TotalUsage(*history.buckets.at(day));
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
    auto total = History::TotalUsage(*history.buckets.at(day));
    assert(total.down == 30 && total.up == 40);

    History old;
    assert(old.Add(day, u"idle", 1, 0));
    std::stringstream output;
    assert(old.Write(output));
    auto bytes = output.str();
    bytes.at(13 + 1 + 8) = 0; // v482 row's download count, little endian
    std::stringstream input(bytes);
    assert(old.Read(input));
    assert(old.buckets.at(day)->at(u"idle").down == 0);
}

static pnm::HistoryCalendar Zone(const wchar_t* key) {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &zone) == ERROR_SUCCESS; ++i)
        if (wcscmp(zone.TimeZoneKeyName, key) == 0) return pnm::HistoryCalendar(zone);
    assert(false && "Windows time zone not installed");
    std::abort();
}

static void LocalDaysAndPnh1() {
    using Calendar = pnm::HistoryCalendar;
    const auto spring = Calendar::AsUnix(SYSTEMTIME{2026, 3, 0, 8});
    const auto autumn = Calendar::AsUnix(SYSTEMTIME{2026, 11, 0, 1});
    const auto west = Zone(L"Pacific Standard Time");
    const auto spring_hours = west.Hours(spring);
    const auto autumn_hours = west.Hours(autumn);
    assert(spring_hours.size() == 23 && autumn_hours.size() == 25);
    assert(west.Local(spring_hours.at(2)).wHour == 3);
    assert(west.Local(autumn_hours.at(1)).wHour == 1 && west.Local(autumn_hours.at(2)).wHour == 1);
    assert(autumn_hours.at(2) - autumn_hours.at(1) == History::Hour);
    const auto east = Zone(L"China Standard Time");
    assert(east.Start(spring) == spring - 8 * History::Hour);
    const auto india = Zone(L"India Standard Time");
    const auto india_hours = india.Hours(spring);
    assert(india_hours.size() == 24 && india.Local(india_hours.front()).wMinute == 30);
    assert(india.Date(india_hours.front() - History::Hour) == spring - History::Day);

    History original;
    assert(original.SetDayTotal(spring - History::LegacyDayOffset, u"daily", 24, 12));
    assert(original.Add(autumn_hours.at(2), u"hour", 5, 7));
    std::stringstream disk;
    assert(original.Write(disk));
    History loaded;
    assert(loaded.Read(disk));
    assert(loaded.buckets.at(autumn_hours.at(2))->at(u"hour").up == 7);
    const auto legacy_tick = spring - History::LegacyDayOffset;
    const auto west_date = west.Date(legacy_tick);
    assert(loaded.Summarize(west.Start(west_date), west.Start(west_date + History::Day)).at(u"daily").down == 24);
}

int main() {
    ClearThenRestartTwice();
    ChartTotals();
    IdleSamplesDoNotUseCapacity();
    LocalDaysAndPnh1();
}