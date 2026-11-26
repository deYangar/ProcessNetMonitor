#pragma once
#include <windows.h>
#include <vector>
#include "hourly_history.h"

namespace pnm {
class HistoryCalendar {
public:
    using Number = HourlyHistory::Number;
    static constexpr Number Hour = HourlyHistory::Hour, Day = HourlyHistory::Day;
    HistoryCalendar() { GetDynamicTimeZoneInformation(&zone_); }
    explicit HistoryCalendar(DYNAMIC_TIME_ZONE_INFORMATION zone) : zone_(zone) {}

    static Number AsUnix(const SYSTEMTIME& time) {
        FILETIME ft{};
        if (!SystemTimeToFileTime(&time, &ft)) return 0;
        const Number stamp = (Number(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return stamp >= Epoch ? (stamp - Epoch) / 10000 : 0;
    }
    static SYSTEMTIME AsUtc(Number tick) {
        const auto stamp = Epoch + tick * 10000;
        const FILETIME ft{DWORD(stamp), DWORD(stamp >> 32)};
        SYSTEMTIME time{};
        FileTimeToSystemTime(&ft, &time);
        return time;
    }
    SYSTEMTIME Local(Number tick) const {
        const auto utc = AsUtc(tick);
        SYSTEMTIME local{};
        return SystemTimeToTzSpecificLocalTimeEx(&zone_, &utc, &local) ? local : utc;
    }
    // A civil date key is not an instant: it advances by one calendar day.
    Number Date(Number tick) const { return AsUnix(Local(tick)) / Day * Day; }

    static Number OldestDate(Number today) { return today >= 29 * Day ? today - 29 * Day : 0; }

    struct DateRange {
        Number first, last;
        bool Valid(Number today) const {
            return first >= OldestDate(today) && first <= last && last <= today &&
                   first % Day == 0 && last % Day == 0;
        }
    };

    std::pair<Number, Number> Bounds(DateRange range, Number now) const {
        const auto end = Start(range.last + Day);
        return {Start(range.first), end < now + 1 ? end : now + 1};
    }

    Number Start(Number date) const {
        if (date < Day) return 0;
        // Find the first UTC instant on this civil date (also works when
        // midnight is skipped or the entire civil date is skipped).
        Number lo = date - Day, hi = date + Day;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            if (Date(mid) < date) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    std::vector<Number> Hours(Number date) const {
        std::vector<Number> hours;
        for (auto tick = (Start(date) + Hour - 1) / Hour * Hour, end = Start(date + Day);
             tick < end; tick += Hour) hours.push_back(tick);
        return hours;
    }

private:
    static constexpr Number Epoch = 116444736000000000ULL;
    DYNAMIC_TIME_ZONE_INFORMATION zone_{};
};
}