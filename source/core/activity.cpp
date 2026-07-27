// source/core/activity.cpp

#include "core/activity.hpp"
#include <SDL2/SDL.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

#include "services/title_surface.hpp"
#include "core/ncm.hpp"
#include "services/save_surface.hpp"

#include <SDL2/SDL.h>
#include <algorithm>

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

std::vector<TitlePlay> title_play_stats(const std::function<void()>& pump) {
    std::vector<TitlePlay> out;

#ifdef PLATFORM_SWITCH
    // Resolve installed-title names first. On the main thread this DRIVES the
    // resolver rather than blocking on it — blocking would park the loop that does
    // the resolving, which is the stall pattern documented in save_surface.
    if (pump) {
        Services::installed_titles_request_nonblocking();
        const uint32_t deadline = SDL_GetTicks() + 20000;
        while (!Services::installed_titles_names_resolved() &&
               (int32_t)(SDL_GetTicks() - deadline) < 0) {
            Services::installed_titles_tick();
            pump();
        }
    } else {
        (void)Services::installed_titles_list();
    }

    // Enumerate via ncm and GROUP BY APPLICATION — the same pattern title_list
    // uses. Grouping matters here beyond tidiness: list_all() returns patches and
    // DLC as separate titles, and querying play statistics for a DLC id would add
    // meaningless rows. Only the base application of each group is asked.
    //
    // (Services::installed_titles_list() is NOT the right source: it returns
    // VirtualEntry — wire filenames and sizes for the transports — and carries no
    // application id at all.)
    bool ncm_ok = false;
    const auto groups = Core::Ncm::group_by_application(Core::Ncm::list_all(&ncm_ok));

    for (const auto& g : groups) {
        const uint64_t app_id = g.app.program_id;
        if (app_id == 0) continue;

        TitlePlay t;
        t.application_id = app_id;
        t.title_label    = Services::save_build_label(app_id);

        // ── THE UNVERIFIED CALL (5.4) ────────────────────────────────────────
        // pdmqryQueryPlayStatisticsByApplicationId is the switchbrew-documented
        // way to read per-title play data, and pdmqryInitialize() already runs at
        // startup — but there is no libnx header in the build sandbox to check the
        // exact name, signature or struct field names against. If the Switch build
        // cannot resolve this, the things to check in order are:
        //   * the name may take a UID:
        //       pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(id, uid, false, &st)
        //   * the bool argument ("include system titles") may be absent on older libnx
        //   * field names: playtime may be `playtime` or `play_time`, and is in
        //     NANOSECONDS on current firmware — divide by 1e9, not 1e6
        //   * launches may be `total_launches` or `launch_count`
        // Everything else in this file is ordinary C++ and does not depend on
        // which spelling is correct.
        PdmPlayStatistics st{};
        const Result rc = pdmqryQueryPlayStatisticsByApplicationId(
            app_id, /*include_system=*/false, &st);
        if (R_SUCCEEDED(rc)) {
            t.playtime_seconds = (uint64_t)(st.playtime / 1000000000ULL);
            t.launches         = (uint32_t)st.total_launches;
            t.valid            = true;
        } else {
            // Not an error worth surfacing per title: a game that has never been
            // launched legitimately has no record. Logged once for diagnosis.
            static bool logged = false;
            if (!logged) {
                logged = true;
                SDL_Log("activity: pdmqryQueryPlayStatisticsByApplicationId rc=0x%08X", rc);
            }
        }
        out.push_back(std::move(t));
    }

    // Titles with no record sort last; the rest by playtime descending, which is
    // the order the question "what have I been playing" actually asks for.
    std::sort(out.begin(), out.end(), [](const TitlePlay& a, const TitlePlay& b) {
        if (a.valid != b.valid) return a.valid;
        return a.playtime_seconds > b.playtime_seconds;
    });
#else
    (void)pump;
#endif
    return out;
}

} // namespace Core::Activity
