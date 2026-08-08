#pragma once
// source/config/config.hpp
// JSON-backed settings. All settings are accessed through Config::get().
// Write to disk via Config::save(). Never touch the JSON file directly in other modules.

#include <cstdint>
#include <string>
#include <vector>

namespace Config {

// ─── Aggregate structs ────────────────────────────────────────────────────────
// One struct per JSON section. Initialized to defaults.

struct App {
    std::string language         = "en";
    std::string theme            = "dark";
    std::string update_check_url;
    std::string titledb_url;
};

struct Behavior {
    bool action_logging         = true;
    bool highlight_update_files = true;
    bool rotate_screen          = false;
    bool use_overclocking       = false;
    bool show_cache_warming     = false;
    int  screen_dim_seconds     = 30;
    bool button_repeat_on_hold  = true;
    bool show_clock             = true;
    bool show_seconds           = true;
    std::string date_format     = "DMY";   // date field order: DMY / MDY / YMD
    bool time_24h               = true;    // 24-hour clock display (logs are always 24h)
    int  save_auto_backup_days  = 0;
    bool verify_hash_on_install = true;
};

struct Paths {
    std::string save_backup;
    std::string log_folder;
    std::string dump_folder;
};

struct Visibility {
    bool browse_sd               = true;
    bool browse_system_partition = true;
    bool browse_user_partition   = true;
    bool browse_usb              = true;
    bool install_from_cartridge  = true;
    bool browse_network          = true;
    bool view_installed_games    = true;
    bool tools                   = true;
    bool view_tickets            = true;
    bool view_saves              = true;
    bool backup_saves            = true;
    bool start_mtp               = true;
    bool start_ftp               = true;
    bool start_http              = true;
};

// Which storage surfaces a transport exposes. One instance PER TRANSPORT, so a
// user can serve saves over FTP without exposing them over MTP, or open NAND on
// one and not the other.
//
// This was a single shared block called `MTP` that FTP and HTTP also read — a
// key named mtp.* silently governing FTP is the kind of thing that misleads
// exactly when someone is trying to lock a surface down.
//
// NOTE for anything that MOUNTS: a mount is global, a toggle is per-transport.
// A partition must be mounted if ANY transport exposes it — see mount_nand().
struct Surfaces {
    bool sd_card         = true;
    bool nand_user       = true;
    bool nand_system     = false;
    bool installed_games = true;
    bool sd_install      = true;
    bool nand_install    = false;
    bool saves           = true;
    bool album           = true;
    bool gamecard        = true;   // physically read-only; see defaults.hpp
    bool user_storages   = true;
};

/// True if `id` is exposed by AT LEAST ONE transport. For global decisions —
/// mounting, service init — that cannot be made per-transport.
bool any_transport_exposes(bool Surfaces::* field);

struct MTP {
    Surfaces surfaces;
};

struct FTP {
    Surfaces    surfaces;
    // FTP server
    uint16_t    server_port        = 5000;
    bool        allow_anonymous    = true;
    std::string login_user         = "garagenx";
    std::string login_pass         = "garagenx";
    // Access-point mode (brought up so services are reachable with no network)
    bool        start_access_point = false;
    std::string ssid               = "GarageNX";
    std::string password;
    bool        use_5ghz           = false;
    bool        hidden_ssid        = false;
};

struct HTTP {
    Surfaces    surfaces;
    // HTTP server
    uint16_t    server_port   = 8080;
    bool        allow_upload  = true;
};

// One saved network connection for Browse Network. The password is deliberately
// ABSENT: the chosen model is prompt-per-session, so a credential never lands in
// config.json. NFS is host/UID-based and needs none; an SMB connection with a
// username prompts at connect time and the answer lives only in memory
// (Services::net_credentials()). See services/net_surface.hpp.
struct NetShare {
    std::string name;       // display label shown under /Network (identifies it)
    std::string protocol;   // "smb" | "nfs"
    std::string host;       // hostname or IP
    std::string share;      // SMB share name, or NFS export path
    std::string path;       // optional start folder WITHIN the share/export (e.g. "media/movies")
    uint16_t    port = 0;   // 0 => protocol default (SMB 445, NFS 2049)
    std::string username;   // SMB only; empty = guest/anonymous. NEVER a password.
    std::string domain;     // SMB workgroup/domain; optional
};

struct Network {
    std::string github_token;
    std::vector<NetShare> shares;   // saved Browse Network connections
};

struct All {
    App        app;
    Behavior   behavior;
    Paths      paths;
    Visibility visibility;
    MTP        mtp;
    FTP        ftp;
    HTTP       http;
    Network    network;
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Load config from disk. If the file doesn't exist, defaults are used and
/// the file is written immediately so it exists for future sessions.
/// config_path: full path to config.json (e.g. "sdmc:/switch/GarageNX/config.json")
bool load(const std::string& config_path);

/// Persist current config to disk.
bool save();

/// Reset all settings to defaults and save.
void reset_to_defaults();

// ─── Access ───────────────────────────────────────────────────────────────────

/// Get the full config (read-only). All modules use this.
const All& get();

/// Get a mutable reference for in-place modification by the Settings screen.
/// Always call save() after modifying.
All& get_mutable();

} // namespace Config
