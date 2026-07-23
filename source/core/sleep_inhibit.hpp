#pragma once
// source/core/sleep_inhibit.hpp
//
// Keeps the console awake while it matters. The Switch auto-sleeps after an idle
// period, and a transfer in progress does not count as "activity" — there is no HID
// input during a big upload — so the console would sleep mid-transfer and drop the
// MTP/FTP/HTTP connection, potentially leaving a half-written file behind.
//
// Scope is per SCREEN, not per connection: while a Connectivity screen (MTP/FTP/
// HTTP) is open, the console stays awake. That is simpler and safer than tracking
// client connections — there is no window where a transfer is live but the
// inhibitor has already lapsed.
//
// Usage: hold a Guard as a member of the screen. Construction acquires, destruction
// releases, so leaving the page (pop, quit, teardown) always restores normal sleep
// behaviour even on paths that forget to call anything.
//
// libnx calls used (both verified against libnx's ISelfController API, not guessed):
//   appletSetAutoSleepDisabled(bool)  — the actual sleep inhibit
//   appletReportUserIsActive()        — refreshes the idle timer so the screen also
//                                       stops dimming; called periodically via tick()

namespace Core {

class SleepInhibit {
public:
    // RAII: while at least one Guard is alive, auto-sleep is disabled.
    class Guard {
    public:
        Guard();
        ~Guard();
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&&)                 = delete;
        Guard& operator=(Guard&&)      = delete;
    };

    /// True while at least one Guard is held.
    static bool active();

    /// Call once per frame from the main loop. While inhibited, periodically tells
    /// the system the user is active, which keeps the screen from dimming as well
    /// as from sleeping. Cheap and rate-limited internally.
    static void tick();

    /// Teardown safety: force the count to zero and re-enable auto-sleep. Called
    /// during shutdown so we can never leave the console unable to sleep.
    static void force_release();
};

} // namespace Core
