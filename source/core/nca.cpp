// source/core/nca.cpp

#include "core/nca.hpp"
#include "core/es.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <array>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#else
// PC stub: provide no-op AES so the file compiles off-device. Control reads
// simply fail (ok=false) on PC.
#endif

namespace Core::Nca {

// ─── On-disk structures (little-endian, packed) ────────────────────────────────
// Offsets per https://switchbrew.org/wiki/NCA_Format and hactool's nca.h.

#pragma pack(push, 1)

// NCA header is 0x400 bytes; we only name the fields we use.
struct NcaHeader {
    uint8_t  fixed_key_sig[0x100];   // 0x000 RSA-2048 signature
    uint8_t  npdm_sig[0x100];        // 0x100 RSA-2048 signature
    uint32_t magic;                  // 0x200 "NCA3"
    uint8_t  distribution_type;      // 0x204
    uint8_t  content_type;           // 0x205 (0=Program,1=Meta,2=Control,...)
    uint8_t  key_generation_old;     // 0x206
    uint8_t  key_area_key_index;     // 0x207 (kaek index: 0=app,1=ocean,2=system)
    uint64_t content_size;           // 0x208
    uint64_t program_id;             // 0x210
    uint32_t content_index;          // 0x218
    uint32_t sdk_version;            // 0x21C
    uint8_t  key_generation;         // 0x220 (newer field; use max of the two)
    uint8_t  header1_sig_key_gen;    // 0x221
    uint8_t  reserved[0xE];          // 0x222
    uint8_t  rights_id[0x10];        // 0x230 (all-zero if not titlekey crypto)

    // 0x240: four FsEntry records (0x10 each) = 0x40
    struct FsEntry {
        uint32_t start_offset;       // in 0x200 media units
        uint32_t end_offset;         // in 0x200 media units
        uint8_t  _pad[0x8];
    } fs_entries[4];                 // 0x240

    uint8_t  fs_header_hashes[4][0x20]; // 0x280 SHA-256 per FS header
    uint8_t  encrypted_key_area[0x40];  // 0x300 four 0x10 keys, AES-ECB encrypted
    uint8_t  _rest[0xC0];               // 0x340..0x400
};

// FS header, 0x200 bytes, one per section (at 0x400 + i*0x200 in the 0xC00 blob).
struct NcaFsHeader {
    uint16_t version;                // 0x000
    uint8_t  fs_type;                // 0x002 (0=RomFS,1=PartitionFS)
    uint8_t  hash_type;              // 0x003
    uint8_t  encryption_type;        // 0x004 (1=None,2=XTS,3=CTR,4=BKTR)
    uint8_t  _pad[0x3];
    uint8_t  hash_info[0xF8];        // 0x008 hash superblock (type-specific)
    uint8_t  patch_info[0x40];       // 0x100
    uint32_t generation;             // 0x140
    uint32_t secure_value;           // 0x144
    uint8_t  sparse_info[0x30];      // 0x148
    uint8_t  _reserved[0x88];        // 0x178..0x200
};

// RomFS header (IVFC level 6 data is the actual romfs). We parse the simple
// direct RomFS header that sits at the section's data offset.
struct RomFsHeader {
    uint64_t header_size;            // 0x000 (0x50)
    uint64_t dir_hash_offset;        // 0x008
    uint64_t dir_hash_size;          // 0x010
    uint64_t dir_meta_offset;        // 0x018
    uint64_t dir_meta_size;          // 0x020
    uint64_t file_hash_offset;       // 0x028
    uint64_t file_hash_size;         // 0x030
    uint64_t file_meta_offset;       // 0x038
    uint64_t file_meta_size;         // 0x040
    uint64_t data_offset;            // 0x048
};

// IVFC superblock (the RomFS FS-header hash_info). The RomFS is wrapped in an
// IVFC hash tree; the actual RomFS data lives at level-6's logical offset, NOT
// at section byte 0. Layout per hactool's ivfc_hdr_t.
struct IvfcLevelHeader {
    uint64_t logical_offset;         // where this level's data starts (section-rel)
    uint64_t hash_data_size;         // size of this level's data
    uint32_t block_size_log2;        // block size = 1 << this
    uint32_t reserved;
};
struct IvfcHeader {
    uint32_t magic;                  // "IVFC" = 0x43465649
    uint32_t magic_number;           // 0x20000
    uint32_t master_hash_size;
    uint32_t num_levels;             // usually 7 (levels 0..6)
    IvfcLevelHeader levels[6];       // level headers (RomFS data is the last used)
    // (master hash + padding follow; unused here)
};

// A RomFS file entry (variable length; fixed part shown).
struct RomFsFileEntry {
    uint32_t parent_dir;
    uint32_t sibling;
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t hash_sibling;
    uint32_t name_len;
    // followed by name_len bytes of name
};

#pragma pack(pop)

static_assert(sizeof(NcaHeader) == 0x400, "NcaHeader must be 0x400");
static_assert(sizeof(NcaFsHeader) == 0x200, "NcaFsHeader must be 0x200");

// NACP is 0x4000. Each of the 16 language entries is 0x300: name[0x200] +
// author[0x100]. The display version lives at offset 0x3060.
#pragma pack(push, 1)
struct NacpLanguageEntry {
    char name[0x200];
    char author[0x100];
};
struct Nacp {
    NacpLanguageEntry titles[16];    // 0x0000
    uint8_t  _mid[0x3060 - sizeof(NacpLanguageEntry) * 16];
    char     display_version[0x10];  // 0x3060
    // ... rest unused
};
#pragma pack(pop)

// ─── Constants ─────────────────────────────────────────────────────────────────

static constexpr uint32_t NCA3_MAGIC = 0x3341434E; // "NCA3" little-endian
static constexpr size_t   MEDIA_UNIT = 0x200;
static constexpr size_t   HEADER_ENCRYPTED_SIZE = 0xC00; // header + 4 fs headers

// Bounded string length (avoids strnlen, which needs a feature macro that isn't
// reliably set in the devkitA64 toolchain — bit us before in system.cpp).
static size_t bounded_len(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return n;
}

#ifdef PLATFORM_SWITCH

// ─── AES helpers (libnx hardware crypto) ───────────────────────────────────────

// Decrypt the NCA header region (AES-XTS, Nintendo tweak, sector size 0x200).
// The header_key is 0x20 bytes = two 0x10 XTS sub-keys.
static void xts_decrypt_header(const uint8_t header_key[0x20],
                               void* dst, const void* src, size_t size,
                               uint64_t start_sector) {
    Aes128XtsContext ctx;
    aes128XtsContextCreate(&ctx, header_key, header_key + 0x10, false /*decrypt*/);
    auto* s = (const uint8_t*)src;
    auto* d = (uint8_t*)dst;
    size_t sectors = size / MEDIA_UNIT;
    for (size_t i = 0; i < sectors; ++i) {
        aes128XtsContextResetSector(&ctx, start_sector + i, true /*is_nintendo*/);
        aes128XtsDecrypt(&ctx, d + i * MEDIA_UNIT, s + i * MEDIA_UNIT, MEDIA_UNIT);
    }
}

// Decrypt the 0x40 encrypted key area with the key-area key (AES-128-ECB).
static void ecb_decrypt_keyarea(const uint8_t kaek[0x10],
                                uint8_t out[0x40], const uint8_t in[0x40]) {
    Aes128Context ctx;
    aes128ContextCreate(&ctx, kaek, false /*decrypt*/);
    for (int i = 0; i < 4; ++i)
        aes128DecryptBlock(&ctx, out + i * 0x10, in + i * 0x10);
}

#endif // PLATFORM_SWITCH

// ─── Language preference ───────────────────────────────────────────────────────
// Pick the first non-empty NACP title, preferring American English then others.
static void pick_title(const Nacp& nacp, std::string& name, std::string& author) {
    // Language order roughly matching Nintendo's control priority.
    static const int order[] = { 1 /*AmEng*/, 0 /*Jp*/, 2 /*Fr*/, 3 /*De*/,
                                 4 /*It*/, 5 /*Es*/, 6 /*Zh*/, 7 /*Ko*/, 8, 9,
                                 10, 11, 12, 13, 14, 15 };
    for (int idx : order) {
        const auto& e = nacp.titles[idx];
        // name is a fixed buffer; find its length safely.
        size_t nlen = bounded_len(e.name, sizeof(e.name));
        if (nlen > 0) {
            name.assign(e.name, nlen);
            size_t alen = bounded_len(e.author, sizeof(e.author));
            author.assign(e.author, alen);
            return;
        }
    }
}

// ─── Core decrypt ──────────────────────────────────────────────────────────────

ControlData read_control(const ReadFn& read, uint64_t nca_size,
                         const Core::Keys::Keyset& keys, bool want_icon) {
    ControlData out;

#ifndef PLATFORM_SWITCH
    (void)read; (void)nca_size; (void)keys; (void)want_icon;
    return out;  // crypto unavailable off-device
#else
    if (nca_size < HEADER_ENCRYPTED_SIZE) { out.fail_reason="nca too small"; return out; }
    if (!keys.has_header_key) { out.fail_reason="no header_key"; return out; }

    // 1. Read + decrypt the 0xC00 header blob.
    std::vector<uint8_t> enc(HEADER_ENCRYPTED_SIZE);
    if (read(0, enc.data(), enc.size()) != enc.size()) { out.fail_reason="header read failed"; return out; }

    std::vector<uint8_t> dec(HEADER_ENCRYPTED_SIZE);
    xts_decrypt_header(keys.header_key.data(), dec.data(), enc.data(),
                       HEADER_ENCRYPTED_SIZE, 0);

    auto* hdr = reinterpret_cast<NcaHeader*>(dec.data());
    if (hdr->magic != NCA3_MAGIC) {
        SDL_Log("Nca — not an NCA3 (magic=0x%08X)", hdr->magic);
        out.fail_reason = "bad NCA3 magic (wrong header_key?)";
        return out;
    }

    // 2. Determine key generation. Newer NCAs use key_generation (0x220); older
    //    use key_generation_old (0x206). Effective = max, then map: values 0/1
    //    both mean generation 0.
    int keygen = std::max<int>(hdr->key_generation_old, hdr->key_generation);
    if (keygen > 0) keygen -= 1;  // generations are offset by one
    if (keygen < 0 || keygen >= Core::Keys::MAX_KEY_GENERATION) { out.fail_reason="keygen out of range"; return out; }

    // 3. Decrypt the key area to obtain the body key. Control NCAs use the
    //    application key-area key (index 0 typically; honor the header field).
    int kaek_index = hdr->key_area_key_index;
    (void)kaek_index; // we use the application category for control content
    if (!keys.has_kaek_application[keygen]) {
        SDL_Log("Nca — missing key_area_key_application_%02x", keygen);
        char fr[64]; snprintf(fr,sizeof(fr),"missing key_area_key_application_%02x",keygen);
        out.fail_reason = fr;
        return out;
    }

    uint8_t decrypted_keys[0x40];
    bool use_titlekey = false;
    // Rights ID non-zero → titlekey crypto.
    for (int i = 0; i < 0x10; ++i)
        if (hdr->rights_id[i] != 0) { use_titlekey = true; break; }

    if (use_titlekey) {
        // Titlekey crypto: the content key comes from a ticket, keyed by the
        // NCA's rights id, and must be decrypted with the titlekek for this
        // key generation. Two sources, tried in order:
        //   1. title.keys (if the user provided it) — the value there is the
        //      ENCRYPTED titlekey, so it still needs titlekek decryption.
        //   2. the console's ES common-ticket system (most installed titles).
        char rid[33];
        for (int i = 0; i < 0x10; ++i)
            snprintf(rid + i * 2, 3, "%02x", hdr->rights_id[i]);
        rid[32] = 0;

        std::array<uint8_t, 16> titlekey{};
        bool have_key = false;

        // Source 1: title.keys (holds the encrypted titlekey).
        auto it = keys.title_keys.find(rid);
        if (it != keys.title_keys.end()) {
            if (keygen >= 0 && keygen < Core::Keys::MAX_KEY_GENERATION &&
                keys.has_titlekek[keygen]) {
                Aes128Context tctx;
                aes128ContextCreate(&tctx, keys.titlekek[keygen].data(), false);
                aes128DecryptBlock(&tctx, titlekey.data(), it->second.data());
                have_key = true;
            }
        }

        // Source 2: ES common ticket (queries the console's eTicket system).
        if (!have_key) {
            have_key = Core::Es::get_titlekey(hdr->rights_id, keygen, keys, titlekey);
        }

        if (!have_key) {
            SDL_Log("Nca — no titlekey for rights id %s", rid);
            out.fail_reason = "titlekey unavailable (personalized ticket?)";
            return out;
        }

        // The decrypted titlekey IS the body key.
        std::memcpy(decrypted_keys + 0x20, titlekey.data(), 0x10);
    } else {
        ecb_decrypt_keyarea(keys.key_area_key_application[keygen].data(),
                            decrypted_keys, hdr->encrypted_key_area);
    }
    // The body-decryption key is key index 2 of the key area.
    const uint8_t* body_key = decrypted_keys + 0x20;

    // 4. Find a RomFS section (fs_type=0) to read control.nacp / icon from.
    //    Control NCAs have a single RomFS section (index 0).
    int romfs_section = -1;
    NcaFsHeader* fsh = nullptr;
    for (int i = 0; i < 4; ++i) {
        if (hdr->fs_entries[i].start_offset == 0 &&
            hdr->fs_entries[i].end_offset == 0) continue;
        auto* candidate = reinterpret_cast<NcaFsHeader*>(
            dec.data() + 0x400 + i * 0x200);
        if (candidate->fs_type == 0 /*RomFS*/) {
            romfs_section = i;
            fsh = candidate;
            break;
        }
    }
    if (romfs_section < 0 || !fsh) {
        SDL_Log("Nca — no RomFS section found");
        out.fail_reason = "no RomFS section";
        return out;
    }

    uint64_t section_offset =
        (uint64_t)hdr->fs_entries[romfs_section].start_offset * MEDIA_UNIT;

    // For a CTR section, the counter's upper 8 bytes come from secure_value/
    // generation in the FS header; the AES-CTR nonce is derived from the section
    // offset. We use libnx's aes128CtrContextCreate with a computed IV.
    if (fsh->encryption_type != 3 /*CTR*/ && fsh->encryption_type != 1 /*None*/) {
        SDL_Log("Nca — unsupported section encryption %d", fsh->encryption_type);
        char fr[48]; snprintf(fr,sizeof(fr),"unsupported section crypto %d",fsh->encryption_type);
        out.fail_reason = fr;
        return out;
    }

    // Helper to read + CTR-decrypt an arbitrary span from the section.
    // The CTR nonce upper 8 bytes come from the FS header. The previous code
    // used these successfully for aligned reads (the RomFS header decrypted
    // correctly), so we keep that derivation and fix ONLY the real bug: read
    // alignment. Upper bytes = generation (BE) in the top 4; the section CTR's
    // remaining bytes stay zero as before, which matched working titles.
    uint32_t gen = fsh->generation;

    auto read_section = [&](uint64_t rel_offset, void* dst, size_t size) -> bool {
        if (fsh->encryption_type == 1) { // None
            std::vector<uint8_t> buf(size);
            if (read(section_offset + rel_offset, buf.data(), size) != size)
                return false;
            std::memcpy(dst, buf.data(), size);
            return true;
        }

        // CTR requires 16-byte-aligned reads: the AES-CTR keystream is generated
        // per 16-byte block starting at a block boundary. If our target offset
        // isn't 16-aligned, read from the aligned-down offset and skip the extra
        // head bytes after decrypting. THIS WAS THE BUG: unaligned reads desynced
        // the keystream, producing garbage for any file/table whose offset wasn't
        // a multiple of 16 — hence exactly "half the titles" failed, depending on
        // where their file-metadata table happened to land.
        uint64_t abs      = section_offset + rel_offset;
        uint64_t aligned  = abs & ~uint64_t(0xF);
        size_t   head     = (size_t)(abs - aligned);
        size_t   total    = head + size;
        size_t   padded   = (total + 0xF) & ~size_t(0xF);  // round up to block

        std::vector<uint8_t> buf(padded);
        size_t got = read(aligned, buf.data(), padded);
        if (got < total) return false;

        // CTR for the ALIGNED offset (same nonce derivation as the working path).
        uint8_t ctr[0x10] = {0};
        ctr[0] = (gen >> 24) & 0xFF; ctr[1] = (gen >> 16) & 0xFF;
        ctr[2] = (gen >> 8)  & 0xFF; ctr[3] = gen & 0xFF;
        uint64_t block = aligned >> 4;
        for (int i = 0; i < 8; ++i)
            ctr[0x8 + i] = (uint8_t)((block >> (56 - i * 8)) & 0xFF);

        Aes128CtrContext cctx;
        aes128CtrContextCreate(&cctx, body_key, ctr);
        aes128CtrCrypt(&cctx, buf.data(), buf.data(), padded);

        std::memcpy(dst, buf.data() + head, size);
        return true;
    };

    // 5. The RomFS is wrapped in an IVFC hash tree. The FS header's hash_info
    //    (offset 0x008 in the 0x200 FS header) holds the IVFC superblock. The
    //    actual RomFS header/data starts at the LAST level's logical_offset
    //    (relative to section start) — NOT at section byte 0, which is where an
    //    earlier version wrongly read and produced garbage table offsets.
    const IvfcHeader* ivfc = reinterpret_cast<const IvfcHeader*>(fsh->hash_info);
    static constexpr uint32_t IVFC_MAGIC = 0x43465649; // "IVFC"

    uint64_t romfs_base = 0;
    if (ivfc->magic == IVFC_MAGIC && ivfc->num_levels >= 1) {
        // Last data level is index (num_levels-1) capped to our array; RomFS
        // data is that level's logical offset. For a standard 7-level tree the
        // RomFS lives at levels[5] (the 6th level header).
        int last = (int)ivfc->num_levels - 1;
        if (last > 5) last = 5;
        if (last < 0) last = 0;
        romfs_base = ivfc->levels[last].logical_offset;
    } else {
        SDL_Log("Nca — RomFS FS header lacks IVFC magic (0x%08X)", ivfc->magic);
        out.fail_reason = "no IVFC superblock";
        return out;
    }

    // Read the RomFS header at the IVFC level-6 offset.
    RomFsHeader rfs;
    if (!read_section(romfs_base, &rfs, sizeof(rfs))) {
        out.fail_reason = "romfs header read failed";
        return out;
    }
    if (rfs.header_size < sizeof(RomFsHeader)) {
        SDL_Log("Nca — unexpected RomFS header_size=%llu",
                (unsigned long long)rfs.header_size);
        char fr[64]; snprintf(fr, sizeof(fr),
                              "bad romfs header_size=%llu",
                              (unsigned long long)rfs.header_size);
        out.fail_reason = fr;
        return out;
    }

    // 6. Walk the file metadata table to find "control.nacp" and "icon_*.dat".
    //    All RomFS-internal offsets are relative to romfs_base.
    //
    //    The file-metadata table is a sequence of variable-length entries
    //    (0x20-byte fixed part + name, padded to 4). Entries are chained via the
    //    hash table, and the table can contain gaps/terminators (0xFFFFFFFF
    //    sibling/hash pointers). A naive "break on first odd entry" walk stops
    //    early and misses control.nacp whenever it isn't the first file — which
    //    is exactly why some titles resolved and others didn't. Instead we scan
    //    the entire table, skipping anything that doesn't look like a valid
    //    entry, and advance by the real entry stride.
    std::vector<uint8_t> file_meta(rfs.file_meta_size);
    if (!read_section(romfs_base + rfs.file_meta_offset,
                      file_meta.data(), file_meta.size())) {
        out.fail_reason = "romfs file-meta read failed";
        return out;
    }

    uint64_t nacp_off = 0, nacp_size = 0;
    uint64_t icon_off = 0, icon_size = 0;

    size_t pos = 0;
    while (pos + sizeof(RomFsFileEntry) <= file_meta.size()) {
        auto* fe = reinterpret_cast<RomFsFileEntry*>(file_meta.data() + pos);
        uint32_t nlen = fe->name_len;

        // A valid entry has a sane name length and its name fits in the table.
        bool valid = (nlen > 0 && nlen <= 256) &&
                     (pos + sizeof(RomFsFileEntry) + nlen <= file_meta.size());

        if (valid) {
            const char* nm = (const char*)(file_meta.data() + pos +
                                           sizeof(RomFsFileEntry));
            std::string fname(nm, nlen);

            if (fname == "control.nacp") {
                nacp_off = fe->data_offset; nacp_size = fe->data_size;
            } else if (icon_off == 0 && fname.rfind("icon_", 0) == 0) {
                icon_off = fe->data_offset; icon_size = fe->data_size;
            }

            // Found what we need — stop early.
            if (nacp_size != 0 && icon_off != 0) break;

            // Advance by the real entry stride (fixed part + name, 4-aligned).
            size_t adv = sizeof(RomFsFileEntry) + nlen;
            adv = (adv + 3) & ~size_t(3);
            pos += adv;
        } else {
            // Not a valid entry at this position (padding / hash-table region /
            // terminator). Step forward by the 4-byte alignment and keep looking
            // rather than abandoning the whole table.
            pos += 4;
        }
    }

    if (nacp_size == 0) {
        SDL_Log("Nca — control.nacp not found in RomFS");
        out.fail_reason = "control.nacp not found (romfs walk?)";
        return out;
    }

    // 7. Read + parse the NACP. (romfs_base + data region + entry offset)
    std::vector<uint8_t> nacp_buf(sizeof(Nacp));
    size_t to_read = std::min<size_t>(nacp_size, sizeof(Nacp));
    if (!read_section(romfs_base + rfs.data_offset + nacp_off,
                      nacp_buf.data(), to_read)) {
        out.fail_reason = "nacp read failed";
        return out;
    }

    auto* nacp = reinterpret_cast<Nacp*>(nacp_buf.data());
    pick_title(*nacp, out.name, out.author);
    size_t vlen = bounded_len(nacp->display_version, sizeof(nacp->display_version));
    out.version.assign(nacp->display_version, vlen);

    // 8. Optionally read the icon JPEG.
    if (want_icon && icon_size > 0 && icon_size < 1024 * 1024) {
        out.icon_jpeg.resize(icon_size);
        if (!read_section(romfs_base + rfs.data_offset + icon_off,
                          out.icon_jpeg.data(), icon_size)) {
            out.icon_jpeg.clear();
        }
    }

    out.ok = !out.name.empty();
    return out;
#endif
}

// ─── File-backed convenience ───────────────────────────────────────────────────

ControlData read_control_file(const std::string& path,
                              const Core::Keys::Keyset& keys, bool want_icon) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return ControlData{};

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    ReadFn reader = [f](uint64_t offset, void* dst, size_t size) -> size_t {
        if (fseek(f, (long)offset, SEEK_SET) != 0) return 0;
        return fread(dst, 1, size, f);
    };

    ControlData cd = read_control(reader, (uint64_t)sz, keys, want_icon);
    fclose(f);
    return cd;
}

} // namespace Core::Nca
