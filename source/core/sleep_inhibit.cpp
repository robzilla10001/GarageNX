// source/core/sleep_inhibit.cpp

#include "core/sleep_inhibit.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <SDL2/SDL.h>
#endif

namespace {

int      g_count       = 0;   // how many Guards are alive
uint32_t g_last_report = 0;   // last appletReportUserIsActive(), ms

// Refresh the idle timer well inside any plausible dim/sleep timeout. The shortest
// the system allows is a minute, so every 15s is comfortably frequent and costs
// nothing measurable.
constexpr uint32_t kReportIntervalMs = 15000;

void set_inhibited(bool on) {
#ifdef PLATFORM_SWITCH
    // Two DIFFERENT mechanisms, both needed — confirmed on hardware:
    //
    //   appletSetAutoSleepDisabled       stops the console AUTO-SLEEPING.
    //   appletSetIdleTimeDetectionExtension
    //                                    extends user-inactivity detection, which
    //                                    is what governs the DIMMING / screen-off
    //                                    timer.
    //
    // The first alone is not enough: with auto-sleep disabled and reading back as
    // disabled, the screen still went dark after ~15 minutes, because screen-off
    // runs off the idle timer rather than the sleep timer. Adding the extension is
    // what actually kept the display alive past 30 minutes.
    Result rc = appletSetAutoSleepDisabled(on);
    if (R_FAILED(rc))
        SDL_Log("sleep_inhibit: appletSetAutoSleepDisabled(%d) rc=0x%08X", (int)on, rc);

    rc = appletSetIdleTimeDetectionExtension(
        on ? AppletIdleTimeDetectionExtension_Extended
           : AppletIdleTimeDetectionExtension_None);
    if (R_FAILED(rc))
        SDL_Log("sleep_inhibit: appletSetIdleTimeDetectionExtension(%d) rc=0x%08X", (int)on, rc);
#else
    (void)on;
#endif
}

} // namespace

namespace Core {

SleepInhibit::Guard::Guard() {
    if (++g_count == 1) {
        set_inhibited(true);
#ifdef PLATFORM_SWITCH
        g_last_report = SDL_GetTicks();
#endif
    }
}

SleepInhibit::Guard::~Guard() {
    if (g_count > 0 && --g_count == 0) set_inhibited(false);
}

bool SleepInhibit::active() { return g_count > 0; }

void SleepInhibit::tick() {
#ifdef PLATFORM_SWITCH
    if (g_count <= 0) return;
    const uint32_t now = SDL_GetTicks();
    if (now - g_last_report < kReportIntervalMs) return;
    g_last_report = now;
    // Equivalent to the user touching the controller: resets the idle timer, so the
    // screen stops dimming too, not just sleeping.
    appletReportUserIsActive();
#endif
}

void SleepInhibit::force_release() {
    if (g_count != 0) {
        g_count = 0;
        set_inhibited(false);
    }
}

} // namespace Core
