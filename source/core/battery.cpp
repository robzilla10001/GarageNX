// source/core/battery.cpp

#include "core/battery.hpp"
#include <SDL2/SDL.h>
#include <cstdio>

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
        switch (charger) {
            case PsmChargerType_EnoughPower:  p.charger_type = "USB-PD (Enough Power)"; break;
            case PsmChargerType_LowPower:     p.charger_type = "USB-PD (Low Power)";    break;
            case PsmChargerType_NotSupported: p.charger_type = "Not Supported";         break;
            default:                          p.charger_type = "No charger";            break;
        }
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
    if (R_FAILED(psmInitialize())) return c;
    PsmBatteryChargeInfoFields f{};
    Result rc = psmGetBatteryChargeInfoFields(&f);
    psmExit();
    if (R_FAILED(rc)) return c;

    auto setv = [](auto& field, auto v) { field.value = v; field.valid = true; };

    setv(c.input_current_limit_ma,  (int)f.input_current_limit);
    setv(c.input_current_boost_ma,  (int)f.boost_mode_current_limit);
    setv(c.fast_charge_current_ma,  (int)f.fast_charge_current_limit);
    setv(c.charge_voltage_limit_mv, (int)f.charge_voltage_limit);
    setv(c.hiz_enabled,             f.hi_z_mode != 0);
    setv(c.charging_enabled,        f.battery_charging);

    const char* route = "Unknown";
    switch (f.vdd50_state) {
        case PsmVdd50State_Vdd50AOffVdd50BOff: route = "A off, B off"; break;
        case PsmVdd50State_Vdd50AOnVdd50BOff:  route = "A on, B off";  break;
        case PsmVdd50State_Vdd50AOffVdd50BOn:  route = "A off, B on";  break;
        default: break;
    }
    setv(c.supply_route, std::string(route));

    // temperature is milli-°C; charge/age are "per cent-mille" (÷1000 → percent).
    setv(c.battery_temp_c,     (float)f.temperature_celcius / 1000.0f);
    setv(c.battery_voltage_mv, (int)f.battery_charge_milli_voltage);
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%.2f%%", (float)f.battery_charge_percentage / 1000.0f);
        setv(c.current_capacity_mah, std::string(b));
        std::snprintf(b, sizeof(b), "%.2f%%", (float)f.battery_age_percentage / 1000.0f);
        setv(c.battery_age_pct, std::string(b));
    }

    // USB PD power role (Sink = console draws power, Source = OTG).
    auto role_str = [](u32 v) -> std::string {
        switch (v) {
            case 0:  return "Unknown";
            case 1:  return "Sink";
            case 2:  return "Source";
            default: { char b[16]; std::snprintf(b, sizeof(b), "0x%X", v); return b; }
        }
    };
    // Charger/source type (distinct from the sink/source role above).
    auto source_str = [](u32 v) -> std::string {
        switch (v) {
            case 0:  return "None";
            case 1:  return "Enough Power";
            case 2:  return "Low Power";
            case 3:  return "Not Supported";
            default: { char b[16]; std::snprintf(b, sizeof(b), "0x%X", v); return b; }
        }
    };
    setv(c.power_role,   role_str(f.usb_power_role));
    setv(c.power_source, source_str(f.usb_charger_type));
    setv(c.source_voltage_mv,     (int)f.charger_input_voltage_limit);
    setv(c.source_current_ma,     (int)f.charger_input_current_limit);
    setv(c.fast_charging_allowed, f.fast_battery_charging);
    setv(c.controller_obtained,   f.controller_power_supply);
    setv(c.otg_requested,         f.otg_request);

    // Charging configuration comes from the BQ24193 charger IC, not psm:
    // POWER_ON_CONFIG register (0x01), CHG_CONFIG bits [5:4].
    // (Register is 8-bit; single-byte read.) Verify the string vs DBI.
    if (R_SUCCEEDED(i2cInitialize())) {
        I2cSession bq;
        if (R_SUCCEEDED(i2cOpenSession(&bq, I2cDevice_Bq24193))) {
            uint8_t reg = 0x01, val = 0;
            if (R_SUCCEEDED(i2csessionSendAuto(&bq, &reg, 1, I2cTransactionOption_All)) &&
                R_SUCCEEDED(i2csessionReceiveAuto(&bq, &val, 1, I2cTransactionOption_All))) {
                const int cfg = (val >> 4) & 0x3;
                const char* s = (cfg == 0) ? "Charge Disabled"
                              : (cfg == 1) ? "Charge Battery"
                                           : "OTG";
                setv(c.charging_config, std::string(s));
            }
            i2csessionClose(&bq);
        }
        i2cExit();
    }
#endif

    return c;
}

Max17050 max17050() {
    Max17050 m;
#ifdef PLATFORM_SWITCH
    // Direct MAX17050 fuel-gauge reads over I2C. Each register is 16-bit,
    // little-endian; the standard read is "write reg addr, then read 2 bytes".
    if (R_FAILED(i2cInitialize())) return m;
    I2cSession s;
    if (R_FAILED(i2cOpenSession(&s, I2cDevice_Max17050))) { i2cExit(); return m; }

    auto rd = [&](uint8_t reg, uint16_t& out) -> bool {
        if (R_FAILED(i2csessionSendAuto(&s, &reg, 1, I2cTransactionOption_All)))
            return false;
        uint16_t v = 0;
        if (R_FAILED(i2csessionReceiveAuto(&s, &v, sizeof(v), I2cTransactionOption_All)))
            return false;
        out = v;   // device is little-endian, matches host
        return true;
    };

    // MAX17050 register map (subset).
    constexpr uint8_t REG_REP_CAP = 0x05, REG_REP_SOC = 0x06, REG_TEMP = 0x08,
                      REG_VCELL   = 0x09, REG_CURRENT = 0x0B, REG_FULL_CAP = 0x10,
                      REG_AVG_TA  = 0x16, REG_DESIGN_CAP = 0x18;

    // Scaling constants for the Switch board. Voltage is fixed by the part
    // (0.078125 mV/LSB). Capacity/current scale with the sense resistor: verified
    // as 10 mΩ on hardware (capacity read exactly 2x with a 5 mΩ assumption vs
    // Hekate — 8909 vs 4455 etc.).
    constexpr long long RSENSE_uOhm = 10000;

    auto setv = [](auto& field, auto v) { field.value = v; field.valid = true; };

    uint16_t raw = 0;
    if (rd(REG_VCELL, raw))    setv(m.voltage_mv,           (int)(((long long)raw * 5) / 64));
    if (rd(REG_REP_SOC, raw))  setv(m.charge_pct,           (int)(raw / 256));
    if (rd(REG_REP_CAP, raw))  setv(m.current_capacity_mah, (int)((long long)raw * 5000000LL / RSENSE_uOhm / 1000));
    if (rd(REG_FULL_CAP, raw)) setv(m.full_capacity_mah,    (int)((long long)raw * 5000000LL / RSENSE_uOhm / 1000));
    if (rd(REG_DESIGN_CAP,raw))setv(m.factory_capacity_mah, (int)((long long)raw * 5000000LL / RSENSE_uOhm / 1000));
    if (rd(REG_CURRENT, raw))  setv(m.current_draw_ma,      (int)((long long)(int16_t)raw * 1562500LL / RSENSE_uOhm / 1000));
    if (rd(REG_TEMP, raw))     setv(m.temperature_c,        (float)(int16_t)raw / 256.0f);
    if (rd(REG_AVG_TA, raw))   setv(m.avg_temperature_c,    (float)(int16_t)raw / 256.0f);

    // charge/discharge health: FullCap / DesignCap as a percentage.
    if (m.full_capacity_mah.valid && m.factory_capacity_mah.valid &&
        m.factory_capacity_mah.value > 0) {
        setv(m.charge_discharge_pct, (int)(100LL * m.full_capacity_mah.value /
                                           m.factory_capacity_mah.value));
    }

    i2csessionClose(&s);
    i2cExit();
#endif
    return m;
}

ControllerParams controller_params() {
    ControllerParams p;
    // Saved battery-controller parameters live in system storage / PMIC.
    // Deferred to hardware validation; all fields report N/A for now.
    return p;
}

} // namespace Core::Battery
