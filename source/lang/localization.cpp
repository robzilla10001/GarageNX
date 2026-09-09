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
static std::string                         s_lang_dir;      // sdmc:/switch/GarageNX/lang

// One-line diagnostics to sdmc:/switch/GarageNX/logs/lang.log. SDL_Log is
// nxlink-only on hardware; without the file copy a scan/load failure leaves no
// evidence at all.
static void lang_log(const std::string& line) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/lang.log", "a");
    if (f) {
        ::fprintf(f, "%s\n", line.c_str());
        ::fclose(f);
    }
    SDL_Log("%s", line.c_str());
}

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
        lang_log("Lang::load_file - cannot open " + path);
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
        lang_log("Lang::load_file - parse error in " + path + ": " + e.what());
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

void set_lang_dir(const std::string& dir) {
    s_lang_dir = dir;
    lang_log("Lang::set_lang_dir - language directory: " + dir);
}

// en.json is the permanent fallback and the source of every key. It lives in
// the same directory as every other language file; loading is deferred to
// first use. Attempted once - a retry on every t() call would hammer the SD
// card and flood the log when the file is missing.
static void ensure_en_fallback() {
    static bool s_fallback_tried = false;
    if (s_fallback_tried || s_lang_dir.empty()) return;
    s_fallback_tried = true;

    std::string en_path = s_lang_dir + "/en.json";
    if (!load_file(en_path, s_fallback)) {
        lang_log("Lang::ensure_en_fallback - FAILED to load " + en_path);
        return;
    }
    lang_log("Lang::ensure_en_fallback - loaded " + en_path + " (" +
             std::to_string(s_fallback.size()) + " keys)");
    // Default active strings to the fallback so the very first frame has text
    // even before load() is called.
    s_strings = s_fallback;
}

// Scan the language directory for *.json, appending to `out`. De-dupes by
// code, so en.json found here never double-lists the fallback already loaded.
static void scan_dir(const std::string& dir, std::vector<LanguageInfo>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) {
        lang_log("Lang::scan - cannot open " + dir);
        return;
    }
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
        if (load_file(dir + "/" + name, tmp, &info)) {
            lang_log("Lang::scan - found language '" + stem + "' (" +
                     info.name + ") at " + dir + "/" + name);
            out.push_back(info);
        } else {
            lang_log("Lang::scan - SKIPPED unparsable " + dir + "/" + name);
        }
    }
    closedir(d);
}

ScanResult scan(const std::string& lang_dir,
                const std::vector<std::string>& known_codes) {
    s_lang_dir = lang_dir;
    ScanResult result;

    ensure_en_fallback();
    scan_dir(lang_dir, result.all);

    for (auto& li : result.all) {
        bool is_known = (li.code == "en");
        for (auto& k : known_codes) if (k == li.code) { is_known = true; break; }
        if (!is_known) result.new_ones.push_back(li);
    }

    s_available = result.all;
    lang_log("Lang::scan - " + std::to_string(result.all.size()) +
             " language(s) available in " + lang_dir);
    return result;
}

bool load(const std::string& code) {
    ensure_en_fallback();

    if (code == "en") {
        s_strings     = s_fallback;
        s_active_code = "en";
        lang_log("Lang::load - active language: English (en)");
        return true;
    }

    std::map<std::string, std::string> loaded;
    bool found = false;
    if (!s_lang_dir.empty())
        found = load_file(s_lang_dir + "/" + code + ".json", loaded);

    if (!found) {
        lang_log("Lang::load - '" + code + "' not found in " + s_lang_dir +
                 "; staying on English");
        s_strings     = s_fallback;
        s_active_code = "en";
        return false;
    }

    // Backfill any missing keys from the English fallback so partial
    // translations never show blank strings.
    for (auto& [key, val] : s_fallback) {
        if (loaded.find(key) == loaded.end()) loaded[key] = val;
    }

    s_strings     = std::move(loaded);
    s_active_code = code;
    lang_log("Lang::load - active language: " + code);
    return true;
}

// ─── Access ───────────────────────────────────────────────────────────────────

const std::string& t(const std::string& key) {
    ensure_en_fallback();

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
