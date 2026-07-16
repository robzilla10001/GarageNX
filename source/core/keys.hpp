#pragma once
// source/core/keys.hpp
// Loads the Switch keyset from sdmc:/switch/prod.keys (and optional title.keys),
// exposing the specific keys GarageNX needs to decrypt Control NCAs for name/icon
// resolution and, later, to install/dump titles.
//
// We only parse the keys we actually use rather than the entire keyset. The two
// essential ones for reading Control NCAs are:
//   - header_key                     (0x20 bytes) — AES-XTS, decrypts NCA header
//   - key_area_key_application_##     (0x10 bytes) — AES-ECB, decrypts key area
// Titlekeys (for content using external/ticket crypto) come from title.keys and
// are looked up by rights ID.

#include <string>
#include <array>
#include <map>
#include <cstdint>

namespace Core::Keys {

using Key128 = std::array<uint8_t, 16>;
using Key256 = std::array<uint8_t, 32>;

// The maximum master-key generation we track. The Switch is well past 0x10 now,
// but generations are contiguous; size generously and bounds-check on use.
constexpr int MAX_KEY_GENERATION = 0x20;

struct Keyset {
    // NCA header key: 0x20 bytes (two 0x10 AES-XTS sub-keys).
    Key256 header_key{};
    bool   has_header_key = false;

    // Per-generation key-area keys. Index by [generation]. Application/ocean/
    // system categories; Control NCAs use the "application" category.
    std::array<Key128, MAX_KEY_GENERATION> key_area_key_application{};
    std::array<bool,   MAX_KEY_GENERATION> has_kaek_application{};

    // Title KEKs (per generation) for decrypting titlekeys from tickets.
    std::array<Key128, MAX_KEY_GENERATION> titlekek{};
    std::array<bool,   MAX_KEY_GENERATION> has_titlekek{};

    // Titlekeys loaded from title.keys, keyed by lowercase-hex rights ID.
    std::map<std::string, Key128> title_keys;
};

// ─── Loading ──────────────────────────────────────────────────────────────────

/// Result of an attempt to load the keyset.
struct LoadResult {
    bool        ok = false;         // true if the minimum required keys are present
    bool        file_found = false; // prod.keys existed at all
    std::string missing;            // human-readable list of missing essentials
};

/// Load keys from the standard location (sdmc:/switch/). Parses prod.keys and,
/// if present, title.keys. Returns whether the essentials for Control-NCA
/// decryption were found. Safe to call multiple times (reloads).
LoadResult load(const std::string& switch_dir = "sdmc:/switch");

/// Access the loaded keyset. Only valid after a successful load().
const Keyset& get();

/// True if load() succeeded with the essential keys for reading Control NCAs.
bool available();

/// A short message suitable for a "keys required" blocking screen, naming what's
/// missing and where to put it. Empty when keys are available.
std::string requirement_message();

} // namespace Core::Keys
