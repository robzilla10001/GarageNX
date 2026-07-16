#pragma once
// source/core/system.hpp
// Firmware, hardware, and region/locale information via libnx setsys/spl/set.
// Each field carries a validity flag so the UI can show "N/A" when a read fails
// rather than a misleading zero or empty string.

#include <string>
#include <cstdint>

namespace Core::System {

// A string field that may or may not have been successfully read.
struct Field {
    std::string value;
    bool        valid = false;

    // Convenience: return value if valid, else a placeholder.
    const std::string& or_na() const;
};

struct FirmwareInfo {
    Field version;             // e.g. "18.1.0"
    Field platform;            // e.g. "NX"
    Field version_hash;        // git-style hash string
    Field displayed_version;   // user-facing version string
    Field display_name;        // e.g. "NintendoSDK Firmware for NX ..."
    Field dram_id;
    Field fuses_burned;        // count of burned anti-downgrade fuses
    Field soc_type;            // Erista / Mariko (inferred)
    Field equipment_type;
    Field purpose;
    Field device_id;           // SENSITIVE
    Field hiz_charging;        // Hi-Z charging mode bool → "Yes"/"No"
    Field kiosk_mode;          // bool
    Field serial_reported;     // SENSITIVE — from setsys
    Field serial_true;         // SENSITIVE — real serial, or a note if blanked
    Field prodinfo_blanked;    // "Yes" if exosphere is blanking PRODINFO
    Field language;            // resolved language name
    Field region;              // resolved region name
    Field nickname;            // console nickname (mii/device)
    Field parental_pin;        // SENSITIVE — may be unavailable
};

struct HardwareInfo {
    Field bluetooth_mac;       // SENSITIVE
    Field wifi_mac;            // SENSITIVE
    Field config_id1;
    Field serial;              // SENSITIVE
    Field battery_lot;
    Field screen_id;
};

// Firmware/hardware are relatively stable; read once and cache.
const FirmwareInfo& firmware();
const HardwareInfo& hardware();

// Force a re-read (e.g. after a settings change like nickname).
void refresh();

// The SDK version libnx was built against (compile-time), for the title bar.
std::string sdk_version();

// Fields marked SENSITIVE above are gated behind this flag in the UI. The core
// layer always reads them; the screen decides whether to display. This helper
// lists which field labels are sensitive so the screen can mask them uniformly.
bool is_sensitive_field(const std::string& label_key);

} // namespace Core::System
