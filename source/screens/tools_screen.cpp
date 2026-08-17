// source/screens/tools_screen.cpp

#include "screens/tools_screen.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/layout.hpp"
#include "ui/input.hpp"
#include "lang/localization.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

#include "core/ncm.hpp"
#include "core/es.hpp"
#include "core/save_mount.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>

namespace {

// ── Operation: clean leftover NCM placeholders ──────────────────────────────
// Placeholders are the temporary files ncm writes during an install. A clean
// install deletes its own; a cancelled or crashed install can leave them behind,
// consuming space while referenced by nothing. Listing + deleting them is safe:
// a placeholder is by definition not yet a registered, in-use content file.

#ifdef PLATFORM_SWITCH
// Count placeholders across the writable content storages (SD + NAND user).
int placeholder_scan(u64* out_bytes) {
    const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    int total = 0;
    u64 bytes = 0;
    for (NcmStorageId sid : storages) {
        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, sid))) continue;
        // List in pages; the count can exceed a single buffer.
        NcmPlaceHolderId ids[64];
        s32 got = 0, offset = 0;
        do {
            got = 0;
            if (R_FAILED(ncmContentStorageListPlaceHolder(&cs, ids, 64, &got))) break;
            for (s32 i = 0; i < got; ++i) {
                total++;
                s64 sz = 0;
                if (R_SUCCEEDED(ncmContentStorageGetSizeFromPlaceHolderId(&cs, &sz, &ids[i])))
                    bytes += (u64)sz;
            }
            offset += got;
        } while (got == 64);
        ncmContentStorageClose(&cs);
    }
    if (out_bytes) *out_bytes = bytes;
    return total;
}

int placeholder_delete() {
    const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    int deleted = 0;
    for (NcmStorageId sid : storages) {
        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, sid))) continue;
        NcmPlaceHolderId ids[64];
        s32 got = 0;
        // Re-list after each page: deleting shrinks the set, so always take the
        // current head rather than tracking a moving offset.
        do {
            got = 0;
            if (R_FAILED(ncmContentStorageListPlaceHolder(&cs, ids, 64, &got))) break;
            for (s32 i = 0; i < got; ++i)
                if (R_SUCCEEDED(ncmContentStorageDeletePlaceHolder(&cs, &ids[i]))) ++deleted;
        } while (got == 64);
        ncmContentStorageClose(&cs);
    }
    return deleted;
}
#endif  // PLATFORM_SWITCH

// ── Operation: clean superseded (old) game update files ────────────────────
// A Patch (game update) title's content-meta "id" is shared by every version of
// that patch — only `version` differs, exactly like an app version bump. Once a
// newer patch for a title is installed, every older patch for the same id is
// dead weight: it cannot be launched (the newest version always wins) and only
// costs space. This walks every installed Patch meta, keeps the highest version
// per id per storage, and flags the rest.
//
// Reuses the SAME enumeration shape as Core::Ncm::list_storage() (open db,
// ncmContentMetaDatabaseList in a WINDOW=256 page, ncmContentMetaDatabaseList-
// ContentInfo per meta) and the SAME delete order the installer's rollback path
// uses (delete each referenced content, then remove the meta record, then
// commit) — see source/install/installer.cpp around the meta-commit rollback.
// No libnx symbol here is new to this codebase.

#ifdef PLATFORM_SWITCH
struct MetaHit {
    NcmContentMetaKey key;
    NcmStorageId       storage;
};

// Every installed Patch across SD + NAND user, tagged with its storage.
std::vector<MetaHit> list_patches() {
    const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    std::vector<MetaHit> out;
    for (NcmStorageId sid : storages) {
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid))) continue;

        constexpr s32 WINDOW = 256;
        std::vector<NcmContentMetaKey> keys(WINDOW);
        s32 total = 0, written = 0;
        Result rc = ncmContentMetaDatabaseList(&db, &total, &written,
                                               keys.data(), WINDOW,
                                               NcmContentMetaType_Patch,
                                               0 /*application_id filter: 0=any*/,
                                               0 /*min*/, UINT64_MAX /*max*/,
                                               NcmContentInstallType_Full);
        if (R_SUCCEEDED(rc)) {
            s32 count = (written < WINDOW) ? written : WINDOW;
            for (s32 i = 0; i < count; ++i) out.push_back({ keys[i], sid });
        }
        ncmContentMetaDatabaseClose(&db);
    }
    return out;
}

// Of every installed patch, the ones that are NOT the highest version for their
// (storage, id) — i.e. the deletion candidates. Two installs sharing an id on
// DIFFERENT storages are independent, same as everywhere else in this file that
// treats SD and NAND as separate worlds.
std::vector<MetaHit> superseded_updates() {
    std::vector<MetaHit> all = list_patches();
    std::vector<MetaHit> losers;
    for (size_t i = 0; i < all.size(); ++i) {
        bool beaten = false;
        for (size_t j = 0; j < all.size(); ++j) {
            if (i == j) continue;
            if (all[j].storage != all[i].storage) continue;
            if (all[j].key.id != all[i].key.id) continue;
            if (all[j].key.version > all[i].key.version) { beaten = true; break; }
        }
        if (beaten) losers.push_back(all[i]);
    }
    return losers;
}

uint64_t content_size_for(NcmContentMetaDatabase& db, const NcmContentMetaKey& key) {
    uint64_t total = 0;
    s32 coff = 0;
    for (;;) {
        NcmContentInfo infos[16];
        s32 cwritten = 0;
        if (R_FAILED(ncmContentMetaDatabaseListContentInfo(
                &db, &cwritten, infos, (s32)(sizeof(infos) / sizeof(infos[0])), &key, coff)))
            break;
        if (cwritten <= 0) break;
        for (s32 ci = 0; ci < cwritten; ++ci) {
            u64 csz = 0;
            ncmContentInfoSizeToU64(&infos[ci], &csz);
            total += (uint64_t)csz;
        }
        coff += cwritten;
    }
    return total;
}

int superseded_scan(u64* out_bytes) {
    std::vector<MetaHit> losers = superseded_updates();
    if (out_bytes) {
        u64 bytes = 0;
        const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
        for (NcmStorageId sid : storages) {
            NcmContentMetaDatabase db;
            if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid))) continue;
            for (const auto& hit : losers)
                if (hit.storage == sid) bytes += content_size_for(db, hit.key);
            ncmContentMetaDatabaseClose(&db);
        }
        *out_bytes = bytes;
    }
    return (int)losers.size();
}

int superseded_delete() {
    // Re-list right before deleting rather than reusing the scan's result —
    // same reason placeholder_delete() re-lists each page instead of trusting a
    // stale offset: state can move between the dry run and the held confirm.
    std::vector<MetaHit> losers = superseded_updates();
    const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    int deleted = 0;

    for (NcmStorageId sid : storages) {
        bool any = false;
        for (const auto& hit : losers) if (hit.storage == sid) { any = true; break; }
        if (!any) continue;

        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid))) continue;
        NcmContentStorage cs;
        bool have_cs = R_SUCCEEDED(ncmOpenContentStorage(&cs, sid));

        for (const auto& hit : losers) {
            if (hit.storage != sid) continue;

            // Delete every content the meta references first, THEN remove the
            // meta record — the same order installer.cpp's rollback path uses
            // on a failed commit.
            if (have_cs) {
                s32 coff = 0;
                for (;;) {
                    NcmContentInfo infos[16];
                    s32 cwritten = 0;
                    if (R_FAILED(ncmContentMetaDatabaseListContentInfo(
                            &db, &cwritten, infos,
                            (s32)(sizeof(infos) / sizeof(infos[0])), &hit.key, coff)))
                        break;
                    if (cwritten <= 0) break;
                    for (s32 ci = 0; ci < cwritten; ++ci) {
                        NcmContentId cid = infos[ci].content_id;
                        bool has = false;
                        if (R_SUCCEEDED(ncmContentStorageHas(&cs, &has, &cid)) && has)
                            ncmContentStorageDelete(&cs, &cid);
                    }
                    coff += cwritten;
                }
            }

            if (R_SUCCEEDED(ncmContentMetaDatabaseRemove(&db, &hit.key))) {
                ncmContentMetaDatabaseCommit(&db);
                ++deleted;
            }
        }

        if (have_cs) ncmContentStorageClose(&cs);
        ncmContentMetaDatabaseClose(&db);
    }

    if (deleted > 0) Core::Ncm::mark_titles_dirty();
    return deleted;
}
#endif  // PLATFORM_SWITCH

// ── Operation: clear downloaded (not-yet-applied) system-update data ───────
// A background OS update that finished downloading but hasn't been applied
// yet sits as a queued task tracked through `ns`'s ISystemUpdateControl. This
// clears that queued task via nssuDestroySystemUpdateTask() — confirmed real
// against the user's own header (no nsDeleteRedundantSystemUpdate exists in
// this libnx; that was checked and ruled out first). Deliberately NOT
// implemented as "enumerate ncm SystemUpdate metas, keep the highest version"
// the way superseded game patches are: which ncm SystemUpdate record
// corresponds to the currently-applied firmware isn't reliably inferable that
// way, and getting it wrong here is a different order of risk than a game
// patch. nssuDestroySystemUpdateTask only touches nim's queued-task state, not
// any ncm-installed content and not the currently running firmware — there is
// no path from this call to the active OS. `ns`/`nssu` are already
// initialized for the app's whole lifetime in main.cpp (nsInitialize /
// nssuInitialize at startup, nssuExit / nsExit at shutdown) — this op adds no
// new service init, it just uses what's already running.
#ifdef PLATFORM_SWITCH
int system_update_scan() {
    NsSystemUpdateControl c{};
    if (R_FAILED(nssuOpenSystemUpdateControl(&c))) return 0;
    bool has = false;
    Result rc = nssuControlHasDownloaded(&c, &has);
    nssuControlClose(&c);
    return (R_SUCCEEDED(rc) && has) ? 1 : 0;
}

int system_update_delete() {
    return R_SUCCEEDED(nssuDestroySystemUpdateTask()) ? 1 : 0;
}
#endif  // PLATFORM_SWITCH

// ── Operation: clear the erpt_reports folder ────────────────────────────────
// Under Atmosphère, error/crash reports are redirected entirely to the SD card
// at sdmc:/atmosphere/erpt_reports/ — NOT committed to any system save data
// (confirmed against Atmosphère's own docs before writing this; the earlier
// plan of treating this as a system-savedata op was wrong and dropped). This
// is genuinely plain SD file cleanup: same opendir/readdir/closedir shape as
// read_serial_from_backup() in core/system.cpp, same stat+unlink shape as the
// HTTP server's DELETE handler in services/http_server.cpp. No libnx symbol,
// no new service. Atmosphère itself already auto-clears this folder past 1000
// files on boot; this is a manual trigger for the same cleanup, not something
// novel or riskier than what the CFW already does unattended.
//
// Only unlinks regular files directly inside the folder and leaves the folder
// itself in place (Atmosphère expects it to exist and keeps writing to it) —
// does not recurse into subdirectories, since erpt reports are flat files and
// finding one would be unexpected.
#ifdef PLATFORM_SWITCH
namespace erpt {
constexpr const char* kDir = "sdmc:/atmosphere/erpt_reports/";
}

int erpt_scan(u64* out_bytes) {
    DIR* dir = opendir(erpt::kDir);
    if (!dir) return 0;

    int count = 0;
    u64 bytes = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue; // skip "." / ".." (and dotfiles, defensively)
        std::string path = std::string(erpt::kDir) + ent->d_name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        ++count;
        bytes += (u64)st.st_size;
    }
    closedir(dir);

    if (out_bytes) *out_bytes = bytes;
    return count;
}

int erpt_delete() {
    DIR* dir = opendir(erpt::kDir);
    if (!dir) return 0;

    int deleted = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string path = std::string(erpt::kDir) + ent->d_name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (::unlink(path.c_str()) == 0) ++deleted;
    }
    closedir(dir);
    return deleted;
}
#endif  // PLATFORM_SWITCH

// ── Operation: clean orphaned content ───────────────────────────────────────
// A previous attempt at this op called ncmContentStorageList(), which does not
// exist in this libnx at all — built, shipped, and reverted mid-session. This
// rewrite uses the real functions, confirmed against the user's actual
// ncm.h (not memory, not a web fetch alone — grepped and pasted back):
//   ncmContentStorageGetContentCount / ncmContentStorageListContentId
//     — enumerates NCA content IDs physically present in a storage. This is
//       the real counterpart to the nonexistent call from the reverted
//       attempt.
//   ncmContentMetaDatabaseLookupOrphanContent
//     — purpose-built for exactly this: hand it content IDs, it reports which
//       ones aren't referenced by ANY content-meta record. This does the
//       storage/meta cross-reference inside ncm itself rather than this code
//       re-deriving "is this referenced anywhere" by hand — a wrong homemade
//       cross-reference here would risk deleting content that's still in use;
//       asking ncm directly removes that entire class of mistake.
// An orphan has no meta record pointing to it at all, so deleting it is
// ncmContentStorageDelete only — there is no meta entry to Remove/Commit here,
// unlike superseded updates (which are live meta records being retired).
#ifdef PLATFORM_SWITCH
struct OrphanHit {
    NcmContentId content_id;
    NcmStorageId storage;
};

std::vector<OrphanHit> list_orphans() {
    const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    std::vector<OrphanHit> out;

    for (NcmStorageId sid : storages) {
        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, sid))) continue;
        NcmContentMetaDatabase db;
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, sid))) {
            ncmContentStorageClose(&cs);
            continue;
        }

        constexpr s32 WINDOW = 256;
        std::vector<NcmContentId> ids(WINDOW);
        std::vector<bool> orphaned_buf(WINDOW);
        s32 offset = 0;
        for (;;) {
            s32 written = 0;
            Result rc = ncmContentStorageListContentId(&cs, ids.data(), WINDOW,
                                                        &written, offset);
            if (R_FAILED(rc) || written <= 0) break;
            if (written > WINDOW) written = WINDOW; // defensive

            // LookupOrphanContent wants a bool array matching the content_id
            // array 1:1. std::vector<bool> is bit-packed, not a real bool
            // array, so this uses std::array<bool,...> — a genuine bool[],
            // .data() gives a real bool* with no cast needed.
            std::array<bool, WINDOW> orphaned{};
            Result orc = ncmContentMetaDatabaseLookupOrphanContent(
                &db, orphaned.data(), ids.data(), written);
            if (R_SUCCEEDED(orc)) {
                for (s32 i = 0; i < written; ++i)
                    if (orphaned[static_cast<size_t>(i)])
                        out.push_back({ ids[static_cast<size_t>(i)], sid });
            }

            if (written < WINDOW) break; // last page
            offset += written;
        }

        ncmContentMetaDatabaseClose(&db);
        ncmContentStorageClose(&cs);
    }
    return out;
}

int orphaned_scan(u64* out_bytes) {
    std::vector<OrphanHit> hits = list_orphans();
    if (out_bytes) {
        u64 bytes = 0;
        const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
        for (NcmStorageId sid : storages) {
            NcmContentStorage cs;
            if (R_FAILED(ncmOpenContentStorage(&cs, sid))) continue;
            for (const auto& h : hits) {
                if (h.storage != sid) continue;
                s64 sz = 0;
                ncmContentStorageGetSizeFromContentId(&cs, &sz, &h.content_id);
                if (sz > 0) bytes += (u64)sz;
            }
            ncmContentStorageClose(&cs);
        }
        *out_bytes = bytes;
    }
    return (int)hits.size();
}

int orphaned_delete() {
    // Re-list right before deleting, same reasoning as every other op in this
    // file: state can move between the dry-run and the held confirm.
    std::vector<OrphanHit> hits = list_orphans();
    const NcmStorageId storages[] = { NcmStorageId_SdCard, NcmStorageId_BuiltInUser };
    int deleted = 0;

    for (NcmStorageId sid : storages) {
        bool any = false;
        for (const auto& h : hits) if (h.storage == sid) { any = true; break; }
        if (!any) continue;

        NcmContentStorage cs;
        if (R_FAILED(ncmOpenContentStorage(&cs, sid))) continue;

        for (const auto& h : hits) {
            if (h.storage != sid) continue;
            bool has = false;
            if (R_SUCCEEDED(ncmContentStorageHas(&cs, &has, &h.content_id)) && has) {
                if (R_SUCCEEDED(ncmContentStorageDelete(&cs, &h.content_id))) ++deleted;
            }
        }

        ncmContentStorageClose(&cs);
    }

    if (deleted > 0) Core::Ncm::mark_titles_dirty();
    return deleted;
}
#endif  // PLATFORM_SWITCH

// ── Operation: clean unused tickets (no matching installed title) ──────────
// A ticket is "unused" here if its derived title id matches NO installed
// content-meta record of ANY type — Application, Patch, or AddOnContent — on
// either storage. That's deliberately the broadest possible "still needed"
// set, not just Applications: patches don't normally carry their own separate
// rights ID in the standard titlekey scheme (they reuse the base
// application's), so folding Patch ids into the "known" set costs nothing —
// no real ticket should ever coincidentally match one — but it biases the
// check toward NOT deleting, which is the direction to be wrong in. This
// project's own removed "Installed Tickets" screen carried the exact warning
// this op has to live up to: "Removing a ticket may break the associated
// title" (assets/lang/en.json, ticket_list.confirm_body).
//
// If Core::Ncm::list_all() itself fails (ok == false), this returns an EMPTY
// candidate list rather than treating "couldn't enumerate installed titles"
// as "nothing is installed" — the latter would flag every ticket on the
// console as unused, which is exactly the failure mode this whole op exists
// to avoid. Fail closed: unknown means touch nothing.
//
// Common tickets only — see core/es.hpp; this codebase has no personalized-
// ticket support. Uses Core::Es::list_common_tickets() (already proven by the
// NSP dump path) and the new Core::Es::delete_ticket() (es cmd 3, no hardware
// mileage yet in this codebase — see that function's doc comment).
#ifdef PLATFORM_SWITCH
std::vector<std::array<uint8_t, 0x10>> unused_ticket_rights_ids() {
    bool ok = false;
    std::vector<Core::Ncm::Title> titles = Core::Ncm::list_all(&ok);
    std::vector<std::array<uint8_t, 0x10>> out;
    if (!ok) return out; // couldn't confirm what's installed — touch nothing

    std::unordered_set<uint64_t> known;
    for (const auto& t : titles) known.insert(t.meta_id);

    for (const auto& tik : Core::Es::list_common_tickets())
        if (known.find(tik.title_id) == known.end())
            out.push_back(tik.rights_id);
    return out;
}

int unused_ticket_scan() {
    return (int)unused_ticket_rights_ids().size();
}

int unused_ticket_delete() {
    // Re-derive right before deleting, same reasoning as every other op here:
    // installs and tickets can both move between the dry-run and the held
    // confirm.
    std::vector<std::array<uint8_t, 0x10>> rights_ids = unused_ticket_rights_ids();
    int deleted = 0;
    for (const auto& rid : rights_ids)
        if (Core::Es::delete_ticket(rid.data())) ++deleted;
    return deleted;
}
#endif  // PLATFORM_SWITCH

// ── Operation: delete saves of deleted users ────────────────────────────────
// A save-data record is a candidate here if its owning uid matches NO account
// list_users() currently reports.
//
// NOTE ON WHEN THIS ACTUALLY FINDS ANYTHING: standard deletion via Settings >
// System > Delete User removes that user's save data along with the account
// (confirmed on real hardware — an earlier version of this comment claimed
// the opposite and was wrong; corrected here rather than silently). So this
// op is NOT cleaning up the normal, expected byproduct of deleting a user the
// ordinary way — there usually isn't one. It exists for the narrower set of
// paths where an account can go away without its save data following: a
// corrupted/removed account record, an SD card moved between consoles with
// mismatched account state, or account removal through something other than
// the standard System Settings flow. Because the ordinary trigger doesn't
// reproduce it, the positive path (an actual orphaned save existing to find)
// has NOT been hardware-tested — only the negative path (no live users
// deleted, dry-run correctly finding nothing) is realistically testable
// on demand.
//
// DIRECT DELETE, NO PRE-SNAPSHOT — a deliberate product decision, not an
// oversight: the Danger modal's warning text is the safety net for this
// specific op, not a backup. This is UNLIKE Save Manager's own Delete flow for
// a live user's title, which snapshots first — that inconsistency is
// intentional, not a gap to "fix" later.
//
// Uses Core::SaveMount::delete_save_record() — the same call Save Manager's
// Delete button already exercises on real hardware, and independently
// confirmed this session against a real libnx fs.h.
// (Result fsDeleteSaveDataFileSystemBySaveDataSpaceId(FsSaveDataSpaceId,
// u64); ///< [2.0.0+] — see core/save_mount.cpp for the full note.)
//
// FAILS CLOSED the same way unused-ticket cleanup does: if
// Core::SaveMount::list_users(&ok) itself fails (ok == false), this returns an
// EMPTY candidate list rather than reading "couldn't enumerate live users" as
// "there are no live users, so everything else is orphaned" — the latter would
// try to delete every save-data record on the console.
#ifdef PLATFORM_SWITCH
std::vector<uint64_t> deleted_user_save_ids() {
    bool ok = false;
    std::vector<Core::SaveMount::User> users = Core::SaveMount::list_users(&ok);
    std::vector<uint64_t> out;
    if (!ok) return out; // couldn't confirm who's live — touch nothing

    std::set<std::pair<uint64_t, uint64_t>> live;
    for (const auto& u : users) live.insert({ u.uid_lo, u.uid_hi });

    for (const auto& s : Core::SaveMount::list_all_account_saves())
        if (live.find({ s.uid_lo, s.uid_hi }) == live.end())
            out.push_back(s.save_data_id);
    return out;
}

int deleted_user_saves_scan() {
    return (int)deleted_user_save_ids().size();
}

int deleted_user_saves_delete() {
    // Re-derive right before deleting, same reasoning as every other op here:
    // accounts and saves can both move between the dry-run and the held
    // confirm.
    std::vector<uint64_t> ids = deleted_user_save_ids();
    int deleted = 0;
    for (uint64_t id : ids)
        if (Core::SaveMount::delete_save_record(id)) ++deleted;
    return deleted;
}
#endif  // PLATFORM_SWITCH

// ── Operation: delete parental controls ─────────────────────────────────────
// Full reset of parental controls (PIN + all restrictions). Scoped to
// pctlDeleteParentalControls() ONLY — deliberately NOT pctlDeletePairing()
// (unlinking the mobile Parental Controls app), which every reference this
// was checked against treats as a distinct, separate action, not part of a
// "delete parental controls" reset. Bundling it in would silently do more
// than the label says.
//
// pctl.h in this project's own libnx is confirmed READ-ONLY — every declared
// function was listed (not just a targeted grep), and it's Count/Get/Is only,
// no Set/Delete of any kind. So this hand-rolls raw IPC against
// pctlGetServiceSession_Service() — which DOES exist in this project's own
// header, confirmed — the same shape core/es.cpp already uses for es
// commands libnx doesn't wrap.
//
// The command ID (1043) is not from Switchbrew or any official source —
// there is no official documentation for homebrew pctl use at all. It comes
// from ITotalJustice/Reset-Parental-Controls-NX, a real, working, open-source
// tool whose author directly annotated it "works" from testing — and, in the
// same source, explicitly marked the adjacent pctlSetPinCode / pctlGetPinCode
// / pctlUnlockRestrictionTemporarily as "doesn't work". That specificity is
// exactly the kind of empirical signal this session has treated as
// sufficient when no official header exists (the same standing as this
// project's own es.cpp command IDs, sourced from community documentation
// rather than an official Nintendo source) — it is not the same as guessing.
// What it is NOT: hardware-tested in THIS codebase. Treat the dry-run
// (parental_controls_enabled(), which uses a function already in this
// project's own confirmed-real pctl.h) as trustworthy once you've seen it
// report correctly, but test execute against a state you can afford to
// reconfigure before trusting it broadly — same caution as the ticket-delete
// op earlier this session.
#ifdef PLATFORM_SWITCH
bool parental_controls_enabled() {
    bool flag = false;
    if (R_FAILED(pctlIsRestrictionEnabled(&flag))) return false;
    return flag;
}

bool parental_controls_delete() {
    // Raw IPC, no input, no output — matches the "works"-annotated reference
    // implementation exactly. See the block comment above for sourcing.
    return R_SUCCEEDED(serviceDispatch(pctlGetServiceSession_Service(), 1043));
}
#endif  // PLATFORM_SWITCH

// ── Operation: delete Wi-Fi profiles ────────────────────────────────────────
// Deletes every SAVED USER Wi-Fi network (NetworkProfileType User only —
// deliberately excludes System/SsidList and Temporary profile types, which a
// cleanup tool has no business touching; this also matches what official
// Nintendo software itself does — Switchbrew's notes on cmd 7 record that
// sdknso hardcodes NetworkProfileType "User").
//
// libnx's nifm.h has NO enumeration function at all (confirmed by reading the
// full header — only GetCurrentNetworkProfile [the ACTIVE one] and
// GetNetworkProfile [needs an already-known Uuid] exist) and no delete
// function either. Both are hand-rolled here against
// nifmGetServiceSession_GeneralService() — real, confirmed in this project's
// own nifm.h — using command numbers and structure layouts from Switchbrew's
// own NIFM_services page (IGeneralService), fetched directly this session:
//
//   cmd 7  EnumerateNetworkProfiles — fully documented: input u8
//          NetworkProfileType, output array of SfNetworkProfileBasicInfo
//          (exact byte layout below, matches the wiki table field-for-field)
//          plus an output s32 total count.
//   cmd 10 RemoveNetworkProfile — the command NUMBER is confirmed real (it's
//          in Switchbrew's own command table), but no prose description of
//          its parameter shape was ever written for it, unlike its immediate
//          neighbors (cmd 8 GetNetworkProfile takes a Uuid; cmd 9
//          SetNetworkProfile returns one). This assumes the same Uuid-in
//          shape BY PATTERN, not confirmed prose — the one genuinely
//          unverified piece here. Failure mode if wrong is SAFE, not
//          dangerous: an IPC size/shape mismatch fails loudly (a Result
//          error), it does not silently target a different profile — the
//          Uuid passed is always read straight back out of this exact call's
//          own EnumerateNetworkProfiles output, never guessed or
//          reconstructed.
//
// No new service init: nifm is already running for the app's whole lifetime
// (main.cpp: nifmInitialize(NifmServiceType_User) / nifmExit()), and User-
// type enumeration/removal is exactly what a User-type session supports per
// Switchbrew's own note on cmd 7 ("any other NetworkProfileType requires
// nifm:a").
#ifdef PLATFORM_SWITCH
// Wire layout per Switchbrew's SfNetworkProfileBasicInfo table — packed, no
// compiler-inserted padding, since this is read directly out of an IPC output
// buffer byte-for-byte. static_assert below catches a layout mistake at
// compile time rather than a silent misread at runtime.
#pragma pack(push, 1)
struct RawNetworkProfileBasicInfo {
    uint8_t id[0x10];        // 0x00 Uuid — the handle RemoveNetworkProfile needs
    char    name[0x40];      // 0x10 NUL-terminated network name
    uint8_t profile_type;    // 0x50 NetworkProfileType
    uint8_t iface_type;      // 0x51 NetworkInterfaceType
    uint8_t ssid[0x21];      // 0x52 Ssid
    uint8_t auth;            // 0x73 Authentication
    uint8_t enc;             // 0x74 Encryption
};
#pragma pack(pop)
static_assert(sizeof(RawNetworkProfileBasicInfo) == 0x75,
             "must match Switchbrew's documented SfNetworkProfileBasicInfo exactly");

constexpr uint8_t kNifmProfileType_User = 1; // bit 0 per Switchbrew's NetworkProfileType table

Result nifmEnumerateNetworkProfilesRaw(uint8_t profile_type,
                                       RawNetworkProfileBasicInfo* out,
                                       s32 max_count, s32* out_total) {
    return serviceDispatchInOut(nifmGetServiceSession_GeneralService(), 7,
        profile_type, *out_total,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, sizeof(RawNetworkProfileBasicInfo) * (size_t)max_count } });
}

Result nifmRemoveNetworkProfileRaw(const uint8_t uuid[0x10]) {
    struct { uint8_t id[0x10]; } raw_uuid;
    std::memcpy(raw_uuid.id, uuid, 0x10);
    return serviceDispatchIn(nifmGetServiceSession_GeneralService(), 10, raw_uuid);
}

std::vector<std::array<uint8_t, 0x10>> wifi_profile_ids() {
    constexpr s32 WINDOW = 32; // generous — a console realistically has a handful
    std::vector<RawNetworkProfileBasicInfo> buf(WINDOW);
    s32 total = 0;
    std::vector<std::array<uint8_t, 0x10>> out;
    if (R_FAILED(nifmEnumerateNetworkProfilesRaw(kNifmProfileType_User, buf.data(),
                                                 WINDOW, &total)))
        return out;
    const s32 count = (total < WINDOW) ? total : WINDOW; // defensive cap
    for (s32 i = 0; i < count; ++i) {
        std::array<uint8_t, 0x10> id{};
        std::memcpy(id.data(), buf[(size_t)i].id, 0x10);
        out.push_back(id);
    }
    return out;
}

int wifi_profiles_scan() {
    return (int)wifi_profile_ids().size();
}

int wifi_profiles_delete() {
    // Re-enumerate right before deleting, same reasoning as every other op
    // here: profiles can change between the dry-run and the held confirm.
    std::vector<std::array<uint8_t, 0x10>> ids = wifi_profile_ids();
    int deleted = 0;
    for (const auto& id : ids)
        if (R_SUCCEEDED(nifmRemoveNetworkProfileRaw(id.data()))) ++deleted;
    return deleted;
}
#endif  // PLATFORM_SWITCH

std::string human_size(u64 bytes) {
    char b[32];
    if (bytes >= (1ull << 30))      std::snprintf(b, sizeof(b), "%.2f GB", bytes / double(1u << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(b, sizeof(b), "%.1f MB", bytes / double(1u << 20));
    else if (bytes >= (1ull << 10)) std::snprintf(b, sizeof(b), "%.0f KB", bytes / double(1u << 10));
    else                            std::snprintf(b, sizeof(b), "%llu B", (unsigned long long)bytes);
    return b;
}

}  // namespace

ToolsScreen::ToolsScreen() {
    // ── Cleanup: leftover install placeholders ──────────────────────────────
    m_ops.push_back({
        Lang::t("tools.clean_placeholders"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            u64 bytes = 0;
            r.count = placeholder_scan(&bytes);
            r.any   = r.count > 0;
            char b[96];
            std::snprintf(b, sizeof(b), "%d placeholder file(s) (%s)",
                          r.count, human_size(bytes).c_str());
            r.detail = b;
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = placeholder_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d placeholder file(s).", n);
            return b;
#else
            return "Removed 0 placeholder file(s).";
#endif
        }
    });

    // ── Cleanup: superseded (old) game update files ─────────────────────────
    m_ops.push_back({
        Lang::t("tools.clean_old_updates"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            u64 bytes = 0;
            r.count = superseded_scan(&bytes);
            r.any   = r.count > 0;
            char b[96];
            std::snprintf(b, sizeof(b), "%d old update file(s) (%s)",
                          r.count, human_size(bytes).c_str());
            r.detail = b;
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = superseded_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d old update file(s).", n);
            return b;
#else
            return "Removed 0 old update file(s).";
#endif
        }
    });

    // ── Cleanup: downloaded (not-yet-applied) system-update data ────────────
    m_ops.push_back({
        Lang::t("tools.clean_system_updates"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            r.count = system_update_scan();
            r.any   = r.count > 0;
            r.detail = r.any
                ? "A downloaded system update is queued and not yet applied."
                : "No downloaded system update pending.";
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            return system_update_delete()
                ? "Cleared the pending downloaded system update."
                : "Nothing to clear (or the clear failed).";
#else
            return "Nothing to clear (or the clear failed).";
#endif
        }
    });

    // ── Cleanup: erpt_reports folder ─────────────────────────────────────────
    m_ops.push_back({
        Lang::t("tools.clean_erpt"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            u64 bytes = 0;
            r.count = erpt_scan(&bytes);
            r.any   = r.count > 0;
            char b[96];
            std::snprintf(b, sizeof(b), "%d crash report(s) (%s)",
                          r.count, human_size(bytes).c_str());
            r.detail = b;
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = erpt_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d crash report(s).", n);
            return b;
#else
            return "Removed 0 crash report(s).";
#endif
        }
    });

    // ── Cleanup: orphaned content (present in storage, no meta reference) ───
    m_ops.push_back({
        Lang::t("tools.clean_orphaned_records"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            u64 bytes = 0;
            r.count = orphaned_scan(&bytes);
            r.any   = r.count > 0;
            char b[96];
            std::snprintf(b, sizeof(b), "%d orphaned file(s) (%s)",
                          r.count, human_size(bytes).c_str());
            r.detail = b;
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = orphaned_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d orphaned file(s).", n);
            return b;
#else
            return "Removed 0 orphaned file(s).";
#endif
        }
    });

    // ── Cleanup: unused tickets (no matching installed title) ───────────────
    m_ops.push_back({
        Lang::t("tools.clean_unused_tickets"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            r.count = unused_ticket_scan();
            r.any   = r.count > 0;
            char b[64];
            std::snprintf(b, sizeof(b), "%d unused ticket(s)", r.count);
            r.detail = b;
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = unused_ticket_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d unused ticket(s).", n);
            return b;
#else
            return "Removed 0 unused ticket(s).";
#endif
        }
    });

    // ── Cleanup: saves of deleted users ──────────────────────────────────────
    m_ops.push_back({
        Lang::t("tools.clean_deleted_user_saves"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            r.count = deleted_user_saves_scan();
            r.any   = r.count > 0;
            char b[64];
            std::snprintf(b, sizeof(b), "%d orphaned save(s)", r.count);
            r.detail = b;
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = deleted_user_saves_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d orphaned save(s).", n);
            return b;
#else
            return "Removed 0 orphaned save(s).";
#endif
        }
    });

    // ── Delete parental controls ─────────────────────────────────────────────
    m_ops.push_back({
        Lang::t("tools.delete_parental_controls"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            const bool enabled = parental_controls_enabled();
            r.any    = enabled;
            r.count  = enabled ? 1 : 0;
            r.detail = enabled ? Lang::t("tools.warn_parental_controls") : "";
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            return parental_controls_delete()
                ? "Parental controls deleted."
                : "Failed to delete parental controls.";
#else
            return "Failed to delete parental controls.";
#endif
        }
    });

    // ── Delete Wi-Fi profiles ─────────────────────────────────────────────────
    m_ops.push_back({
        Lang::t("tools.delete_wifi_profiles"),
        [] () -> ScanResult {
            ScanResult r;
#ifdef PLATFORM_SWITCH
            r.count  = wifi_profiles_scan();
            r.any    = r.count > 0;
            r.detail = r.any ? Lang::t("tools.warn_wifi_profiles") : "";
#endif
            return r;
        },
        [] (const ScanResult&) -> std::string {
#ifdef PLATFORM_SWITCH
            int n = wifi_profiles_delete();
            char b[64];
            std::snprintf(b, sizeof(b), "Removed %d Wi-Fi profile(s).", n);
            return b;
#else
            return "Removed 0 Wi-Fi profile(s).";
#endif
        }
    });

    std::vector<Widgets::ListItem> rows;
    for (const auto& op : m_ops) {
        Widgets::ListItem row;
        row.label = op.label;
        rows.push_back(row);
    }
    m_list.set_items(std::move(rows));
}

void ToolsScreen::select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_ops.size())) return;

    m_pending_scan = m_ops[idx].scan();
    if (!m_pending_scan.any) {
        Modal::show({ m_ops[idx].label,
                      Lang::t("tools.nothing_to_do"),
                      Modal::Kind::Info, Lang::t("modal.ok"), "" });
        m_pending = -1;
        return;
    }

    std::string body = m_pending_scan.detail + "\n" + Lang::t("tools.confirm_body");
    Modal::show({ m_ops[idx].label, body,
                  Modal::Kind::Danger,
                  Lang::t("tools.confirm_remove"),
                  Lang::t("modal.cancel") });
    m_pending = idx;
}

void ToolsScreen::on_modal_result(int result) {
    if (m_pending < 0) return;
    const int idx = m_pending;
    m_pending = -1;

    if (static_cast<Modal::Result>(result) != Modal::Result::Confirmed) return;

    const std::string msg = m_ops[idx].run(m_pending_scan);
    Modal::show({ m_ops[idx].label, msg,
                  Modal::Kind::Info, Lang::t("modal.ok"), "" });
}

std::unique_ptr<Screen> ToolsScreen::update(bool& pop) {
    pop = false;
    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }
    if (m_list.handle_input()) select(m_list.cursor());
    return nullptr;
}

void ToolsScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0, y = Layout::CONTENT_Y, w = Layout::SCREEN_W, h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x      = Layout::MENU_INDENT_X;
    style.show_checkbox = false;
    style.show_dividers = true;
    m_list.draw(x, y, w, h - 36, style);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.select") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
