#pragma once
// source/core/datetime.hpp
// Central date/time formatting. Display strings honour the user's configured
// preferences (Behavior::date_format and Behavior::time_24h); log filenames use
// a fixed, filesystem-safe 24-hour stamp so they sort/label predictably.
//
// Everything routes through here so the clock, logs, and any future timestamps
// stay consistent — never hand-roll strftime at call sites.

#include <ctime>
#include <string>

namespace Core::DateTime {

// Order of the day/month/year fields in a date. Stored in config as the strings
// "DMY" / "MDY" / "YMD".
enum class DateOrder { DMY, MDY, YMD };

DateOrder   parse_order(const std::string& s);      // case-insensitive; unknown -> DMY
const char* order_to_string(DateOrder o);

// ─── Pure formatters (no config dependency) ───────────────────────────────────

// Date with the given field order and separator, zero-padded, 4-digit year.
std::string date(const std::tm& tm, DateOrder order, char sep = '/');

// Time of day. 24h -> "14:30[:05]"; 12h -> "2:30[:05] PM".
std::string time_of_day(const std::tm& tm, bool time_24h, bool seconds);

// ─── Config-aware convenience (reads Config::get().behavior) ──────────────────

// Full clock string, e.g. "12/07/2026  14:30:05" (order + 12/24h + seconds all
// follow the user's settings).
std::string clock_string(std::time_t t);
std::string clock_string_now();

// Filename-safe stamp for a log title. ALWAYS 24-hour (am/pm would muddy the
// name). Uses the user's date order but '-' separators, since '/' and ':' are
// not valid in filenames. e.g. DMY -> "12-07-2026_14-30-05",
// YMD -> "2026-07-12_14-30-05".
std::string log_stamp(std::time_t t);
std::string log_stamp_now();

} // namespace Core::DateTime
