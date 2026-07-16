#pragma once
// source/lang/localization.hpp
// Language file loader and t() lookup.
// Drop a XX.json into sdmc:/switch/GarageNX/lang/ to add a new language.
// en.json is the authoritative template; missing keys always fall back to English.

#include <string>
#include <vector>

namespace Lang {

struct LanguageInfo {
    std::string code;       // filename stem, e.g. "en", "es", "br"
    std::string name;       // human-readable, from meta.language
    std::string author;     // from meta.author
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Set the baseline (bundled) language directory — e.g. "romfs:/lang".
/// This directory MUST contain a complete en.json. Loaded once at startup and
/// used as the permanent fallback for every key. Call this first.
void set_baseline_dir(const std::string& dir);

/// Scan the user language directory (e.g. "sdmc:/switch/GarageNX/lang") plus the
/// baseline dir for *.json files. Returns discovered languages; new_ones lists
/// languages found beyond known_codes (for first-run language prompts).
struct ScanResult {
    std::vector<LanguageInfo> all;      // all discovered languages
    std::vector<LanguageInfo> new_ones; // languages not in known_codes
};

ScanResult scan(const std::string& user_dir,
                const std::vector<std::string>& known_codes);

/// Activate a language by code (e.g. "en", "es").
/// Resolution order for each key: user file for `code` → baseline `code` →
/// baseline en → the key itself. A good load is never clobbered by a missing
/// file. Returns false only if `code` couldn't be found in either directory
/// (English still remains active in that case).
bool load(const std::string& code);

// ─── Access ───────────────────────────────────────────────────────────────────

/// Look up a localized string by dot-separated key path.
/// e.g. t("main_menu.browse_sd") → "Browse SD Card"
/// Returns the key itself if not found (never crashes, never returns empty).
const std::string& t(const std::string& key);

/// Current active language code.
const std::string& active_code();

/// All discovered languages (from last scan()).
const std::vector<LanguageInfo>& available();

} // namespace Lang
