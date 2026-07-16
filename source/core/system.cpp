// source/core/system.cpp

#include "core/system.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <dirent.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::System {

// ─── Field helper ─────────────────────────────────────────────────────────────

static const std::string s_na = "N/A";

const std::string& Field::or_na() const {
    return valid ? value : s_na;
}

static Field ok(const std::string& v)  { return Field{v, true}; }
static Field na()                       { return Field{"", false}; }
static Field boolfield(bool b)          { return Field{b ? "Yes" : "No", true}; }

// ─── State ────────────────────────────────────────────────────────────────────

static FirmwareInfo s_fw;
static HardwareInfo s_hw;
static bool         s_loaded = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────

#ifdef PLATFORM_SWITCH

static std::string mac_to_string(const uint8_t mac[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

// Resolve a SetLanguage enum to a readable name.
static std::string language_name(u64 lang_code) {
    // lang_code is a packed ASCII language tag like "en-US". Just display it.
    char buf[16] = {0};
    std::memcpy(buf, &lang_code, sizeof(lang_code) < 15 ? sizeof(lang_code) : 15);
    // Ensure printable
    for (char& c : buf) if (c != 0 && (c < 0x20 || c > 0x7E)) c = 0;
    return buf[0] ? std::string(buf) : "Unknown";
}

static std::string region_name(SetRegion r) {
    switch (r) {
        case SetRegion_JPN: return "Japan";
        case SetRegion_USA: return "The Americas";
        case SetRegion_EUR: return "Europe";
        case SetRegion_AUS: return "Australia/NZ";
        case SetRegion_HTK: return "Hong Kong/Taiwan/Korea";
        case SetRegion_CHN: return "China";
        default:            return "Unknown";
    }
}

#endif // PLATFORM_SWITCH

#ifdef PLATFORM_SWITCH

// Recover the true serial from Atmosphère's pre-blank PRODINFO backup. The file
// is named "<SERIAL>_PRODINFO.bin" in sdmc:/atmosphere/automatic_backups/. We
// only need the directory entry name — the file contents are irrelevant (and
// may read as 0 bytes). Returns an invalid Field if no such backup exists.
static Field read_serial_from_backup() {
    const char* dir_path = "sdmc:/atmosphere/automatic_backups/";
    DIR* dir = opendir(dir_path);
    if (!dir) return na();

    Field result = na();
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        // Match "*_PRODINFO.bin" (case-insensitive on the suffix).
        auto us = name.find('_');
        if (us == std::string::npos || us == 0) continue;

        std::string upper = name;
        for (char& c : upper) c = (char)toupper((unsigned char)c);
        if (upper.find("_PRODINFO.BIN") == std::string::npos) continue;

        // Serial is everything before the first underscore.
        std::string serial = name.substr(0, us);
        // Basic sanity: retail serials are ~14 alphanumeric chars.
        if (serial.size() >= 4 && serial.size() <= 20) {
            result = ok(serial);
            break;
        }
    }
    closedir(dir);
    return result;
}

#else
static Field read_serial_from_backup() { return ok("XAW10012345678"); }
#endif

// ─── Load ─────────────────────────────────────────────────────────────────────

static void load_all() {
    // Reset to N/A so any field we can't read shows correctly.
    s_fw = FirmwareInfo{};
    s_hw = HardwareInfo{};

#ifdef PLATFORM_SWITCH
    // ── Firmware version (setsys) ──────────────────────────────────────────────
    SetSysFirmwareVersion fw;
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        char ver[32];
        snprintf(ver, sizeof(ver), "%u.%u.%u", fw.major, fw.minor, fw.micro);
        s_fw.version           = ok(ver);
        s_fw.platform          = ok(fw.platform);
        s_fw.version_hash      = ok(fw.version_hash);
        s_fw.displayed_version = ok(fw.display_version);
        s_fw.display_name      = ok(fw.display_title);
    }

    // ── Serial numbers ─────────────────────────────────────────────────────────
    // set:sys reports the system serial. Atmosphère's set.mitm blanks it (3-char
    // prefix + zeros) when blank_prodinfo is active.
    SetSysSerialNumber serial;
    if (R_SUCCEEDED(setsysGetSerialNumber(&serial))) {
        s_fw.serial_reported = ok(serial.number);
        s_hw.serial          = ok(serial.number);
    }

    // The TRUE serial (accurate even when PRODINFO is blanked) is what DBI
    // shows. Atmosphère writes a PRODINFO backup before blanking to
    //   sdmc:/atmosphere/automatic_backups/<SERIAL>_PRODINFO.bin
    // with the real serial encoded IN THE FILENAME. We read it straight from the
    // directory listing — no NAND/BIS access or keys required. This is exactly
    // how DBI recovers the "guessed"/true serial.
    s_fw.serial_true = read_serial_from_backup();

    // ── Device nickname (setsys) ───────────────────────────────────────────────
    SetSysDeviceNickName nick;
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick))) {
        s_fw.nickname = ok(nick.nickname);
    }

    // ── Region & language (set) ────────────────────────────────────────────────
    SetRegion region;
    if (R_SUCCEEDED(setGetRegionCode(&region))) {
        s_fw.region = ok(region_name(region));
    }
    u64 lang_code = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&lang_code))) {
        s_fw.language = ok(language_name(lang_code));
    }

    // ── Product model → SoC generation + board/equipment model ────────────────
    // These are two different things, and DBI shows the board model:
    //   SetSysProductModel  Board name   SoC generation
    //   Nx                  Icosa        Erista  (retail original)
    //   Copper              Copper       Erista  (dev unit)
    //   Iowa                Iowa         Mariko  (retail V2)
    //   Hoag                Hoag         Mariko  (Lite)
    //   Calcio              Calcio       Mariko  (dev unit)
    //   Aula                Aula         Mariko  (OLED)
    SetSysProductModel model;
    if (R_SUCCEEDED(setsysGetProductModel(&model))) {
        const char* board;
        const char* soc;
        switch (model) {
            case SetSysProductModel_Nx:     board = "Icosa";  soc = "Erista"; break;
            case SetSysProductModel_Copper: board = "Copper"; soc = "Erista"; break;
            case SetSysProductModel_Iowa:   board = "Iowa";   soc = "Mariko"; break;
            case SetSysProductModel_Hoag:   board = "Hoag";   soc = "Mariko"; break;
            case SetSysProductModel_Calcio: board = "Calcio"; soc = "Mariko"; break;
            case SetSysProductModel_Aula:   board = "Aula";   soc = "Mariko"; break;
            default:                        board = "Unknown"; soc = "Unknown"; break;
        }
        s_fw.soc_type       = ok(soc);
        s_fw.equipment_type = ok(board);
    }

    // ── MAC addresses ──────────────────────────────────────────────────────────
    // Wi-Fi / BT MACs from calibration data.
    {
        SetCalMacAddress wifi_mac_cal;
        if (R_SUCCEEDED(setcalGetWirelessLanMacAddress(&wifi_mac_cal))) {
            s_hw.wifi_mac = ok(mac_to_string(wifi_mac_cal.addr));
        }
        SetCalBdAddress bt_addr;
        if (R_SUCCEEDED(setcalGetBdAddress(&bt_addr))) {
            s_hw.bluetooth_mac = ok(mac_to_string(bt_addr.bd_addr));
        }
    }

    // ── Configuration ID 1 (calibration) ──────────────────────────────────────
    // SetCalConfigurationId1 is a fixed 0x1E-byte blob (cfg[]); it's a printable
    // ASCII string on retail units. Copy up to its length, trimming at any NUL.
    {
        SetCalConfigurationId1 cfg1;
        if (R_SUCCEEDED(setcalGetConfigurationId1(&cfg1))) {
            size_t len = 0;
            while (len < sizeof(cfg1.cfg) && cfg1.cfg[len] != 0) len++;
            s_hw.config_id1 = ok(std::string(reinterpret_cast<const char*>(cfg1.cfg), len));
        }
    }
    // Battery lot code is not exposed by a public setcal call in libnx, so it
    // remains N/A (honest placeholder per the design decision).

    // ── Fuses / device id / blanked-PRODINFO detection ────────────────────────
    {
        u64 device_id = 0;
        bool blank_prodinfo = false;

        if (R_SUCCEEDED(splInitialize())) {
            // Device ID (best effort)
            if (R_SUCCEEDED(splGetConfig((SplConfigItem)18 /*DeviceId*/, &device_id))) {
                char buf[24];
                snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)device_id);
                s_fw.device_id = ok(buf);
            }

            // Atmosphère exosphere config item 65005 = ShouldBlankProdInfo.
            // When set, set:sys reports a dummied serial. We surface this as its
            // own row so a blanked "reported" serial has a visible explanation
            // rather than looking like a bug.
            u64 blank = 0;
            if (R_SUCCEEDED(splGetConfig((SplConfigItem)65005, &blank))) {
                blank_prodinfo = (blank != 0);
            }
            splExit();
        }

        s_fw.prodinfo_blanked = boolfield(blank_prodinfo);
    }

    // ── Battery lot (calibration) ──────────────────────────────────────────────
    // The libnx type is SetBatteryLot (not SetCalBatteryLot).
    {
        SetBatteryLot lot;
        if (R_SUCCEEDED(setcalGetBatteryLot(&lot))) {
            size_t len = 0;
            while (len < sizeof(lot.lot) && lot.lot[len] != 0) len++;
            s_hw.battery_lot = ok(std::string(lot.lot, len));
        }
    }

    // Fields we don't yet have a reliable public-service read for on all
    // firmwares are left as N/A intentionally: fuses_burned, purpose,
    // hiz_charging, kiosk_mode, dram_id, parental_pin, screen_id.
    // These get filled in as we validate the correct service calls against real
    // hardware; N/A is the honest placeholder per the design decision.

#else
    // ── PC stub: fabricate plausible values for UI development ──────────────────
    s_fw.version           = ok("18.1.0");
    s_fw.platform          = ok("NX (PC stub)");
    s_fw.version_hash      = ok("0000000000000000");
    s_fw.displayed_version = ok("18.1.0");
    s_fw.display_name      = ok("GarageNX PC Development Stub");
    s_fw.soc_type          = ok("N/A (PC)");
    s_fw.equipment_type    = ok("N/A (PC)");
    s_fw.region            = ok("The Americas");
    s_fw.language          = ok("en-US");
    s_fw.nickname          = ok("DevSwitch");
    s_fw.serial_reported   = ok("XAW00000000000");
    s_fw.serial_true       = ok("XAW00000000000");
    s_fw.prodinfo_blanked  = ok("No");
    s_fw.device_id         = ok("DEADBEEFCAFE0000");
    s_hw.serial            = ok("XAW00000000000");
    s_hw.wifi_mac          = ok("00:11:22:33:44:55");
    s_hw.bluetooth_mac     = ok("00:11:22:33:44:56");
    s_hw.config_id1        = ok("PRODINFO_STUB");
    s_hw.battery_lot       = ok("LOT-0000");
#endif

    s_loaded = true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

const FirmwareInfo& firmware() {
    if (!s_loaded) load_all();
    return s_fw;
}

const HardwareInfo& hardware() {
    if (!s_loaded) load_all();
    return s_hw;
}

void refresh() {
    s_loaded = false;
    load_all();
}

std::string sdk_version() {
    // The NintendoSDK version the *firmware* was built against (what DBI shows,
    // e.g. "21.4.0" on FW 21.0.1). This is a genuinely separate three-part
    // number — it is NOT derivable from SetSysFirmwareVersion's major/minor/
    // micro/revision fields (verified on hardware: those yield "21.0.1.1").
    //
    // The real value lives in the SystemVersion title (program ID
    // 0100000000000809) on NAND, which must be read through NCM. That plumbing
    // arrives with Milestone 4 (title management), so rather than display a
    // wrong number we show an explicit placeholder. Honest beats plausible.
    return "—";  // em dash placeholder until Milestone 4
}

bool is_sensitive_field(const std::string& label_key) {
    // Keys (from en.json) whose values are device-identifying and should be
    // masked until the user reveals them.
    static const char* kSensitive[] = {
        "system_info.fw_device_id",
        "system_info.fw_serial_reported",
        "system_info.fw_serial_true",
        "system_info.fw_dram_id",
        "system_info.fw_parental_pin",
        "system_info.hw_bt_mac",
        "system_info.hw_wifi_mac",
        "system_info.hw_serial",
        "system_info.hw_config_id1",
        "system_info.hw_screen_id",
    };
    for (auto* k : kSensitive) if (label_key == k) return true;
    return false;
}

} // namespace Core::System
