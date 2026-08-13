// source/screens/settings_screen.cpp

#include "screens/settings_screen.hpp"
#include "config/config.hpp"
#include "core/nand_mount.hpp"
#include "lang/localization.hpp"
#include "ui/input.hpp"
#include "ui/keyboard.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"

#include <SDL2/SDL.h>

namespace Settings {
namespace {
bool g_dirty = false;
}

bool dirty() { return g_dirty; }
void mark_dirty() { g_dirty = true; }

bool flush() {
    if (!g_dirty) return true;
    if (!Config::save()) {
        // Deliberately do NOT clear the flag. A failed write is retried at the
        // next boundary; clearing here would turn one failed write into a lost
        // setting the user believes they made.
        SDL_Log("Settings::flush — save FAILED, keeping changes dirty for retry");
        return false;
    }
    g_dirty = false;

    // Config-gated mounts are re-run so a newly enabled NAND surface works NOW
    // rather than after a restart. mount_nand() is written to be safe to call
    // repeatedly (it tracks what it already mounted), which is what makes this a
    // one-line change instead of a "restart required" label.
    //
    // A surface turned OFF is deliberately NOT unmounted here: the transports
    // stop listing it immediately, and unmounting a partition that a running
    // transport may be mid-read on is a far worse failure than a mount that
    // lingers until the next launch.
    Core::mount_nand();
    return true;
}

} // namespace Settings

// ─── Row construction ─────────────────────────────────────────────────────────

namespace {

// The ten surface toggles for one transport. Built from member pointers so all
// three transports share one definition — the alternative is thirty lambdas that
// disagree the first time a surface is added.
std::vector<SettingsScreen::Row> surface_rows(Config::Surfaces& s) {
    struct Def {
        bool Config::Surfaces::* field;
        const char*              lang_key;
        bool                     confirm_on_enable;
    };
    static const Def defs[] = {
        { &Config::Surfaces::sd_card,         "settings.mtp_sd",              false },
        { &Config::Surfaces::nand_user,       "settings.mtp_nand_user",       false },
        // The one surface where a mistake can brick the console.
        { &Config::Surfaces::nand_system,     "settings.mtp_nand_system",     true  },
        { &Config::Surfaces::installed_games, "settings.mtp_installed_games", false },
        { &Config::Surfaces::sd_install,      "settings.mtp_sd_install",      false },
        { &Config::Surfaces::nand_install,    "settings.mtp_nand_install",    false },
        { &Config::Surfaces::saves,           "settings.mtp_saves",           false },
        { &Config::Surfaces::album,           "settings.mtp_album",           false },
        { &Config::Surfaces::gamecard,        "settings.mtp_gamecard",        false },
        { &Config::Surfaces::user_storages,   "settings.mtp_user_storages",   false },
    };

    std::vector<SettingsScreen::Row> rows;
    for (const auto& d : defs) {
        SettingsScreen::Row r;
        r.kind  = SettingsScreen::Row::Kind::Toggle;
        r.label = Lang::t(d.lang_key);
        // Capturing &s is safe: it points into the process-lifetime config
        // singleton, not into anything this screen owns.
        r.get = [&s, f = d.field]() { return s.*f; };
        r.set = [&s, f = d.field](bool v) { s.*f = v; };
        if (d.confirm_on_enable) {
            r.confirm_on_enable = true;
            r.confirm_title     = Lang::t("settings.nand_system_confirm_title");
            r.confirm_body      = Lang::t("settings.nand_system_confirm_body");
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

// Compact builders. Settings rows are overwhelmingly "point at a config field",
// so spelling that out longhand thirty times would bury the few rows that are
// actually interesting (NAND System's confirmation, the clamped ports).
SettingsScreen::Row toggle_row(std::string label, bool Config::Behavior::* f) {
    SettingsScreen::Row r;
    r.kind  = SettingsScreen::Row::Kind::Toggle;
    r.label = std::move(label);
    r.get = [f] { return Config::get().behavior.*f; };
    r.set = [f](bool v) { Config::get_mutable().behavior.*f = v; };
    return r;
}

SettingsScreen::Row vis_row(std::string label, bool Config::Visibility::* f) {
    SettingsScreen::Row r;
    r.kind  = SettingsScreen::Row::Kind::Toggle;
    r.label = std::move(label);
    r.get = [f] { return Config::get().visibility.*f; };
    r.set = [f](bool v) { Config::get_mutable().visibility.*f = v; };
    return r;
}

SettingsScreen::Row text_row(std::string label,
                             std::function<std::string()> get,
                             std::function<void(const std::string&)> set,
                             bool secret = false) {
    SettingsScreen::Row r;
    r.kind        = SettingsScreen::Row::Kind::Text;
    r.label       = std::move(label);
    r.text_get    = std::move(get);
    r.text_set    = std::move(set);
    r.text_secret = secret;
    return r;
}

SettingsScreen::Row num_row(std::string label,
                            std::function<int()> get, std::function<void(int)> set,
                            int lo, int hi, std::string suffix = "") {
    SettingsScreen::Row r;
    r.kind       = SettingsScreen::Row::Kind::Number;
    r.label      = std::move(label);
    r.num_get    = std::move(get);
    r.num_set    = std::move(set);
    r.num_min    = lo;
    r.num_max    = hi;
    r.num_suffix = std::move(suffix);
    return r;
}

SettingsScreen::Row submenu_row(std::string label,
                                std::function<std::unique_ptr<Screen>()> open) {
    SettingsScreen::Row r;
    r.kind  = SettingsScreen::Row::Kind::Submenu;
    r.label = std::move(label);
    r.open  = std::move(open);
    return r;
}

} // namespace

std::unique_ptr<Screen> SettingsScreen::root() {
    std::vector<Row> storages = {
        submenu_row(Lang::t("settings.transport_mtp"), [] {
            return std::unique_ptr<Screen>(new SettingsScreen(
                Lang::t("settings.transport_mtp"),
                surface_rows(Config::get_mutable().mtp.surfaces)));
        }),
        submenu_row(Lang::t("settings.transport_ftp"), [] {
            return std::unique_ptr<Screen>(new SettingsScreen(
                Lang::t("settings.transport_ftp"),
                surface_rows(Config::get_mutable().ftp.surfaces)));
        }),
        submenu_row(Lang::t("settings.transport_http"), [] {
            return std::unique_ptr<Screen>(new SettingsScreen(
                Lang::t("settings.transport_http"),
                surface_rows(Config::get_mutable().http.surfaces)));
        }),
    };

    std::vector<Row> rows;
    rows.push_back(submenu_row(Lang::t("settings.section_storages"),
                               [storages] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_storages"), storages));
    }));

    // Saves section — currently just the auto-backup frequency, as a cycling
    // choice (Off / 1 / 3 / 7 / 14 / 30 days). 0 means off, which is the default.
    std::vector<Row> saves;
    {
        Row r;
        r.kind          = Row::Kind::Choice;
        r.label         = Lang::t("settings.auto_backup");
        r.choice_get    = [] { return Config::get().behavior.save_auto_backup_days; };
        r.choice_set    = [](int v) {
            Config::get_mutable().behavior.save_auto_backup_days = v;
        };
        r.choice_values = { 0, 1, 3, 7, 14, 30 };
        r.choice_labels = {
            Lang::t("common.off"),
            Lang::t("settings.days_1"),  Lang::t("settings.days_3"),
            Lang::t("settings.days_7"),  Lang::t("settings.days_14"),
            Lang::t("settings.days_30"),
        };
        saves.push_back(std::move(r));
    }
    rows.push_back(submenu_row(Lang::t("settings.section_saves"), [saves] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_saves"), saves));
    }));

    // ── FTP server ──────────────────────────────────────────────────────────
    // Port and credentials are read by FTPScreen::start_server(), so a change
    // takes effect the NEXT time the server is started, not on one already
    // running. Stopping and restarting from the FTP page applies it.
    std::vector<Row> ftp = {
        num_row(Lang::t("settings.ftp_port"),
                [] { return (int)Config::get().ftp.server_port; },
                [](int v) { Config::get_mutable().ftp.server_port = (uint16_t)v; },
                1, 65535),
        [] { Row r; r.kind = Row::Kind::Toggle;
             r.label = Lang::t("settings.ftp_anonymous");
             r.get = [] { return Config::get().ftp.allow_anonymous; };
             r.set = [](bool v) { Config::get_mutable().ftp.allow_anonymous = v; };
             return r; }(),
        text_row(Lang::t("settings.ftp_user"),
                 [] { return Config::get().ftp.login_user; },
                 [](const std::string& v) { Config::get_mutable().ftp.login_user = v; }),
        text_row(Lang::t("settings.ftp_pass"),
                 [] { return Config::get().ftp.login_pass; },
                 [](const std::string& v) { Config::get_mutable().ftp.login_pass = v; },
                 /*secret=*/true),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_ftp"), [ftp] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_ftp"), ftp));
    }));

    // ── Access point ────────────────────────────────────────────────────────
    std::vector<Row> ap = {
        [] { Row r; r.kind = Row::Kind::Toggle;
             r.label = Lang::t("settings.ap_enable");
             r.get = [] { return Config::get().ftp.start_access_point; };
             r.set = [](bool v) { Config::get_mutable().ftp.start_access_point = v; };
             return r; }(),
        text_row(Lang::t("settings.ap_ssid"),
                 [] { return Config::get().ftp.ssid; },
                 [](const std::string& v) { Config::get_mutable().ftp.ssid = v; }),
        text_row(Lang::t("settings.ap_password"),
                 [] { return Config::get().ftp.password; },
                 [](const std::string& v) { Config::get_mutable().ftp.password = v; },
                 /*secret=*/true),
        [] { Row r; r.kind = Row::Kind::Toggle;
             r.label = Lang::t("settings.ap_5ghz");
             r.get = [] { return Config::get().ftp.use_5ghz; };
             r.set = [](bool v) { Config::get_mutable().ftp.use_5ghz = v; };
             return r; }(),
        [] { Row r; r.kind = Row::Kind::Toggle;
             r.label = Lang::t("settings.ap_hidden");
             r.get = [] { return Config::get().ftp.hidden_ssid; };
             r.set = [](bool v) { Config::get_mutable().ftp.hidden_ssid = v; };
             return r; }(),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_ap"), [ap] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_ap"), ap));
    }));

    // ── HTTP server ─────────────────────────────────────────────────────────
    std::vector<Row> http = {
        num_row(Lang::t("settings.http_port"),
                [] { return (int)Config::get().http.server_port; },
                [](int v) { Config::get_mutable().http.server_port = (uint16_t)v; },
                1, 65535),
        [] { Row r; r.kind = Row::Kind::Toggle;
             r.label = Lang::t("settings.http_upload");
             r.get = [] { return Config::get().http.allow_upload; };
             r.set = [](bool v) { Config::get_mutable().http.allow_upload = v; };
             return r; }(),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_http"), [http] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_http"), http));
    }));

    // ── Behaviour ───────────────────────────────────────────────────────────
    // Every toggle here is honoured by real code. Four settings that never had a
    // consumer (highlight_update_files, show_cache_warming, rotate_screen,
    // use_overclocking) were removed from Config entirely rather than shown as
    // toggles that do nothing. action_logging, verify_hash_on_install, and the two
    // screen-dim timers are wired and offered below.
    std::vector<Row> behav = {
        [] {
            Row r;
            r.kind  = Row::Kind::Choice;
            r.label = Lang::t("settings.date_format");
            // Stored as a 3-letter order (DMY/MDY/YMD); shown as the familiar
            // slash form. datetime.cpp already reads behavior.date_format for the
            // clock + log names, so this takes effect immediately.
            r.choice_get = [] {
                const std::string& f = Config::get().behavior.date_format;
                return f == "MDY" ? 1 : (f == "YMD" ? 2 : 0);
            };
            r.choice_set = [](int v) {
                Config::get_mutable().behavior.date_format =
                    (v == 1) ? "MDY" : (v == 2) ? "YMD" : "DMY";
            };
            r.choice_values = { 0, 1, 2 };
            r.choice_labels = { "DD/MM/YYYY", "MM/DD/YYYY", "YYYY/MM/DD" };
            return r;
        }(),
        toggle_row(Lang::t("settings.button_repeat"),
                   &Config::Behavior::button_repeat_on_hold),
        toggle_row(Lang::t("settings.action_logging"),
                   &Config::Behavior::action_logging),
        toggle_row(Lang::t("settings.verify_hash_on_install"),
                   &Config::Behavior::verify_hash_on_install),
        num_row(Lang::t("settings.screen_dim_seconds"),
                [] { return Config::get().behavior.screen_dim_seconds; },
                [](int v) { Config::get_mutable().behavior.screen_dim_seconds = v; },
                0, 600, " s"),
        num_row(Lang::t("settings.screen_dim_seconds_net"),
                [] { return Config::get().behavior.screen_dim_seconds_net; },
                [](int v) { Config::get_mutable().behavior.screen_dim_seconds_net = v; },
                0, 600, " s"),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_behavior"), [behav] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_behavior"), behav));
    }));

    // ── Appearance ──────────────────────────────────────────────────────────
    std::vector<Row> appear = {
        [] {
            Row r;
            r.kind  = Row::Kind::Choice;
            r.label = Lang::t("settings.theme");
            // Theme is a std::string in config ("dark"/"light"), but Choice is
            // int-valued, so map through the index. Keeping Choice int-valued and
            // adapting here is less machinery than a second string-valued kind for
            // the one setting that needs it.
            r.choice_get = [] {
                return Config::get().app.theme == "light" ? 1 : 0;
            };
            r.choice_set = [](int v) {
                Config::get_mutable().app.theme = (v == 1) ? "light" : "dark";
                // Same call main.cpp makes at startup; the header says it is safe
                // at any time and takes effect next frame, so the theme switches
                // live rather than at restart.
                Theme::set(v == 1 ? Theme::Variant::Light : Theme::Variant::Dark);
            };
            r.choice_values = { 0, 1 };
            r.choice_labels = { Lang::t("settings.theme_dark"),
                                Lang::t("settings.theme_light") };
            return r;
        }(),
        [] {
            Row r;
            r.kind  = Row::Kind::Choice;
            r.label = Lang::t("settings.language");
            // Same int-index adaptation as the theme row. Only languages that
            // actually ship a file are offered — listing a language with no
            // translation would silently fall back to English and look broken.
            r.choice_get = [] { return Config::get().app.language == "en" ? 0 : 0; };
            r.choice_set = [](int) { Config::get_mutable().app.language = "en"; };
            r.choice_values = { 0 };
            r.choice_labels = { "English" };
            return r;
        }(),
        toggle_row(Lang::t("settings.show_clock"), &Config::Behavior::show_clock),
        toggle_row(Lang::t("settings.show_seconds"), &Config::Behavior::show_seconds),
        toggle_row(Lang::t("settings.time_24h"), &Config::Behavior::time_24h),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_appearance"), [appear] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_appearance"), appear));
    }));

    // ── Paths ───────────────────────────────────────────────────────────────
    std::vector<Row> paths = {
        text_row(Lang::t("settings.save_backup_path"),
                 [] { return Config::get().paths.save_backup; },
                 [](const std::string& v) { Config::get_mutable().paths.save_backup = v; }),
        text_row(Lang::t("settings.log_path"),
                 [] { return Config::get().paths.log_folder; },
                 [](const std::string& v) { Config::get_mutable().paths.log_folder = v; }),
        text_row(Lang::t("settings.dump_path"),
                 [] { return Config::get().paths.dump_folder; },
                 [](const std::string& v) { Config::get_mutable().paths.dump_folder = v; }),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_paths"), [paths] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_paths"), paths));
    }));

    // ── Menu visibility ─────────────────────────────────────────────────────
    std::vector<Row> vis = {
        vis_row(Lang::t("settings.vis_install_cartridge"),
                &Config::Visibility::install_from_cartridge),
        vis_row(Lang::t("settings.vis_installed_games"),
                &Config::Visibility::view_installed_games),
        vis_row(Lang::t("settings.vis_backup_saves"), &Config::Visibility::backup_saves),
        vis_row(Lang::t("settings.vis_browse_sd"),    &Config::Visibility::browse_sd),
        vis_row(Lang::t("settings.vis_browse_user"),
                &Config::Visibility::browse_user_partition),
        vis_row(Lang::t("settings.vis_browse_system"),
                &Config::Visibility::browse_system_partition),
        vis_row(Lang::t("settings.vis_browse_usb"),   &Config::Visibility::browse_usb),
        vis_row(Lang::t("settings.vis_browse_network"),
                &Config::Visibility::browse_network),
        vis_row(Lang::t("settings.vis_saves"),        &Config::Visibility::view_saves),
        vis_row(Lang::t("settings.vis_system_info"),        &Config::Visibility::tools),
        vis_row(Lang::t("settings.vis_mtp"),          &Config::Visibility::start_mtp),
        vis_row(Lang::t("settings.vis_ftp"),          &Config::Visibility::start_ftp),
        vis_row(Lang::t("settings.vis_http"),         &Config::Visibility::start_http),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_visibility"), [vis] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_visibility"), vis));
    }));

    // ── Network ─────────────────────────────────────────────────────────────
    std::vector<Row> net = {
        text_row(Lang::t("settings.update_check_url"),
                 [] { return Config::get().app.update_check_url; },
                 [](const std::string& v) { Config::get_mutable().app.update_check_url = v; }),
        text_row(Lang::t("settings.titledb_url"),
                 [] { return Config::get().app.titledb_url; },
                 [](const std::string& v) { Config::get_mutable().app.titledb_url = v; }),
        text_row(Lang::t("settings.github_token"),
                 [] { return Config::get().network.github_token; },
                 [](const std::string& v) { Config::get_mutable().network.github_token = v; },
                 /*secret=*/true),
    };
    rows.push_back(submenu_row(Lang::t("settings.section_network"), [net] {
        return std::unique_ptr<Screen>(new SettingsScreen(
            Lang::t("settings.section_network"), net));
    }));

    // Reset lives at the root and asks first. It is the only settings action that
    // discards work rather than changing one value, so it gets the same Danger
    // treatment as the destructive save operations.
    {
        Row r;
        r.kind  = Row::Kind::Toggle;
        r.label = Lang::t("settings.reset_defaults");
        r.get   = [] { return false; };          // never "on" — it is an action
        r.set   = [](bool) {};                   // performed via the confirmation
        r.confirm_on_enable = true;
        r.confirm_title     = Lang::t("settings.reset_defaults");
        r.confirm_body      = Lang::t("settings.confirm_reset");
        r.is_reset_action   = true;
        rows.push_back(std::move(r));
    }

    return std::unique_ptr<Screen>(
        new SettingsScreen(Lang::t("settings.title"), std::move(rows)));
}

// ─── Screen ───────────────────────────────────────────────────────────────────

SettingsScreen::SettingsScreen(std::string title, std::vector<Row> rows)
    : m_title(std::move(title)), m_rows(std::move(rows)) {
    rebuild_rows();
}

void SettingsScreen::on_enter() {
    // A child screen may have changed a value; re-read so the checkmarks are
    // right rather than showing the state as of construction.
    rebuild_rows();
}

void SettingsScreen::on_exit() {
    Settings::flush();
}

void SettingsScreen::rebuild_rows() {
    std::vector<Widgets::ListItem> items;
    items.reserve(m_rows.size());
    for (const auto& r : m_rows) {
        Widgets::ListItem it;
        if (r.kind == Row::Kind::Submenu) {
            it.label = r.label + "...";       // matches the submenu convention
        } else if (r.kind == Row::Kind::Choice) {
            it.label = r.label;
            // Show the label for the current value, defaulting to the first.
            const int cur = r.choice_get ? r.choice_get() : 0;
            std::string meta = r.choice_labels.empty() ? std::string()
                                                       : r.choice_labels.front();
            for (size_t i = 0; i < r.choice_values.size(); ++i)
                if (r.choice_values[i] == cur &&
                    i < r.choice_labels.size()) { meta = r.choice_labels[i]; break; }
            it.meta = meta;
        } else if (r.kind == Row::Kind::Text) {
            it.label = r.label;
            const std::string v = r.text_get ? r.text_get() : std::string();
            if (v.empty())            it.meta = Lang::t("settings.not_set");
            else if (r.text_secret)   it.meta = std::string(v.size() > 12 ? 12 : v.size(), '*');
            else                      it.meta = v;
        } else if (r.kind == Row::Kind::Number) {
            it.label = r.label;
            it.meta  = std::to_string(r.num_get ? r.num_get() : 0) + r.num_suffix;
        } else {
            it.label       = r.label;
            it.is_selected = r.get && r.get();
            it.meta        = it.is_selected ? Lang::t("common.on") : Lang::t("common.off");
        }
        items.push_back(std::move(it));
    }
    // update_items keeps the cursor where it was, so toggling a row does not
    // bounce the selection back to the top.
    if (m_list.count() == static_cast<int>(items.size()))
        m_list.update_items(std::move(items));
    else
        m_list.set_items(std::move(items));
}

void SettingsScreen::apply_toggle(int idx, bool value) {
    if (idx < 0 || idx >= static_cast<int>(m_rows.size())) return;
    const Row& r = m_rows[idx];
    if (!r.set) return;
    if (r.get && r.get() == value) return;    // no change, nothing to persist
    r.set(value);
    Settings::mark_dirty();
    rebuild_rows();
}

std::unique_ptr<Screen> SettingsScreen::update(bool& pop) {
    pop = false;

    // While a modal is up, this screen must not act on input — the modal owns it.
    if (Modal::is_active()) return nullptr;

    // The Choice picker owns input while open. B cancels (no change); A commits
    // the highlighted value.
    if (m_picker_row >= 0) {
        if (Input::pressed(Input::Button::B)) { m_picker_row = -1; return nullptr; }
        if (m_picker_list.handle_input()) {
            const int sel = m_picker_list.cursor();
            Row& r = m_rows[m_picker_row];
            if (sel >= 0 && sel < (int)r.choice_values.size() &&
                r.choice_get && r.choice_set) {
                const int chosen = r.choice_values[sel];
                if (chosen != r.choice_get()) {
                    r.choice_set(chosen);
                    Settings::mark_dirty();
                    rebuild_rows();
                }
            }
            m_picker_row = -1;
        }
        return nullptr;
    }

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    if (m_list.handle_input()) {
        const int idx = m_list.cursor();
        if (idx < 0 || idx >= static_cast<int>(m_rows.size())) return nullptr;
        const Row& r = m_rows[idx];

        if (r.kind == Row::Kind::Submenu)
            return r.open ? r.open() : nullptr;

        if (r.kind == Row::Kind::Choice) {
            // Open a list picker rather than cycle in place — a modal-style list
            // reads far better than blind cycling for anything past two options.
            open_picker(idx);
            return nullptr;
        }

        if (r.kind == Row::Kind::Text) {
            // swkbd is BLOCKING — it hands control to the OS overlay and returns
            // on confirm/cancel. Safe here because this is a button press, never
            // the draw path.
            Keyboard::Options ko;
            ko.header       = r.label;
            ko.initial_text = r.text_get ? r.text_get() : std::string();
            ko.max_length   = r.text_max_len;
            ko.allow_empty  = r.text_allow_empty;
            std::string out;
            if (Keyboard::get_text(ko, out) && r.text_set) {
                if (out != ko.initial_text) {
                    r.text_set(out);
                    Settings::mark_dirty();
                    rebuild_rows();
                }
            }
            return nullptr;
        }

        if (r.kind == Row::Kind::Number) {
            const int cur = r.num_get ? r.num_get() : 0;
            int out = cur;
            if (Keyboard::get_number(r.label, cur, out) && r.num_set) {
                // Clamp HERE. The keyboard will happily return 0 or 99999 for a
                // port; that only fails later at bind(), by which time the user
                // has left this screen and the cause is invisible.
                if (out < r.num_min) out = r.num_min;
                if (out > r.num_max) out = r.num_max;
                if (out != cur) {
                    r.num_set(out);
                    Settings::mark_dirty();
                    rebuild_rows();
                }
            }
            return nullptr;
        }

        const bool now  = r.get && r.get();
        const bool want = !now;

        // Enabling a guarded surface asks first. Disabling never does: taking
        // access away is not the dangerous direction, and a confirmation there
        // would only train the user to dismiss this dialog without reading it.
        if (want && r.confirm_on_enable) {
            Modal::Options o;
            o.kind          = Modal::Kind::Danger;
            o.title         = r.confirm_title;
            o.body          = r.confirm_body;
            o.confirm_label = Lang::t("common.enable");
            o.cancel_label  = Lang::t("common.cancel");
            Modal::show(o);
            m_pending_row = idx;
            return nullptr;
        }

        apply_toggle(idx, want);
    }
    return nullptr;
}

void SettingsScreen::on_modal_result(int result) {
    const int idx = m_pending_row;
    m_pending_row = -1;
    if (idx < 0) return;
    // Anything other than an explicit confirmation leaves the setting alone.
    if (static_cast<Modal::Result>(result) != Modal::Result::Confirmed) return;

    if (idx < (int)m_rows.size() && m_rows[idx].is_reset_action) {
        Config::reset_to_defaults();
        Settings::mark_dirty();
        Settings::flush();      // write immediately: the user asked for a reset,
                                // not for one that lands whenever they navigate
        rebuild_rows();
        return;
    }
    apply_toggle(idx, true);
}

void SettingsScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x      = Layout::MENU_INDENT_X;
    style.show_checkbox = true;      // toggles read as checkboxes
    style.show_dividers = true;

    m_list.draw(x, y, w, h - 36, style);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.toggle") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);

    if (m_picker_row >= 0) draw_picker();
}

void SettingsScreen::open_picker(int row_idx) {
    m_picker_row = row_idx;
    const Row& r = m_rows[row_idx];

    std::vector<Widgets::ListItem> items;
    items.reserve(r.choice_labels.size());
    int cur_at = 0;
    const int cur = r.choice_get ? r.choice_get() : 0;
    for (size_t i = 0; i < r.choice_labels.size(); ++i) {
        Widgets::ListItem it;
        it.label = r.choice_labels[i];
        if (i < r.choice_values.size() && r.choice_values[i] == cur)
            cur_at = (int)i;
        items.push_back(std::move(it));
    }
    m_picker_list.set_items(std::move(items));
    m_picker_list.set_cursor(cur_at);   // open on the current value
}

void SettingsScreen::draw_picker() {
    const Row& r = m_rows[m_picker_row];

    // Dim the screen behind the picker so it reads as a modal layer.
    SDL_Color scrim = Theme::get(Theme::Token::BgBase);
    scrim.a = 200;
    SDL_SetRenderDrawBlendMode(Renderer::get(), SDL_BLENDMODE_BLEND);
    Theme::apply(Renderer::get(), Theme::Token::BgBase);
    SDL_SetRenderDrawColor(Renderer::get(), scrim.r, scrim.g, scrim.b, scrim.a);
    Renderer::fill_rect(0, 0, Layout::SCREEN_W, Layout::SCREEN_H);

    const int rows = (int)r.choice_labels.size();
    const int cw = 560;
    const int ch = 72 + rows * Layout::MENU_ITEM_H + 44;
    const int cx = (Layout::SCREEN_W - cw) / 2;
    const int cy = (Layout::SCREEN_H - ch) / 2;

    Theme::apply(Renderer::get(), Theme::Token::BgSurface);
    Renderer::fill_rect(cx, cy, cw, ch);

    SDL_Color fg = Theme::get(Theme::Token::FgPrimary);
    Renderer::draw_text(r.label, (int)Font::Size::Large, (int)Font::Weight::Bold,
                        (int)Font::Family::Sans, fg, cx + 24, cy + 20,
                        nullptr, nullptr, cw - 48);

    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x      = 24;
    style.show_checkbox = false;
    style.show_dividers = true;
    m_picker_list.draw(cx, cy + 64, cw, rows * Layout::MENU_ITEM_H, style);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("common.select") },
        { "B", Lang::t("common.cancel") },
    };
    Widgets::draw_button_legend(cx, cy + ch - 36, cw, hints);
}
