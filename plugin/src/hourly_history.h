#pragma once
#include <cstdint>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

namespace pnm {
class HourlyHistory {
public:
    using Number = std::uint64_t;
    static constexpr Number Hour = 3600000;
    static constexpr Number Day = 24 * Hour;
    // PNH1 v482 daily rows were aligned to UTC+8; hourly rows have always
    // used UTC Unix milliseconds at UTC hour boundaries.
    static constexpr Number LegacyDayOffset = 8 * Hour;
    static constexpr Number MaxTick = 253402271999999ULL;
    static constexpr Number MaxBytes = 9007199254740991ULL;
    static constexpr unsigned MaxRows = 250000;
    static constexpr Number MaxFile = 32ULL * 1024 * 1024;
    struct Usage { Number down = 0, up = 0; };
    using Apps = std::map<std::u16string, Usage>;
    using Table = std::map<Number, std::shared_ptr<const Apps>>;
    Table buckets;
    Table daily;

    static Number DayStart(Number tick) {
        if (tick < Day - LegacyDayOffset) return 0;
        return (tick + LegacyDayOffset) / Day * Day - LegacyDayOffset;
    }
    static Number SaturatingAdd(Number a, Number b) {
        return b > MaxBytes - a ? MaxBytes : a + b;
    }
    static Usage TotalUsage(const Apps& apps) {
        Usage total;
        for (const auto& [name, usage] : apps) {
            total.down = SaturatingAdd(total.down, usage.down);
            total.up = SaturatingAdd(total.up, usage.up);
        }
        return total;
    }
    Apps Summarize(Number start, Number end) const {
        Apps totals;
        for (auto it = daily.lower_bound(start); it != daily.end() && it->first < end; ++it) {
            for (const auto& [name, usage] : *it->second) {
                auto& total = totals[name];
                total.down = SaturatingAdd(total.down, usage.down);
                total.up = SaturatingAdd(total.up, usage.up);
            }
        }
        for (auto it = buckets.lower_bound(start); it != buckets.end() && it->first < end; ++it) {
            const auto day = DayStart(it->first);
            const auto daily_it = daily.find(day);
            for (const auto& [name, usage] : *it->second) {
                if (daily_it != daily.end() && daily_it->second->count(name)) continue;
                auto& total = totals[name];
                total.down = SaturatingAdd(total.down, usage.down);
                total.up = SaturatingAdd(total.up, usage.up);
            }
        }
        return totals;
    }
    Apps SummarizeDay(Number dayStart) const {
        return Summarize(dayStart, dayStart + Day);
    }

    static Number Oldest(Number now) {
        // Keep enough UTC buckets for the last 30 civil dates, including DST.
        return now >= 31 * Day ? now / Hour * Hour - 31 * Day : 0;
    }
    bool SetDayTotal(Number tick, const std::u16string& name, Number down, Number up) {
        if (tick > MaxTick || tick < Day - LegacyDayOffset || name.empty() || name.size() > 512 ||
            down > MaxBytes || up > MaxBytes) return false;
        const Number day = DayStart(tick);
        auto& apps = Mutable(daily[day]);
        auto it = apps.find(name);
        if (it == apps.end()) {
            const auto rowBytes = 27 + 2 * name.size();
            if (rows_ >= MaxRows || bytes_ + rowBytes > MaxFile) return false;
            apps.emplace(name, Usage{down, up});
            bytes_ += rowBytes;
            ++rows_;
        } else {
            it->second = Usage{down, up};
        }
        return true;
    }
    bool Prune(Number now) {
        bool changed = false;
        for (auto* table : {&buckets, &daily}) {
            const auto end = table->lower_bound(Oldest(now));
            changed |= end != table->begin();
            for (auto it = table->begin(); it != end; ++it) {
                rows_ -= it->second->size();
                for (const auto& app : *it->second) bytes_ -= 27 + 2 * app.first.size();
            }
            table->erase(table->begin(), end);
        }
        return changed;
    }
    bool Add(Number tick, const std::u16string& name, Number down, Number up, bool day = false) {
        if (!down && !up) return false;
        return Insert(tick, name, down, up, day);
    }

private:
    static Apps& Mutable(std::shared_ptr<const Apps>& apps) {
        if (!apps) apps = std::make_shared<Apps>();
        // use_count()==1 等价于已废弃的 unique()（STL4016），C++17 下零警告写法
        else if (apps.use_count() != 1) apps = std::make_shared<Apps>(*apps);
        return *std::const_pointer_cast<Apps>(apps);
    }
    bool Insert(Number tick, const std::u16string& name, Number down, Number up, bool day) {
        if (tick > MaxTick || name.empty() || name.size() > 512) return false;
        if (day && tick < Day - LegacyDayOffset) return false;
        const Number hour = day ? DayStart(tick) : tick / Hour * Hour;
        auto& table = day ? daily : buckets;
        auto found = table.find(hour);
        if (found != table.end()) {
            auto app = found->second->find(name);
            if (app != found->second->end()) {
                const auto value = app->second;
                if (down > MaxBytes - value.down || up > MaxBytes - value.up) return false;
                Mutable(found->second).at(name) = {value.down + down, value.up + up};
                return down || up;
            }
        }
        const auto rowBytes = 27 + 2 * name.size();
        if (rows_ >= MaxRows || bytes_ + rowBytes > MaxFile || down > MaxBytes || up > MaxBytes) return false;
        Mutable(table[hour]).emplace(name, Usage{down, up});
        bytes_ += rowBytes;
        ++rows_;
        return true;
    }
public:
    bool Write(std::ostream& out) const {
        char row[27 + 2 * 512];
        auto cursor = row;
        auto put = [&](Number value, unsigned bytes) {
            for (unsigned i = 0; i < bytes; ++i) *cursor++ = static_cast<char>(value >> (8 * i));
        };
        out.write("PNH1", 4);
        put(482, 4);
        put(rows_, 4);
        put(0, 1);  // reserved: bit 0 once marked a cleared library
        out.write(row, cursor - row);
        for (const auto* table : {&buckets, &daily}) {
            for (const auto& [hour, apps] : *table) {
                for (const auto& [name, usage] : *apps) {
                    cursor = row;
                    put(table == &daily, 1);
                    put(hour, 8);
                    put(usage.down, 8);
                    put(usage.up, 8);
                    put(name.size(), 2);
                    for (auto unit : name) put(unit, 2);
                    if (!out.write(row, cursor - row)) return false;
                }
            }
        }
        return static_cast<bool>(out);
    }
    bool Read(std::istream& in) {
        HourlyHistory candidate;
        char magic[4]{};
        in.read(magic, 4);
        if (!in || std::string(magic, 4) != "PNH1") return false;
        const auto version = Get(in, 4);
        if (!in || (version != 480 && version != 481 && version != 482)) return false;
        const auto count = Get(in, 4);
        if (!in || count > MaxRows) return false;
        if (version == 482) {
            // Bit 0 once marked a cleared library and no longer has an effect;
            // files that carry it must stay readable.
            const auto flags = Get(in, 1);
            if (!in || flags > 1) return false;
        }
        for (Number i = 0; i < count; ++i) {
            const auto kind = version == 482 ? Get(in, 1) : 0;
            const auto hour = Get(in, 8), down = Get(in, 8), up = Get(in, 8);
            const auto units = Get(in, 2);
            if (!in || kind > 1 || hour > MaxTick || hour % Hour || down > MaxBytes || up > MaxBytes || !units || units > 512 ||
                (kind == 1 && (hour + LegacyDayOffset) % Day)) return false;
            std::u16string name;
            for (Number j = 0; j < units; ++j) name.push_back(static_cast<char16_t>(Get(in, 2)));
            const auto& table = kind ? candidate.daily : candidate.buckets;
            const auto bucket = table.find(hour);
            if (!in || (bucket != table.end() && bucket->second->count(name)) ||
                !candidate.Insert(hour, name, down, up, kind == 1)) return false;
        }
        if (version == 481) {
            const auto flags = Get(in, 1);
            // The experimental aggregate flag did not encode per-row granularity.
            if (!in || flags != 0) return false;
        }
        if (in.peek() != std::char_traits<char>::eof() || in.bad()) return false;
        *this = std::move(candidate);
        return true;
    }

private:
    std::size_t rows_ = 0;
    Number bytes_ = 13;
    static Number Get(std::istream& in, unsigned bytes) {
        Number value = 0;
        for (unsigned i = 0; i < bytes; ++i) {
            const int byte = in.get();
            if (byte == std::char_traits<char>::eof()) return 0;
            value |= static_cast<Number>(byte) << (i * 8);
        }
        return value;
    }
};
}
