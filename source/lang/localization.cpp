// source/lang/localization.cpp

#include "lang/localization.hpp"
#include <SDL2/SDL.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <map>
#include <dirent.h>
#include <cstring>

using json = nlohmann::json;

namespace Lang {

// ─── State ────────────────────────────────────────────────────────────────────

static std::map<std::string, std::string> s_strings;      // active language
static std::map<std::string, std::string> s_fallback;     // en.json baseline
static std::string                         s_active_code = "en";
static std::vector<LanguageInfo>           s_available;
static std::string                         s_baseline_dir;  // bundled assets (romfs)
static std::string                         s_user_dir;      // user drop-in (sdmc)

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Flatten a nested JSON object into dot-separated keys.
// {"main_menu": {"browse_sd": "Browse SD"}} → {"main_menu.browse_sd": "Browse SD"}
static void flatten(const json& j, std::map<std::string, std::string>& out,
                    const std::string& prefix = "") {
    for (auto it = j.begin(); it != j.end(); ++it) {
        std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (it->is_object()) {
            flatten(*it, out, key);
        } else if (it->is_string()) {
            out[key] = it->get<std::string>();
        }
    }
}

static bool load_file(const std::string& path,
                       std::map<std::string, std::string>& out,
                       LanguageInfo* info_out = nullptr) {
    std::ifstream file(path);
    if (!file.is_open()) {
        SDL_Log("Lang::load_file — cannot open %s", path.c_str());
        return false;
    }

    try {
        json j;
        file >> j;

        if (info_out && j.contains("meta")) {
            auto& m = j["meta"];
            info_out->name   = m.value("language", "Unknown");
            info_out->author = m.value("author",   "Unknown");
        }

        flatten(j, out);
        return true;
    } catch (const std::exception& e) {
        SDL_Log("Lang::load_file — parse error in %s: %s", path.c_str(), e.what());
        return false;
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

// Extract the filename stem (without extension) from a filename.
// e.g. "es.json" → "es", "pt-br.json" → "pt-br"
static std::string stem_of(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return filename;
    return filename.substr(0, dot);
}

// Extract the extension (with dot) from a filename. e.g. "es.json" → ".json"
static std::string ext_of(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return "";
    return filename.substr(dot);
}

void set_baseline_dir(const std::string& dir) {
    s_baseline_dir = dir;
    // Load the permanent English fallback from the baseline immediately.
    s_fallback.clear();
    std::string en_path = dir + "/en.json";
    if (!load_file(en_path, s_fallback)) {
        SDL_Log("Lang::set_baseline_dir — WARNING: could not load %s", en_path.c_str());
    } else {
        SDL_Log("Lang::set_baseline_dir — baseline loaded from %s (%zu keys)",
                en_path.c_str(), s_fallback.size());
    }
    // Default active strings to the baseline so the very first frame has text
    // even before load() is called.
    s_strings     = s_fallback;
    s_active_code = "en";
}

// Scan a single directory for *.json, appending to `out`. De-dupes by code
// (a language already present is not added again — user dir is scanned first
// so user versions take precedence in the listing).
static void scan_dir(const std::string& dir, std::vector<LanguageInfo>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (ext_of(name) != ".json") continue;

        std::string stem = stem_of(name);
        bool exists = false;
        for (auto& li : out) if (li.code == stem) { exists = true; break; }
        if (exists) continue;

        LanguageInfo info;
        info.code = stem;
        std::map<std::string, std::string> tmp;
        load_file(dir + "/" + name, tmp, &info);
        out.push_back(info);
    }
    closedir(d);
}

ScanResult scan(const std::string& user_dir,
                const std::vector<std::string>& known_codes) {
    s_user_dir = user_dir;
    ScanResult result;

    // User directory first (takes precedence), then baseline.
    scan_dir(user_dir,       result.all);
    scan_dir(s_baseline_dir, result.all);

    for (auto& li : result.all) {
        bool is_known = (li.code == "en");
        for (auto& k : known_codes) if (k == li.code) { is_known = true; break; }
        if (!is_known) result.new_ones.push_back(li);
    }

    s_available = result.all;
    return result;
}

bool load(const std::string& code) {
    // English is always available via the baseline fallback that
    // set_baseline_dir() loaded. Nothing more to do.
    if (code == "en") {
        s_strings     = s_fallback;
        s_active_code = "en";
        SDL_Log("Lang::load — active language: English (en)");
        return true;
    }

    // Try the user dir first, then the baseline dir, for "<code>.json".
    std::map<std::string, std::string> loaded;
    bool found = false;

    if (!s_user_dir.empty())
        found = load_file(s_user_dir + "/" + code + ".json", loaded);
    if (!found && !s_baseline_dir.empty())
        found = load_file(s_baseline_dir + "/" + code + ".json", loaded);

    if (!found) {
        SDL_Log("Lang::load — '%s' not found; staying on English", code.c_str());
        s_strings     = s_fallback;
        s_active_code = "en";
        return false;
    }

    // Backfill any missing keys from the English baseline so partial
    // translations never show blank strings.
    for (auto& [key, val] : s_fallback) {
        if (loaded.find(key) == loaded.end()) loaded[key] = val;
    }

    s_strings     = std::move(loaded);
    s_active_code = code;
    SDL_Log("Lang::load — active language: %s", code.c_str());
    return true;
}

// ─── Access ───────────────────────────────────────────────────────────────────

const std::string& t(const std::string& key) {
    auto it = s_strings.find(key);
    if (it != s_strings.end()) return it->second;

    // Fallback to English
    auto fb = s_fallback.find(key);
    if (fb != s_fallback.end()) return fb->second;

    // Last resort: return the key itself (static so we can return a ref)
    static std::map<std::string, std::string> s_key_cache;
    auto& cached = s_key_cache[key];
    if (cached.empty()) cached = key;
    return cached;
}

const std::string&              active_code() { return s_active_code; }
const std::vector<LanguageInfo>& available()  { return s_available;   }

} // namespace Lang
