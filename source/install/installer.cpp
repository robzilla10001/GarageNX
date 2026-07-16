// source/install/installer.cpp
// NSP/XCI installation pipeline.
//
// NCM install pipeline mirrors move_title (M4, hardware-validated):
//   GeneratePlaceHolderId → CreatePlaceHolder → WritePlaceHolder (streamed) →
//   Register → ncmContentMetaDatabaseSet → ncmContentMetaDatabaseCommit
//
// No PushApplicationRecord call: the meta-DB write alone is sufficient for HOS
// to enumerate and launch the installed title, confirmed by M4 move testing.
// (NS records are auto-populated when NCM has content + a committed meta entry.)
//
// CNMT NCA decryption: the CNMT is inside a .cnmt.nca file in the container.
// We decrypt the NCA header (AES-XTS, header_key) to find the section offset,
// decrypt the section (AES-CTR), then parse the raw CNMT structure to build the
// NcmContentMetaKey and content list. This tells us each NCA's content_id, type,
// and size — which we need for placeholder creation and meta-DB registration.
//
// Ticket install: if a .tik is present, we call ES ImportTicket (cmd 1). This
// is required for titlekey-crypto titles to be launchable. We use a hand-rolled
// IPC dispatch (same pattern as es.cpp: raw serviceDispatch on the "es" service;
// NO NX_SERVICE_ASSUME_NON_DOMAIN because ES is a domain service, unlike ns).

#include "install/installer.hpp"
#include "install/ncz.hpp"
#include "core/nca.hpp"
#include "core/keys.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <string>
#include <algorithm>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Install {

#ifdef PLATFORM_SWITCH

// ── CNMT structures (raw, packed, little-endian) ─────────────────────────────
// Reference: switchbrew.org/wiki/CNMT

#pragma pack(push, 1)
struct CnmtHeader {
    uint64_t title_id;
    uint32_t version;
    uint8_t  meta_type;
    uint8_t  field_D;
    uint16_t extended_header_size;
    uint16_t content_count;
    uint16_t content_meta_count;
    uint8_t  attributes;
    uint8_t  storage_id;
    uint8_t  content_install_type;
    uint8_t  padding;
    uint8_t  reserved[8];   // 8 extra bytes present on real Switch CNMTs
};
// Total: 8+4+1+1+2+2+2+1+1+1+1+8 = 32 = 0x20
// The records start at 0x20 + extended_header_size.

struct CnmtContentRecord {
    uint8_t  hash[0x20];       // SHA-256 of the NCA (also the content_id)
    uint8_t  content_id[0x10];
    uint8_t  size_le[6];       // u48 LE — use a helper to decode
    uint8_t  content_type;     // NcmContentType values
    uint8_t  id_offset;
};
#pragma pack(pop)

static_assert(sizeof(CnmtHeader) == 0x20, "CnmtHeader size");
static_assert(sizeof(CnmtContentRecord) == 0x38, "CnmtContentRecord size");

static uint64_t u48_le(const uint8_t b[6]) {
    return (uint64_t)b[0]
         | (uint64_t)b[1] << 8
         | (uint64_t)b[2] << 16
         | (uint64_t)b[3] << 24
         | (uint64_t)b[4] << 32
         | (uint64_t)b[5] << 40;
}

// ── CNMT parsing ─────────────────────────────────────────────────────────────

struct ParsedCnmt {
    NcmContentMetaKey           key{};
    std::vector<NcmContentInfo> infos;      // for progress counting only
    std::vector<uint8_t>        raw_bytes;  // raw CNMT binary → passed to meta-DB Set
    std::string                 cnmt_nca_name; // filename of the .cnmt.nca (content_id hex)
    uint64_t                    cnmt_nca_size = 0; // file size of the .cnmt.nca
    bool ok = false;
};

// Read the CNMT section from a decrypted NCA section blob.
// The CNMT is the first file in the plain section data (no inner filesystem
// for Meta NCAs — the section data IS the CNMT directly, starting at offset 0).
static ParsedCnmt parse_cnmt_bytes(const uint8_t* data, size_t len) {
    ParsedCnmt out;
    if (len < sizeof(CnmtHeader)) return out;

    const CnmtHeader* hdr = reinterpret_cast<const CnmtHeader*>(data);

    out.key.id           = hdr->title_id;
    out.key.version      = hdr->version;
    out.key.type         = (NcmContentMetaType)hdr->meta_type;
    out.key.install_type = NcmContentInstallType_Full;

    // Store the raw CNMT bytes — passed directly to ncmContentMetaDatabaseSet.
    // This is the PackagedContentMeta format NCM expects; we never reconstruct it.
    out.raw_bytes.assign(data, data + len);

    // Also parse content records for progress counting (infos.size() = NCA count).
    size_t ext_size    = hdr->extended_header_size;
    size_t records_off = sizeof(CnmtHeader) + ext_size;

    for (uint16_t i = 0; i < hdr->content_count; ++i) {
        size_t rec_off = records_off + i * sizeof(CnmtContentRecord);
        if (rec_off + sizeof(CnmtContentRecord) > len) break;
        const CnmtContentRecord* rec =
            reinterpret_cast<const CnmtContentRecord*>(data + rec_off);
        NcmContentInfo info{};
        std::memcpy(info.content_id.c, rec->content_id, 0x10);
        {
            uint64_t sz = u48_le(rec->size_le);
            uint8_t size_bytes[6];
            for (int b = 0; b < 6; ++b)
                size_bytes[b] = (uint8_t)((sz >> (b * 8)) & 0xFF);
            std::memcpy(reinterpret_cast<uint8_t*>(&info) + 0x10, size_bytes, 6);
        }
        info.attr         = 0;
        info.content_type = (NcmContentType)rec->content_type;
        info.id_offset    = rec->id_offset;
        out.infos.push_back(info);
    }

    out.ok = true;
    SDL_Log("Installer: CNMT parsed — title_id=%016llX version=%u type=0x%02X count=%u raw=%zu",
            (unsigned long long)out.key.id, out.key.version,
            (unsigned)out.key.type, (unsigned)out.infos.size(), len);
    return out;
}

// Decrypt a CNMT NCA (header AES-XTS + section AES-CTR) and parse the CNMT.
// Validated layout (from hardware diagnostics):
//   - NcaFsEntry at 0x240: u32 start_block, u32 end_block (media units = 0x200 bytes each)
//   - NcaFsHeader at 0x400: generation at +0x140, hash_info at +0x008
//   - hash_info.data_offset (+0x038): section-relative offset to the PFS0 wrapper
//   - Inside the PFS0: one file which is the raw CNMT binary
//   - key_gen = max(dec_hdr[0x206], dec_hdr[0x220]); if > 0, subtract 1 (offset-by-one)
static ParsedCnmt decrypt_and_parse_cnmt(const ReadFn& read_fn, uint64_t nca_size,
                                          const Core::Keys::Keyset& keys) {
    ParsedCnmt fail;
    if (!keys.has_header_key) return fail;

    // ── Read + decrypt NCA header (AES-XTS) ──────────────────────────────────
    static constexpr size_t HDR_SIZE = 0xC00;
    if (nca_size < HDR_SIZE) return fail;

    std::vector<uint8_t> enc_hdr(HDR_SIZE), dec_hdr(HDR_SIZE);
    if (read_fn(0, enc_hdr.data(), HDR_SIZE) != HDR_SIZE) return fail;

    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, keys.header_key.data(), keys.header_key.data() + 0x10, false);
    for (size_t s = 0; s < HDR_SIZE / 0x200; ++s) {
        aes128XtsContextResetSector(&xts, s, true);
        aes128XtsDecrypt(&xts, dec_hdr.data() + s*0x200, enc_hdr.data() + s*0x200, 0x200);
    }

    if (std::memcmp(dec_hdr.data() + 0x200, "NCA3", 4) != 0 &&
        std::memcmp(dec_hdr.data() + 0x200, "NCA2", 4) != 0) {
        SDL_Log("Installer: bad NCA magic after header decrypt");
        return fail;
    }

    // ── Section 0 location ────────────────────────────────────────────────────
    static constexpr size_t SECTION_ENTRY_BASE = 0x240;
    static constexpr uint64_t MEDIA_UNIT = 0x200;
    uint32_t start_block = 0, end_block = 0;
    std::memcpy(&start_block, dec_hdr.data() + SECTION_ENTRY_BASE + 0x00, 4);
    std::memcpy(&end_block,   dec_hdr.data() + SECTION_ENTRY_BASE + 0x04, 4);
    if (start_block == 0 && end_block == 0) { SDL_Log("Installer: section 0 empty"); return fail; }

    uint64_t section_file_off = (uint64_t)start_block * MEDIA_UNIT;
    uint64_t section_size     = (uint64_t)(end_block - start_block) * MEDIA_UNIT;

    // ── FS header: generation at NcaFsHeader+0x140 ───────────────────────────
    static constexpr size_t FS_HDR_BASE = 0x400;
    uint32_t generation = 0;
    std::memcpy(&generation, dec_hdr.data() + FS_HDR_BASE + 0x140, 4);

    // ── Key generation and key-area decrypt ──────────────────────────────────
    uint8_t key_gen_old = dec_hdr[0x206];
    uint8_t key_gen_new = dec_hdr[0x220];
    int key_gen = std::max<int>(key_gen_old, key_gen_new);
    if (key_gen > 0) key_gen -= 1;
    if (key_gen < 0 || key_gen >= Core::Keys::MAX_KEY_GENERATION) key_gen = 0;

    if (!keys.has_kaek_application[key_gen]) {
        SDL_Log("Installer: missing key_area_key_application_%02x", key_gen);
        return fail;
    }

    uint8_t enc_keys[0x40];
    std::memcpy(enc_keys, dec_hdr.data() + 0x300, 0x40);

    Aes128Context kak_ctx;
    aes128ContextCreate(&kak_ctx, keys.key_area_key_application[key_gen].data(), false);
    uint8_t decrypted_keys[0x40];
    for (int k = 0; k < 4; ++k)
        aes128DecryptBlock(&kak_ctx, decrypted_keys + k*0x10, enc_keys + k*0x10);
    const uint8_t* ctr_key = decrypted_keys + 0x20;

    // ── Read + AES-128-CTR decrypt section ────────────────────────────────────
    size_t read_size = (size_t)std::min<uint64_t>(section_size, 128 * 1024);
    std::vector<uint8_t> enc_sec(read_size), dec_sec(read_size);

    if (read_fn(section_file_off, enc_sec.data(), read_size) != read_size) {
        SDL_Log("Installer: cannot read CNMT section");
        return fail;
    }

    {
        uint64_t block = section_file_off / 0x10;
        uint8_t ctr[0x10] = {0};
        ctr[0] = (generation >> 24) & 0xFF; ctr[1] = (generation >> 16) & 0xFF;
        ctr[2] = (generation >>  8) & 0xFF; ctr[3] =  generation        & 0xFF;
        for (int i = 0; i < 8; ++i)
            ctr[0x8 + i] = (uint8_t)((block >> (56 - i * 8)) & 0xFF);
        Aes128CtrContext ctr_ctx;
        aes128CtrContextCreate(&ctr_ctx, ctr_key, ctr);
        aes128CtrCrypt(&ctr_ctx, dec_sec.data(), enc_sec.data(), read_size);
    }

    // ── Locate PFS0 via hash_info.data_offset (NcaFsHeader+0x008+0x038) ──────
    // The section starts with a SHA-256 hash table; actual data follows at data_offset.
    uint64_t hash_info_data_offset = 0;
    std::memcpy(&hash_info_data_offset, dec_hdr.data() + FS_HDR_BASE + 0x008 + 0x038, 8);

    size_t pfs0_start = (size_t)hash_info_data_offset;
    if (pfs0_start >= read_size) pfs0_start = 0;

    const uint8_t* pfs0 = dec_sec.data() + pfs0_start;
    size_t pfs0_avail   = read_size - pfs0_start;

    // ── Parse PFS0 → CNMT binary ─────────────────────────────────────────────
    if (pfs0_avail < 0x28) { SDL_Log("Installer: PFS0 too small"); return fail; }
    if (std::memcmp(pfs0, "PFS0", 4) != 0) {
        SDL_Log("Installer: expected PFS0, got %02x%02x%02x%02x",
                pfs0[0], pfs0[1], pfs0[2], pfs0[3]);
        return fail;
    }

    uint32_t pfs0_file_count = 0, pfs0_strtab_size = 0;
    std::memcpy(&pfs0_file_count,  pfs0 + 0x04, 4);
    std::memcpy(&pfs0_strtab_size, pfs0 + 0x08, 4);
    if (pfs0_file_count == 0 || pfs0_file_count > 64) return fail;

    uint64_t entry_data_off = 0, entry_data_size = 0;
    std::memcpy(&entry_data_off,  pfs0 + 0x10 + 0x00, 8);
    std::memcpy(&entry_data_size, pfs0 + 0x10 + 0x08, 8);

    size_t data_region = 0x10 + pfs0_file_count * 0x18 + pfs0_strtab_size;
    size_t cnmt_off    = data_region + (size_t)entry_data_off;
    size_t cnmt_size   = (size_t)entry_data_size;

    if (cnmt_off >= pfs0_avail || cnmt_size == 0 || cnmt_off + cnmt_size > pfs0_avail) {
        SDL_Log("Installer: CNMT offset 0x%zx+0x%zx outside PFS0 data (0x%zx)",
                cnmt_off, cnmt_size, pfs0_avail);
        return fail;
    }

    return parse_cnmt_bytes(pfs0 + cnmt_off, cnmt_size);
}

// ── Content ID from hex name ──────────────────────────────────────────────────

bool content_id_from_name(const std::string& name, NcmContentId& out) {
    // NCA names are "<32 hex chars>.nca" or "<32 hex chars>.cnmt.nca".
    if (name.size() < 32) return false;
    for (int i = 0; i < 0x10; ++i) {
        unsigned hi, lo;
        if (std::sscanf(name.c_str() + i*2, "%1x%1x", &hi, &lo) != 2) return false;
        out.c[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

// ── ES ticket import ──────────────────────────────────────────────────────────
// ES is a domain service — do NOT use NX_SERVICE_ASSUME_NON_DOMAIN.
// Cmd 1: ImportTicket(ticket_buf, cert_buf) — no return value.
static Result es_import_ticket(Service* es, const void* tik, size_t tik_size,
                                const void* cert, size_t cert_size) {
    return serviceDispatch(es, 1,
        .buffer_attrs = {
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
            SfBufferAttr_HipcMapAlias | SfBufferAttr_In,
        },
        .buffers = {
            { tik,  tik_size  },
            { cert, cert_size },
        },
    );
}

// A minimal certificate chain placeholder if the NSP lacks a .cert entry.
// ImportTicket needs a cert buffer even if empty — pass a zeroed 0x700 block.
static std::vector<uint8_t> make_empty_cert(size_t size = 0x700) {
    return std::vector<uint8_t>(size, 0);
}

// ── Main install routine ──────────────────────────────────────────────────────

bool install(std::vector<ContentEntry> contents,
             Core::Ncm::Storage storage,
             const Core::Keys::Keyset& keys,
             Progress& progress) {
    progress.reset();
    progress.running = true;

    NcmStorageId storage_id = (storage == Core::Ncm::Storage::SdCard)
        ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    // ── 1. Collect all CNMT entries ─────────────────────────────────────────
    progress.push_log(std::string("Install target: ") +
        (storage == Core::Ncm::Storage::SdCard ? "SD card" : "internal (NAND)"));
    progress.stage = "reading cnmt";
    progress.push_log("Reading content metadata (CNMT)...");
    std::vector<const ContentEntry*> cnmt_entries;
    for (const auto& e : contents)
        if (e.is_cnmt_nca) cnmt_entries.push_back(&e);
    if (cnmt_entries.empty()) {
        progress.message = "No .cnmt.nca found in container";
        progress.push_log("ERROR: no .cnmt.nca found in container");
        progress.done = true; return false;
    }

    // Parse all CNMTs up front.
    std::vector<ParsedCnmt> cnmts;
    for (const ContentEntry* ce : cnmt_entries) {
        progress.current_file = ce->name;
        ParsedCnmt c = decrypt_and_parse_cnmt(ce->read, ce->size, keys);
        if (!c.ok) {
            progress.message = "Failed to decrypt/parse CNMT: " + ce->name;
            progress.done = true; return false;
        }
        c.cnmt_nca_name = ce->name;
        c.cnmt_nca_size = ce->size;
        SDL_Log("Installer: CNMT parsed — title_id=%016llX version=%u type=%u ncas=%zu",
                (unsigned long long)c.key.id, c.key.version,
                (unsigned)c.key.type, c.infos.size());
        cnmts.push_back(std::move(c));
    }

    // ── 2. Open destination content storage + meta database ──────────────────
    NcmContentStorage dst_cs;
    NcmContentMetaDatabase dst_db;
    bool have_cs = false, have_db = false;

    auto cleanup = [&]() {
        if (have_cs) { ncmContentStorageClose(&dst_cs); have_cs = false; }
        if (have_db) { ncmContentMetaDatabaseClose(&dst_db); have_db = false; }
    };

    if (R_FAILED(ncmOpenContentStorage(&dst_cs, storage_id))) {
        progress.message = "Cannot open destination content storage";
        progress.done = true; return false;
    }
    have_cs = true;

    if (R_FAILED(ncmOpenContentMetaDatabase(&dst_db, storage_id))) {
        progress.message = "Cannot open destination meta database";
        cleanup(); progress.done = true; return false;
    }
    have_db = true;

    // ── 3. Compute total bytes for progress bar ───────────────────────────────
    uint64_t total = 0;
    for (const auto& e : contents)
        if (e.is_nca) total += e.size;
    progress.bytes_total = total;
    progress.ncas_total  = (int)std::count_if(contents.begin(), contents.end(),
                                [](const ContentEntry& e){ return e.is_nca; });
    {
        char lb[96];
        std::snprintf(lb, sizeof(lb), "Found %d content file(s), %.1f MB total",
                      progress.ncas_total.load(), (double)total / (1024.0 * 1024.0));
        progress.push_log(lb);
    }

    // ── 4. Install each NCA ──────────────────────────────────────────────────
    progress.stage = "installing";
    progress.push_log("Installing content...");

    std::vector<NcmContentId> installed;
    bool ok = true;
    constexpr size_t CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK);

    for (const auto& e : contents) {
        if (!e.is_nca) continue;
        if (progress.cancel.load()) { ok = false; break; }

        progress.current_file = e.name;
        const int nca_index = progress.ncas_done.load() + 1;
        const int nca_total = progress.ncas_total.load();

        NcmContentId content_id{};
        if (!content_id_from_name(e.name, content_id)) {
            progress.message = "Bad NCA filename: " + e.name;
            progress.push_log("ERROR: bad NCA filename: " + e.name);
            ok = false; break;
        }

        // Skip if already registered (idempotent re-install).
        bool has = false;
        if (R_SUCCEEDED(ncmContentStorageHas(&dst_cs, &has, &content_id)) && has) {
            progress.bytes_done.fetch_add(e.size);
            progress.ncas_done.fetch_add(1);
            char lb[128];
            std::snprintf(lb, sizeof(lb), "[%d/%d] %s - already installed, skipped",
                          nca_index, nca_total, e.name.c_str());
            progress.push_log(lb);
            continue;
        }

        {
            char lb[160];
            std::snprintf(lb, sizeof(lb), "[%d/%d] %s (%.1f MB)%s",
                          nca_index, nca_total, e.name.c_str(),
                          (double)e.size / (1024.0 * 1024.0),
                          (e.is_ncz ? " [NSZ]" : ""));
            progress.push_log(lb);
        }

        NcmPlaceHolderId ph{};
        if (R_FAILED(ncmContentStorageGeneratePlaceHolderId(&dst_cs, &ph))) {
            progress.message = "GeneratePlaceHolderId failed";
            progress.push_log("ERROR: GeneratePlaceHolderId failed");
            ok = false; break;
        }
        ncmContentStorageDeletePlaceHolder(&dst_cs, &ph);

        // For NCZ, placeholder must be sized to the DECOMPRESSED NCA size.
        const bool is_ncz_entry = e.is_ncz && NczDecompressor::is_ncz(e.read, e.size);
        const s64 placeholder_size = is_ncz_entry
            ? (s64)NczDecompressor::get_decompressed_size(e.read, e.size, keys)
            : (s64)e.size;
        if (placeholder_size <= 0) {
            progress.message = "NCZ: cannot determine decompressed size for " + e.name;
            progress.push_log("ERROR: cannot determine decompressed size for " + e.name);
            ok = false; break;
        }
        if (is_ncz_entry) {
            char lb[96];
            std::snprintf(lb, sizeof(lb), "      decompressing NSZ -> %.1f MB...",
                          (double)placeholder_size / (1024.0 * 1024.0));
            progress.push_log(lb);
        }

        if (R_FAILED(ncmContentStorageCreatePlaceHolder(&dst_cs, &content_id, &ph, placeholder_size))) {
            progress.message = "CreatePlaceHolder failed for " + e.name;
            progress.push_log("ERROR: CreatePlaceHolder failed for " + e.name);
            ok = false; break;
        }

        bool nca_ok = true;

        if (is_ncz_entry) {
            // ── NCZ/NSZ: decompress + re-encrypt, then write in chunks ────────
            uint64_t write_off = 0;
            std::string ncz_err = NczDecompressor::decompress(
                e.read, e.size, keys,
                [&](uint64_t nca_offset, const uint8_t* data, size_t len) -> bool {
                    if (progress.cancel.load()) return false;
                    if (R_FAILED(ncmContentStorageWritePlaceHolder(
                            &dst_cs, &ph, nca_offset, data, len))) {
                        progress.message = "WritePlaceHolder failed (NCZ) for " + e.name;
                        return false;
                    }
                    write_off = nca_offset + len;
                    progress.bytes_done.fetch_add(len);
                    return true;
                });
            if (!ncz_err.empty()) {
                progress.message = ncz_err;
                progress.push_log("ERROR: " + ncz_err);
                nca_ok = false;
            }
        } else {
            // ── Plain NCA: stream directly into placeholder ───────────────────
            uint64_t off = 0;
            while (off < e.size) {
                if (progress.cancel.load()) { nca_ok = false; break; }
                size_t chunk = (size_t)std::min<uint64_t>(e.size - off, CHUNK);
                size_t got = e.read(off, buf.data(), chunk);
                if (got == 0) {
                    progress.message = "Read error in " + e.name;
                    nca_ok = false; break;
                }
                if (R_FAILED(ncmContentStorageWritePlaceHolder(&dst_cs, &ph, off, buf.data(), got))) {
                    progress.message = "WritePlaceHolder failed for " + e.name;
                    nca_ok = false; break;
                }
                off += got;
                progress.bytes_done.fetch_add(got);
            }
        }

        if (!nca_ok) {
            ncmContentStorageDeletePlaceHolder(&dst_cs, &ph);
            ok = false; break;
        }

        if (R_FAILED(ncmContentStorageRegister(&dst_cs, &content_id, &ph))) {
            ncmContentStorageDeletePlaceHolder(&dst_cs, &ph);
            progress.message = "Register failed for " + e.name;
            progress.push_log("ERROR: register failed for " + e.name);
            ok = false; break;
        }
        installed.push_back(content_id);
        progress.ncas_done.fetch_add(1);
        progress.push_log("      registered");
        SDL_Log("Installer: registered NCA %s", e.name.c_str());
    }

    // ── Rollback on NCA failure ───────────────────────────────────────────────
    if (!ok) {
        progress.push_log("Rolling back partially installed content...");
        for (const auto& cid : installed) {
            bool has = false;
            if (R_SUCCEEDED(ncmContentStorageHas(&dst_cs, &has, &cid)) && has)
                ncmContentStorageDelete(&dst_cs, &cid);
        }
        cleanup();
        progress.success = false;
        progress.done = true;
        return false;
    }

    // ── 5. Write meta-DB record for every CNMT ─────────────────────────────
    // Based on Sphaira/yati RegisterNcasAndPushRecord (hardware-validated):
    //
    // Blob = NcmContentMetaHeader
    //      + extended_header (raw bytes from CNMT, exact size from header)
    //      + NcmContentInfo for the CNMT NCA itself (type=Meta, first entry)
    //      + NcmContentInfo[] for all other NCAs from the CNMT content list
    //
    // meta_header.content_count = infos.size() + 1 (the +1 is the CNMT NCA)
    // meta_header.storage_id = 0 (always zeroed before writing)
    //
    // No ncmExit/ncmInitialize — that corrupts other titles by causing NCM
    // to reload stale data and Remove the wrong entries.
    // No Remove before Set — RemoveInstalledNcas (via ListContentInfo) handles
    // cleanup of old entries cleanly.
    //
    // The extended_header bytes come from the raw CNMT binary. We read them
    // from raw_bytes at: sizeof(CnmtHeader) .. sizeof(CnmtHeader)+ext_hdr_size

    progress.stage = "registering meta";
    progress.push_log("Registering title metadata...");

    for (const ParsedCnmt& cnmt : cnmts) {

        // ── Parse NcmContentInfo list from CNMT PackagedContentInfo records ──
        // PackagedContentInfo (0x38): hash[0x20] + content_id[0x10] + size[6] + type + id_offset
        // NcmContentInfo     (0x18):              content_id[0x10] + size[5] + attr + type + id_offset
        // Sphaira reads infos via ListContentInfo from the gamecard NCM DB —
        // we parse them from raw_bytes (same data, just from the CNMT binary).

        if (cnmt.raw_bytes.size() < sizeof(CnmtHeader)) {
            progress.message = "CNMT raw bytes too small";
            cleanup(); progress.done = true; return false;
        }

        const uint8_t* raw = cnmt.raw_bytes.data();
        uint16_t ext_sz    = raw[14] | (raw[15] << 8);
        uint16_t cnt_count = raw[16] | (raw[17] << 8);
        size_t   rec_off   = sizeof(CnmtHeader) + ext_sz;

        // Read extended header bytes from raw CNMT.
        std::vector<uint8_t> extended_header(ext_sz, 0);
        if (ext_sz > 0 && rec_off <= cnmt.raw_bytes.size()) {
            std::memcpy(extended_header.data(), raw + sizeof(CnmtHeader), ext_sz);
        }

        // Zero required_system_version so the title launches on any firmware.
        // Application/Patch extended headers both have required_system_version at offset 0x08.
        if (ext_sz >= 0x0C &&
            (cnmt.key.type == NcmContentMetaType_Application ||
             cnmt.key.type == NcmContentMetaType_Patch)) {
            std::memset(extended_header.data() + 0x08, 0, 4); // required_system_version
        }

        // Build NcmContentInfo list from PackagedContentInfo records.
        std::vector<NcmContentInfo> infos;
        for (uint16_t i = 0; i < cnt_count; ++i) {
            size_t r = rec_off + i * sizeof(CnmtContentRecord);
            if (r + sizeof(CnmtContentRecord) > cnmt.raw_bytes.size()) break;
            // Skip DeltaFragment (type=0x06).
            uint8_t ctype = raw[r + 0x36];
            if (ctype == 0x06) continue;
            NcmContentInfo info{};
            std::memcpy(info.content_id.c, raw + r + 0x20, 0x10); // skip 0x20-byte hash
            // NcmContentInfo.size is u40 (5 bytes); PackagedContentInfo has u48 (6 bytes).
            std::memcpy(reinterpret_cast<uint8_t*>(&info) + 0x10, raw + r + 0x30, 5);
            info.attr         = 0;
            info.content_type = (NcmContentType)ctype;
            info.id_offset    = raw[r + 0x37];
            infos.push_back(info);
        }

        // ── Build the CNMT NCA's own NcmContentInfo (Meta type, first entry) ──
        NcmContentInfo cnmt_info{};
        if (content_id_from_name(cnmt.cnmt_nca_name, cnmt_info.content_id)) {
            uint64_t sz = cnmt.cnmt_nca_size;
            for (int b = 0; b < 5; ++b)
                reinterpret_cast<uint8_t*>(&cnmt_info)[0x10 + b] = (uint8_t)((sz >> (b*8)) & 0xFF);
            cnmt_info.attr         = 0;
            cnmt_info.content_type = NcmContentType_Meta;
            cnmt_info.id_offset    = 0;
        }

        // ── Build NcmContentMetaHeader ────────────────────────────────────────
        // content_count = infos.size() + 1 (the +1 is the CNMT NCA itself)
        // storage_id = 0 (always zeroed, per Sphaira)
        NcmContentMetaHeader meta_hdr{};
        meta_hdr.extended_header_size = ext_sz;
        meta_hdr.content_count        = (uint16_t)(infos.size() + 1);
        meta_hdr.content_meta_count   = 0;
        meta_hdr.attributes           = 0;
        meta_hdr.storage_id           = 0;  // zeroed per Sphaira

        // ── Assemble blob: header + ext_hdr + cnmt_info + infos[] ────────────
        std::vector<uint8_t> blob;
        auto bwrite = [&](const void* data, size_t sz_) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
            blob.insert(blob.end(), p, p + sz_);
        };
        bwrite(&meta_hdr,           sizeof(meta_hdr));
        bwrite(extended_header.data(), extended_header.size());
        bwrite(&cnmt_info,          sizeof(cnmt_info));  // CNMT NCA first
        for (const auto& info : infos)
            bwrite(&info,           sizeof(info));

        SDL_Log("Installer: meta blob: hdr=%zu ext=%u cnmt_info=%zu infos=%zu total=%zu",
                sizeof(meta_hdr), ext_sz, sizeof(cnmt_info), infos.size(), blob.size());

        ::Result set_rc = ncmContentMetaDatabaseSet(&dst_db, &cnmt.key,
                                                     blob.data(), blob.size());
        if (R_FAILED(set_rc)) {
            SDL_Log("Installer: ncmContentMetaDatabaseSet failed 0x%08X", set_rc);
            for (const auto& cid : installed) {
                bool has = false;
                if (R_SUCCEEDED(ncmContentStorageHas(&dst_cs, &has, &cid)) && has)
                    ncmContentStorageDelete(&dst_cs, &cid);
            }
            cleanup();
            char ebuf[64]; snprintf(ebuf, sizeof(ebuf), "Meta DB set failed (0x%08X)", set_rc);
            progress.message = ebuf;
            progress.done = true; return false;
        }

        ::Result commit_rc = ncmContentMetaDatabaseCommit(&dst_db);
        if (R_FAILED(commit_rc)) {
            SDL_Log("Installer: ncmContentMetaDatabaseCommit failed 0x%08X", commit_rc);
            ncmContentMetaDatabaseRemove(&dst_db, &cnmt.key);
            ncmContentMetaDatabaseCommit(&dst_db);
            for (const auto& cid : installed) {
                bool has = false;
                if (R_SUCCEEDED(ncmContentStorageHas(&dst_cs, &has, &cid)) && has)
                    ncmContentStorageDelete(&dst_cs, &cid);
            }
            cleanup();
            char ebuf[64]; snprintf(ebuf, sizeof(ebuf), "Meta DB commit failed (0x%08X)", commit_rc);
            progress.message = ebuf;
            progress.done = true; return false;
        }

        SDL_Log("Installer: meta DB committed for %016llX count=%u+1 blob=%zu",
                (unsigned long long)cnmt.key.id, (unsigned)infos.size(), blob.size());
    }

    cleanup();  // close content storage + meta-DB

    // Push application records into NS.
    // From ITotalJustice XCI installer (confirmed working):
    //   Delete = cmd 27, Push = cmd 16, buffer = HipcMapAlias (not HipcPointer)
    //   last_modified_event = 3 = NsApplicationRecordType_Installed
    //   Read existing records via cmd 17, skip GameCard, append SD record.

    #pragma pack(push, 1)
    struct ContentStorageRecord {
        NcmContentMetaKey key;
        u8 storage_id_byte;
        u8 _pad[7];
    };
    #pragma pack(pop)
    static_assert(sizeof(ContentStorageRecord) == 0x18, "CSR size");

    for (const ParsedCnmt& cnmt : cnmts) {
        if (cnmt.key.type != NcmContentMetaType_Application) continue;

        const u64 app_id = cnmt.key.id;
        Service ns_srv{};
        if (R_FAILED(nsGetApplicationManagerInterface(&ns_srv))) {
            SDL_Log("nsGetApplicationManagerInterface failed");
            continue;
        }

        // Read existing records for this app (from other storage locations).
        std::vector<ContentStorageRecord> records;
        s32 existing_count = 0;
        if (R_SUCCEEDED(nsCountApplicationContentMeta(app_id, &existing_count)) && existing_count > 0) {
            std::vector<ContentStorageRecord> existing(existing_count);
            s32 out_count = 0;
            struct { u64 offset; u64 tid; } list_in = { 0, app_id };
            ::Result list_rc = serviceDispatchInOut(&ns_srv, 17, list_in, out_count,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { existing.data(), sizeof(ContentStorageRecord) * existing.size() } });
            SDL_Log("ListAppRecordContentMeta rc=0x%08X count=%d", list_rc, out_count);
            if (R_SUCCEEDED(list_rc)) {
                for (s32 i = 0; i < out_count; ++i) {
                    if (existing[i].storage_id_byte == NcmStorageId_GameCard) continue;
                    records.push_back(existing[i]);
                }
            }
        }

        // Append our new SD/NAND record.
        ContentStorageRecord new_rec{};
        new_rec.key             = cnmt.key;
        new_rec.storage_id_byte = (u8)storage_id;
        records.push_back(new_rec);

        // Delete existing record (cmd 27), then push (cmd 16).
        ::Result del_rc = serviceDispatchIn(&ns_srv, 27, app_id);
        SDL_Log("DeleteApplicationRecord(%016llX) rc=0x%08X", (unsigned long long)app_id, del_rc);

        const struct { u8 lme; u8 _pad[7]; u64 tid; } push_in = { 3, {0}, app_id };
        ::Result push_rc = serviceDispatchIn(&ns_srv, 16, push_in,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
            .buffers = { { records.data(), sizeof(ContentStorageRecord) * records.size() } });
        SDL_Log("PushApplicationRecord(%016llX) cmd16 rc=0x%08X records=%zu",
           (unsigned long long)app_id, push_rc, records.size());

        serviceClose(&ns_srv);
    }

    // Trigger NS to synchronize its application record list with the meta-DB.
    {
        // First verify NCM responds correctly to GetContentIdByType — the exact
        // call NS makes when scanning. If this fails, NS rejects the title.
        {
            NcmContentMetaDatabase scan_db;
            NcmContentStorage scan_cs;
            bool have_db = R_SUCCEEDED(ncmOpenContentMetaDatabase(&scan_db, storage_id));
            bool have_cs = R_SUCCEEDED(ncmOpenContentStorage(&scan_cs, storage_id));
            for (const ParsedCnmt& cnmt : cnmts) {
                NcmContentId meta_cid{}, ctrl_cid{};
                ::Result meta_rc = have_db
                    ? ncmContentMetaDatabaseGetContentIdByType(&scan_db, &meta_cid, &cnmt.key, NcmContentType_Meta)
                    : 0xDEAD;
                ::Result ctrl_rc = have_db
                    ? ncmContentMetaDatabaseGetContentIdByType(&scan_db, &ctrl_cid, &cnmt.key, NcmContentType_Control)
                    : 0xDEAD;
                char mhex[33]={}, chex[33]={};
                for(int b=0;b<16;b++) snprintf(mhex+b*2,3,"%02x",meta_cid.c[b]);
                for(int b=0;b<16;b++) snprintf(chex+b*2,3,"%02x",ctrl_cid.c[b]);
                SDL_Log("GetCidByType Meta(%016llX) rc=0x%08X id=%s",
                   (unsigned long long)cnmt.key.id, meta_rc, mhex);
                SDL_Log("GetCidByType Ctrl(%016llX) rc=0x%08X id=%s",
                   (unsigned long long)cnmt.key.id, ctrl_rc, chex);
                if (have_cs && R_SUCCEEDED(meta_rc)) {
                    char path[512]={};
                    ::Result pr = ncmContentStorageGetPath(&scan_cs, path, sizeof(path), &meta_cid);
                    SDL_Log("  Meta path rc=0x%08X: %s", pr, path);
                }
                if (have_cs && R_SUCCEEDED(ctrl_rc)) {
                    char path[512]={};
                    ::Result pr = ncmContentStorageGetPath(&scan_cs, path, sizeof(path), &ctrl_cid);
                    SDL_Log("  Ctrl path rc=0x%08X: %s", pr, path);
                }
            }
            if (have_cs) ncmContentStorageClose(&scan_cs);
            if (have_db) ncmContentMetaDatabaseClose(&scan_db);
        }

        NsApplicationRecord dummy[1];
        s32 out_count = 0;
        SDL_Log("nsListApplicationRecord(trigger) rc=0x%08X count=%d",
           nsListApplicationRecord(dummy, 1, 0, &out_count), out_count);

        Event upd_evt{};
        ::Result evt_rc = nsGetApplicationRecordUpdateSystemEvent(&upd_evt);
        SDL_Log("nsGetApplicationRecordUpdateSystemEvent rc=0x%08X", evt_rc);
        if (R_SUCCEEDED(evt_rc)) {
            SDL_Log("eventWait rc=0x%08X", eventWait(&upd_evt, 2000000000ULL));
            eventClose(&upd_evt);
        }

        for (const ParsedCnmt& cnmt : cnmts) {
            if (cnmt.key.type != NcmContentMetaType_Application) continue;
            NsApplicationRecord recs[128]; s32 count = 0;
            nsListApplicationRecord(recs, 128, 0, &count);
            bool found = false;
            for (s32 i = 0; i < count; ++i)
                if (recs[i].application_id == cnmt.key.id) { found = true; break; }
            SDL_Log("ns record for %016llX found=%d (count=%d)",
               (unsigned long long)cnmt.key.id, (int)found, count);
        }
    }
    SDL_Log("cleanup done");

    // ── Critical persistence check ────────────────────────────────────────────
    // Reopen the meta-DB and verify the entries still exist after close.
    {
        NcmContentMetaDatabase verify_db;
        ::Result open_rc = ncmOpenContentMetaDatabase(&verify_db, storage_id);
        SDL_Log("reopen meta-DB rc=0x%08X", open_rc);
        if (R_SUCCEEDED(open_rc)) {
            for (const ParsedCnmt& cnmt : cnmts) {
                u64 size_after = 0;
                ::Result sz_rc = ncmContentMetaDatabaseGetSize(&verify_db, &size_after, &cnmt.key);
                SDL_Log("after-reopen GetSize(%016llX) rc=0x%08X size=%llu",
                   (unsigned long long)cnmt.key.id, sz_rc, (unsigned long long)size_after);
            }
            NcmContentMetaKey keys[64];
            s32 total = 0, written = 0;
            ::Result list_rc = ncmContentMetaDatabaseList(&verify_db,
                &total, &written, keys, 64, NcmContentMetaType_Unknown,
                0, 0, UINT64_MAX, NcmContentInstallType_Full);
            SDL_Log("ncmContentMetaDatabaseList rc=0x%08X total=%d written=%d", list_rc, total, written);
            for (int i = 0; i < written && i < 64; ++i) {
                SDL_Log("  db[%d]: id=%016llX version=%u type=0x%02X",
                   i, (unsigned long long)keys[i].id, keys[i].version,
                   (unsigned)keys[i].type);
            }
            ncmContentMetaDatabaseClose(&verify_db);
        }
    }

    // ── Content ID cross-check ────────────────────────────────────────────────
    // Verify that every PackagedContentInfo record in each CNMT has a matching
    // NCA registered in content storage. If any are missing, HOS will silently
    // refuse to show the title even if the meta-DB entry is correct.
    {
        NcmContentStorage verify_cs;
        ::Result cs_rc = ncmOpenContentStorage(&verify_cs, storage_id);
        SDL_Log("reopen content storage rc=0x%08X", cs_rc);
        if (R_SUCCEEDED(cs_rc)) {
            for (const ParsedCnmt& cnmt : cnmts) {
                SDL_Log("=== CNMT %016llX content check ===", (unsigned long long)cnmt.key.id);
                // Parse PackagedContentInfo records from raw_bytes.
                // Layout: CnmtHeader(0x18) + ext_hdr(ext_hdr_size) + records(content_count × 0x38)
                if (cnmt.raw_bytes.size() < sizeof(CnmtHeader)) { SDL_Log("raw too small"); continue; }
                const uint8_t* raw = cnmt.raw_bytes.data();
                uint16_t ext_sz    = raw[14] | (raw[15] << 8);
                uint16_t cnt_count = raw[16] | (raw[17] << 8);
                size_t rec_off = sizeof(CnmtHeader) + ext_sz;
                SDL_Log("header=0x%zx ext_hdr_size=%u content_count=%u records_at=0x%zx",
                   sizeof(CnmtHeader), ext_sz, cnt_count, rec_off);
                for (uint16_t i = 0; i < cnt_count; ++i) {
                    size_t r = rec_off + i * 0x38;
                    if (r + 0x38 > cnmt.raw_bytes.size()) { SDL_Log("  [%d] truncated", i); break; }
                    // PackagedContentInfo: hash[0x20], content_id[0x10], size[6], type, id_offset
                    NcmContentId cid{};
                    std::memcpy(cid.c, raw + r + 0x20, 0x10);
                    uint8_t ctype = raw[r + 0x36];
                    // Check if this content ID exists in storage.
                    bool has = false;
                    ::Result has_rc = ncmContentStorageHas(&verify_cs, &has, &cid);
                    char id_hex[33] = {};
                    for (int b = 0; b < 0x10; ++b)
                        snprintf(id_hex + b*2, 3, "%02x", cid.c[b]);
                    SDL_Log("  [%d] type=0x%02X id=%s has_rc=0x%08X has=%d",
                       i, ctype, id_hex, has_rc, (int)has);
                }
            }
            ncmContentStorageClose(&verify_cs);
        }
    }

    // ── 7. Ticket install ─────────────────────────────────────────────────────
    // A titlekey NCA keeps its rights_id, so HOS needs a ticket to obtain the
    // titlekey at launch. Without a usable ticket the title verifies (hash + sig
    // OK) but won't boot ("titlekey cannot be initialized", surfaced as fatal
    // 2123-0011). Rules, mirroring the working NSP path:
    //   * container has a COMMON .tik      -> import it VERBATIM with its real
    //     cert (its titlekey is already titlekek-encrypted and console-agnostic,
    //     and it is validly signed, so no sig patch is needed — exactly like NSP).
    //   * container has a PERSONALISED .tik -> rebuild it into a common ticket
    //     using the raw NCZ section titlekey (falls back to verbatim if we can't).
    //   * container has NO .tik            -> fabricate a common ticket per
    //     titlekey NCA from its NCZ section key.
    // Import failures are surfaced, never silently dropped.

    const ContentEntry* tik_entry  = nullptr;
    const ContentEntry* cert_entry = nullptr;
    for (const auto& e : contents) {
        if (e.is_tik  && !tik_entry)  tik_entry  = &e;
        if (e.is_cert && !cert_entry) cert_entry = &e;
    }

    // Offset of the ticket body (past the signature block) from its sig type.
    auto ticket_body_off = [](const uint8_t* t, size_t n) -> size_t {
        if (n < 4) return 0x140;
        uint32_t sig = t[0] | (t[1] << 8) | (t[2] << 16) | (t[3] << 24);
        switch (sig) {
            case 0x10000: case 0x10003: return 0x240; // RSA-4096
            case 0x10002: case 0x10005: return 0x80;  // ECDSA
            default:                    return 0x140; // RSA-2048 (0x10001 / 0x10004)
        }
    };

    // Raw (decrypted) titlekey from an NCZ crypto=3 section of `e`, if present.
    auto ncz_titlekey = [](const ContentEntry& e, uint8_t out[0x10]) -> bool {
        if (!e.is_ncz || e.size < 0x5000) return false;
        uint8_t h[0x10]{};
        if (e.read(0x4000, h, 0x10) != 0x10) return false;
        uint64_t magic = 0, nsec = 0;
        std::memcpy(&magic, h, 8); std::memcpy(&nsec, h + 8, 8);
        if (magic != 0x4E544345535A434EULL || !nsec || nsec > 64) return false;
        std::vector<uint8_t> sb(nsec * 0x40);
        if (e.read(0x4010, sb.data(), sb.size()) != sb.size()) return false;
        for (uint64_t i = 0; i < nsec; ++i) {
            uint64_t c = 0; std::memcpy(&c, sb.data() + i * 0x40 + 0x10, 8);
            if (c == 3) { std::memcpy(out, sb.data() + i * 0x40 + 0x20, 0x10); return true; }
        }
        return false;
    };

    // Import a ticket (+cert) via ES; log and return the result.
    auto import_ticket = [&progress](std::vector<uint8_t>& tik, std::vector<uint8_t>& cert,
                            const char* tag) -> ::Result {
        Service es_srv{};
        ::Result rc = smGetService(&es_srv, "es");
        if (R_SUCCEEDED(rc)) {
            rc = es_import_ticket(&es_srv, tik.data(), tik.size(), cert.data(), cert.size());
            serviceClose(&es_srv);
        }
        SDL_Log("ES ImportTicket (%s) rc=0x%08X", tag, (unsigned)rc);
        if (FILE* lg = std::fopen("sdmc:/switch/GarageNX/ncz_sections.txt", "ab")) {
            std::fprintf(lg, "ES ImportTicket (%s) rc=0x%08X\n", tag, (unsigned)rc);
            std::fclose(lg);
        }
        char lb[96];
        std::snprintf(lb, sizeof(lb), "Importing ticket (%s)... rc=0x%08X", tag, (unsigned)rc);
        progress.push_log(lb);
        return rc;
    };

    ::Result tik_rc = 0;
    bool attempted_ticket = false;

    // Real cert chain from the container (used for verbatim + rebuilt imports).
    std::vector<uint8_t> cert_data;
    if (cert_entry) {
        cert_data.resize(cert_entry->size);
        if (cert_entry->read(0, cert_data.data(), cert_data.size()) != cert_data.size())
            cert_data = make_empty_cert();
    } else {
        cert_data = make_empty_cert();
    }

    if (tik_entry) {
        std::vector<uint8_t> tik_data(tik_entry->size);
        if (tik_entry->read(0, tik_data.data(), tik_data.size()) == tik_data.size() &&
            tik_data.size() >= 0x2C0) {

            const size_t  body     = ticket_body_off(tik_data.data(), tik_data.size());
            const uint8_t key_type = (body + 0x141 < tik_data.size())
                                     ? tik_data[body + 0x141] : 0xFF;

            if (key_type == 0 /* Common */) {
                // Already common — import verbatim, exactly like NSP.
                progress.stage = "importing ticket";
                attempted_ticket = true;
                tik_rc = import_ticket(tik_data, cert_data, "common-verbatim");
            } else {
                // Personalised: rebuild into a common ticket if we can recover the
                // raw titlekey from an NCZ section; otherwise import as-is.
                uint8_t raw[0x10]{};
                bool rebuilt = false;
                for (const auto& e : contents) {
                    if (!ncz_titlekey(e, raw)) continue;
                    uint8_t mkr = (body + 0x145 < tik_data.size()) ? tik_data[body + 0x145] : 0;
                    int kg = (mkr < Core::Keys::MAX_KEY_GENERATION) ? mkr : 0;
                    if (!keys.has_titlekek[kg]) break;
                    uint8_t enc[0x10];
                    Aes128Context c;
                    aes128ContextCreate(&c, keys.titlekek[kg].data(), true);
                    aes128EncryptBlock(&c, enc, raw);
                    std::memset(tik_data.data() + body + 0x40, 0, 0x100);  // title_key_block
                    std::memcpy(tik_data.data() + body + 0x40, enc, 0x10); // titlekek-encrypted
                    tik_data[body + 0x141] = 0;                            // -> Common
                    std::memset(tik_data.data() + body + 0x158, 0, 0x08);  // device_id
                    std::memset(tik_data.data() + body + 0x170, 0, 0x04);  // account_id
                    std::memset(tik_data.data() + body + 0x174, 0, 0x0C);  // sect_* fields
                    std::memset(tik_data.data() + 4, 0xFF, 0x100);         // dummy sig
                    rebuilt = true;
                    break;
                }
                progress.stage = "importing ticket";
                attempted_ticket = true;
                tik_rc = import_ticket(tik_data, cert_data,
                                       rebuilt ? "personalised->common" : "personalised-verbatim");
            }
        }
    }

    // No usable .tik in the container: fabricate a common ticket for each
    // titlekey NCA from its NCZ section key. make_common_ticket() returns empty
    // for standard-crypto NCAs (rights_id == 0), which need no ticket.
    if (!attempted_ticket) {
        std::vector<uint8_t> empty_cert = make_empty_cert();
        for (const auto& e : contents) {
            uint8_t raw[0x10]{};
            if (!ncz_titlekey(e, raw)) continue;
            std::vector<uint8_t> tik =
                NczDecompressor::make_common_ticket(e.read, e.size, keys, raw);
            if (tik.empty()) continue;  // standard-crypto NCA: no ticket needed
            progress.stage = "importing ticket";
            attempted_ticket = true;
            tik_rc = import_ticket(tik, empty_cert, "fabricated-common");
            break;  // one titlekey per title
        }
    }

    if (attempted_ticket && R_FAILED(tik_rc)) {
        char tbuf[96];
        std::snprintf(tbuf, sizeof(tbuf),
            "Installed, but ticket import failed (0x%08X) - title may not launch", (unsigned)tik_rc);
        SDL_Log("Installer: %s", tbuf);
        progress.push_log(std::string("ERROR: ") + tbuf);
        progress.message = tbuf;
        progress.success = true; progress.done = true;
        return true;
    }

    SDL_Log("ok=1 path=Switch");

    if (!attempted_ticket)
        progress.push_log("No ticket required (standard-crypto title).");
    progress.push_log("Installation complete.");
    progress.message = "Installation complete";
    progress.success = true; progress.done = true;
    return true;
}

#else  // !PLATFORM_SWITCH

bool install(std::vector<ContentEntry> /*contents*/,
             Core::Ncm::Storage /*storage*/,
             const Core::Keys::Keyset& /*keys*/,
             Progress& progress) {
    progress.reset();
    progress.running = true;
    // PC stub: simulate a brief install.
    progress.bytes_total = 1;
    progress.bytes_done  = 1;
    progress.ncas_total  = 1;
    progress.ncas_done   = 1;
    progress.message = "Installation complete (stub)";
    progress.success = true;
    progress.done    = true;
    if (FILE* lg = std::fopen("sdmc:/switch/GarageNX/install_result.txt", "wb")) {
        std::fprintf(lg, "ok=1 path=PC_STUB\n");
        std::fclose(lg);
    }
    return true;
}

#endif // PLATFORM_SWITCH

} // namespace Install
