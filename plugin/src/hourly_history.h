#pragma once
#include <cstdint>
#include <cstring>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

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
        return Insert(tick, name, down, up, day, true);
    }

private:
    static Apps& Mutable(std::shared_ptr<const Apps>& apps) {
        if (!apps) apps = std::make_shared<Apps>();
        // use_count()==1 等价于已废弃的 unique()（STL4016），C++17 下零警告写法
        else if (apps.use_count() != 1) apps = std::make_shared<Apps>(*apps);
        return *std::const_pointer_cast<Apps>(apps);
    }
    // 行数/字节到上限后不再静默拒绝新行（review B1：满额丢新数据会形成周期性
    // 整段空白且无提示）：逐个淘汰最旧的桶腾位，哪怕它还在 31 天窗口内——库保
    // 持滑动窗口语义，新数据永远可写。仅运行期 Add 路径启用；Read 重放满库文件
    // 必须原样复原，不淘汰。
    void EvictOldestUntilFits(Number rowBytes) {
        while (rows_ >= MaxRows || bytes_ + rowBytes > MaxFile) {
            if (buckets.empty() && daily.empty()) return;
            Table* victim = nullptr;
            if (buckets.empty()) victim = &daily;
            else if (daily.empty()) victim = &buckets;
            else victim = buckets.begin()->first <= daily.begin()->first ? &buckets : &daily;
            for (const auto& app : *victim->begin()->second) bytes_ -= 27 + 2 * app.first.size();
            rows_ -= victim->begin()->second->size();
            victim->erase(victim->begin());
        }
    }
    bool Insert(Number tick, const std::u16string& name, Number down, Number up, bool day, bool evict = false) {
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
        if (down > MaxBytes || up > MaxBytes) return false;
        if (evict) EvictOldestUntilFits(rowBytes);
        if (rows_ >= MaxRows || bytes_ + rowBytes > MaxFile) return false;
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
    bool Read(std::istream& in) { return Read(in, nullptr); }
    // newer_format 非 null 时接收「文件出自比本插件更新的版本」的判定：这不是
    // 损坏（review B2），调用方不得把它当坏文件改名备份、清空重记。
    bool Read(std::istream& in, bool* newer_format) {
        std::vector<char> data;
        // 整块读入再解析（review P1：逐字节 istream::get 在 32 MB 规模实测拖慢
        // 宿主启动 0.6~1.1 s）。可定位的流按大小一次读入，其余退回分块读。
        in.seekg(0, std::ios::end);
        const auto end = in.tellg();
        if (in.fail()) {
            in.clear();
            char chunk[1 << 16];
            while (in) {
                in.read(chunk, sizeof chunk);
                data.insert(data.end(), chunk, chunk + in.gcount());
            }
            if (in.bad()) return false;
        } else {
            in.seekg(0, std::ios::beg);
            data.resize(static_cast<std::size_t>(end));
            if (end > 0) in.read(data.data(), end);
            if (in.bad()) return false;
        }
        if (data.size() > MaxFile) return false;
        return Parse(data.data(), data.size(), newer_format);
    }

private:
    std::size_t rows_ = 0;
    Number bytes_ = 13;
    // 内存游标：替代原逐字节 istream::get，读越界置 failed（等价流的 eof/fail）
    struct Cursor {
        const char* p;
        std::size_t left;
        bool failed = false;
        Number take(unsigned bytes) {
            if (left < bytes) { failed = true; return 0; }
            Number value = 0;
            for (unsigned i = 0; i < bytes; ++i)
                value |= static_cast<Number>(static_cast<unsigned char>(p[i])) << (8 * i);
            p += bytes;
            left -= bytes;
            return value;
        }
    };
    bool Parse(const char* data, std::size_t size, bool* newer_format) {
        HourlyHistory candidate;
        Cursor cursor{data, size};
        if (size < 4 || memcmp(data, "PNH1", 4) != 0) return false;
        cursor.take(4);   // magic
        const auto version = cursor.take(4);
        if (cursor.failed) return false;
        if (version != 480 && version != 481 && version != 482) {
            // 范围内更大的合法版本号形状 = 文件由更新版本的插件写出；全 1 等随
            // 机损坏值超出 16 位版本号形状，仍按损坏处理
            if (newer_format && version > 482 && version <= 0xFFFF) *newer_format = true;
            return false;
        }
        const auto count = cursor.take(4);
        if (cursor.failed || count > MaxRows) return false;
        if (version == 482) {
            // Bit 0 once marked a cleared library and no longer has an effect;
            // files that carry it must stay readable.
            const auto flags = cursor.take(1);
            if (cursor.failed || flags > 1) return false;
        }
        for (Number i = 0; i < count; ++i) {
            const auto kind = version == 482 ? cursor.take(1) : 0;
            const auto hour = cursor.take(8), down = cursor.take(8), up = cursor.take(8);
            const auto units = cursor.take(2);
            if (cursor.failed || kind > 1 || hour > MaxTick || hour % Hour || down > MaxBytes || up > MaxBytes || !units || units > 512 ||
                (kind == 1 && (hour + LegacyDayOffset) % Day)) return false;
            std::u16string name;
            for (Number j = 0; j < units; ++j) name.push_back(static_cast<char16_t>(cursor.take(2)));
            if (cursor.failed) return false;
            const auto& table = kind ? candidate.daily : candidate.buckets;
            const auto bucket = table.find(hour);
            if ((bucket != table.end() && bucket->second->count(name)) ||
                !candidate.Insert(hour, name, down, up, kind == 1)) return false;
        }
        if (version == 481) {
            const auto flags = cursor.take(1);
            // The experimental aggregate flag did not encode per-row granularity.
            if (cursor.failed || flags != 0) return false;
        }
        if (cursor.left != 0) return false;   // 尾部多余字节
        *this = std::move(candidate);
        return true;
    }
};
}
