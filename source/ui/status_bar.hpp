#pragma once
// source/ui/status_bar.hpp
// Persistent bottom bar: SD/NAND usage, SoC temp, battery, clock.
// Milestone 1: static placeholder data.
// Milestone 3: live data from libnx.

#include <string>

namespace StatusBar {

struct Info {
    // Storage (shown as "XX.X/XXXGB (XX%)")
    float sd_free_gb    = 0.f;
    float sd_total_gb   = 0.f;
    float nand_free_gb  = 0.f;
    float nand_total_gb = 0.f;

    // Thermals
    float soc_temp_c = 0.f;

    // Battery
    float battery_pct    = 1.f;     // [0.0, 1.0]
    bool  is_charging    = false;

    // Clock (populated when show_clock config is true)
    std::string clock_str;   // e.g. "14:32" or "14:32:07"
};

/// Update displayed data (call from main loop when data refreshes).
void set(const Info& info);

/// Draw the status bar. Call every frame.
void draw();

} // namespace StatusBar
