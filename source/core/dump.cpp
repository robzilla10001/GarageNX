// source/core/dump.cpp
// NSP dumping - standard (no NCA modification) pipeline.
//
// Streams every NCA of a title byte-for-byte from NCM content storage into a
// PFS0 (NSP) container, so every NCA keeps its original SHA-256 and the CNMT
// stays valid - the NSP installs cleanly. Titlekey titles additionally need a
// .tik/.cert pair (included when fetchable, flagged otherwise).
//
// PFS0 layout (little-endian):
//   0x00 "PFS0" | 0x04 u32 file_count | 0x08 u32 string_table_size |
//   0x0C u32 reserved | 0x10 entries[0x18]{u64 data_off,u64 size,u32 name_off,
//   u32 rsvd} | string table (NUL-terminated) padded to 0x10 | file data.

#include "core/dump.hpp"

#include <cerrno>
#include <cstdarg>
#include "core/nsp_stream.hpp"
#include "core/fs.hpp"
#include "core/datetime.hpp"
#include "core/es.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <unordered_set>
#include <functional>
#include <vector>
#include <string>
#include <algorithm>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Dump {

#ifdef PLATFORM_SWITCH

// ── Persistent diagnostics: sdmc:/switch/GarageNX/logs/firmware_dump.log ────
// SDL_Log writes to stdout, which on hardware only exists over nxlink - the
// firmware dump was debugged once and produced NO log file anywhere, so the
// failure stayed unexplained. Every diagnostic on the firmware path goes
// through fw_log: one copy to the console (nxlink users see it live), one copy
// appended per line to the SD card. Append-per-line keeps the file readable
// even if the app dies mid-dump - exactly the case that needs diagnosing.
static void fw_log_line(const std::string& line) {
    // Self-heal the logs folder: a fresh SD card has no
    // sdmc:/switch/GarageNX/logs yet, and fopen("a") never creates parents.
    static const bool dir_ok =
        Fs::make_directory_recursive("sdmc:/switch/GarageNX/logs");
    (void)dir_ok;

    FILE* f = fopen("sdmc:/switch/GarageNX/logs/firmware_dump.log", "a");
    if (!f) return;  // no SD / no logs dir - the console copy still went out
    fprintf(f, "%s %s\n", Core::DateTime::sortable_stamp_now().c_str(),
            line.c_str());
    fclose(f);
}

// See dump.hpp for the contract.
void fw_log(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    SDL_Log("Firmware dump: %s", line);
    fw_log_line(line);
}

// Diagnostic: what happened with tik/cert on the last dump (shown in result).
static constexpr size_t STREAM_CHUNK = 4 * 1024 * 1024;  // 4 MB write buffer

// NOTE: the PFS0 header construction, NCA enumeration, rights-id detection and
// ticket/certificate handling that used to live here now live in
// core/nsp_stream.cpp, shared with the transports. Deliberately not duplicated.

bool dump_title_to_nsp(const Core::Ncm::Title& title,
                       const Core::Keys::Keyset& keys,
                       Progress& progress,
                       std::string& out_path) {
    // Thin wrapper over Core::NspStream - the SAME code path a PC client uses to
    // download a title over FTP/MTP/HTTP. Previously this function had its own copy
    // of the PFS0/ticket logic; two implementations of NSP construction was a
    // standing drift risk (an NSP that installs from one path and not the other),
    // so the streaming version - now verified on hardware - is the single source.
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

    // ── Split output ─────────────────────────────────────────────────────────
    // A Switch SD card is FAT32 unless reformatted, and FAT32 cannot hold a
    // file of 4 GiB or more - large dumps used to fail partway with a short
    // fwrite misreported as "SD full?". Past the limit, write a split archive:
    // a DIRECTORY named <name>.nsp containing 00, 01, 02 ... parts (the layout
    // DBI and Tinfoil produce and consume). Under the limit, write a single
    // ordinary file. Split is decided by size, not filesystem probing.
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
                // wrong one - errno distinguishes a genuinely full card (ENOSPC)
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

    // A partial NSP is worse than none - it looks installable and is not.
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
            snprintf(msg, sizeof(msg), "Packed %d files (%s) - %s",
                     (int)src->file_count(), type_str, note.c_str());
        progress.message = msg;
    }
    progress.done    = true;
    progress.running = false;
    return ok;
}

// Helper: format a 16-byte ContentId as 32-char lowercase hex string
static std::string format_content_id(const NcmContentId& id) {
    char buf[33];  // 32 hex chars + NUL
    for (int i = 0; i < 16; ++i) {
        sprintf(buf + i*2, "%02x", id.c[i]);
    }
    buf[32] = '\0';
    return std::string(buf);
}

// One piece of firmware content to dump. Gathered in a SINGLE enumeration pass
// so the totals reported to the user and the files actually written can never
// come from two different walks of the database (the previous two-pass shape
// re-enumerated with a fresh dedup set, so any transient failure between the
// passes silently diverged the count from the dump).
struct FirmwareContent {
    NcmContentId id{};
    uint8_t      content_type = 0;
    uint64_t     size = 0;        // authoritative: from the NcmContentInfo record
    bool         is_meta = false; // content_type == NcmContentType_Meta → .cnmt.nca
};

// Internal gather: the full content list (used by the dump).
// See enumerate_firmware_content() for the enumeration contract.
static bool gather_firmware_content(NcmStorageId storage_id,
                                    std::vector<FirmwareContent>& out,
                                    s32& meta_count,
                                    uint32_t& max_version) {
    NcmContentMetaDatabase db;
    const Result db_rc = ncmOpenContentMetaDatabase(&db, storage_id);
    if (R_FAILED(db_rc)) {
        // This used to return silently: the scan then reported 0 NCA(s), the
        // run never started, and nothing anywhere recorded WHY the database
        // was unavailable. Log the Result so hardware triage starts from
        // evidence instead of another blind theory.
        fw_log("Cannot open content meta database (storage %d): Result 0x%08X",
               storage_id, db_rc);
        return false;
    }
    fw_log("Content meta database opened (storage %d)", storage_id);

    constexpr NcmContentMetaType FIRMWARE_TYPES[] = {
        NcmContentMetaType_SystemProgram,
        NcmContentMetaType_SystemData,
        NcmContentMetaType_SystemUpdate,
        NcmContentMetaType_BootImagePackage,
        NcmContentMetaType_BootImagePackageSafe
    };

    std::unordered_set<NcmContentId,
                       Core::Ncm::NcmContentIdHash,
                       Core::Ncm::NcmContentIdEqual> seen;

    constexpr s32 WINDOW = 256;
    std::vector<NcmContentMetaKey> keys_buf(WINDOW);

    for (size_t ft = 0; ft < sizeof(FIRMWARE_TYPES) / sizeof(FIRMWARE_TYPES[0]); ++ft) {
        s32 total = 0, written = 0;
        Result rc = ncmContentMetaDatabaseList(&db, &total, &written,
                                               keys_buf.data(), WINDOW,
                                               FIRMWARE_TYPES[ft],
                                               0, 0, UINT64_MAX,
                                               NcmContentInstallType_Full);
        if (R_FAILED(rc)) {
            fw_log("ncmContentMetaDatabaseList failed for meta type %d: Result 0x%08X",
                   FIRMWARE_TYPES[ft], rc);
            continue;
        }
        if (written <= 0) continue;
        fw_log("Found %d meta record(s) for meta type %d", written, FIRMWARE_TYPES[ft]);
        meta_count += written;

        for (s32 i = 0; i < written; ++i) {
            const NcmContentMetaKey& k = keys_buf[i];
            if (k.version > max_version) max_version = k.version;

            s32 coff = 0;
            for (;;) {
                NcmContentInfo infos[16];
                s32 cwritten = 0;
                Result ci_rc = ncmContentMetaDatabaseListContentInfo(
                        &db, &cwritten, infos,
                        (s32)(sizeof(infos) / sizeof(infos[0])), &k, coff);
                if (R_FAILED(ci_rc)) {
                    fw_log("ncmContentMetaDatabaseListContentInfo failed at coff=%d: Result 0x%08X",
                           coff, ci_rc);
                    break;
                }
                if (cwritten <= 0) break;

                for (s32 ci = 0; ci < cwritten; ++ci) {
                    const NcmContentInfo& info = infos[ci];
                    if (seen.find(info.content_id) != seen.end()) continue;
                    seen.insert(info.content_id);

                    FirmwareContent e;
                    e.id           = info.content_id;
                    e.content_type = info.content_type;
                    e.is_meta      = (info.content_type == NcmContentType_Meta);
                    u64 csz = 0;
                    ncmContentInfoSizeToU64(&info, &csz);
                    e.size = csz;
                    out.push_back(e);
                }
                coff += cwritten;
            }
        }
    }

    ncmContentMetaDatabaseClose(&db);
    return true;
}

// ONE definition of firmware enumeration, shared by the Tools-menu dry-run and
// the dump itself - see dump.hpp for the full contract. This is the
// meta-database view of firmware: what is REGISTERED. The dump additionally
// logs the storage-side view (log_storage_cross_check) so a gap between the two
// is visible in the log instead of only in a missing file.
FirmwareScan enumerate_firmware_content(Core::Ncm::Storage storage) {
    fw_log("Scan (dry-run) requested (storage %d)", to_ncm_storage_id(storage));
    FirmwareScan scan;
    std::vector<FirmwareContent> contents;
    s32      meta_count  = 0;
    uint32_t max_version = 0;   // unused here; the dump derives the output dir
    if (!gather_firmware_content(to_ncm_storage_id(storage),
                                 contents, meta_count, max_version)) {
        fw_log("Scan aborted: storage or meta database unavailable - reporting zeros");
        return scan;  // storage not present / meta db unavailable - zeros, not an error
    }
    scan.meta_count = meta_count;
    scan.unique_ncas = (int)contents.size();
    for (const auto& e : contents) scan.total_bytes += e.size;
    fw_log("Scan result: %d meta record(s), %d unique NCA(s), %llu byte(s); max meta version %u",
           scan.meta_count, scan.unique_ncas, (unsigned long long)scan.total_bytes,
           (unsigned)max_version);
    return scan;
}

// Storage-side cross-check: how many NCAs are physically present in the content
// storage, referenced or not. The meta-walk only counts what firmware metas
// REGISTER; the difference between the two views is where a "scan finds fewer
// NCAs than the console holds" discrepancy lives - logging both turns that
// question into a number on the next hardware run. Uses
// ncmContentStorageListContentId (hardware-proven in the orphaned-content op).
static void log_storage_cross_check(NcmContentStorage* cs,
                                    const std::vector<FirmwareContent>& referenced,
                                    bool have_cs) {
    if (!have_cs) {
        fw_log("Storage cross-check skipped: content storage unavailable");
        return;
    }

    std::unordered_set<NcmContentId,
                       Core::Ncm::NcmContentIdHash,
                       Core::Ncm::NcmContentIdEqual> ref;
    for (const auto& e : referenced) ref.insert(e.id);

    constexpr s32 WINDOW = 256;
    std::vector<NcmContentId> ids(WINDOW);
    s32 in_storage = 0, unreferenced = 0;
    s32 offset = 0;
    for (;;) {
        s32 written = 0;
        Result rc = ncmContentStorageListContentId(cs, ids.data(), WINDOW, &written, offset);
        if (R_FAILED(rc)) {
            fw_log("ncmContentStorageListContentId failed at offset %d: Result 0x%08X",
                   offset, rc);
            break;
        }
        if (written <= 0) break;
        if (written > WINDOW) written = WINDOW;  // defensive
        for (s32 i = 0; i < written; ++i) {
            ++in_storage;
            if (ref.find(ids[static_cast<size_t>(i)]) == ref.end()) ++unreferenced;
        }
        if (written < WINDOW) break;  // last page
        offset += written;
    }

    fw_log("Storage cross-check: %d NCA(s) present in content storage, %d referenced by firmware metas, %d present-but-unreferenced",
           in_storage, (int)ref.size(), unreferenced);
}

// Dump all NCAs from BuiltInSystem storage to individual .nca files
bool dump_ncas_from_storage(Core::Ncm::Storage storage,
                            const Core::Keys::Keyset& keys,
                            Progress& progress,
                            std::string& out_dir) {
    (void)keys;  // Not needed for raw NCA extraction

    NcmStorageId storage_id = to_ncm_storage_id(storage);

    fw_log("=== Dump run started (storage %d) ===", storage_id);
    fw_log("Opening content storage");

    // Open the content storage FIRST: the cross-check needs it, and its Result
    // is worth having even when everything downstream works.
    NcmContentStorage cs;
    Result cs_rc = ncmOpenContentStorage(&cs, storage_id);
    bool have_cs = R_SUCCEEDED(cs_rc);
    if (R_FAILED(cs_rc)) {
        fw_log("Warning: Cannot open content storage: Result 0x%08X (cross-check will be skipped)", cs_rc);
    } else {
        fw_log("Content storage opened");
    }

    // ONE enumeration pass - the dump loop below works from this list. The
    // Tools-menu dry-run counts the SAME set through the same helper, so the
    // number the user confirmed and the number of files written cannot drift
    // apart the way the two hand-copied loops used to.
    std::vector<FirmwareContent> contents;
    s32      meta_count  = 0;
    uint32_t max_version = 0;
    if (!gather_firmware_content(storage_id, contents, meta_count, max_version)) {
        progress.message = "Cannot open content meta database";
        progress.done    = true;
        if (have_cs) ncmContentStorageClose(&cs);
        return false;
    }

    const s32 total_ncas = (s32)contents.size();
    u64 total_bytes = 0;
    for (const auto& e : contents) total_bytes += e.size;

    fw_log("Enumeration complete: %d meta record(s), %d unique NCA(s), %llu byte(s); max meta version %u",
           meta_count, total_ncas, (unsigned long long)total_bytes, (unsigned)max_version);

    // Log the storage-side view too (see the helper's comment for why).
    log_storage_cross_check(&cs, contents, have_cs);

    if (total_ncas == 0) {
        progress.message = "No firmware NCAs found";
        progress.done = true;
        if (have_cs) ncmContentStorageClose(&cs);
        return false;
    }

    // Output directory: sdmc:/switch/GarageNX/dumps/firmware/<version>/
    // Version format: plain integer (no 'v' prefix)
    char version_str[16];
    snprintf(version_str, sizeof(version_str), "%u", max_version);
    out_dir = "sdmc:/switch/GarageNX/dumps/firmware/" + std::string(version_str);

    fw_log("Output directory: %s", out_dir.c_str());

    // mkdir creates only the final component, so single-level make_directory()
    // failed on every first-ever attempt (dumps/firmware not yet present) and
    // aborted the dump before a byte was written - the original "firmware fails
    // to dump" bug. Create the whole chain instead.
    if (!Fs::make_directory_recursive(out_dir)) {
        progress.message = "Cannot create output directory";
        fw_log("Cannot create output directory %s (errno %d) - every level must be creatable",
               out_dir.c_str(), errno);
        progress.done = true;
        if (have_cs) ncmContentStorageClose(&cs);
        return false;
    }
    fw_log("Output directory ready");

    // Initialize progress
    progress.bytes_total = total_bytes;
    progress.ncas_total = total_ncas;
    progress.ncas_done = 0;

    // Track files created during this operation (for cleanup on failure)
    std::vector<std::string> created_files;

    bool ok = true;
    int  ncas_done = 0;
    std::vector<uint8_t> buf(4 * 1024 * 1024);  // 4 MB buffer, reused per NCA

    for (const FirmwareContent& e : contents) {
        if (progress.cancel.load()) {
            ok = false;
            progress.message = "Cancelled";
            fw_log("Cancelled at NCA %d of %d", ncas_done + 1, total_ncas);
            break;
        }

        // Filename: 32-char lowercase hex content id. NcmContentType_Meta = 0
        // means this is the content manifest (.cnmt.nca); everything else .nca.
        std::string filename = format_content_id(e.id);
        filename += e.is_meta ? ".cnmt.nca" : ".nca";
        const std::string filepath = out_dir + "/" + filename;

        progress.current_file = filename;
        fw_log("Dumping %d/%d: %s (%llu bytes)",
               ncas_done + 1, total_ncas, filename.c_str(),
               (unsigned long long)e.size);

        FILE* out = fopen(filepath.c_str(), "wb");
        if (!out) {
            ok = false;
            progress.message = "Cannot create output file";
            fw_log("fopen(\"%s\", \"wb\") failed (errno %d)", filepath.c_str(), errno);
            break;
        }
        created_files.push_back(filepath);

        // Read exactly the size the meta record declared. The authoritative size
        // comes from the NcmContentInfo record (gathered above), NOT from a
        // per-NCA storage query and NOT from "read until EOF": the read call
        // returns a Result, not a count, so an EOF-driven loop cannot tell a
        // short fill (stale buffer bytes) from real data - the old code wrote
        // the FULL buffer on every iteration and only bounded the loop by a
        // size query that could fail.
        s64 bytes_written = 0;
        bool file_ok = true;
        while (bytes_written < (s64)e.size) {
            if (progress.cancel.load()) { file_ok = false; progress.message = "Cancelled"; break; }

            const size_t to_read =
                (size_t)std::min<uint64_t>(buf.size(), e.size - (uint64_t)bytes_written);

            Result r = ncmContentStorageReadContentIdFile(
                &cs, buf.data(), to_read, &e.id, bytes_written);
            if (R_FAILED(r)) {
                file_ok = false;
                char m[128];
                snprintf(m, sizeof(m), "NCA read failed (%s, offset %lld, Result 0x%08X)",
                         filename.c_str(), bytes_written, (unsigned)r);
                progress.message = m;
                fw_log("ncmContentStorageReadContentIdFile failed for %s at offset %lld: Result 0x%08X",
                       filename.c_str(), bytes_written, r);
                break;
            }

            size_t wrote = fwrite(buf.data(), 1, to_read, out);
            if (wrote != to_read) {
                file_ok = false;
                // Same diagnosis dump_title_to_nsp uses: errno separates a
                // genuinely full card (ENOSPC) from the FAT32 size limit (EFBIG).
                const int err = errno;
                if (err == ENOSPC)      progress.message = "SD card is full";
                else if (err == EFBIG)  progress.message = "File too large for this SD card";
                else {
                    char m[96];
                    snprintf(m, sizeof(m), "Write failed (errno %d)", err);
                    progress.message = m;
                }
                fw_log("fwrite failed for %s: wrote %zu of %zu bytes (errno %d)",
                       filename.c_str(), wrote, to_read, err);
                break;
            }

            bytes_written += (s64)wrote;
            progress.bytes_done += wrote;
        }

        fclose(out);

        if (file_ok && (uint64_t)bytes_written != e.size) {
            file_ok = false;
            progress.message = "Incomplete NCA (read fewer bytes than recorded size)";
            fw_log("%s incomplete: wrote %llu of %llu bytes",
                   filename.c_str(), (unsigned long long)bytes_written,
                   (unsigned long long)e.size);
        }

        if (!file_ok) {
            ok = false;
            // Clean up the file we just created - a partial NCA looks real.
            remove(filepath.c_str());
            auto it = std::find(created_files.begin(), created_files.end(), filepath);
            if (it != created_files.end()) created_files.erase(it);
            break;
        }

        ++ncas_done;
        progress.ncas_done = ncas_done;
    }

    if (have_cs) ncmContentStorageClose(&cs);

    progress.done = true;
    progress.success = ok;

    fw_log("Dump complete: %s (%d/%d NCAs written)", ok ? "SUCCESS" : "FAILED",
           ncas_done, total_ncas);

    if (ok) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Dumped %d NCA(s) to %s", ncas_done, out_dir.c_str());
        progress.message = msg;
        // Clear created_files since all were successfully completed
        created_files.clear();
    } else {
        // Clean up any files we created - a partial firmware dump masquerades
        // as a complete one. Log what is being removed so the log reconciles
        // with what is actually left on the card.
        for (const auto& f : created_files) {
            fw_log("Cleanup: removing partial file %s", f.c_str());
            remove(f.c_str());
        }
        if (progress.message.empty()) {
            progress.message = "Dump failed for unknown reason";
        }
    }

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

// PC stub for firmware dump
bool dump_ncas_from_storage(Core::Ncm::Storage, const Core::Keys::Keyset&,
                            Progress& progress, std::string& out_dir) {
    progress.reset();
    out_dir = "sdmc:/switch/GarageNX/dumps/firmware/0";
    progress.message = "Firmware dump not available on PC stub";
    progress.done = true;
    return false;
}

// PC no-op - matches the declaration in dump.hpp; PC never dumps firmware.
void fw_log(const char*, ...) {}

#endif

} // namespace Core::Dump