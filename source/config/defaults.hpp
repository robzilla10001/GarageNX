#pragma once
// source/config/defaults.hpp
// Single source of truth for all default config values.
// If config.json is missing a key, these values are used.
// When adding a new setting, add its default here first.

#include <cstdint>
#include <string>

namespace Config::Defaults {

// ── App ────────────────────────────────────────────────────────────────────────
inline constexpr const char* LANGUAGE         = "en";
inline constexpr const char* THEME            = "dark";
inline constexpr const char* UPDATE_CHECK_URL = "https://github.com/YOUR_ORG/GarageNX/releases/latest";
inline constexpr const char* TITLEDB_URL      = "https://github.com/blawar/titledb/raw/master/versions.txt";

// ── Behavior ──────────────────────────────────────────────────────────────────
inline constexpr bool ACTION_LOGGING        = true;
inline constexpr bool HIGHLIGHT_UPDATE_FILES= true;
inline constexpr bool ROTATE_SCREEN        = false;
inline constexpr bool USE_OVERCLOCKING     = false;
inline constexpr bool SAVES_RO_MODE        = false;
inline constexpr bool SHOW_CACHE_WARMING   = false;
inline constexpr int  SCREEN_DIM_SECONDS   = 30;
inline constexpr bool BUTTON_REPEAT        = true;
inline constexpr int  BUTTON_REPEAT_DELAY_MS    = 400;
inline constexpr int  BUTTON_REPEAT_INTERVAL_MS = 80;
inline constexpr bool SHOW_CLOCK           = true;
inline constexpr bool SHOW_SECONDS         = true;
inline constexpr const char* DATE_FORMAT   = "DMY";   // DMY / MDY / YMD
inline constexpr bool TIME_24H             = true;    // 24-hour display (logs always 24h)
inline constexpr int  SAVE_AUTO_BACKUP_DAYS = 0;
inline constexpr bool VERIFY_HASH_ON_INSTALL = true;

// ── Paths ─────────────────────────────────────────────────────────────────────
inline constexpr const char* SAVE_BACKUP_PATH = "sdmc:/switch/GarageNX/backups";
inline constexpr const char* LOG_PATH         = "sdmc:/switch/GarageNX/logs";
inline constexpr const char* DUMP_PATH        = "sdmc:/switch/GarageNX/dumps";

// ── Visibility ────────────────────────────────────────────────────────────────
inline constexpr bool VIS_BROWSE_SD               = true;
inline constexpr bool VIS_BROWSE_SYSTEM_PARTITION = true;
inline constexpr bool VIS_BROWSE_USER_PARTITION   = true;
inline constexpr bool VIS_BROWSE_USB              = true;
inline constexpr bool VIS_INSTALL_FROM_CARTRIDGE  = true;
inline constexpr bool VIS_BROWSE_NETWORK          = true;
inline constexpr bool VIS_VIEW_INSTALLED_GAMES    = true;
inline constexpr bool VIS_TOOLS                   = true;
inline constexpr bool VIS_VIEW_TICKETS            = true;
inline constexpr bool VIS_VIEW_SAVES              = true;
inline constexpr bool VIS_START_MTP               = true;
inline constexpr bool VIS_START_FTP               = true;
inline constexpr bool VIS_START_HTTP              = true;

// ── MTP storages ──────────────────────────────────────────────────────────────
inline constexpr bool MTP_SD_CARD        = true;
inline constexpr bool MTP_NAND_USER      = true;
inline constexpr bool MTP_NAND_SYSTEM    = false;
inline constexpr bool MTP_INSTALLED_GAMES= true;
inline constexpr bool MTP_SD_INSTALL     = true;
inline constexpr bool MTP_NAND_INSTALL   = false;
inline constexpr bool MTP_SAVES          = true;
inline constexpr bool MTP_ALBUM          = true;
inline constexpr bool MTP_GAMECARD       = false;
inline constexpr bool MTP_USER_STORAGES  = true;

// ── FTP / access point ────────────────────────────────────────────────────────
inline constexpr uint16_t     FTP_SERVER_PORT   = 5000;
inline constexpr bool         FTP_ALLOW_ANON    = true;
inline constexpr const char*  FTP_LOGIN_USER    = "garagenx";
inline constexpr const char*  FTP_LOGIN_PASS    = "garagenx";
inline constexpr bool        FTP_START_AP   = false;
inline constexpr const char* FTP_SSID       = "GarageNX";
inline constexpr const char* FTP_PASSWORD   = "";
inline constexpr bool        FTP_USE_5GHZ   = false;
inline constexpr bool        FTP_HIDDEN_SSID= false;

// ── HTTP ──────────────────────────────────────────────────────────────────────
inline constexpr uint16_t     HTTP_SERVER_PORT   = 8080;
inline constexpr bool         HTTP_ALLOW_UPLOAD  = true;

// ── Network ───────────────────────────────────────────────────────────────────
inline constexpr const char* GITHUB_TOKEN = "";

} // namespace Config::Defaults
