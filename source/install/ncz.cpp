// source/install/ncz.cpp
// NCZ/NSZ decompressor — zstd decompression + AES-CTR re-encryption for NCA install.
//
// CORRECTNESS MODEL (this is why NSP/XCI worked but NSZ did not):
//   A registered NCA is identified by its content_id = first 0x10 bytes of the
//   SHA-256 of the ENTIRE NCA file. NCM (and every downstream integrity check)
//   only accepts the placeholder if the bytes we write hash back to that id.
//   NSP/XCI copy the NCA verbatim, so they always match. To make NSZ match we
//   must reconstruct the ORIGINAL NCA byte-for-byte:
//
//     [0x0000..0x3FFF]  original NCA header  -> written VERBATIM (never patched)
//     [0x4000..]        original NCA body    -> zstd-decompress, then RE-ENCRYPT
//                                               each section with its stored key
//
//   The body inside an .ncz is stored DECRYPTED (that is what lets it compress).
//   Re-encryption is therefore ALWAYS required — for titlekey NCAs (rights_id
//   set) and standard-crypto NCAs alike. The only per-section switch is the
//   section's own crypto_type: type >= AesCtr(3) is re-encrypted with the
//   section key/counter, type None(1) is passed through untouched.
//
//   The AES-CTR counter is keyed to the ABSOLUTE NCA offset (offset >> 4),
//   NOT a section-relative offset. We create one CTR context per section at the
//   section's absolute start and let it auto-increment across chunk boundaries,
//   which keeps the keystream correct even when zstd hands us unaligned chunks.

#include "install/ncz.hpp"
#include "core/keys.hpp"
#include <cstring>
#include <algorithm>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <zstd.h>
#endif

namespace Install {

#ifdef PLATFORM_SWITCH

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool safe_read(const ReadFn& fn, uint64_t off, void* buf, size_t len) {
    return fn(off, buf, len) == len;
}

// AES-CTR encryption type in the NCA fs-header sense. Section crypto_type uses
// the same numbering: 0=Auto 1=None 2=Xts 3=Ctr 4=CtrEx 5/6=SkipLayerHash.
// Anything >= 3 is CTR-based and must be re-encrypted with the section key.
static constexpr uint64_t NCZ_CRYPTO_AES_CTR = 3;

// Find the section covering an absolute NCA offset, or nullptr.
static const NczSection* find_section(const std::vector<NczSection>& sections,
                                       uint64_t nca_offset) {
    for (const auto& s : sections) {
        if (nca_offset >= s.offset && nca_offset < s.offset + s.size)
            return &s;
    }
    return nullptr;
}

// ── Stateful section re-encryptor ─────────────────────────────────────────────
// Re-encrypts the decompressed NCA body back into its original ciphertext.
// Feed it the decompressed body strictly sequentially (it tracks the absolute
// NCA offset internally). Mirrors sphaira/yati.cpp decompressFuncInternal:
//   * one Aes128CtrContext is created per section, at the section's ABSOLUTE
//     start offset (counter = section.counter[0..7] || (start >> 4) BE),
//   * aes128CtrCrypt then auto-increments the counter across every call, so a
//     chunk that straddles a 16-byte block boundary is handled correctly,
//   * sections with crypto_type < AesCtr are copied through unmodified.
class SectionRecryptor {
public:
    SectionRecryptor(const std::vector<NczSection>& sections, uint64_t start_offset)
        : m_sections(sections), m_offset(start_offset) {}

    uint64_t offset() const { return m_offset; }

    // Re-encrypt `len` bytes of decompressed body in place. Returns false only
    // if some byte is not covered by any section (malformed NCZ).
    bool process(uint8_t* data, size_t len) {
        size_t done = 0;
        while (done < len) {
            if (!m_cur || !in_range(m_cur, m_offset)) {
                m_cur = find_section(m_sections, m_offset);
                if (!m_cur) return false;
                if (m_cur->crypto_type >= NCZ_CRYPTO_AES_CTR) {
                    // Counter = section nonce (upper 8) || abs_offset>>4 (lower 8, BE).
                    const uint64_t block_be = __builtin_bswap64(m_offset >> 4);
                    uint8_t ctr[0x10];
                    std::memcpy(ctr,     m_cur->counter, 8);
                    std::memcpy(ctr + 8, &block_be,      8);
                    aes128CtrContextCreate(&m_ctx, m_cur->key, ctr);
                }
            }

            const uint64_t sec_end = m_cur->offset + m_cur->size;
            const size_t   chunk   = (size_t)std::min<uint64_t>(len - done, sec_end - m_offset);

            if (m_cur->crypto_type >= NCZ_CRYPTO_AES_CTR) {
                // aes128CtrCrypt keeps its own counter/keystream state, so it is
                // safe across unaligned chunk boundaries within one section.
                aes128CtrCrypt(&m_ctx, data + done, data + done, chunk);
            }
            // crypto_type < AesCtr (e.g. None): plaintext region, pass through.

            m_offset += chunk;
            done     += chunk;
        }
        return true;
    }

private:
    static bool in_range(const NczSection* s, uint64_t off) {
        return off >= s->offset && off < s->offset + s->size;
    }

    const std::vector<NczSection>& m_sections;
    const NczSection* m_cur = nullptr;
    Aes128CtrContext  m_ctx{};
    uint64_t          m_offset;
};

// ── Public API ───────────────────────────────────────────────────────────────

uint64_t NczDecompressor::get_decompressed_size(const ReadFn& read_fn,
                                                    uint64_t nca_size,
                                                    const Core::Keys::Keyset& keys) {
    if (nca_size < NCZ_NORMAL_SIZE + sizeof(NczHeader)) return 0;

    // Read NCZ header to get section count.
    NczHeader ncz_hdr{};
    if (!safe_read(read_fn, NCZ_NORMAL_SIZE, &ncz_hdr, sizeof(ncz_hdr))) return 0;
    if (ncz_hdr.magic != NCZ_SECTION_MAGIC || ncz_hdr.total_sections == 0) return 0;

    // Skip sections.
    uint64_t off = NCZ_NORMAL_SIZE + sizeof(ncz_hdr) + ncz_hdr.total_sections * sizeof(NczSection);

    // Check for block header — it has the decompressed size explicitly.
    if (off + sizeof(NczBlockHeader) <= nca_size) {
        NczBlockHeader blk{};
        if (safe_read(read_fn, off, &blk, sizeof(blk)) &&
            blk.magic == NCZ_BLOCK_MAGIC &&
            blk.version == NCZ_BLOCK_VERSION && blk.type == NCZ_BLOCK_TYPE &&
            blk.total_blocks > 0 && blk.block_size_exponent >= 14) {
            // decompressed_size covers only the body (after NCA header).
            return NCZ_NORMAL_SIZE + blk.decompressed_size;
        }
    }

    // Stream NCZ: decompressed size is in the NCA header content_size field (0x208, u64).
    // Decrypt NCA header to read it.
    if (!keys.has_header_key) return 0;
    std::vector<uint8_t> enc(0xC00), dec(0xC00);
    if (!safe_read(read_fn, 0, enc.data(), 0xC00)) return 0;
    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, keys.header_key.data(), keys.header_key.data() + 0x10, false);
    for (int s = 0; s < 6; ++s) {
        aes128XtsContextResetSector(&xts, s, true);
        aes128XtsDecrypt(&xts, dec.data() + s*0x200, enc.data() + s*0x200, 0x200);
    }
    if (memcmp(dec.data() + 0x200, "NCA3", 4) != 0 &&
        memcmp(dec.data() + 0x200, "NCA2", 4) != 0) return 0;
    uint64_t content_size = 0;
    memcpy(&content_size, dec.data() + 0x208, 8);
    return content_size;
}

bool NczDecompressor::is_ncz(const ReadFn& read_fn, uint64_t nca_size) {
    if (nca_size < NCZ_NORMAL_SIZE + sizeof(NczHeader)) return false;
    NczHeader hdr{};
    if (!safe_read(read_fn, NCZ_NORMAL_SIZE, &hdr, sizeof(hdr))) return false;
    return hdr.magic == NCZ_SECTION_MAGIC && hdr.total_sections > 0 && hdr.total_sections < 64;
}

std::string NczDecompressor::decompress(
    const ReadFn& read_fn,
    uint64_t      nca_size,
    const Core::Keys::Keyset& keys,
    WriteCallback write_cb)
{
    (void)keys; // header is written verbatim; no key material needed for the body.

    // ── 1. Read NCA header (0x4000 bytes) and write it VERBATIM ───────────────
    // The header is part of the SHA-256 that defines content_id. Patching any
    // byte (distribution bit, key area, ...) would make the reconstructed NCA
    // hash to a different id than the one we register under -> "hash mismatch".
    // We reproduce the original NCA exactly, so we never touch the header.
    std::vector<uint8_t> header(NCZ_NORMAL_SIZE, 0);
    if (!safe_read(read_fn, 0, header.data(), NCZ_NORMAL_SIZE))
        return "NCZ: failed to read NCA header";

    // ── 2. Read NCZ header + sections ────────────────────────────────────────
    NczHeader ncz_hdr{};
    if (!safe_read(read_fn, NCZ_NORMAL_SIZE, &ncz_hdr, sizeof(ncz_hdr)))
        return "NCZ: failed to read NCZ header";
    if (ncz_hdr.magic != NCZ_SECTION_MAGIC || ncz_hdr.total_sections == 0)
        return "NCZ: bad magic or zero sections";

    uint64_t off = NCZ_NORMAL_SIZE + sizeof(ncz_hdr);
    std::vector<NczSection> sections(ncz_hdr.total_sections);
    if (!safe_read(read_fn, off, sections.data(), sizeof(NczSection) * sections.size()))
        return "NCZ: failed to read sections";
    off += sizeof(NczSection) * sections.size();

    // Optional: dump section layout to SD for diagnosis (non-fatal).
    if (FILE* sf = std::fopen("sdmc:/switch/GarageNX/ncz_sections.txt", "wb")) {
        std::fprintf(sf, "total_sections=%llu\n", (unsigned long long)ncz_hdr.total_sections);
        for (size_t si = 0; si < sections.size(); ++si) {
            std::fprintf(sf, "section[%zu]: offset=0x%llX size=0x%llX crypto=%llu\n", si,
                (unsigned long long)sections[si].offset,
                (unsigned long long)sections[si].size,
                (unsigned long long)sections[si].crypto_type);
        }
        std::fclose(sf);
    }

    // Write the (verbatim) NCA header now.
    if (!write_cb(0, header.data(), NCZ_NORMAL_SIZE))
        return "NCZ: write callback failed (header)";

    // ── 3. Check for optional block header ───────────────────────────────────
    static constexpr uint32_t ZSTD_FRAME_MAGIC = 0xFD2FB528u;

    NczBlockHeader blk_hdr{};
    bool has_blocks = false;
    std::vector<uint32_t> block_sizes;

    if (off + sizeof(blk_hdr) <= nca_size) {
        NczBlockHeader candidate{};
        if (safe_read(read_fn, off, &candidate, sizeof(candidate)) &&
            candidate.magic == NCZ_BLOCK_MAGIC &&
            candidate.version == NCZ_BLOCK_VERSION &&
            candidate.type == NCZ_BLOCK_TYPE &&
            candidate.total_blocks > 0 &&
            candidate.block_size_exponent >= 14 && candidate.block_size_exponent <= 32) {
            blk_hdr = candidate;
            has_blocks = true;
            off += sizeof(blk_hdr);
            block_sizes.resize(blk_hdr.total_blocks);
            if (!safe_read(read_fn, off, block_sizes.data(), sizeof(uint32_t) * block_sizes.size()))
                return "NCZ: failed to read block sizes";
            off += sizeof(uint32_t) * block_sizes.size();
        }
    }

    const uint64_t compressed_start = off;

    // Sanity: for stream NCZ, verify a zstd frame magic at compressed_start.
    if (!has_blocks) {
        uint32_t magic = 0;
        safe_read(read_fn, compressed_start, &magic, 4);
        const bool is_std_frame  = (magic == ZSTD_FRAME_MAGIC);
        const bool is_skip_frame = ((magic & 0xFFFFFFF0u) == 0x184D2A50u);
        if (!is_std_frame && !is_skip_frame) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                "NCZ: expected zstd frame at 0x%llX but got magic 0x%08X",
                (unsigned long long)compressed_start, magic);
            return msg;
        }
    }

    // ── 4. Decompress body and re-encrypt with one recryptor for the whole NCA ─
    // The recryptor tracks the absolute NCA offset, starting right after the
    // header. Both the block and stream paths feed it strictly sequentially.
    SectionRecryptor recryptor(sections, NCZ_NORMAL_SIZE);

    if (has_blocks) {
        // Block-compressed: each block is an independent zstd frame (random access).
        const uint32_t block_sz = 1u << blk_hdr.block_size_exponent;
        uint64_t block_read_off = compressed_start;

        for (uint32_t b = 0; b < blk_hdr.total_blocks; ++b) {
            const uint32_t cmp_size = block_sizes[b];

            // Last block may be smaller in decompressed size.
            uint32_t dec_size = block_sz;
            if (b == blk_hdr.total_blocks - 1) {
                const uint64_t remainder = blk_hdr.decompressed_size % block_sz;
                if (remainder) dec_size = (uint32_t)remainder;
            }

            std::vector<uint8_t> cmp(cmp_size);
            if (!safe_read(read_fn, block_read_off, cmp.data(), cmp_size))
                return "NCZ: failed to read compressed block";
            block_read_off += cmp_size;

            std::vector<uint8_t> dec(dec_size);
            if (cmp_size < dec_size) {
                const size_t res = ZSTD_decompress(dec.data(), dec_size, cmp.data(), cmp_size);
                if (ZSTD_isError(res) || res != dec_size)
                    return "NCZ: ZSTD_decompress block failed";
            } else {
                // Stored uncompressed (incompressible block).
                std::memcpy(dec.data(), cmp.data(), dec_size);
            }

            const uint64_t out_off = recryptor.offset();
            if (!recryptor.process(dec.data(), dec_size))
                return "NCZ: no section covers block offset";
            if (!write_cb(out_off, dec.data(), dec_size))
                return "NCZ: write callback failed (block)";
        }
    } else {
        // Stream-compressed: a single zstd stream covering the whole body.
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        if (!dctx) return "NCZ: ZSTD_createDCtx failed";

        static constexpr size_t READ_CHUNK = 1024 * 1024;      // 1 MB read chunks
        static constexpr size_t OUT_CHUNK  = 4 * 1024 * 1024;  // 4 MB output chunks

        std::vector<uint8_t> in_buf(READ_CHUNK);
        std::vector<uint8_t> out_buf(OUT_CHUNK);
        std::string error;
        bool frame_done = false;
        uint64_t src_off = compressed_start;

        while (!frame_done && error.empty()) {
            if (src_off >= nca_size) break;
            size_t to_read = (size_t)std::min<uint64_t>(READ_CHUNK, nca_size - src_off);
            size_t got = read_fn(src_off, in_buf.data(), to_read);
            if (got == 0) { error = "NCZ: read returned 0 at stream"; break; }
            src_off += got;

            ZSTD_inBuffer in_s = { in_buf.data(), got, 0 };
            while (in_s.pos < in_s.size && error.empty()) {
                ZSTD_outBuffer out_s = { out_buf.data(), OUT_CHUNK, 0 };
                const size_t ret = ZSTD_decompressStream(dctx, &out_s, &in_s);
                if (ZSTD_isError(ret)) {
                    error = std::string("NCZ: zstd stream error: ") + ZSTD_getErrorName(ret);
                    break;
                }

                if (out_s.pos > 0) {
                    const uint64_t out_off = recryptor.offset();
                    if (!recryptor.process(out_buf.data(), out_s.pos)) {
                        error = "NCZ: no section covers stream offset";
                        break;
                    }
                    if (!write_cb(out_off, out_buf.data(), out_s.pos)) {
                        error = "NCZ: write callback failed (stream)";
                        break;
                    }
                }

                if (ret == 0) { frame_done = true; break; }  // frame complete
            }
        }

        ZSTD_freeDCtx(dctx);
        if (!error.empty()) return error;
    }

    return {};  // success — reconstructed NCA is byte-identical to the original
}


// Build a minimal "common" ticket for a titlekey-crypto NCA that has no .tik file.
// Atmosphere's ES accepts key_type=0 tickets with the raw (unencrypted) titlekey.
// This is the standard approach used by Tinfoil/DBI for NSZ without tickets.
//
// NOTE: this is orthogonal to the hash problem. Because we now reproduce the
// original NCA verbatim, a titlekey NCA keeps its rights_id, so HOS still needs
// a ticket to decrypt at runtime. If the NSP/NSZ container already carries a
// .tik, install that instead and skip this.
//
// Returns the ticket bytes, or empty on failure.
std::vector<uint8_t> NczDecompressor::make_common_ticket(
    const ReadFn& read_fn, uint64_t nca_size,
    const Core::Keys::Keyset& keys,
    const uint8_t* titlekey) {

    // Decrypt NCA header to get rights_id and key_generation.
    if (!keys.has_header_key || nca_size < 0xC00) return {};
    std::vector<uint8_t> enc(0xC00), dec(0xC00);
    if (!safe_read(read_fn, 0, enc.data(), 0xC00)) return {};
    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, keys.header_key.data(), keys.header_key.data() + 0x10, false);
    for (int s = 0; s < 6; ++s) {
        aes128XtsContextResetSector(&xts, s, true);
        aes128XtsDecrypt(&xts, dec.data() + s*0x200, enc.data() + s*0x200, 0x200);
    }
    if (memcmp(dec.data() + 0x200, "NCA3", 4) != 0 &&
        memcmp(dec.data() + 0x200, "NCA2", 4) != 0) return {};

    // Check rights_id — if zero, no ticket needed.
    bool has_rights = false;
    for (int i = 0; i < 0x10; ++i) if (dec[0x230+i]) { has_rights = true; break; }
    if (!has_rights) return {};

    uint8_t rights_id[0x10];
    memcpy(rights_id, dec.data() + 0x230, 0x10);
    // key generation == titlekek index (same offset-by-one rule as the key area).
    uint8_t key_gen = std::max(dec[0x206], dec[0x220]);
    if (key_gen > 0) key_gen--;
    if (key_gen >= Core::Keys::MAX_KEY_GENERATION || !keys.has_titlekek[key_gen])
        return {};

    // A COMMON ticket stores the titlekey ENCRYPTED with titlekek[key_gen]
    // (AES-128-ECB); ES decrypts it with the same titlekek at runtime. The NCZ
    // section key is the RAW titlekey, so encrypt it first.
    uint8_t enc_titlekey[0x10];
    {
        Aes128Context tk;
        aes128ContextCreate(&tk, keys.titlekek[key_gen].data(), true);
        aes128EncryptBlock(&tk, enc_titlekey, titlekey);
    }

    // Build a 0x2C0-byte common ticket.
    // Layout: sig_type(4) + RSA2048 sig(0x100) + padding(0x3C) + body(0x180).
    // Body-relative field offsets (see es::TicketData):
    //   0x000 issuer[0x40]        0x040 title_key_block[0x100]
    //   0x140 format_version      0x141 title_key_type    0x145 master_key_revision
    //   0x158 device_id[8]        0x160 rights_id[0x10]   0x170 account_id[4]
    std::vector<uint8_t> tik(0x2C0, 0);

    // Signature type: RSA-2048-SHA256 = 0x10004 (LE u32).
    tik[0] = 0x04; tik[1] = 0x00; tik[2] = 0x01; tik[3] = 0x00;
    // Dummy signature. (A fabricated ticket is unsigned either way; it only
    // imports on an ES with sig patches, where the value is irrelevant.)
    memset(tik.data() + 4, 0xFF, 0x100);

    uint8_t* body = tik.data() + 0x140;

    static const char issuer[] = "Root-CA00000003-XS00000020";
    memcpy(body + 0x00, issuer, strlen(issuer));

    memcpy(body + 0x40, enc_titlekey, 0x10);  // titlekek-encrypted titlekey

    body[0x140] = 2;         // format_version = 2
    body[0x141] = 0;         // title_key_type = 0 (Common)
    body[0x145] = key_gen;   // master_key_revision

    memcpy(body + 0x160, rights_id, 0x10);    // rights_id (correct offset)

    return tik;
}

#else // !PLATFORM_SWITCH

bool NczDecompressor::is_ncz(const ReadFn&, uint64_t) { return false; }

uint64_t NczDecompressor::get_decompressed_size(const ReadFn&, uint64_t,
                                                  const Core::Keys::Keyset&) { return 0; }
std::vector<uint8_t> NczDecompressor::make_common_ticket(const ReadFn&, uint64_t,
                                                           const Core::Keys::Keyset&,
                                                           const uint8_t*) { return {}; }

std::string NczDecompressor::decompress(const ReadFn&, uint64_t,
                                         const Core::Keys::Keyset&, WriteCallback) {
    return "NCZ: not supported on this platform";
}

#endif // PLATFORM_SWITCH

} // namespace Install
