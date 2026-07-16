#pragma once
// source/core/storage.hpp
// SD card and NAND capacity, plus SoC temperature. Fast to query — safe to poll
// from the status bar on an interval.

#include <cstdint>

namespace Core::Storage {

struct SpaceInfo {
    uint64_t total_bytes = 0;
    uint64_t free_bytes  = 0;
    bool     valid       = false;

    uint64_t used_bytes() const { return (total_bytes > free_bytes)
                                       ? total_bytes - free_bytes : 0; }
    float used_fraction() const { return (total_bytes > 0)
                                       ? (float)used_bytes() / (float)total_bytes : 0.f; }
    float gb_total() const { return total_bytes / (1024.f*1024.f*1024.f); }
    float gb_free()  const { return free_bytes  / (1024.f*1024.f*1024.f); }
};

// SD card capacity (the sdmc: mount).
SpaceInfo sd_card();

// NAND user partition capacity. On PC stub returns a plausible value.
SpaceInfo nand_user();

} // namespace Core::Storage


namespace Core::Thermal {

// SoC temperature in degrees Celsius. valid=false if the sensor read failed.
struct Temp {
    float celsius = 0.f;
    bool  valid   = false;
};

Temp soc();

// PCB/skin temperature, if available (used later; harmless to expose now).
Temp pcb();

} // namespace Core::Thermal
