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

#include <cerrno>
#include "core/nsp_stream.hpp"
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
static constexpr size_t STREAM_CHUNK = 4 * 1024 * 1024;  // 4 MB write buffer

// NOTE: the PFS0 header construction, NCA enumeration, rights-id detection and
// ticket/certificate handling that used to live here now live in
// core/nsp_stream.cpp, shared with the transports. Deliberately not duplicated.

bool dump_title_to_nsp(const Core::Ncm::Title& title,
                       const Core::Keys::Keyset& keys,
                       Progress& progress,
                       std::string& out_path) {
    // Thin wrapper over Core::NspStream — the SAME code path a PC client uses to
    // download a title over FTP/MTP/HTTP. Previously this function had its own copy
    // of the PFS0/ticket logic; two implementations of NSP construction was a
    // standing drift risk (an NSP that installs from one path and not the other),
    // so the streaming version — now verified on hardware — is the single source.
    progress.running = true;
    progress.done    = false;
    progress.success = false;

    std::string err;
    auto src = Core::NspStream::open(title, keys, &err);
    if (!src) {
        progress.message = err.empty() ? "Cannot read title" : err;
        progress.done    = true;
        progress.running = false;
        return false;
    }

    progress.bytes_total = src->total_size();
    progress.ncas_total  = (int)src->file_count();

    std::string dir = "sdmc:/switch/GarageNX/dumps";
    Fs::make_directory(dir);
    char idbuf[24];
    snprintf(idbuf, sizeof(idbuf), "%016llX", (unsigned long long)title.program_id);
    out_path = dir + "/" + idbuf + "_v" + std::to_string(title.version) + ".nsp";

    // ── SPLIT OUTPUT ────────────────────────────────────────────────────────
    // A Switch SD card is FAT32 unless the owner deliberately reformatted it, and
    // FAT32 cannot hold a file of 4 GiB or more. Cartridge dumps routinely exceed
    // that, so a single-file NSP fails partway through a large title with a short
    // fwrite — which this code used to report as "Write failed (SD full?)", a
    // guess that sent the user looking at free space. It was reported against a
    // card with 120 GB free.
    //
    // So the output is written as a SPLIT archive when it grows past the limit:
    // a DIRECTORY named <name>.nsp containing 00, 01, 02 ... parts. That is the
    // layout DBI and Tinfoil produce and consume, and the Switch treats such a
    // directory as one archive, so nothing downstream needs to know.
    //
    // The split is decided by SIZE, not by filesystem probing: a dump that stays
    // under the limit is written as an ordinary single file, which is what a user
    // with an exFAT card expects and what other tools produce.
    constexpr uint64_t kSplitPart = 0xFFFF0000ULL;   // just under 4 GiB per part

    const bool will_split = src->total_size() >= kSplitPart;
    std::string split_dir;
    if (will_split) {
        split_dir = out_path;                 // "<...>.nsp" becomes a directory
        Fs::make_directory(split_dir);        // NO-COMMIT: dump output on SD
        if (!Fs::is_directory(split_dir)) {
            progress.message = "Cannot create split output folder";
            progress.done = true; progress.running = false;
            return false;
        }
    }

    auto open_part = [&](int idx) -> FILE* {
        if (!will_split) return fopen(out_path.c_str(), "wb");
        char pn[8];
        snprintf(pn, sizeof(pn), "%02d", idx);
        return fopen((split_dir + "/" + pn).c_str(), "wb");
    };

    int      part      = 0;
    uint64_t part_used = 0;
    FILE* out = open_part(part);
    if (!out) {
        progress.message = "Cannot create output file";
        progress.done    = true;
        progress.running = false;
        return false;
    }

    std::vector<uint8_t> buf(STREAM_CHUNK);
    bool ok = true;
    for (;;) {
        if (progress.cancel.load()) { ok = false; progress.message = "Cancelled"; break; }

        const int64_t got = src->read(buf.data(), buf.size());
        if (got < 0) { ok = false; progress.message = "NCA read failed"; break; }
        if (got == 0) break;

        // Write across the part boundary rather than assuming a chunk fits.
        size_t off = 0;
        while (off < (size_t)got) {
            if (will_split && part_used >= kSplitPart) {
                fclose(out);
                out = open_part(++part);
                if (!out) { ok = false; progress.message = "Cannot create next part"; break; }
                part_used = 0;
            }
            size_t n = (size_t)got - off;
            if (will_split && n > (size_t)(kSplitPart - part_used))
                n = (size_t)(kSplitPart - part_used);

            if (fwrite(buf.data() + off, 1, n, out) != n) {
                ok = false;
                // Report the REAL reason. "SD full?" was a guess, and it was the
                // wrong one — errno distinguishes a genuinely full card (ENOSPC)
                // from the FAT32 size limit (EFBIG) from anything else.
                const int e = errno;
                if (e == ENOSPC)      progress.message = "SD card is full";
                else if (e == EFBIG)  progress.message = "File too large for this SD card";
                else {
                    char m[96];
                    snprintf(m, sizeof(m), "Write failed (errno %d)", e);
                    progress.message = m;
                }
                break;
            }
            off       += n;
            part_used += n;
        }
        if (!ok) break;

        progress.bytes_done = src->position();
        progress.ncas_done  = (int)src->current_index();
        progress.current_file = src->current_name();
    }

    // May be null if opening a later part failed and we broke out of the loop.
    if (out) fclose(out);

    // A partial NSP is worse than none — it looks installable and is not.
    if (ok && src->position() != src->total_size()) {
        ok = false;
        progress.message = "Incomplete dump";
    }

    if (!ok) {
        remove(out_path.c_str());
        progress.success = false;
    } else {
        progress.success = true;
        const char* type_str =
            title.type == Core::Ncm::TitleType::Application  ? "base app" :
            title.type == Core::Ncm::TitleType::Patch        ? "update"   :
            title.type == Core::Ncm::TitleType::AddOnContent ? "DLC"      : "title";
        const std::string note = src->note();
        char msg[160];
        if (note == "n/a")
            snprintf(msg, sizeof(msg), "Packed %d files (%s)",
                     (int)src->file_count(), type_str);
        else
            snprintf(msg, sizeof(msg), "Packed %d files (%s) — %s",
                     (int)src->file_count(), type_str, note.c_str());
        progress.message = msg;
    }
    progress.done    = true;
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
