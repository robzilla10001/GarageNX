// source/config/config.cpp

#include "config/config.hpp"
#include "config/defaults.hpp"
#include <SDL2/SDL.h>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace Config {

static All         s_config;
static std::string s_path;

// ─── JSON helpers ─────────────────────────────────────────────────────────────
// Each get_or() call reads a key from JSON with a typed default fallback.
// Missing keys never throw — they silently use the default value.

template<typename T>
static T jget(const json& j, const std::string& key, T def) {
    try {
        if (j.contains(key) && !j[key].is_null())
            return j[key].get<T>();
    } catch (...) {}
    return def;
}

// ─── Serialize ────────────────────────────────────────────────────────────────

static json to_json(const All& c) {
    return {
        {"app", {
            {"language",         c.app.language},
            {"theme",            c.app.theme},
            {"update_check_url", c.app.update_check_url},
            {"titledb_url",      c.app.titledb_url},
        }},
        {"behavior", {
            {"action_logging",          c.behavior.action_logging},
            {"highlight_update_files",  c.behavior.highlight_update_files},
            {"rotate_screen",           c.behavior.rotate_screen},
            {"use_overclocking",        c.behavior.use_overclocking},
            {"saves_ro_mode",           c.behavior.saves_ro_mode},
            {"show_cache_warming",      c.behavior.show_cache_warming},
            {"screen_dim_seconds",      c.behavior.screen_dim_seconds},
            {"button_repeat_on_hold",   c.behavior.button_repeat_on_hold},
            {"show_clock",              c.behavior.show_clock},
            {"show_seconds",            c.behavior.show_seconds},
            {"date_format",             c.behavior.date_format},
            {"time_24h",                c.behavior.time_24h},
            {"save_auto_backup_days",   c.behavior.save_auto_backup_days},
            {"verify_hash_on_install",  c.behavior.verify_hash_on_install},
        }},
        {"paths", {
            {"save_backup", c.paths.save_backup},
            {"log_folder",  c.paths.log_folder},
            {"dump_folder", c.paths.dump_folder},
        }},
        {"visibility", {
            {"browse_sd",                c.visibility.browse_sd},
            {"browse_system_partition",  c.visibility.browse_system_partition},
            {"browse_user_partition",    c.visibility.browse_user_partition},
            {"browse_usb",               c.visibility.browse_usb},
            {"install_from_cartridge",   c.visibility.install_from_cartridge},
            {"browse_network",           c.visibility.browse_network},
            {"view_installed_games",     c.visibility.view_installed_games},
            {"tools",                    c.visibility.tools},
            {"view_tickets",             c.visibility.view_tickets},
            {"view_saves",               c.visibility.view_saves},
            {"start_mtp",                c.visibility.start_mtp},
            {"start_ftp",                c.visibility.start_ftp},
            {"start_http",               c.visibility.start_http},
        }},
        {"mtp", {
            {"sd_card",         c.mtp.sd_card},
            {"nand_user",       c.mtp.nand_user},
            {"nand_system",     c.mtp.nand_system},
            {"installed_games", c.mtp.installed_games},
            {"sd_install",      c.mtp.sd_install},
            {"nand_install",    c.mtp.nand_install},
            {"saves",           c.mtp.saves},
            {"album",           c.mtp.album},
            {"gamecard",        c.mtp.gamecard},
            {"user_storages",   c.mtp.user_storages},
        }},
        {"ftp", {
            {"server_port",        c.ftp.server_port},
            {"allow_anonymous",    c.ftp.allow_anonymous},
            {"login_user",         c.ftp.login_user},
            {"login_pass",         c.ftp.login_pass},
            {"start_access_point", c.ftp.start_access_point},
            {"ssid",               c.ftp.ssid},
            {"password",           c.ftp.password},
            {"use_5ghz",           c.ftp.use_5ghz},
            {"hidden_ssid",        c.ftp.hidden_ssid},
        }},
        {"http", {
            {"server_port",   c.http.server_port},
            {"allow_upload",  c.http.allow_upload},
        }},
        {"network", {
            {"github_token", c.network.github_token},
        }},
    };
}

// ─── Deserialize ──────────────────────────────────────────────────────────────

static void from_json(const json& j, All& c) {
    auto app = j.value("app", json::object());
    c.app.language         = jget<std::string>(app, "language",         Defaults::LANGUAGE);
    c.app.theme            = jget<std::string>(app, "theme",            Defaults::THEME);
    c.app.update_check_url = jget<std::string>(app, "update_check_url", Defaults::UPDATE_CHECK_URL);
    c.app.titledb_url      = jget<std::string>(app, "titledb_url",      Defaults::TITLEDB_URL);

    auto beh = j.value("behavior", json::object());
    c.behavior.action_logging         = jget<bool>(beh, "action_logging",         Defaults::ACTION_LOGGING);
    c.behavior.highlight_update_files = jget<bool>(beh, "highlight_update_files", Defaults::HIGHLIGHT_UPDATE_FILES);
    c.behavior.rotate_screen          = jget<bool>(beh, "rotate_screen",          Defaults::ROTATE_SCREEN);
    c.behavior.use_overclocking       = jget<bool>(beh, "use_overclocking",       Defaults::USE_OVERCLOCKING);
    c.behavior.saves_ro_mode          = jget<bool>(beh, "saves_ro_mode",          Defaults::SAVES_RO_MODE);
    c.behavior.show_cache_warming     = jget<bool>(beh, "show_cache_warming",     Defaults::SHOW_CACHE_WARMING);
    c.behavior.screen_dim_seconds     = jget<int> (beh, "screen_dim_seconds",     Defaults::SCREEN_DIM_SECONDS);
    c.behavior.button_repeat_on_hold  = jget<bool>(beh, "button_repeat_on_hold",  Defaults::BUTTON_REPEAT);
    c.behavior.show_clock             = jget<bool>(beh, "show_clock",             Defaults::SHOW_CLOCK);
    c.behavior.show_seconds           = jget<bool>(beh, "show_seconds",           Defaults::SHOW_SECONDS);
    c.behavior.date_format            = jget<std::string>(beh, "date_format",     Defaults::DATE_FORMAT);
    c.behavior.time_24h               = jget<bool>(beh, "time_24h",               Defaults::TIME_24H);
    c.behavior.save_auto_backup_days  = jget<int> (beh, "save_auto_backup_days",  Defaults::SAVE_AUTO_BACKUP_DAYS);
    c.behavior.verify_hash_on_install = jget<bool>(beh, "verify_hash_on_install", Defaults::VERIFY_HASH_ON_INSTALL);

    auto paths = j.value("paths", json::object());
    c.paths.save_backup = jget<std::string>(paths, "save_backup", Defaults::SAVE_BACKUP_PATH);
    c.paths.log_folder  = jget<std::string>(paths, "log_folder",  Defaults::LOG_PATH);
    c.paths.dump_folder = jget<std::string>(paths, "dump_folder", Defaults::DUMP_PATH);

    auto vis = j.value("visibility", json::object());
    c.visibility.browse_sd               = jget<bool>(vis, "browse_sd",               Defaults::VIS_BROWSE_SD);
    c.visibility.browse_system_partition = jget<bool>(vis, "browse_system_partition",  Defaults::VIS_BROWSE_SYSTEM_PARTITION);
    c.visibility.browse_user_partition   = jget<bool>(vis, "browse_user_partition",    Defaults::VIS_BROWSE_USER_PARTITION);
    c.visibility.browse_usb              = jget<bool>(vis, "browse_usb",               Defaults::VIS_BROWSE_USB);
    c.visibility.install_from_cartridge  = jget<bool>(vis, "install_from_cartridge",   Defaults::VIS_INSTALL_FROM_CARTRIDGE);
    c.visibility.browse_network          = jget<bool>(vis, "browse_network",            Defaults::VIS_BROWSE_NETWORK);
    c.visibility.view_installed_games    = jget<bool>(vis, "view_installed_games",      Defaults::VIS_VIEW_INSTALLED_GAMES);
    c.visibility.tools                   = jget<bool>(vis, "tools",                     Defaults::VIS_TOOLS);
    c.visibility.view_tickets            = jget<bool>(vis, "view_tickets",              Defaults::VIS_VIEW_TICKETS);
    c.visibility.view_saves              = jget<bool>(vis, "view_saves",                Defaults::VIS_VIEW_SAVES);
    c.visibility.start_mtp               = jget<bool>(vis, "start_mtp",                Defaults::VIS_START_MTP);
    c.visibility.start_ftp               = jget<bool>(vis, "start_ftp",                Defaults::VIS_START_FTP);
    c.visibility.start_http              = jget<bool>(vis, "start_http",               Defaults::VIS_START_HTTP);

    auto mtp = j.value("mtp", json::object());
    c.mtp.sd_card         = jget<bool>(mtp, "sd_card",         Defaults::MTP_SD_CARD);
    c.mtp.nand_user       = jget<bool>(mtp, "nand_user",       Defaults::MTP_NAND_USER);
    c.mtp.nand_system     = jget<bool>(mtp, "nand_system",     Defaults::MTP_NAND_SYSTEM);
    c.mtp.installed_games = jget<bool>(mtp, "installed_games", Defaults::MTP_INSTALLED_GAMES);
    c.mtp.sd_install      = jget<bool>(mtp, "sd_install",      Defaults::MTP_SD_INSTALL);
    c.mtp.nand_install    = jget<bool>(mtp, "nand_install",    Defaults::MTP_NAND_INSTALL);
    c.mtp.saves           = jget<bool>(mtp, "saves",           Defaults::MTP_SAVES);
    c.mtp.album           = jget<bool>(mtp, "album",           Defaults::MTP_ALBUM);
    c.mtp.gamecard        = jget<bool>(mtp, "gamecard",        Defaults::MTP_GAMECARD);
    c.mtp.user_storages   = jget<bool>(mtp, "user_storages",   Defaults::MTP_USER_STORAGES);

    auto ftp = j.value("ftp", json::object());
    c.ftp.server_port        = (uint16_t)jget<int>(ftp, "server_port",     Defaults::FTP_SERVER_PORT);
    c.ftp.allow_anonymous    = jget<bool>       (ftp, "allow_anonymous",   Defaults::FTP_ALLOW_ANON);
    c.ftp.login_user         = jget<std::string>(ftp, "login_user",        Defaults::FTP_LOGIN_USER);
    c.ftp.login_pass         = jget<std::string>(ftp, "login_pass",        Defaults::FTP_LOGIN_PASS);
    c.ftp.start_access_point = jget<bool>       (ftp, "start_access_point", Defaults::FTP_START_AP);
    c.ftp.ssid               = jget<std::string>(ftp, "ssid",               Defaults::FTP_SSID);
    c.ftp.password           = jget<std::string>(ftp, "password",           Defaults::FTP_PASSWORD);
    c.ftp.use_5ghz           = jget<bool>       (ftp, "use_5ghz",           Defaults::FTP_USE_5GHZ);
    c.ftp.hidden_ssid        = jget<bool>       (ftp, "hidden_ssid",        Defaults::FTP_HIDDEN_SSID);

    auto http = j.value("http", json::object());
    c.http.server_port   = (uint16_t)jget<int>(http, "server_port",   Defaults::HTTP_SERVER_PORT);
    c.http.allow_upload  = jget<bool>       (http, "allow_upload",  Defaults::HTTP_ALLOW_UPLOAD);

    auto net = j.value("network", json::object());
    c.network.github_token = jget<std::string>(net, "github_token", Defaults::GITHUB_TOKEN);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

bool load(const std::string& config_path) {
    s_path = config_path;

    // Apply defaults first — so even a partial file works
    reset_to_defaults();

    std::ifstream file(config_path);
    if (!file.is_open()) {
        SDL_Log("Config::load — no config found at %s, writing defaults",
                config_path.c_str());
        return save();
    }

    try {
        json j;
        file >> j;
        from_json(j, s_config);
        SDL_Log("Config::load — loaded from %s", config_path.c_str());
        return true;
    } catch (const std::exception& e) {
        SDL_Log("Config::load — parse error: %s — using defaults", e.what());
        return save();
    }
}

bool save() {
    if (s_path.empty()) {
        SDL_Log("Config::save — no path set");
        return false;
    }

    try {
        json j = to_json(s_config);
        std::ofstream file(s_path);
        if (!file.is_open()) {
            SDL_Log("Config::save — cannot open %s for writing", s_path.c_str());
            return false;
        }
        file << j.dump(2);
        SDL_Log("Config::save — written to %s", s_path.c_str());
        return true;
    } catch (const std::exception& e) {
        SDL_Log("Config::save — error: %s", e.what());
        return false;
    }
}

void reset_to_defaults() {
    s_config = All{};
    s_config.app.update_check_url = Defaults::UPDATE_CHECK_URL;
    s_config.app.titledb_url      = Defaults::TITLEDB_URL;
    s_config.paths.save_backup    = Defaults::SAVE_BACKUP_PATH;
    s_config.paths.log_folder     = Defaults::LOG_PATH;
    s_config.paths.dump_folder    = Defaults::DUMP_PATH;
    s_config.ftp.ssid             = Defaults::FTP_SSID;
}

// ─── Access ───────────────────────────────────────────────────────────────────

const All& get()         { return s_config; }
All&       get_mutable() { return s_config; }

} // namespace Config
