// source/core/dump.cpp
// NSP dumping — STANDARD (no NCA modification) pipeline.
//
// Streams every NCA of a title byte-for-byte from NCM content storage into a
// PFS0 (NSP) container on the SD card. Because NO NCA is modified, every NCA
// keeps its original SHA-256 (and filename), and the CNMT remains valid — so the
// NSP installs cleanly. This proves the full PFS0 + streaming + NCM-read
// pipeline before ticket-less header surgery is layered on top later.
//
// For titles that use titlekey crypto, a proper install also needs a .tik/.cert
// pair — that is the next increment. For now such titles are flagged so the user
// knows the dump is content-only.
//
// PFS0 layout (little-endian):
//   0x00 "PFS0" | 0x04 u32 file_count | 0x08 u32 string_table_size |
//   0x0C u32 reserved | 0x10 entries[0x18]{u64 data_off,u64 size,u32 name_off,
//   u32 rsvd} | string table (NUL-terminated) padded to 0x10 | file data.

#include "core/dump.hpp"
#include "core/fs.hpp"
#include "core/es.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Dump {

#ifdef PLATFORM_SWITCH

// Diagnostic: what happened with tik/cert on the last dump (shown in result).
static std::string s_tikcert_note;

static constexpr size_t STREAM_CHUNK    = 4 * 1024 * 1024;  // 4 MB copy buffer
static constexpr size_t PFS0_ENTRY_SIZE = 0x18;
static constexpr size_t NCA_HEADER_SIZE = 0xC00;

struct Content {
    NcmContentId id;
    std::string  filename;
    uint64_t     size = 0;
    bool         is_meta = false;
    // For appended tik/cert "virtual" files, the bytes live here and id is unused.
    std::vector<uint8_t> inline_data;
    bool         is_inline = false;
};

static NcmContentMetaKey make_key(const Core::Ncm::Title& t) {
    NcmContentMetaKey key;
    std::memset(&key, 0, sizeof(key));
    key.id      = t.meta_id;
    key.version = t.version;
    key.type    = (t.type == Core::Ncm::TitleType::Application)  ? NcmContentMetaType_Application
                : (t.type == Core::Ncm::TitleType::Patch)        ? NcmContentMetaType_Patch
                : (t.type == Core::Ncm::TitleType::AddOnContent) ? NcmContentMetaType_AddOnContent
                                                                 : (NcmContentMetaType)0;
    key.install_type = NcmContentInstallType_Full;
    return key;
}

static std::string content_id_hex(const NcmContentId& id) {
    char buf[33];
    for (int i = 0; i < 0x10; ++i) snprintf(buf + i*2, 3, "%02x", id.c[i]);
    buf[32] = 0;
    return buf;
}

// Read an NCA's rights id (0x10 bytes) if it uses titlekey crypto. Returns true
// and fills `rights_id` when non-zero; false for standard-crypto NCAs.
static bool nca_rights_id(NcmContentStorage* cs, const NcmContentId* id,
                          const Core::Keys::Keyset& keys,
                          uint8_t rights_id[0x10]) {
    if (!keys.has_header_key) return false;
    uint8_t enc[NCA_HEADER_SIZE];
    if (R_FAILED(ncmContentStorageReadContentIdFile(cs, enc, NCA_HEADER_SIZE, id, 0)))
        return false;
    uint8_t dec[NCA_HEADER_SIZE];
    Aes128XtsContext xts;
    aes128XtsContextCreate(&xts, keys.header_key.data(), keys.header_key.data()+0x10, false);
    for (size_t s = 0; s < NCA_HEADER_SIZE/0x200; ++s) {
        aes128XtsContextResetSector(&xts, s, true);
        aes128XtsDecrypt(&xts, dec+s*0x200, enc+s*0x200, 0x200);
    }
    bool nonzero = false;
    for (int i = 0; i < 0x10; ++i) if (dec[0x230+i] != 0) { nonzero = true; break; }
    if (nonzero) std::memcpy(rights_id, dec + 0x230, 0x10);
    return nonzero;
}

bool dump_title_to_nsp(const Core::Ncm::Title& title,
                       const Core::Keys::Keyset& keys,
                       Progress& progress,
                       std::string& out_path) {
    progress.reset();
    progress.running = true;
    s_tikcert_note = "n/a";

    NcmStorageId storage_id = (title.storage == Core::Ncm::Storage::SdCard)
        ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    NcmContentMetaDatabase db;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage_id))) {
        progress.message = "Cannot open meta database";
        progress.done = true; return false;
    }
    NcmContentStorage cs;
    if (R_FAILED(ncmOpenContentStorage(&cs, storage_id))) {
        ncmContentMetaDatabaseClose(&db);
        progress.message = "Cannot open content storage";
        progress.done = true; return false;
    }

    NcmContentMetaKey key = make_key(title);

    // 1. Enumerate the meta's contents.
    std::vector<Content> contents;
    {
        constexpr s32 MAXC = 48;
        std::vector<NcmContentInfo> infos(MAXC);
        s32 written = 0;
        Result rc = ncmContentMetaDatabaseListContentInfo(
            &db, &written, infos.data(), MAXC, &key, 0);
        if (R_FAILED(rc) || written <= 0) {
            progress.message = "No content info for title";
            ncmContentStorageClose(&cs); ncmContentMetaDatabaseClose(&db);
            progress.done = true; return false;
        }
        bool have_rights = false;
        uint8_t rights_id[0x10] = {0};
        for (s32 i = 0; i < written; ++i) {
            const NcmContentInfo& ci = infos[i];
            if (ci.content_type == NcmContentType_DeltaFragment) continue;

            Content c;
            c.id = ci.content_id;
            c.is_meta = (ci.content_type == NcmContentType_Meta);
            u64 sz = 0;
            ncmContentInfoSizeToU64(&ci, &sz);
            c.size = sz;
            c.filename = content_id_hex(ci.content_id) +
                         (c.is_meta ? ".cnmt.nca" : ".nca");
            if (!have_rights)
                have_rights = nca_rights_id(&cs, &ci.content_id, keys, rights_id);
            contents.push_back(std::move(c));
        }

        // If this title uses titlekey crypto, add its ticket + certificate as
        // "<rights_id>.tik" and "<rights_id>.cert" so the NSP installs. The tik
        // and cert are read from the console's own ES system (cmd 22/23).
        if (have_rights) {
            char rid_hex[33];
            for (int i = 0; i < 0x10; ++i)
                snprintf(rid_hex + i*2, 3, "%02x", rights_id[i]);
            rid_hex[32] = 0;
            SDL_Log("Dump — rights id %s (titlekey crypto)", rid_hex);

            std::vector<uint8_t> tik, cert;
            bool got = Core::Es::get_ticket_and_cert(rights_id, tik, cert);
            SDL_Log("Dump — get_ticket_and_cert=%d tik=%zu cert=%zu",
                    got, tik.size(), cert.size());

            if (got && !tik.empty()) {
                Content tikc;
                tikc.is_inline = true;
                tikc.filename = std::string(rid_hex) + ".tik";
                tikc.size = tik.size();
                tikc.inline_data = std::move(tik);
                contents.push_back(std::move(tikc));

                if (!cert.empty()) {
                    Content certc;
                    certc.is_inline = true;
                    certc.filename = std::string(rid_hex) + ".cert";
                    certc.size = cert.size();
                    certc.inline_data = std::move(cert);
                    contents.push_back(std::move(certc));
                    s_tikcert_note = "tik+cert added";
                } else {
                    s_tikcert_note = "tik added (no cert)";
                }
            } else {
                char note[80];
                snprintf(note, sizeof(note), "no tik: %s",
                         Core::Es::g_es_diag.c_str());
                s_tikcert_note = note;
            }
        } else {
            s_tikcert_note = "no rights id (standard crypto?)";
        }
    }

    // 2. Build the PFS0 header.
    const uint32_t file_count = (uint32_t)contents.size();
    std::string strtab;
    std::vector<uint32_t> name_offsets;
    for (auto& c : contents) {
        name_offsets.push_back((uint32_t)strtab.size());
        strtab += c.filename; strtab += '\0';
    }
    size_t header_unpadded = 0x10 + (size_t)file_count * PFS0_ENTRY_SIZE + strtab.size();
    size_t header_size = (header_unpadded + 0xF) & ~size_t(0xF);
    uint32_t strtab_size32 = (uint32_t)(header_size - (0x10 + (size_t)file_count * PFS0_ENTRY_SIZE));

    std::vector<uint8_t> header(header_size, 0);
    std::memcpy(header.data(), "PFS0", 4);
    std::memcpy(header.data() + 4, &file_count, 4);
    std::memcpy(header.data() + 8, &strtab_size32, 4);

    uint64_t data_cursor = 0, total_data = 0;
    for (size_t i = 0; i < contents.size(); ++i) {
        uint8_t* e = header.data() + 0x10 + i * PFS0_ENTRY_SIZE;
        uint64_t off = data_cursor, sz = contents[i].size;
        std::memcpy(e + 0x00, &off, 8);
        std::memcpy(e + 0x08, &sz, 8);
        std::memcpy(e + 0x10, &name_offsets[i], 4);
        data_cursor += sz; total_data += sz;
    }
    std::memcpy(header.data() + 0x10 + (size_t)file_count * PFS0_ENTRY_SIZE,
                strtab.data(), strtab.size());

    progress.ncas_total = (int)contents.size();
    progress.bytes_total = total_data;

    // 3. Output file.
    std::string dir = "sdmc:/switch/GarageNX/dumps";
    Fs::make_directory(dir);
    char idbuf[24];
    snprintf(idbuf, sizeof(idbuf), "%016llX", (unsigned long long)title.program_id);
    out_path = dir + "/" + idbuf + "_v" + std::to_string(title.version) + ".nsp";

    FILE* out = fopen(out_path.c_str(), "wb");
    if (!out) {
        progress.message = "Cannot create output file";
        ncmContentStorageClose(&cs); ncmContentMetaDatabaseClose(&db);
        progress.done = true; return false;
    }
    if (fwrite(header.data(), 1, header.size(), out) != header.size()) {
        progress.message = "Header write failed";
        fclose(out); remove(out_path.c_str());
        ncmContentStorageClose(&cs); ncmContentMetaDatabaseClose(&db);
        progress.done = true; return false;
    }

    // 4. Stream each NCA byte-for-byte (NO modification).
    std::vector<uint8_t> buf(STREAM_CHUNK);
    bool ok = true;

    for (size_t ci = 0; ci < contents.size() && ok; ++ci) {
        Content& c = contents[ci];
        progress.current_file = c.filename;

        // Inline entries (tik/cert): write their bytes directly.
        if (c.is_inline) {
            if (fwrite(c.inline_data.data(), 1, c.inline_data.size(), out)
                    != c.inline_data.size()) {
                progress.message = "Write failed (SD full?)"; ok = false; break;
            }
            progress.bytes_done.fetch_add(c.inline_data.size());
            progress.ncas_done.fetch_add(1);
            continue;
        }

        uint64_t remaining = c.size, off = 0;

        while (remaining > 0) {
            if (progress.cancel.load()) { progress.message = "Cancelled"; ok = false; break; }
            size_t chunk = (size_t)std::min<uint64_t>(remaining, STREAM_CHUNK);
            if (R_FAILED(ncmContentStorageReadContentIdFile(&cs, buf.data(), chunk, &c.id, (s64)off))) {
                progress.message = "NCA read failed"; ok = false; break;
            }
            if (fwrite(buf.data(), 1, chunk, out) != chunk) {
                progress.message = "Write failed (SD full?)"; ok = false; break;
            }
            off += chunk; remaining -= chunk;
            progress.bytes_done.fetch_add(chunk);
        }
        progress.ncas_done.fetch_add(1);
    }

    fclose(out);
    ncmContentStorageClose(&cs);
    ncmContentMetaDatabaseClose(&db);

    if (!ok) {
        remove(out_path.c_str());
        progress.success = false;
    } else {
        progress.success = true;
        // Report what was actually packed, so it's clear the NSP contains only
        // this one meta's contents (a base app legitimately has several NCAs:
        // Program, Control, Meta, LegalInfo, HtmlDocument — not DLC).
        const char* type_str =
            title.type == Core::Ncm::TitleType::Application  ? "base app" :
            title.type == Core::Ncm::TitleType::Patch        ? "update"   :
            title.type == Core::Ncm::TitleType::AddOnContent ? "DLC"      : "title";
        char msg[128];
        if (s_tikcert_note == "n/a")
            snprintf(msg, sizeof(msg), "Packed %d files (%s)",
                     (int)contents.size(), type_str);
        else
            snprintf(msg, sizeof(msg), "Packed %d files (%s) — %s",
                     (int)contents.size(), type_str, s_tikcert_note.c_str());
        progress.message = msg;
    }
    progress.done = true;
    progress.running = false;
    return ok;
}

#else  // PC stub

bool dump_title_to_nsp(const Core::Ncm::Title&, const Core::Keys::Keyset&,
                       Progress& progress, std::string& out_path) {
    progress.reset();
    out_path = "dump.nsp";
    progress.message = "Dump not available on PC stub";
    progress.done = true;
    return false;
}

#endif

} // namespace Core::Dump
