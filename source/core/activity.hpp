#pragma once
// source/core/activity.hpp
// Aggregate game-activity statistics via the pdm (play data manager) service.
// Used by the System Information screen's activity section. Detailed per-session
// logging for the Activity Log screen comes in Milestone 7.

#include <cstdint>
#include <string>

namespace Core::Activity {

template<typename T>
struct Val { T value{}; bool valid = false; };

struct Summary {
    Val<std::string> rtc_started;        // date the RTC was first set
    Val<std::string> first_event_date;   // first recorded gameplay event
    Val<int>         total_sessions;     // total play sessions
    Val<int>         unique_games;       // distinct titles played
    Val<float>       total_playtime_h;   // total wall-clock playtime (hours)
    Val<float>       active_playtime_h;  // total active playtime (hours)
};

Summary summary();

} // namespace Core::Activity
