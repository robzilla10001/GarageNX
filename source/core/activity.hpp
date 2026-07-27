#pragma once
// source/core/activity.hpp
// Aggregate game-activity statistics via the pdm (play data manager) service.
// Used by the System Information screen's activity section. Detailed per-session
// logging for the Activity Log screen comes in Milestone 7.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

// ── Per-title play statistics ────────────────────────────────────────────────
//
// This is what the Activity Log screen shows, and it is a DIFFERENT query from
// summary() above. The aggregate totals are unreliable (see activity.cpp: the
// event stream includes system-applet churn and predates the RTC being set), but
// pdm's PER-TITLE statistics are the numbers other homebrew activity tools show
// and are queried directly by application id rather than derived from the event
// log. Wrong aggregates and correct per-title figures can coexist.
struct TitlePlay {
    uint64_t    application_id   = 0;
    std::string title_label;              // "<Name> [APPID]" or the id fallback
    uint64_t    playtime_seconds = 0;
    uint32_t    launches         = 0;
    bool        valid            = false; // false = pdm had nothing for this title
};

/// Play statistics for every INSTALLED title that pdm has a record for, sorted by
/// playtime descending.
///
/// LIMITATION, stated rather than hidden: this enumerates installed titles, so a
/// game that was played and then deleted has statistics pdm still holds but this
/// list will not show. Enumerating pdm's own title set needs the play-log save
/// archive that summary() documents as out of reach.
///
/// `pump`, when supplied, marks the caller as being on the MAIN THREAD: title-name
/// resolution is driven rather than blocked on, and pump is called between units
/// so the caller can draw. Same contract as Services::save_enumerate_all().
std::vector<TitlePlay> title_play_stats(const std::function<void()>& pump = nullptr);

} // namespace Core::Activity
