// source/core/datetime.cpp

#include "core/datetime.hpp"
#include "config/config.hpp"
#include <cctype>
#include <cstdio>

namespace Core::DateTime {

DateOrder parse_order(const std::string& s) {
    std::string u;
    for (char c : s) u += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u == "MDY") return DateOrder::MDY;
    if (u == "YMD") return DateOrder::YMD;
    return DateOrder::DMY;
}

const char* order_to_string(DateOrder o) {
    switch (o) {
        case DateOrder::MDY: return "MDY";
        case DateOrder::YMD: return "YMD";
        default:             return "DMY";
    }
}

std::string date(const std::tm& tm, DateOrder order, char sep) {
    const int d = tm.tm_mday;
    const int m = tm.tm_mon + 1;
    const int y = tm.tm_year + 1900;
    char buf[40];
    switch (order) {
        case DateOrder::MDY:
            std::snprintf(buf, sizeof(buf), "%02d%c%02d%c%04d", m, sep, d, sep, y);
            break;
        case DateOrder::YMD:
            std::snprintf(buf, sizeof(buf), "%04d%c%02d%c%02d", y, sep, m, sep, d);
            break;
        default: // DMY
            std::snprintf(buf, sizeof(buf), "%02d%c%02d%c%04d", d, sep, m, sep, y);
            break;
    }
    return buf;
}

std::string time_of_day(const std::tm& tm, bool time_24h, bool seconds) {
    char buf[16];
    if (time_24h) {
        if (seconds)
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
        else
            std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        int h = tm.tm_hour % 12;
        if (h == 0) h = 12;
        const char* ap = (tm.tm_hour < 12) ? "AM" : "PM";
        if (seconds)
            std::snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", h, tm.tm_min, tm.tm_sec, ap);
        else
            std::snprintf(buf, sizeof(buf), "%d:%02d %s", h, tm.tm_min, ap);
    }
    return buf;
}

static bool to_local(std::time_t t, std::tm& out) {
    std::tm* lt = std::localtime(&t);
    if (!lt) return false;
    out = *lt;
    return true;
}

std::string clock_string(std::time_t t) {
    std::tm tm;
    if (!to_local(t, tm)) return {};
    const auto& b = Config::get().behavior;
    const DateOrder order = parse_order(b.date_format);
    return date(tm, order, '/') + "  " + time_of_day(tm, b.time_24h, b.show_seconds);
}

std::string clock_string_now() { return clock_string(std::time(nullptr)); }

std::string log_stamp(std::time_t t) {
    std::tm tm;
    if (!to_local(t, tm)) return "unknown";
    const DateOrder order = parse_order(Config::get().behavior.date_format);
    // Date in the user's order but filename-safe separators; time always 24h.
    char tbuf[16];
    std::snprintf(tbuf, sizeof(tbuf), "%02d-%02d-%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return date(tm, order, '-') + "_" + tbuf;
}

std::string log_stamp_now() { return log_stamp(std::time(nullptr)); }

} // namespace Core::DateTime
