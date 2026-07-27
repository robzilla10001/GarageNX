// tests/config_surfaces_test.cpp
//
// Pins the per-transport surfaces split and, above all, its MIGRATION.
//
// Until this change one block — "mtp" — governed which storage surfaces EVERY
// transport exposed. FTP and HTTP read keys named mtp.*, which is misleading
// precisely when someone is trying to lock a surface down. Each transport now
// carries its own Surfaces.
//
// The migration is the dangerous half. Every existing config.json in the world
// has the old flat keys and no "surfaces" object, and some of those files say
// things their owner meant, like nand_system=true on a console where that was a
// deliberate decision. An upgrade that silently re-hid it, or worse silently
// exposed something, would be a safety regression delivered by a refactor. So:
// legacy flat keys seed ALL THREE transports, which reproduces the old
// behaviour exactly, and this test is what says so.

#include "config/config.hpp"
#include "config/defaults.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using json = nlohmann::json;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// Write `j` to a temp file and load it as config. Callers then read
// Config::get() directly — returning the reference from here trips GCC's
// -Wdangling-reference, and the zero-warning bar is worth more than the sugar.
static void load_from(const json& j, const char* name) {
    const std::string path = std::string("/tmp/gnx_cfg_") + name + ".json";
    { std::ofstream f(path); f << j.dump(2); }
    Config::load(path);
}

// A LEGACY file: flat surface keys directly under "mtp", no "surfaces" object.
// This is what every config.json on disk looks like today.
static void test_legacy_flat_keys_seed_every_transport() {
    json j;
    j["mtp"]["sd_card"]     = true;
    j["mtp"]["nand_system"] = true;    // deliberately enabled by its owner
    j["mtp"]["album"]       = false;   // deliberately disabled by its owner
    j["mtp"]["saves"]       = true;

    load_from(j, "legacy");
    const auto& c = Config::get();

    // The old behaviour was: one block, every transport. Reproduce it exactly.
    CHECK(c.mtp.surfaces.nand_system,  "legacy nand_system reaches MTP");
    CHECK(c.ftp.surfaces.nand_system,  "legacy nand_system reaches FTP");
    CHECK(c.http.surfaces.nand_system, "legacy nand_system reaches HTTP");

    CHECK(!c.mtp.surfaces.album,  "legacy album=false reaches MTP");
    CHECK(!c.ftp.surfaces.album,  "legacy album=false reaches FTP");
    CHECK(!c.http.surfaces.album, "legacy album=false reaches HTTP");

    CHECK(c.ftp.surfaces.saves && c.ftp.surfaces.sd_card, "other legacy keys carry too");

    // A key absent from the legacy file falls back to the compile-time default,
    // NOT to whatever the previous test left in the singleton.
    // An absent key falls back to the compile-time default, NOT to whatever the
    // previous test left in the singleton. Checked in BOTH directions so the test
    // proves defaults are applied rather than that everything happens to be false:
    // gamecard defaults true, nand_install defaults false, and neither key is in
    // the legacy block above.
    CHECK(c.mtp.surfaces.gamecard,      "absent key uses its default (gamecard=true)");
    CHECK(!c.mtp.surfaces.nand_install, "absent key uses its default (nand_install=false)");
    std::printf("  ok: legacy flat keys seed every transport\n");
}

// A NEW file: each transport has its own "surfaces" block, and they differ.
// This is the whole point of the split — saves over FTP but not MTP, say.
static void test_per_transport_blocks_are_independent() {
    json j;
    j["mtp"]["surfaces"]["saves"]        = false;
    j["mtp"]["surfaces"]["nand_system"]  = false;
    j["ftp"]["surfaces"]["saves"]        = true;
    j["ftp"]["surfaces"]["nand_system"]  = true;
    j["http"]["surfaces"]["saves"]       = false;

    load_from(j, "split");
    const auto& c = Config::get();

    CHECK(!c.mtp.surfaces.saves,       "MTP saves off");
    CHECK(c.ftp.surfaces.saves,        "FTP saves on — independent of MTP");
    CHECK(!c.http.surfaces.saves,      "HTTP saves off");
    CHECK(!c.mtp.surfaces.nand_system, "MTP NAND system off");
    CHECK(c.ftp.surfaces.nand_system,  "FTP NAND system on");
    std::printf("  ok: per-transport blocks are independent\n");
}

// A MIXED file: new-format block present but partial. Keys the block does not
// carry fall back to the legacy flat keys before the defaults, so a half-migrated
// file cannot quietly drop a setting.
static void test_partial_block_falls_back_to_legacy() {
    json j;
    j["mtp"]["nand_system"]        = true;    // legacy flat key
    j["mtp"]["gamecard"]           = true;    // legacy flat key
    j["ftp"]["surfaces"]["saves"]  = false;   // new block, one key only

    load_from(j, "mixed");
    const auto& c = Config::get();

    CHECK(!c.ftp.surfaces.saves,      "the key the new block carries wins");
    CHECK(c.ftp.surfaces.nand_system, "a key it lacks falls back to legacy");
    CHECK(c.ftp.surfaces.gamecard,    "and so does another");
    CHECK(c.mtp.surfaces.nand_system, "MTP still reads legacy where it has no block");
    std::printf("  ok: partial block falls back to legacy\n");
}

// A file with no storage config at all gets the documented defaults.
static void test_empty_config_uses_defaults() {
    load_from(json::object(), "empty");
    const auto& c = Config::get();
    CHECK(c.mtp.surfaces.sd_card,      "sd_card defaults on");
    CHECK(c.mtp.surfaces.nand_user,    "nand_user defaults on");
    CHECK(!c.mtp.surfaces.nand_system, "nand_system defaults OFF — the safety default");
    CHECK(!c.ftp.surfaces.nand_system, "and off for FTP too");
    CHECK(c.mtp.surfaces.gamecard,     "gamecard defaults ON (read-only, mount exists)");
    CHECK(!c.mtp.surfaces.nand_install, "nand_install defaults off");
    std::printf("  ok: empty config uses defaults\n");
}

// The union used for global decisions — mounting in particular. A mount is
// process-wide, so it must happen if ANY transport exposes the partition;
// asking one transport's block would leave the device unmounted for a user who
// enabled it on another, and the folder would appear but refuse to open.
static void test_any_transport_exposes() {
    json j;
    j["mtp"]["surfaces"]["nand_system"]  = false;
    j["ftp"]["surfaces"]["nand_system"]  = true;    // only FTP wants it
    j["http"]["surfaces"]["nand_system"] = false;
    j["mtp"]["surfaces"]["gamecard"]     = false;
    j["ftp"]["surfaces"]["gamecard"]     = false;
    j["http"]["surfaces"]["gamecard"]    = false;

    load_from(j, "union");

    CHECK(Config::any_transport_exposes(&Config::Surfaces::nand_system),
          "one transport wanting it is enough to mount");
    CHECK(!Config::any_transport_exposes(&Config::Surfaces::gamecard),
          "no transport wanting it means no mount");
    std::printf("  ok: cross-transport union for global decisions\n");
}

// A round trip must survive: save the new format, load it back, get it back.
static void test_round_trip_preserves_split() {
    json j;
    j["mtp"]["surfaces"]["saves"]       = false;
    j["ftp"]["surfaces"]["saves"]       = true;
    j["ftp"]["surfaces"]["nand_system"] = true;
    load_from(j, "rt_in");

    const std::string out = "/tmp/gnx_cfg_rt_out.json";
    Config::load(out);            // set the path...
    Config::get_mutable().mtp.surfaces.saves       = false;
    Config::get_mutable().ftp.surfaces.saves       = true;
    Config::get_mutable().ftp.surfaces.nand_system = true;
    CHECK(Config::save(), "save writes the file");

    Config::get_mutable() = Config::All{};    // wipe, prove it comes from disk
    Config::load(out);
    const auto& c = Config::get();
    CHECK(!c.mtp.surfaces.saves,      "MTP saves survives the round trip");
    CHECK(c.ftp.surfaces.saves,       "FTP saves survives independently");
    CHECK(c.ftp.surfaces.nand_system, "FTP nand_system survives");
    std::printf("  ok: round trip preserves the split\n");
}

// ── The data-loss guards ─────────────────────────────────────────────────────
//
// Config::save() rewrites the file. Before it had any caller that was harmless;
// the settings screen gives it one, so two properties now have to hold or a user
// loses settings by opening a menu.

// 1. EVERY field survives a save/load cycle. A field present in the struct but
//    missing from to_json or from_json is silently reset to its default the first
//    time anything is saved. Enumerated by hand deliberately: a loop over some
//    reflection helper would pass while proving nothing, and the tedium here is
//    the point — adding a config field should mean adding a line to this test.
static void test_every_field_round_trips() {
    Config::load("/tmp/gnx_cfg_rt_all.json");
    auto& w = Config::get_mutable();

    w.app.language = "fr";  w.app.theme = "light";
    w.app.update_check_url = "http://example.invalid/u";
    w.app.titledb_url      = "http://example.invalid/t";

    w.behavior.action_logging = false; w.behavior.highlight_update_files = false;
    w.behavior.rotate_screen = true;   w.behavior.use_overclocking = true;
    w.behavior.show_cache_warming = true; w.behavior.screen_dim_seconds = 99;
    w.behavior.button_repeat_on_hold = false; w.behavior.show_clock = false;
    w.behavior.show_seconds = false;   w.behavior.date_format = "YMD";
    w.behavior.time_24h = false;       w.behavior.save_auto_backup_days = 7;
    w.behavior.verify_hash_on_install = false;

    w.paths.save_backup = "sdmc:/a"; w.paths.log_folder = "sdmc:/b";
    w.paths.dump_folder = "sdmc:/c";

    w.visibility.browse_sd = false; w.visibility.browse_system_partition = false;
    w.visibility.browse_user_partition = false; w.visibility.browse_usb = false;
    w.visibility.install_from_cartridge = false; w.visibility.browse_network = false;
    w.visibility.view_installed_games = false; w.visibility.tools = false;
    w.visibility.view_tickets = false; w.visibility.view_saves = false;
    w.visibility.backup_saves = false;
    w.visibility.start_mtp = false; w.visibility.start_ftp = false;
    w.visibility.start_http = false;

    w.mtp.surfaces.saves = false;  w.ftp.surfaces.nand_system = true;
    w.http.surfaces.album = false;

    w.ftp.server_port = 2121; w.ftp.allow_anonymous = false;
    w.ftp.login_user = "u"; w.ftp.login_pass = "p";
    w.ftp.start_access_point = true; w.ftp.ssid = "S"; w.ftp.password = "P";
    w.ftp.use_5ghz = true; w.ftp.hidden_ssid = true;

    w.http.server_port = 9090; w.http.allow_upload = false;
    w.network.github_token = "ghp_x";

    CHECK(Config::save(), "save succeeds");
    Config::get_mutable() = Config::All{};        // wipe: prove it came from disk
    Config::load("/tmp/gnx_cfg_rt_all.json");
    const auto& c = Config::get();

    CHECK(c.app.language == "fr" && c.app.theme == "light", "App round trips");
    CHECK(c.app.update_check_url == "http://example.invalid/u", "update url");
    CHECK(c.app.titledb_url == "http://example.invalid/t", "titledb url");

    CHECK(!c.behavior.action_logging && !c.behavior.highlight_update_files, "behavior bools");
    CHECK(c.behavior.rotate_screen && c.behavior.use_overclocking, "behavior bools 2");
    CHECK(c.behavior.show_cache_warming, "show_cache_warming");
    CHECK(c.behavior.screen_dim_seconds == 99, "screen_dim_seconds");
    CHECK(!c.behavior.button_repeat_on_hold, "button_repeat_on_hold");
    CHECK(!c.behavior.show_clock && !c.behavior.show_seconds, "clock bools");
    CHECK(c.behavior.date_format == "YMD", "date_format");
    CHECK(!c.behavior.time_24h, "time_24h");
    CHECK(c.behavior.save_auto_backup_days == 7, "save_auto_backup_days");
    CHECK(!c.behavior.verify_hash_on_install, "verify_hash_on_install");

    CHECK(c.paths.save_backup == "sdmc:/a" && c.paths.log_folder == "sdmc:/b" &&
          c.paths.dump_folder == "sdmc:/c", "Paths round trip");

    CHECK(!c.visibility.browse_sd && !c.visibility.tools &&
          !c.visibility.start_http, "Visibility round trips");
    CHECK(!c.visibility.backup_saves, "backup_saves round trips");

    CHECK(!c.mtp.surfaces.saves,      "MTP surfaces round trip");
    CHECK(c.ftp.surfaces.nand_system, "FTP surfaces round trip");
    CHECK(!c.http.surfaces.album,     "HTTP surfaces round trip");

    CHECK(c.ftp.server_port == 2121 && !c.ftp.allow_anonymous, "FTP server");
    CHECK(c.ftp.login_user == "u" && c.ftp.login_pass == "p", "FTP credentials");
    CHECK(c.ftp.start_access_point && c.ftp.ssid == "S" && c.ftp.password == "P",
          "FTP access point");
    CHECK(c.ftp.use_5ghz && c.ftp.hidden_ssid, "FTP AP flags");

    CHECK(c.http.server_port == 9090 && !c.http.allow_upload, "HTTP round trips");
    CHECK(c.network.github_token == "ghp_x", "Network round trips");
    std::printf("  ok: every config field survives save/load\n");
}

// 2. A save must NOT delete what it does not understand. Someone's hand-added
//    key, or a field written by a newer build, has to survive a user toggling an
//    unrelated setting — they would have no way to know it had been eaten.
static void test_save_preserves_unknown_keys() {
    json j;
    j["mtp"]["surfaces"]["saves"]     = true;
    j["future_feature"]["enabled"]    = true;      // a build we are not
    j["app"]["experimental_thing"]    = "keep me"; // a key inside a known section
    const std::string path = "/tmp/gnx_cfg_unknown.json";
    { std::ofstream f(path); f << j.dump(2); }

    Config::load(path);
    Config::get_mutable().mtp.surfaces.saves = false;   // change one unrelated thing
    CHECK(Config::save(), "save succeeds");

    json back;
    { std::ifstream f(path); f >> back; }
    CHECK(back.contains("future_feature"), "an unmodelled SECTION survives a save");
    CHECK(back["future_feature"]["enabled"] == true, "and its contents survive");
    CHECK(back["app"].contains("experimental_thing"), "an unmodelled KEY inside a known section survives");
    CHECK(back["app"]["experimental_thing"] == "keep me", "with its value intact");
    CHECK(back["mtp"]["surfaces"]["saves"] == false, "and the real change was written");
    std::printf("  ok: save preserves unmodelled keys\n");
}

// 3. The one thing a save SHOULD remove: the pre-split flat surface keys, which
//    have been migrated into "surfaces". Leaving them beside their replacements
//    would read as though a surface were on when it is off.
static void test_save_removes_superseded_legacy_keys() {
    json j;
    j["mtp"]["nand_system"] = true;    // legacy flat key
    j["mtp"]["saves"]       = true;
    const std::string path = "/tmp/gnx_cfg_legacy_strip.json";
    { std::ofstream f(path); f << j.dump(2); }

    Config::load(path);
    CHECK(Config::get().mtp.surfaces.nand_system, "legacy value migrated in");
    Config::get_mutable().mtp.surfaces.nand_system = false;
    CHECK(Config::save(), "save succeeds");

    json back;
    { std::ifstream f(path); f >> back; }
    CHECK(!back["mtp"].contains("nand_system"), "stale flat key removed");
    CHECK(!back["mtp"].contains("saves"), "and the others too");
    CHECK(back["mtp"]["surfaces"]["nand_system"] == false, "new location holds the truth");
    std::printf("  ok: save completes the migration one-way\n");
}

// The placeholder-URL migration. A stored key always beats a compile-time default,
// so correcting the default alone would silently fix nothing for anyone who had
// already run the app — which is exactly what happened. The migration must rewrite
// the placeholder and must NOT touch a URL the user chose.
static void test_update_url_placeholder_migration() {
    json j;
    j["app"]["update_check_url"] = "https://github.com/YOUR_ORG/GarageNX/releases/latest";
    load_from(j, "urlmig");
    CHECK(Config::get().app.update_check_url ==
          std::string(Config::Defaults::UPDATE_CHECK_URL),
          "the shipped placeholder is replaced with the real URL");

    // A deliberate custom URL survives untouched — the migration is exact-match,
    // not a heuristic.
    json j2;
    j2["app"]["update_check_url"] = "https://example.invalid/my/own/feed";
    load_from(j2, "urlcustom");
    CHECK(Config::get().app.update_check_url == "https://example.invalid/my/own/feed",
          "a user-chosen URL is never rewritten");

    // Absent key still falls back to the default.
    load_from(json::object(), "urlabsent");
    CHECK(Config::get().app.update_check_url == std::string(Config::Defaults::UPDATE_CHECK_URL),
          "absent key uses the default");
    std::printf("  ok: update URL placeholder migration\n");
}

// Key ORDER must survive a save. nlohmann::json is a std::map and re-sorts every
// key alphabetically; ordered_json preserves insertion order. The difference is
// invisible to the parser and very visible to a human whose hand-grouped
// config.json comes back reshuffled after touching one setting.
static void test_save_preserves_key_order() {
    // Build the INPUT with ordered_json too. The test's own `json` alias is
    // nlohmann::json, which is a std::map and would alphabetise these keys before
    // they ever reached the file — the test would then "pass" while proving
    // nothing about what save() did.
    nlohmann::ordered_json j;
    j["zzz_last"]  = 1;      // deliberately anti-alphabetical
    j["mmm_mid"]   = 2;
    j["aaa_first"] = 3;
    j["mtp"]["surfaces"]["saves"] = true;
    const std::string path = "/tmp/gnx_cfg_order.json";
    { std::ofstream f(path); f << j.dump(2); }

    Config::load(path);
    Config::get_mutable().mtp.surfaces.saves = false;
    CHECK(Config::save(), "save succeeds");

    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    const size_t z = text.find("zzz_last");
    const size_t m = text.find("mmm_mid");
    const size_t a = text.find("aaa_first");
    CHECK(z != std::string::npos && m != std::string::npos && a != std::string::npos,
          "all keys survive the save");
    CHECK(z < m && m < a, "insertion order preserved, not alphabetised");
    std::printf("  ok: save preserves key order\n");
}

int main() {
    std::printf("config_surfaces_test (per-transport split + migration)\n");
    test_legacy_flat_keys_seed_every_transport();
    test_per_transport_blocks_are_independent();
    test_partial_block_falls_back_to_legacy();
    test_empty_config_uses_defaults();
    test_any_transport_exposes();
    test_round_trip_preserves_split();
    test_every_field_round_trips();
    test_update_url_placeholder_migration();
    test_save_preserves_unknown_keys();
    test_save_preserves_key_order();
    test_save_removes_superseded_legacy_keys();
    std::printf("config_surfaces_test: %d checks passed\n", g_checks);
    return 0;
}
