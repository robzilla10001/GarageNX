#pragma once
// source/services/lockpick_fetch.hpp
//
// Downloads the latest Lockpick_RCM_Pro release from GitHub and installs its
// payload to sdmc:/bootloader/payloads/. Used by the startup prod.keys check when
// the keys are missing and the payload is not on the SD card.
//
// The pure parts (picking the release asset, picking the zip entry) are header-
// only and host-tested (tests/lockpick_fetch_test.cpp); the network + zip work is
// in lockpick_fetch.cpp (libcurl + minizip, Switch only).
//
// TLS CERTIFICATES ARE NOT VERIFIED (a deliberate project decision, the console
// has no CA store). The payload therefore comes over a channel an on-path attacker
// could alter; the user is told which file was written and where.

#include "nlohmann/json.hpp"

#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace Services::LockpickFetch {

constexpr const char* kRepoApiUrl =
    "https://api.github.com/repos/sthetix/Lockpick_RCM_Pro/releases/latest";
constexpr const char* kPayloadDir  = "sdmc:/bootloader/payloads";
constexpr const char* kPayloadPath = "sdmc:/bootloader/payloads/Lockpick_RCM_pro.bin";

// ─── Pure helpers ─────────────────────────────────────────────────────────────

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

/// From a GitHub "latest release" JSON document, the https download URL of the
/// release's .zip asset (preferring one whose name mentions "lockpick"). Empty if
/// the JSON is malformed or has no usable zip. Only https:// URLs are accepted.
inline std::string pick_zip_url(const std::string& release_json, std::string* tag = nullptr) {
    nlohmann::json j = nlohmann::json::parse(release_json, nullptr, false);
    if (!j.is_object()) return {};
    if (tag && j.contains("tag_name") && j["tag_name"].is_string())
        *tag = j["tag_name"].get<std::string>();
    if (!j.contains("assets") || !j["assets"].is_array()) return {};

    std::string best, fallback;
    for (const auto& a : j["assets"]) {
        if (!a.is_object() || !a.contains("name") || !a["name"].is_string() ||
            !a.contains("browser_download_url") || !a["browser_download_url"].is_string())
            continue;
        const std::string name = lower(a["name"].get<std::string>());
        const std::string url  = a["browser_download_url"].get<std::string>();
        if (!ends_with(name, ".zip") || url.compare(0, 8, "https://") != 0) continue;
        if (name.find("lockpick") != std::string::npos) { best = url; break; }
        if (fallback.empty()) fallback = url;
    }
    return best.empty() ? fallback : best;
}

/// Index of the zip entry that is the payload: any path whose final component is
/// Lockpick_RCM_pro.bin (case-insensitive). The entry's directory is ignored, so
/// a crafted path inside the archive can never choose where the file lands.
/// -1 if absent.
inline int find_payload_entry(const std::vector<std::string>& names) {
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string n = lower(names[i]);
        const size_t slash  = n.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? n : n.substr(slash + 1);
        if (base == "lockpick_rcm_pro.bin") return (int)i;
    }
    return -1;
}

// ─── Network + install (lockpick_fetch.cpp) ───────────────────────────────────

struct Status {
    enum class Phase { Release, Download, Extract } phase = Phase::Release;
    double fraction = 0.0;   // 0..1 within Download; 0 otherwise
};

struct Result {
    bool        ok = false;
    std::string tag;     // release tag on success, e.g. "v1.2.3"
    std::string error;   // short English reason on failure (also logged)
};

/// Blocking. `on_progress` is called from inside the transfer (same thread) and
/// may repaint the screen; it must be quick.
Result fetch_latest(const std::function<void(const Status&)>& on_progress);

} // namespace Services::LockpickFetch
