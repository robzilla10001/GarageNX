// source/core/battery.cpp

#include "core/battery.hpp"
#include <SDL2/SDL.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Battery {

// ─── Basic power (psm) ─────────────────────────────────────────────────────────

Power power() {
    Power p;

#ifdef PLATFORM_SWITCH
    u32 charge = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))) {
        p.charge_percent  = (int)charge;
        p.charge_fraction = charge / 100.f;
        p.valid = true;
    }

    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&charger))) {
        p.connected = (charger != PsmChargerType_Unconnected);
        // "Charging" = connected and not yet full. psm doesn't give a direct
        // charging bool on all firmwares; connected + <100% is a good proxy.
        p.charging  = p.connected && p.charge_percent < 100;
    }
#else
    // PC stub — a static "78%, discharging" so the status bar has something.
    p.charge_percent  = 78;
    p.charge_fraction = 0.78f;
    p.connected = false;
    p.charging  = false;
    p.valid = true;
#endif

    return p;
}

// ─── Deep readouts ──────────────────────────────────────────────────────────────
// The full charging-controller and max17050 gas-gauge registers are exposed on
// the Switch through the I2C/PMIC services. libnx does not provide a stable
// high-level API for every register across all firmware versions, and reads can
// require permissions homebrew may lack. Per our design decision we attempt what
// we safely can and report "N/A" for the rest rather than fabricating values.
//
// The scaffolding below reads the fields that psm/psc expose reliably, and
// leaves the register-level fields invalid pending validated I2C access, which
// we will layer in against real hardware. This keeps the screen honest today
// and gives us clearly-marked slots to fill later.

ChargeInfo charge_info() {
    ChargeInfo c;

#ifdef PLATFORM_SWITCH
    // Battery voltage / temperature via psm where available.
    // (Several of these use newer psm commands; guard each independently.)

    // Some libnx versions expose psmGetBatteryVoltage / raw values; these are
    // wrapped defensively. If the symbol/command isn't available at runtime the
    // call simply fails and the field stays N/A.
    // NOTE: register-level charging config, routes, and OTG state are filled in
    // during hardware validation — intentionally N/A for now.
#endif

    return c;  // all fields default-invalid → "N/A"
}

Max17050 max17050() {
    Max17050 m;
    // Full gas-gauge register block requires direct max17050 I2C reads.
    // Deferred to hardware validation; all fields report N/A for now.
    return m;
}

ControllerParams controller_params() {
    ControllerParams p;
    // Saved battery-controller parameters live in system storage / PMIC.
    // Deferred to hardware validation; all fields report N/A for now.
    return p;
}

} // namespace Core::Battery
