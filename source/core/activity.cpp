// source/core/activity.cpp

#include "core/activity.hpp"
#include <SDL2/SDL.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Activity {

// ─── Deferred to Milestone 4 ────────────────────────────────────────────────────
//
// Getting these values ACCURATE (matching what DBI / NX-Activity-Log show)
// requires reading the per-user play-log save archive at
// SYSTEM:/save/80000000000000F0, which is the source those tools use. The pdm
// query APIs we tried (pdmqryQueryAppletEvent / pdmqryQueryPlayStatistics...)
// don't reproduce DBI's numbers on real hardware:
//   - the raw applet-event log's first entries predate the RTC being set, so
//     the "first gameplay" timestamp reads as a near-epoch value (~2025-01-01);
//   - session counts include system-applet churn we can't cleanly filter from
//     the event stream alone.
//
// The correct approach is tied to the account (user profile) and needs the
// save-archive mount + parse infrastructure that Milestone 4 introduces for
// title management. Rather than display wrong numbers, every field reports N/A
// until then — consistent with our "never fabricate" rule.
//
// Tracked in docs/GarageNX_Architecture.md under Milestone 4.

Summary summary() {
    Summary s;   // all fields default-constructed → valid=false → "N/A"
#ifndef PLATFORM_SWITCH
    // PC stub: give the UI something to lay out against during development.
    s.rtc_started       = { "N/A", false };
    s.first_event_date  = { "28-05-2026 16:36:10", true };
    s.total_sessions    = { 737, true };
    s.unique_games      = { 18, true };
    s.total_playtime_h  = { 892.5f, true };
    s.active_playtime_h = { 892.5f, true };
#endif
    return s;
}

} // namespace Core::Activity
