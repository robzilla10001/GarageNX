#pragma once
// source/lang/localization.hpp
// Language file loader and t() lookup.
// Language files are always external: drop a XX.json into
// sdmc:/switch/GarageNX/lang/ to add or update a language. Nothing is bundled.
// en.json in that folder is the authoritative template; missing keys always
// fall back to English.

#include <string>
#include <vector>

namespace Lang {

struct LanguageInfo {
    std::string code;       // filename stem, e.g. "en", "es", "pt-br"
    std::string name;       // human-readable, from meta.language
    std::string author;     // from meta.author
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Set the language directory - e.g. "sdmc:/switch/GarageNX/lang". This is the
/// only place language files are read from. Call this first.
void set_lang_dir(const std::string& dir);

/// Scan the language directory for *.json files. Returns discovered languages;
/// new_ones lists languages found beyond known_codes (for first-run language
/// prompts).
struct ScanResult {
    std::vector<LanguageInfo> all;      // all discovered languages
    std::vector<LanguageInfo> new_ones; // languages not in known_codes
};

ScanResult scan(const std::string& lang_dir,
                const std::vector<std::string>& known_codes);

/// Activate a language by code (e.g. "en", "es"). Resolution order per key:
/// file for `code` -> en.json -> the key itself. Returns false only if the
/// file couldn't be found or parsed (English stays active in that case).
bool load(const std::string& code);

// ─── Access ───────────────────────────────────────────────────────────────────

/// Look up a localized string by dot-separated key path.
/// e.g. t("main_menu.browse_sd") -> "Browse SD Card"
/// Returns the key itself if not found (never crashes, never returns empty).
const std::string& t(const std::string& key);

/// Current active language code.
const std::string& active_code();

/// All discovered languages (from last scan()).
const std::vector<LanguageInfo>& available();

} // namespace Lang
