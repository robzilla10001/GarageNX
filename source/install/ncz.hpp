#pragma once
// source/install/ncz.hpp
// NCZ/NSZ section-based zstd decompression for NCA install.
//
// An .ncz is a single NCA whose ENCRYPTED body has been decrypted (so it will
// compress) and then zstd-compressed. An .nsz / .xcz is just an NSP / XCI whose
// .nca entries have been replaced with these .ncz entries — so "NSZ" is NOT a
// zipped NSP; each NCA is transformed individually.
//
// On-disk NCZ layout:
//   [0x0000..0x3FFF] : original NCA header (0x4000 bytes) — unmodified
//   [0x4000..0x4007] : ncz::Header { u64 magic, u64 total_sections }
//   [0x4008..]       : Section[total_sections] (0x40 bytes each):
//                        u64 offset, u64 size, u64 crypto_type, u64 padding,
//                        u8 key[0x10], u8 counter[0x10]   (upper 8 = CTR nonce)
//   [after sections] : optional BlockHeader + u32 block_sizes[] (block-compressed)
//   [data region]    : zstd stream(s) of the DECRYPTED NCA body (from 0x4000 on)
//
// Reconstructing the installable NCA:
//   1. Write the 0x4000 header VERBATIM. It is part of the SHA-256 that defines
//      content_id, so it must not be altered (no distribution-bit patch, no
//      crypto conversion) or the reconstructed NCA won't match its id.
//   2. zstd-decompress the body.
//   3. RE-ENCRYPT it. The body is stored DECRYPTED for BOTH titlekey NCAs
//      (rights_id set) and standard-crypto NCAs, so re-encryption is always
//      required. It is applied per-section: crypto_type >= AesCtr(3) is CTR
//      re-encrypted with the section's key/counter; crypto_type None(1) passes
//      through. The CTR counter is keyed to the ABSOLUTE NCA offset (offset>>4),
//      one context per section, auto-incrementing across chunks.
//
// The result is byte-identical to the original NCA, so SHA-256 == content_id
// and NCM accepts it — exactly like a verbatim NSP/XCI install.
//
// Titlekey NCAs keep their rights_id, so a matching ticket must be installed
// (use the container's .tik, or make_common_ticket() when none is present).

#include "core/keys.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Install {

// ReadFn matches the definition in installer.hpp — callbacks bound to container readers.
using ReadFn = std::function<size_t(uint64_t offset_in_entry, void* buf, size_t len)>;
using WriteCallback = std::function<bool(uint64_t nca_offset, const uint8_t* data, size_t size)>;

static constexpr uint64_t NCZ_SECTION_MAGIC = 0x4E544345535A434EULL;
static constexpr uint64_t NCZ_NORMAL_SIZE   = 0x4000; // NCA header size

#pragma pack(push, 1)
struct NczHeader {
    uint64_t magic;          // NCZ_SECTION_MAGIC
    uint64_t total_sections;
};

struct NczSection {
    uint64_t offset;         // NCA-relative (absolute) byte offset where section starts
    uint64_t size;           // size of this section in the decompressed NCA
    uint64_t crypto_type;    // 1=None, 3=AES-CTR (>=3 is re-encrypted)
    uint64_t padding;
    uint8_t  key[0x10];      // AES-128 CTR key (decrypted content/title key)
    uint8_t  counter[0x10];  // upper 8 bytes = CTR nonce; lower 8 overwritten by offset
};
static_assert(sizeof(NczSection) == 0x40, "NczSection size");

struct NczBlockHeader {
    uint64_t magic;               // NCZ_BLOCK_MAGIC
    uint8_t  version;             // must be 2
    uint8_t  type;                // must be 1
    uint8_t  padding;
    uint8_t  block_size_exponent; // block_size = 1 << exponent (typically 0x14 = 1MB)
    uint32_t total_blocks;
    uint64_t decompressed_size;
};
static_assert(sizeof(NczBlockHeader) == 0x18, "NczBlockHeader size");
#pragma pack(pop)

static constexpr uint64_t NCZ_BLOCK_MAGIC   = 0x4B434F4C425A434EULL;
static constexpr uint8_t  NCZ_BLOCK_VERSION = 2;
static constexpr uint8_t  NCZ_BLOCK_TYPE    = 1;

// ── Decompressor ─────────────────────────────────────────────────────────────

struct NczDecompressor {
    // Returns true if this file is NSZ/NCZ format (has the NCZ section magic).
    static bool is_ncz(const ReadFn& read_fn, uint64_t nca_size);

    // Returns the decompressed NCA size (including the 0x4000 header).
    // For block-compressed NCZ: NCZ_NORMAL_SIZE + blk_hdr.decompressed_size.
    // For stream NCZ: reads the NCA size field from the decrypted header.
    // Returns 0 on failure or if not NCZ.
    static uint64_t get_decompressed_size(const ReadFn& read_fn, uint64_t nca_size,
                                           const Core::Keys::Keyset& keys);

    // Build a minimal common ticket for a titlekey-crypto NSZ that has no .tik file.
    // titlekey = the raw AES-128 titlekey (from NCZ section key for crypto=3 sections).
    // Returns the 0x2C0-byte ticket, or empty if the NCA doesn't use titlekey crypto.
    static std::vector<uint8_t> make_common_ticket(const ReadFn& read_fn, uint64_t nca_size,
                                                    const Core::Keys::Keyset& keys,
                                                    const uint8_t* titlekey);

    // Decompress a single NSZ/NCZ entry, calling `write_cb` with each chunk in
    // order. The first call delivers the verbatim NCA header (0x4000 bytes);
    // subsequent calls deliver decompressed + re-encrypted body chunks. The
    // reconstructed NCA is byte-identical to the original.
    // Returns error string on failure, empty string on success.
    static std::string decompress(
        const ReadFn& read_fn,
        uint64_t      nca_size,
        const Core::Keys::Keyset& keys,
        WriteCallback write_cb);
};

} // namespace Install
