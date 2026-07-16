#pragma once
// source/core/atmosphere.hpp
// Detects Atmosphère CFW and reports its version and configuration.
// Values come from the exosphere SMC / SPL config items Atmosphère exposes.
// On stock firmware or when unavailable, fields are marked invalid.

#include <string>
#include <cstdint>

namespace Core::Atmosphere {

template<typename T>
struct Val { T value{}; bool valid = false; };

struct Info {
    bool             detected = false;   // true if Atmosphère is running
    Val<std::string> version;            // e.g. "1.7.1"
    Val<std::string> target_firmware;    // e.g. "18.1.0"
    Val<int>         key_generation;     // keygen index
    Val<std::string> git_hash;           // build hash if exposed
    Val<bool>        has_rcm_bug;         // console is RCM-exploitable (Erista)
    Val<bool>        exosphere_clears_cal0;
    Val<bool>        allow_cal_writes;
    Val<bool>        emummc_enabled;
    Val<bool>        force_usb3;
    Val<std::string> supported_hos;      // supported HOS version string
};

const Info& info();
void refresh();

} // namespace Core::Atmosphere
