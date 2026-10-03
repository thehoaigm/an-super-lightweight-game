#pragma once
// Game time = int64 minutes since the start of Year 1. Integer-only, so it is exact and deterministic.
// Calendar: 60 min/hour, 24 h/day, 30 days/month, 12 months/year (360-day year), 3 months/season,
// 1000 years/era. Every month is identical, which keeps the math trivial and testable.
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace cx {

enum class Season : uint8_t { Spring = 0, Summer = 1, Autumn = 2, Winter = 3 };

struct Date {
    int64_t year;  // 1-based
    int month;     // 1..12
    int day;       // 1..30
    int hour;
    int minute;
    Season season;
    int64_t era;   // 1-based
};

namespace calendar {

constexpr int64_t kMinutesPerHour = 60;
constexpr int64_t kMinutesPerDay = 24 * kMinutesPerHour;
constexpr int64_t kDaysPerMonth = 30;
constexpr int64_t kMonthsPerYear = 12;
constexpr int64_t kDaysPerYear = kDaysPerMonth * kMonthsPerYear;
constexpr int64_t kMinutesPerYear = kDaysPerYear * kMinutesPerDay;
constexpr int64_t kYearsPerEra = 1000;

// Time never goes negative; negative input is clamped to 0.
inline Date to_date(int64_t t) {
    if (t < 0) t = 0;
    const int64_t days = t / kMinutesPerDay;
    const int64_t minute_of_day = t % kMinutesPerDay;
    const int64_t day_of_year = days % kDaysPerYear;
    Date d;
    d.year = days / kDaysPerYear + 1;
    d.month = int(day_of_year / kDaysPerMonth) + 1;
    d.day = int(day_of_year % kDaysPerMonth) + 1;
    d.hour = int(minute_of_day / kMinutesPerHour);
    d.minute = int(minute_of_day % kMinutesPerHour);
    d.season = Season((d.month - 1) / 3);
    d.era = (d.year - 1) / kYearsPerEra + 1;
    return d;
}

inline int64_t from_date(int64_t year, int month, int day, int hour = 0, int minute = 0) {
    const int64_t days = (year - 1) * kDaysPerYear + (month - 1) * kDaysPerMonth + (day - 1);
    return days * kMinutesPerDay + hour * kMinutesPerHour + minute;
}

inline const char* season_name(Season s) {
    switch (s) {
        case Season::Spring: return "Spring";
        case Season::Summer: return "Summer";
        case Season::Autumn: return "Autumn";
        case Season::Winter: return "Winter";
    }
    return "?";
}

inline std::string format(int64_t t) {
    const Date d = to_date(t);
    char buf[96];
    std::snprintf(buf, sizeof buf, "Year %lld, Month %d, Day %d, %02d:%02d (%s, Era %lld)", (long long)d.year,
                  d.month, d.day, d.hour, d.minute, season_name(d.season), (long long)d.era);
    return buf;
}

// "90m", "12h", "30d", "2mo", "1y" -> minutes. Returns false on anything else.
inline bool parse_duration(std::string_view s, int64_t& out) {
    size_t i = 0;
    int64_t n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        n = n * 10 + (s[i] - '0');
        if (n > 1000000000) return false;
        ++i;
    }
    if (i == 0 || i == s.size()) return false;
    const std::string_view u = s.substr(i);
    int64_t unit;
    if (u == "m") unit = 1;
    else if (u == "h") unit = kMinutesPerHour;
    else if (u == "d") unit = kMinutesPerDay;
    else if (u == "mo") unit = kDaysPerMonth * kMinutesPerDay;
    else if (u == "y") unit = kMinutesPerYear;
    else return false;
    out = n * unit;
    return true;
}

}  // namespace calendar
}  // namespace cx
