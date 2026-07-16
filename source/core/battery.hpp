#pragma once
// source/core/battery.hpp
// Battery charge, charging state, and deep controller registers.
// The basic charge/charging fields are reliable (psm service). The deep
// max17050 gas-gauge registers and charging-controller values require lower
// level access that may not be available; those fields carry validity flags.

#include <cstdint>
#include <string>

namespace Core::Battery {

// ─── Basic power (psm) — reliable, used by status bar ──────────────────────────

struct Power {
    float charge_fraction = 0.f;   // [0,1]
    int   charge_percent  = 0;     // 0..100
    bool  charging        = false; // true if charger connected AND charging
    bool  connected       = false; // charger/USB power connected
    bool  valid           = false;
};

Power power();

// ─── Deep readout for the System Information screen ─────────────────────────────
// Every field carries a validity flag; the UI shows "N/A" when invalid.

template<typename T>
struct Val {
    T    value{};
    bool valid = false;
};

struct ChargeInfo {
    Val<int>    input_current_limit_ma;
    Val<int>    input_current_boost_ma;
    Val<int>    fast_charge_current_ma;
    Val<int>    charge_voltage_limit_mv;
    Val<std::string> charging_config;
    Val<bool>   hiz_enabled;
    Val<bool>   charging_enabled;
    Val<std::string> supply_route;
    Val<float>  battery_temp_c;
    Val<int>    current_capacity_mah;
    Val<int>    battery_voltage_mv;
    Val<int>    battery_age_pct;
    Val<std::string> power_role;
    Val<std::string> power_source;
    Val<int>    source_voltage_mv;
    Val<int>    source_current_ma;
    Val<bool>   fast_charging_allowed;
    Val<bool>   controller_obtained;
    Val<bool>   otg_requested;
};

struct Max17050 {
    Val<int>    charge_pct;
    Val<int>    current_capacity_mah;
    Val<int>    full_capacity_mah;
    Val<int>    factory_capacity_mah;
    Val<int>    voltage_mv;
    Val<int>    current_draw_ma;
    Val<int>    charge_discharge_pct;
    Val<float>  temperature_c;
    Val<float>  avg_temperature_c;
};

struct ControllerParams {
    Val<int> rcomp0;
    Val<int> tempc0;
    Val<int> full_cap;
    Val<int> full_cap_nom;
    Val<int> iavg_empty;
    Val<int> qr_table_00;
    Val<int> qr_table_10;
    Val<int> qr_table_20;
    Val<int> qr_table_30;
    Val<int> sum_charge_discharge_pct;
};

// These deep reads are attempted best-effort. On hardware without the required
// access, or on the PC stub, most fields will be invalid (→ "N/A").
ChargeInfo       charge_info();
Max17050         max17050();
ControllerParams controller_params();

} // namespace Core::Battery
