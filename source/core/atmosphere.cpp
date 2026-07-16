// source/core/atmosphere.cpp

#include "core/atmosphere.hpp"
#include <SDL2/SDL.h>
#include <cstdio>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Atmosphere {

static Info s_info;
static bool s_loaded = false;

#ifdef PLATFORM_SWITCH

// Exosphère custom SPL config items, per Atmosphère's spl_types.hpp:
//   65000 ExosphereApiVersion    65001 ExosphereNeedsReboot
//   65002 ExosphereNeedsShutdown 65003 ExosphereGitCommitHash
//   65004 ExosphereHasRcmBugPatch 65005 ExosphereBlankProdInfo
//   65006 ExosphereAllowCalWrites 65007 ExosphereEmummcType
//   65010 ExosphereForceEnableUsb30
static constexpr SplConfigItem CfgApiVersion     = (SplConfigItem)65000;
static constexpr SplConfigItem CfgGitCommitHash  = (SplConfigItem)65003;
static constexpr SplConfigItem CfgHasRcmBugPatch = (SplConfigItem)65004;
static constexpr SplConfigItem CfgBlankProdInfo  = (SplConfigItem)65005;
static constexpr SplConfigItem CfgAllowCalWrites = (SplConfigItem)65006;
static constexpr SplConfigItem CfgEmummcType     = (SplConfigItem)65007;
static constexpr SplConfigItem CfgForceUsb30     = (SplConfigItem)65010;

static void load_switch() {
    if (R_FAILED(splInitialize())) {
        s_info.detected = false;
        return;
    }

    u64 packed = 0;
    if (R_SUCCEEDED(splGetConfig(CfgApiVersion, &packed)) && packed != 0) {
        s_info.detected = true;

        // ExosphereApiVersion packs the Atmosphère version in the upper bytes:
        //   [56:48] major  [48:40] minor  [40:32] micro
        unsigned major = (packed >> 48) & 0xFF;
        unsigned minor = (packed >> 40) & 0xFF;
        unsigned micro = (packed >> 32) & 0xFF;

        char ver[32];
        snprintf(ver, sizeof(ver), "%u.%u.%u", major, minor, micro);
        s_info.version = { std::string(ver), (major || minor || micro) };

        // Key generation / master key revision sits in the low byte region.
        unsigned keygen = (packed >> 24) & 0xFF;
        if (keygen) s_info.key_generation = { (int)keygen, true };

        u64 v = 0;
        if (R_SUCCEEDED(splGetConfig(CfgGitCommitHash, &v)) && v != 0) {
            char h[24];
            snprintf(h, sizeof(h), "%016llX", (unsigned long long)v);
            s_info.git_hash = { std::string(h), true };
        }
        if (R_SUCCEEDED(splGetConfig(CfgHasRcmBugPatch, &v)))
            s_info.has_rcm_bug = { v == 0, true };   // patched → no RCM bug
        if (R_SUCCEEDED(splGetConfig(CfgBlankProdInfo, &v)))
            s_info.exosphere_clears_cal0 = { v != 0, true };
        if (R_SUCCEEDED(splGetConfig(CfgAllowCalWrites, &v)))
            s_info.allow_cal_writes = { v != 0, true };
        if (R_SUCCEEDED(splGetConfig(CfgEmummcType, &v)))
            s_info.emummc_enabled = { v != 0, true };
        if (R_SUCCEEDED(splGetConfig(CfgForceUsb30, &v)))
            s_info.force_usb3 = { v != 0, true };
    } else {
        s_info.detected = false;
    }

    splExit();
}

#endif // PLATFORM_SWITCH

static void load_all() {
    s_info = Info{};

#ifdef PLATFORM_SWITCH
    load_switch();
#else
    // PC stub — pretend we're on Atmosphère for UI development.
    s_info.detected            = true;
    s_info.version             = { "1.7.1", true };
    s_info.target_firmware     = { "18.1.0", true };
    s_info.key_generation      = { 17, true };
    s_info.git_hash            = { "stubbuild", true };
    s_info.has_rcm_bug         = { false, true };
    s_info.exosphere_clears_cal0 = { false, true };
    s_info.allow_cal_writes    = { false, true };
    s_info.emummc_enabled      = { true, true };
    s_info.force_usb3          = { false, true };
    s_info.supported_hos       = { "18.1.0", true };
#endif

    s_loaded = true;
}

const Info& info() {
    if (!s_loaded) load_all();
    return s_info;
}

void refresh() {
    s_loaded = false;
    load_all();
}

} // namespace Core::Atmosphere
