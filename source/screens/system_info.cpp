// source/screens/system_info.cpp

#include "screens/system_info.hpp"
#include "core/system.hpp"
#include "core/battery.hpp"
#include "core/storage.hpp"
#include "core/atmosphere.hpp"
#include "core/activity.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <algorithm>

SystemInfoScreen::SystemInfoScreen() {}

void SystemInfoScreen::on_enter() {
    rebuild();
}

// ─── Row builders ──────────────────────────────────────────────────────────────

void SystemInfoScreen::add_header(const std::string& key) {
    Row r;
    r.is_header = true;
    r.label = Lang::t(key);
    m_rows.push_back(std::move(r));
}

void SystemInfoScreen::add_row(const std::string& label_key,
                                const std::string& value, bool sensitive) {
    Row r;
    r.label     = Lang::t(label_key);
    r.value     = value;
    r.sensitive = sensitive;
    m_rows.push_back(std::move(r));
}

// Helpers to stringify Val<T> with N/A fallback (Battery::Val).
template<typename T> static std::string vstr(const Core::Battery::Val<T>& v);
template<> std::string vstr<int>(const Core::Battery::Val<int>& v) {
    return v.valid ? std::to_string(v.value) : "N/A"; }
template<> std::string vstr<bool>(const Core::Battery::Val<bool>& v) {
    return v.valid ? (v.value ? "Yes" : "No") : "N/A"; }
template<> std::string vstr<float>(const Core::Battery::Val<float>& v) {
    if (!v.valid) return "N/A"; char b[32]; snprintf(b,sizeof(b),"%.1f",v.value); return b; }
template<> std::string vstr<std::string>(const Core::Battery::Val<std::string>& v) {
    return v.valid ? v.value : "N/A"; }

template<typename T> static std::string astr(const Core::Atmosphere::Val<T>& v);
template<> std::string astr<int>(const Core::Atmosphere::Val<int>& v) {
    return v.valid ? std::to_string(v.value) : "N/A"; }
template<> std::string astr<bool>(const Core::Atmosphere::Val<bool>& v) {
    return v.valid ? (v.value ? "Yes" : "No") : "N/A"; }
template<> std::string astr<std::string>(const Core::Atmosphere::Val<std::string>& v) {
    return v.valid ? v.value : "N/A"; }

template<typename T> static std::string cstr(const Core::Activity::Val<T>& v);
template<> std::string cstr<int>(const Core::Activity::Val<int>& v) {
    return v.valid ? std::to_string(v.value) : "N/A"; }
template<> std::string cstr<float>(const Core::Activity::Val<float>& v) {
    if (!v.valid) return "N/A"; char b[32]; snprintf(b,sizeof(b),"%.1f",v.value); return b; }
template<> std::string cstr<std::string>(const Core::Activity::Val<std::string>& v) {
    return v.valid ? v.value : "N/A"; }

// ─── Build all rows ─────────────────────────────────────────────────────────────

void SystemInfoScreen::rebuild() {
    m_rows.clear();

    const auto& fw  = Core::System::firmware();
    const auto& hw  = Core::System::hardware();
    const auto& ams = Core::Atmosphere::info();

    // Application / licence. AGPLv3 section 13 requires that users interacting
    // with a network-facing deployment be offered its corresponding source; the
    // service screens show this too, and this is the always-available copy.
    add_header("system_info.section_app");
    add_row("system_info.app_version", APP_VERSION);
    add_row("system_info.app_license", "AGPLv3");
    add_row("system_info.app_source",  APP_SOURCE_URL);

    add_header("system_info.section_firmware");
    add_row("system_info.fw_version",           fw.version.or_na());
    add_row("system_info.fw_platform",          fw.platform.or_na());
    add_row("system_info.fw_version_hash",      fw.version_hash.or_na());
    add_row("system_info.fw_displayed_version", fw.displayed_version.or_na());
    add_row("system_info.fw_display_name",      fw.display_name.or_na());
    add_row("system_info.fw_dram_id",           fw.dram_id.or_na(), true);
    add_row("system_info.fw_fuses_burned",      fw.fuses_burned.or_na());
    add_row("system_info.fw_soc_type",          fw.soc_type.or_na());
    add_row("system_info.fw_equipment_type",    fw.equipment_type.or_na());
    add_row("system_info.fw_purpose",           fw.purpose.or_na());
    add_row("system_info.fw_device_id",         fw.device_id.or_na(), true);
    add_row("system_info.fw_hiz_charging",      fw.hiz_charging.or_na());
    add_row("system_info.fw_kiosk_mode",        fw.kiosk_mode.or_na());
    add_row("system_info.fw_serial_reported",   fw.serial_reported.or_na(), true);
    add_row("system_info.fw_serial_true",       fw.serial_true.or_na(), true);
    add_row("system_info.fw_prodinfo_blanked",  fw.prodinfo_blanked.or_na());
    add_row("system_info.fw_language",          fw.language.or_na());
    add_row("system_info.fw_region",            fw.region.or_na());
    add_row("system_info.fw_nickname",          fw.nickname.or_na());
    add_row("system_info.fw_parental_pin",      fw.parental_pin.or_na(), true);

    add_header("system_info.section_atmosphere");
    if (ams.detected) {
        add_row("system_info.atm_version",        astr(ams.version));
        add_row("system_info.atm_key_generation", astr(ams.key_generation));
        add_row("system_info.atm_target_fw",      astr(ams.target_firmware));
        add_row("system_info.atm_git_hash",       astr(ams.git_hash));
        add_row("system_info.atm_has_rcm_fix",    astr(ams.has_rcm_bug));
        add_row("system_info.atm_clears_cal0",    astr(ams.exosphere_clears_cal0));
        add_row("system_info.atm_emummc_enabled", astr(ams.emummc_enabled));
        add_row("system_info.atm_force_usb3",     astr(ams.force_usb3));
        add_row("system_info.atm_supported_hos",  astr(ams.supported_hos));
    } else {
        add_row("system_info.atm_version", "Not detected (stock)");
    }

    add_header("system_info.section_sd");
    {
        auto sd = Core::Storage::sd_card();
        if (sd.valid) {
            char cap[64];
            snprintf(cap, sizeof(cap), "%.1f GB free / %.0f GB",
                     sd.gb_free(), sd.gb_total());
            add_row("system_info.sd_product_name", cap);
        } else {
            add_row("system_info.sd_product_name", "N/A");
        }
        add_row("system_info.sd_cid",               "N/A", true);
        add_row("system_info.sd_manufacturer",      "N/A");
        add_row("system_info.sd_oem_id",            "N/A");
        add_row("system_info.sd_product_revision",  "N/A");
        add_row("system_info.sd_serial",            "N/A", true);
        add_row("system_info.sd_manufacturing_date","N/A");
    }

    add_header("system_info.section_power");
    {
        auto p = Core::Battery::power();
        add_row("system_info.pwr_battery_pct",
                p.valid ? (std::to_string(p.charge_percent) + "%") : "N/A");
        add_row("system_info.pwr_source_type", p.connected ? "Connected" : "Battery");
        add_row("system_info.pwr_sufficient", p.valid ? "Yes" : "N/A");
    }

    add_header("system_info.section_charging");
    {
        auto c = Core::Battery::charge_info();
        add_row("system_info.chg_input_current",   vstr(c.input_current_limit_ma));
        add_row("system_info.chg_input_boost",     vstr(c.input_current_boost_ma));
        add_row("system_info.chg_fast_current",    vstr(c.fast_charge_current_ma));
        add_row("system_info.chg_voltage_limit",   vstr(c.charge_voltage_limit_mv));
        add_row("system_info.chg_configuration",   vstr(c.charging_config));
        add_row("system_info.chg_hiz_mode",        vstr(c.hiz_enabled));
        add_row("system_info.chg_charging_enabled",vstr(c.charging_enabled));
        add_row("system_info.chg_supply_route",    vstr(c.supply_route));
        add_row("system_info.chg_temperature",     vstr(c.battery_temp_c));
        add_row("system_info.chg_current_capacity",vstr(c.current_capacity_mah));
        add_row("system_info.chg_voltage_mv",      vstr(c.battery_voltage_mv));
        add_row("system_info.chg_age_pct",         vstr(c.battery_age_pct));
        add_row("system_info.chg_power_role",      vstr(c.power_role));
        add_row("system_info.chg_power_source",    vstr(c.power_source));
        add_row("system_info.chg_source_voltage",  vstr(c.source_voltage_mv));
        add_row("system_info.chg_source_current",  vstr(c.source_current_ma));
        add_row("system_info.chg_fast_allowed",    vstr(c.fast_charging_allowed));
        add_row("system_info.chg_controller_obtained", vstr(c.controller_obtained));
        add_row("system_info.chg_otg_requested",   vstr(c.otg_requested));
    }

    add_header("system_info.section_max17050");
    {
        auto m = Core::Battery::max17050();
        add_row("system_info.max_charge_pct",       vstr(m.charge_pct));
        add_row("system_info.max_capacity_mah",     vstr(m.current_capacity_mah));
        add_row("system_info.max_full_capacity",    vstr(m.full_capacity_mah));
        add_row("system_info.max_factory_capacity", vstr(m.factory_capacity_mah));
        add_row("system_info.max_voltage_mv",       vstr(m.voltage_mv));
        add_row("system_info.max_current_draw",     vstr(m.current_draw_ma));
        add_row("system_info.max_charge_discharge", vstr(m.charge_discharge_pct));
        add_row("system_info.max_temperature",      vstr(m.temperature_c));
        add_row("system_info.max_avg_temperature",  vstr(m.avg_temperature_c));
    }

    add_header("system_info.section_battery_params");
    {
        auto pr = Core::Battery::controller_params();
        add_row("system_info.bp_rcomp0",      vstr(pr.rcomp0));
        add_row("system_info.bp_tempc0",      vstr(pr.tempc0));
        add_row("system_info.bp_fullcap",     vstr(pr.full_cap));
        add_row("system_info.bp_fullcapnom",  vstr(pr.full_cap_nom));
        add_row("system_info.bp_iavg_empty",  vstr(pr.iavg_empty));
        add_row("system_info.bp_qr_table_00", vstr(pr.qr_table_00));
        add_row("system_info.bp_qr_table_10", vstr(pr.qr_table_10));
        add_row("system_info.bp_qr_table_20", vstr(pr.qr_table_20));
        add_row("system_info.bp_qr_table_30", vstr(pr.qr_table_30));
        add_row("system_info.bp_sum",         vstr(pr.sum_charge_discharge_pct));
    }

    add_header("system_info.section_hardware");
    add_row("system_info.hw_bt_mac",     hw.bluetooth_mac.or_na(), true);
    add_row("system_info.hw_wifi_mac",   hw.wifi_mac.or_na(), true);
    add_row("system_info.hw_config_id1", hw.config_id1.or_na(), true);
    add_row("system_info.hw_serial",     hw.serial.or_na(), true);
    add_row("system_info.hw_battery_lot",hw.battery_lot.or_na());
    add_row("system_info.hw_screen_id",  hw.screen_id.or_na(), true);

    add_header("system_info.section_activity");
    {
        auto a = Core::Activity::summary();
        add_row("system_info.act_rtc_started",    cstr(a.rtc_started));
        add_row("system_info.act_first_event",    cstr(a.first_event_date));
        add_row("system_info.act_play_sessions",  cstr(a.total_sessions));
        add_row("system_info.act_unique_games",   cstr(a.unique_games));
        add_row("system_info.act_total_playtime", cstr(a.total_playtime_h));
        add_row("system_info.act_active_playtime",cstr(a.active_playtime_h));
    }
}

// ─── Update ───────────────────────────────────────────────────────────────────

std::unique_ptr<Screen> SystemInfoScreen::update(bool& pop) {
    pop = false;

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    if (Input::pressed(Input::Button::Y)) m_reveal = !m_reveal;

    const int row_h = 28;
    int visible = (Layout::CONTENT_H - 40) / row_h;

    if (Input::repeat(Input::Button::DDown)) {
        if (m_scroll < (int)m_rows.size() - visible) m_scroll++;
    }
    if (Input::repeat(Input::Button::DUp)) {
        if (m_scroll > 0) m_scroll--;
    }
    if (Input::pressed(Input::Button::R)) {
        m_scroll = std::min(m_scroll + visible, std::max(0, (int)m_rows.size() - visible));
    }
    if (Input::pressed(Input::Button::L)) {
        m_scroll = std::max(m_scroll - visible, 0);
    }

    return nullptr;
}

// ─── Draw ───────────────────────────────────────────────────────────────────

void SystemInfoScreen::draw() {
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    const int row_h    = 28;
    const int label_x  = x + Layout::PAD_LG;
    const int value_x  = x + 420;
    int visible = (h - 40) / row_h;

    int cur_y = y + Layout::PAD_SM;

    for (int i = m_scroll; i < (int)m_rows.size() && i < m_scroll + visible; ++i) {
        const Row& row = m_rows[i];

        if (row.is_header) {
            Widgets::draw_text(label_x - Layout::PAD_SM, cur_y + 4, row.label,
                               Font::Size::Small, Font::Weight::Bold,
                               Theme::Token::Accent);
            Theme::apply(r, Theme::Token::Border);
            Renderer::hline(label_x - Layout::PAD_SM, cur_y + row_h - 2,
                            w - (label_x - x) - Layout::PAD_LG);
        } else {
            Widgets::draw_text(label_x, cur_y, row.label,
                               Font::Size::Small, Font::Weight::Regular,
                               Theme::Token::FgSecondary, value_x - label_x - 8);
            std::string shown = row.value;
            Theme::Token vcolor = Theme::Token::FgPrimary;
            if (row.sensitive && !m_reveal && row.value != "N/A") {
                shown = "••••••••";
                vcolor = Theme::Token::FgDisabled;
            }
            if (row.value == "N/A") vcolor = Theme::Token::FgDisabled;

            Widgets::draw_text(value_x, cur_y, shown,
                               Font::Size::Small, Font::Weight::Regular,
                               vcolor, w - value_x - Layout::PAD_LG);
        }
        cur_y += row_h;
    }

    if ((int)m_rows.size() > visible) {
        char pos[48];
        snprintf(pos, sizeof(pos), "%d-%d / %zu",
                 m_scroll + 1,
                 std::min(m_scroll + visible, (int)m_rows.size()),
                 m_rows.size());
        Widgets::draw_text(x + Layout::PAD_LG, y + h - 28, pos,
                           Font::Size::Tiny, Font::Weight::Regular,
                           Theme::Token::FgDisabled);
    }

    std::vector<Widgets::ButtonHint> hints = {
        { "Y", m_reveal ? "Hide IDs" : "Reveal IDs" },
        { "L/R", Lang::t("hints.page_next") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 30, w, hints);
}
