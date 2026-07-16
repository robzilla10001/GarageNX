// source/core/keys.cpp

#include "core/keys.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace Core::Keys {

// ─── State ────────────────────────────────────────────────────────────────────

static Keyset     s_keys;
static bool       s_available = false;
static std::string s_requirement;

// ─── Hex helpers ──────────────────────────────────────────────────────────────

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse a hex string into bytes. Returns true if exactly out_len bytes parsed.
static bool parse_hex(const std::string& s, uint8_t* out, size_t out_len) {
    if (s.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_nibble(s[i * 2]);
        int lo = hex_nibble(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// ─── Line parsing ─────────────────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// If `name` matches "<prefix>XX" where XX is 2 hex digits, return the generation
// number in `gen` and true; else false.
static bool match_indexed(const std::string& name, const std::string& prefix,
                          int& gen) {
    if (name.size() != prefix.size() + 2) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    int hi = hex_nibble(name[prefix.size()]);
    int lo = hex_nibble(name[prefix.size() + 1]);
    if (hi < 0 || lo < 0) return false;
    gen = (hi << 4) | lo;
    return true;
}

// Handle a single "key = value" pair from prod.keys.
static void handle_prod_pair(const std::string& key_lc, const std::string& val) {
    // header_key (0x20 bytes). Accept both "header_key" and the nstool/hactool
    // alias "nca_header_key".
    if (key_lc == "header_key" || key_lc == "nca_header_key") {
        if (parse_hex(val, s_keys.header_key.data(), 32))
            s_keys.has_header_key = true;
        return;
    }

    int gen = -1;
    // key_area_key_application_XX (0x10). Accept the nca_ alias too.
    if (match_indexed(key_lc, "key_area_key_application_", gen) ||
        match_indexed(key_lc, "nca_key_area_key_application_", gen)) {
        if (gen >= 0 && gen < MAX_KEY_GENERATION &&
            parse_hex(val, s_keys.key_area_key_application[gen].data(), 16)) {
            s_keys.has_kaek_application[gen] = true;
        }
        return;
    }

    // titlekek_XX (0x10)
    if (match_indexed(key_lc, "titlekek_", gen)) {
        if (gen >= 0 && gen < MAX_KEY_GENERATION &&
            parse_hex(val, s_keys.titlekek[gen].data(), 16)) {
            s_keys.has_titlekek[gen] = true;
        }
        return;
    }
    // Everything else we don't need for now — ignore silently.
}

// Parse a keyfile. Each line: "name = hexvalue". Comments (';', '#') and blank
// lines ignored. `is_title_keys` switches to rights-id = titlekey parsing.
static bool parse_file(const std::string& path, bool is_title_keys) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;

        auto eq = l.find('=');
        if (eq == std::string::npos) continue;

        std::string name = trim(l.substr(0, eq));
        std::string val  = trim(l.substr(eq + 1));
        if (name.empty() || val.empty()) continue;

        if (is_title_keys) {
            // rights_id (0x10 hex = 32 chars) = titlekey (0x10 hex = 32 chars)
            std::string rid = to_lower(name);
            Key128 tk;
            if (rid.size() == 32 && parse_hex(val, tk.data(), 16)) {
                s_keys.title_keys[rid] = tk;
            }
        } else {
            handle_prod_pair(to_lower(name), val);
        }
    }
    fclose(f);
    return true;
}

// ─── Public API ───────────────────────────────────────────────────────────────

LoadResult load(const std::string& switch_dir) {
    // Reset
    s_keys = Keyset{};
    s_available = false;
    s_requirement.clear();

    LoadResult result;

    std::string prod = switch_dir + "/prod.keys";
    bool found = parse_file(prod, false);
    result.file_found = found;

    if (!found) {
        s_requirement =
            "Keys required.\n\n"
            "GarageNX needs your console keys to read title names and icons.\n"
            "Place prod.keys at:\n"
            "  sdmc:/switch/prod.keys\n\n"
            "You can dump them with Lockpick_RCM.";
        result.ok = false;
        result.missing = "prod.keys not found";
        return result;
    }

    // Optional title.keys (only needed for ticket-crypto content).
    parse_file(switch_dir + "/title.keys", true);

    // Validate essentials: header_key + at least one application key-area key.
    bool have_any_kaek = false;
    for (int i = 0; i < MAX_KEY_GENERATION; ++i)
        if (s_keys.has_kaek_application[i]) { have_any_kaek = true; break; }

    if (!s_keys.has_header_key || !have_any_kaek) {
        std::string missing;
        if (!s_keys.has_header_key) missing += "header_key ";
        if (!have_any_kaek)         missing += "key_area_key_application_XX ";

        s_requirement =
            "Incomplete keys.\n\n"
            "prod.keys was found but is missing required entries:\n  "
            + missing +
            "\n\nRe-dump your keys with Lockpick_RCM and replace\n"
            "sdmc:/switch/prod.keys.";
        result.ok = false;
        result.missing = missing;
        SDL_Log("Keys::load — incomplete: %s", missing.c_str());
        return result;
    }

    s_available = true;
    result.ok = true;
    SDL_Log("Keys::load — OK (header_key + %s title keys)",
            std::to_string(s_keys.title_keys.size()).c_str());
    return result;
}

const Keyset& get() { return s_keys; }
bool available() { return s_available; }
std::string requirement_message() { return s_requirement; }

} // namespace Core::Keys
