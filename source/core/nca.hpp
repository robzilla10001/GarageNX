#pragma once
// source/core/nca.hpp
// Minimal NCA reader focused on what GarageNX needs: decrypt a Control NCA and
// extract the title's display name (from control.nacp) and icon (icon_*.dat).
//
// This is NOT a general-purpose NCA tool. It handles the common retail Control
// NCA case: NCA3 header, AES-XTS header crypto with header_key, key-area crypto
// via key_area_key_application, and a RomFS section using AES-128-CTR. Titlekey
// (ticket) crypto is supported when the titlekey is available.
//
// All crypto uses libnx's hardware-accelerated AES (aes.h / aes_ctr.h /
// aes_xts.h), including the Nintendo non-standard XTS tweak.

#include "core/keys.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

namespace Core::Nca {

// A reader abstraction so the same NCA logic works over different backends
// (NCM ContentStorage, a raw file on SD, etc.). Reads `size` bytes at absolute
// `offset` into `dst`; returns bytes actually read.
using ReadFn = std::function<size_t(uint64_t offset, void* dst, size_t size)>;

// Parsed control data extracted from a Control NCA.
struct ControlData {
    bool                 ok = false;
    std::string          name;        // best-language application name
    std::string          author;      // best-language author/publisher
    std::string          version;     // display version string from NACP
    std::vector<uint8_t> icon_jpeg;   // raw JPEG bytes (empty if not extracted)
    std::string          fail_reason; // when !ok, a short human-readable stage
};

// Decrypt a Control NCA via `read` and extract its control data. `nca_size` is
// the full NCA size. `keys` supplies header_key + key-area keys. If the NCA uses
// titlekey crypto, the matching titlekey must be present in keys.title_keys.
// `want_icon` controls whether the (larger) icon is extracted.
ControlData read_control(const ReadFn& read, uint64_t nca_size,
                         const Core::Keys::Keyset& keys, bool want_icon);

// Convenience: read a Control NCA from a plain file path on the SD card.
ControlData read_control_file(const std::string& path,
                              const Core::Keys::Keyset& keys, bool want_icon);

} // namespace Core::Nca
