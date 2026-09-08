// source/screens/tools_screen.cpp

#include "screens/tools_screen.hpp"
#include "screens/wifi_profile_screen.hpp"
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
#include "core/dump.hpp"
#include "core/es.hpp"
#include "core/fs.hpp"
#include "core/save_mount.hpp"
#include "ui/font.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include "tools/nsp_repair.hpp"
#include <cstring>
#include <dirent.h>
#include <set>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>


// ── Firmware dump helpers ────────────────────────────────────────────────────
// Enumeration lives in ONE place: Core::Dump::enumerate_firmware_content(),
// shared by the scan and the dump. Do not re-inline it here.
static int firmware_dump_scan(int* out_count, u64* out_bytes) {
    const Core::Dump::FirmwareScan scan =
        Core::Dump::enumerate_firmware_content(Core::Ncm::Storage::BuiltInSystem);
    if (out_count) *out_count = scan.unique_ncas;
    if (out_bytes) *out_bytes = scan.total_bytes;
    return scan.unique_ncas;
}

// Firmware dump body, run on the background thread. Keys are optional: raw
// NCA extraction reads NCM content storage, no titlekey decryption involved.
// Diagnostics land in logs/firmware_dump.log via Core::Dump::fw_log().
static void firmware_dump_body(Core::Dump::Progress& progress,
                               std::string& out_dir) {
    Core::Keys::LoadResult kr = Core::Keys::load();
    if (!kr.ok) {
        Core::Dump::fw_log("Keys not loaded (continuing without): %s",
                           kr.missing.c_str());
    }
    Core::Dump::dump_ncas_from_storage(
        Core::Ncm::Storage::BuiltInSystem, Core::Keys::get(), progress, out_dir);
}

// True on the frame ANY button is first pressed.
static bool fw_any_button_pressed() {
    using B = Input::Button;
    static const B all[] = {
        B::A, B::B, B::X, B::Y, B::L, B::R, B::ZL, B::ZR,
        B::Plus, B::Minus, B::DUp, B::DDown, B::DLeft, B::DRight,
        B::LStickClick, B::RStickClick,
    };
    for (B b : all) if (Input::pressed(b)) return true;
    return false;
}

// ── Background dump lifecycle: thread + polled progress ─────────────────
// Shared by the firmware dump and the ticket-repair re-dump. The worker
// publishes into the atomic Progress; the UI polls it every frame.
void ToolsScreen::fw_dump_thread_fn(void* arg) {
    auto* self = static_cast<ToolsScreen*>(arg);
#ifdef PLATFORM_SWITCH
    if (self->m_bg_op == BgOp::TicketRepair) {
        // Re-dump each confirmed candidate in turn; stop on cancel, keep
        // going past individual failures (the point of the op is to fix
        // everything fixable, then report what could not be).
        for (; self->m_repair_next < self->m_repair_list.size();
               ++self->m_repair_next) {
            if (self->m_fw_progress.cancel.load()) break;
            const RepairCandidate& c = self->m_repair_list[self->m_repair_next];
            Core::Dump::fw_log("Repair: re-dumping %016llX (replacing %s)",
                               (unsigned long long)c.title.program_id,
                               c.nsp_path.c_str());
            std::string out_path;
            Core::Dump::Progress one;
            one.reset();
            one.running.store(true);
            const bool ok = self->repair_one(c, one, out_path);
            if (ok) ++self->m_repair_ok;
            else    ++self->m_repair_fail;
            self->m_fw_progress.current_file = c.title.name.empty()
                ? c.nsp_path : c.title.name;
        }
        self->m_fw_progress.done    = true;
        self->m_fw_progress.success = (self->m_repair_fail == 0);
        if (self->m_repair_fail == 0 && self->m_repair_ok == 0)
            self->m_fw_progress.message = "Nothing repaired";
        return;
    }
#endif
    firmware_dump_body(self->m_fw_progress, self->m_fw_out_dir);
}

void ToolsScreen::start_background_dump(BgOp op) {
    m_bg_op      = op;
    m_fw_failed  = false;
    m_fw_fail_msg.clear();
    m_fw_out_dir.clear();
    m_fw_progress.reset();
    m_fw_dumping = true;
#ifdef PLATFORM_SWITCH
    m_repair_next = 0;
    m_repair_ok   = 0;
    m_repair_fail = 0;
#endif
    if (op == BgOp::FirmwareDump)
        Core::Dump::fw_log("Dump run requested from Tools menu");

#ifdef PLATFORM_SWITCH
    if (R_SUCCEEDED(threadCreate(&m_fw_thread, fw_dump_thread_fn, this, nullptr,
                                 0x20000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&m_fw_thread))) {
            m_fw_thread_active = true;
            return;
        }
        threadClose(&m_fw_thread);
    }
#endif
    // Thread refused to start (out of resources): fall back to synchronous.
    fw_dump_thread_fn(this);
    poll_firmware_dump();
}

void ToolsScreen::poll_firmware_dump() {
    if (!m_fw_dumping) return;
    if (!m_fw_progress.done.load()) return;   // still running - keep polling

#ifdef PLATFORM_SWITCH
    if (m_fw_thread_active) {
        threadWaitForExit(&m_fw_thread);
        threadClose(&m_fw_thread);
        m_fw_thread_active = false;
    }
#endif
    m_fw_dumping = false;

#ifdef PLATFORM_SWITCH
    if (m_bg_op == BgOp::TicketRepair) {
        // The repair batch reports a tally; cancel mid-batch lands here too.
        char msg[192];
        if (m_repair_fail == 0)
            snprintf(msg, sizeof(msg), "Repaired %d dumped NSP(s) - ticket now included.",
                     m_repair_ok);
        else
            snprintf(msg, sizeof(msg), "Repaired %d NSP(s); %d could not be repaired (see logs).",
                     m_repair_ok, m_repair_fail);
        Core::Dump::fw_log("Repair batch done: %d ok, %d failed", m_repair_ok, m_repair_fail);
        Modal::show({ Lang::t("tools.repair_tickets"), msg,
                      Modal::Kind::Info, Lang::t("modal.ok"), "" });
        return;
    }
#endif

    if (m_fw_progress.success.load()) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", Lang::t("dump_fw_done").c_str());
        std::string result = msg;
        size_t pos = result.find("{path}");
        if (pos != std::string::npos) result.replace(pos, 6, m_fw_out_dir);
        Modal::show({ Lang::t("tools.dump_firmware"), result,
                      Modal::Kind::Info, Lang::t("modal.ok"), "" });
        return;
    }

    // FAILURE: the dump already halted itself and cleaned up its partials.
    // Show a persistent notification; update() pops on ANY button while up.
    std::string tmpl = Lang::t("dump_fw_failed");
    const std::string error = m_fw_progress.cancel.load()
        ? Lang::t("common.cancel")
        : (m_fw_progress.message.empty() ? std::string("unknown error")
                                         : m_fw_progress.message);
    size_t pos = tmpl.find("{error}");
    if (pos != std::string::npos) tmpl.replace(pos, 7, error);
    m_fw_fail_msg = tmpl;
    m_fw_failed   = true;
    Core::Dump::fw_log("Run reported failure: %s", error.c_str());
}

namespace {

// ── Operation: clean leftover NCM placeholders ──────────────────────────────
// A cancelled or crashed install can leave placeholders behind, consuming
// space while referenced by nothing. By definition not yet registered content,
// so listing + deleting them is safe.

#ifdef PLATFORM_SWITCH
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
        // Deleting shrinks the set, so re-list from the head each pass.
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
// A Patch title's content-meta id is shared by every version of that patch;
// once a newer patch is installed, older ones cannot be launched. Keeps the
// highest version per id per storage.

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

// Patches that are NOT the highest version for their (storage, id).
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
    // Re-list right before deleting: state can move between the dry run and
    // the held confirm.
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

            // Delete referenced content first, then the meta record - the
            // order installer.cpp's rollback path uses.
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
// Clears nim's queued system-update task via nssuDestroySystemUpdateTask().
// Deliberately not a "keep highest meta version" cleanup like game patches:
// which ncm record matches the applied firmware is not reliably inferable.
// Touches only the queued-task state, never installed content or firmware.
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
// Atmosphère redirects error/crash reports to sdmc:/atmosphere/erpt_reports/
// and auto-clears the folder past 1000 files on boot; this is a manual trigger
// for the same cleanup. Unlinks only regular files directly inside.
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
// ncmContentStorageGetContentCount/ListContentId enumerates physically present
// NCA content ids; ncmContentMetaDatabaseLookupOrphanContent reports which are
// unreferenced by any content-meta record. Asking ncm directly avoids a wrong
// homemade cross-reference that could delete in-use content.
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

            // std::vector<bool> is bit-packed; LookupOrphanContent needs a
            // real bool array.
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
    // Re-list right before deleting: state can move between the dry-run and
    // the held confirm.
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
// A ticket is "unused" if its title id matches no installed content-meta record
// of any type on either storage - deliberately broad, since patches reuse the
// base application's rights id. If list_all() fails, return an empty list:
// treating "couldn't enumerate" as "nothing installed" would flag every ticket
// as unused. Fail closed. Common tickets only (see core/es.hpp).
#ifdef PLATFORM_SWITCH
std::vector<std::array<uint8_t, 0x10>> unused_ticket_rights_ids() {
    bool ok = false;
    std::vector<Core::Ncm::Title> titles = Core::Ncm::list_all(&ok);
    std::vector<std::array<uint8_t, 0x10>> out;
    if (!ok) return out; // couldn't confirm what's installed - touch nothing

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
// A save-data record is a candidate if its owning uid matches no account
// list_users() reports. Standard user deletion removes saves with the account,
// so this only finds the narrower paths where an account disappears without
// its saves (corrupt record, SD moved between consoles, non-Settings removal);
// the positive path is not hardware-tested for that reason. Direct delete by
// design - the Danger modal is the safety net, unlike Save Manager's
// snapshot-first flow for a live user. Fails closed: if list_users() fails,
// return an empty list rather than flagging every save as orphaned.
#ifdef PLATFORM_SWITCH
std::vector<uint64_t> deleted_user_save_ids() {
    bool ok = false;
    std::vector<Core::SaveMount::User> users = Core::SaveMount::list_users(&ok);
    std::vector<uint64_t> out;
    if (!ok) return out; // couldn't confirm who's live - touch nothing

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
// Full reset of parental controls (PIN + all restrictions) via
// pctlDeleteParentalControls() only - deliberately not pctlDeletePairing(),
// which is a separate action (unlinking the mobile app).
//
// This project's pctl.h is read-only (Get/Is only), so the delete is raw IPC
// against pctlGetServiceSession_Service(), same shape as core/es.cpp. Command
// id 1043 comes from ITotalJustice/Reset-Parental-Controls-NX (author-annotated
// "works" from testing; adjacent commands marked "doesn't work"). Not
// hardware-tested in this codebase.
//
// The 2026-09-07 "nothing to tidy" root cause: every pctl wrapper dispatches
// on the session pctlInitialize() creates. The old code never called it, so
// every command hit a zeroed Service and failed, and the scan misread "cannot
// detect" as "parental controls off". pctlInitialize() is now called first.
#ifdef PLATFORM_SWITCH

// Appends one line to sdmc:/switch/GarageNX/logs/pctl.log (the dir is created
// by boot's ensure_directories). Every probe/delete Result lands here so
// hardware triage starts from recorded codes instead of "it said nothing to
// tidy" - the same discipline as the firmware-dump and wifi logs.
static void pctl_log(const char* fmt, ...) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/pctl.log", "a");
    if (!f) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    ::fprintf(f, "%s\n", line);
    ::fclose(f);
}

enum class PctlState { Enabled, Disabled, Unknown };

// Probe the console's parental-controls state. Unknown means "could not
// detect" - never silently treated as Disabled. Primary signal: pctlGetSafety
// Level (cmd 1032) - level None(0) means no configuration exists. Second
// signal: pctlIsRestrictionEnabled (cmd 1031). Either proving true = Enabled.
PctlState parental_controls_probe(std::string* reason) {
    Result rc = pctlInitialize();
    if (R_FAILED(rc)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "pctl service unavailable (Result 0x%08X)", rc);
        pctl_log("probe: %s", msg);
        if (reason) *reason = msg;
        return PctlState::Unknown;
    }

    u32  level = 0;
    bool restriction = false;
    bool pairing = false;
    const Result rc_level       = pctlGetSafetyLevel(&level);
    const Result rc_restriction = pctlIsRestrictionEnabled(&restriction);
    const Result rc_pairing     = pctlIsPairingActive(&pairing);
    pctlExit();

    pctl_log("probe: safety_level=%u (rc 0x%08X) restriction=%d (rc 0x%08X) pairing=%d (rc 0x%08X)",
             level, rc_level, restriction ? 1 : 0, rc_restriction,
             pairing ? 1 : 0, rc_pairing);

    if (R_FAILED(rc_level) && R_FAILED(rc_restriction)) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Could not read parental-controls state (Results 0x%08X, 0x%08X)",
                 rc_level, rc_restriction);
        if (reason) *reason = msg;
        return PctlState::Unknown;
    }

    const bool enabled = (R_SUCCEEDED(rc_level) && level != 0) ||
                         (R_SUCCEEDED(rc_restriction) && restriction);
    return enabled ? PctlState::Enabled : PctlState::Disabled;
}

bool parental_controls_delete() {
    // pctlInitialize() FIRST - see the probe's comment for why this is not
    // optional. Without it 1043 dispatched against a zeroed session.
    Result rc = pctlInitialize();
    if (R_FAILED(rc)) {
        pctl_log("delete: pctl service unavailable (Result 0x%08X)", rc);
        return false;
    }
    // Raw IPC, no input, no output - matches the "works"-annotated reference
    // implementation exactly (DeleteSettings). See the block comment above.
    const Result rc_del = serviceDispatch(pctlGetServiceSession_Service(), 1043);
    pctlExit();
    pctl_log("delete: DeleteSettings rc=0x%08X", rc_del);
    return R_SUCCEEDED(rc_del);
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

ToolsScreen::~ToolsScreen() {
#ifdef PLATFORM_SWITCH
    if (m_fw_thread_active) {
        // Ask the dump to stop, then WAIT: cancelling without waiting would
        // leave a worker running against members that are about to die - the
        // exact use-after-free class gamecard.cpp's destructor documents.
        m_fw_progress.cancel.store(true);
        threadWaitForExit(&m_fw_thread);
        threadClose(&m_fw_thread);
        m_fw_thread_active = false;
    }
#endif
}

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
            std::string why;
            const PctlState st = parental_controls_probe(&why);
            r.any    = st == PctlState::Enabled;
            r.count  = r.any ? 1 : 0;
            // Enabled → the warning text; Unknown → the reason (shown in the
            // nothing-to-do modal, so detection failure is never dressed up as
            // "already tidy"); Disabled → empty detail.
            r.detail = st == PctlState::Enabled
                           ? Lang::t("tools.warn_parental_controls")
                           : why;
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
    // Picker, not batch delete - see the Op::push doc comment in
    // tools_screen.hpp for why. Enumerate/select/confirm/delete all live in
    // WifiProfileScreen now; this row just pushes it.
    m_ops.push_back({
        Lang::t("tools.delete_wifi_profiles"),
        nullptr,
        nullptr,
        false,  // is_destructive = false (read-only)
        [] () -> std::unique_ptr<Screen> { return std::make_unique<WifiProfileScreen>(); }
    });

    // ── Operation: repair tickets with dump errors ───────────────────────────
// Scoped to patching already-dumped NSP output files on
// SD that are missing a ticket/cert because a prior dump failed to fetch one
// - NOT touching the console's own ES ticket store.
#ifdef PLATFORM_SWITCH
    m_ops.push_back({
        Lang::t("tools.repair_tickets"),
        [this] () -> ScanResult {
            ScanResult r;
            const std::vector<RepairCandidate> cands =
                const_cast<ToolsScreen*>(this)->repair_scan();
            r.count = (int)cands.size();
            r.any   = r.count > 0;
            if (r.any) {
                char b[256];
                std::snprintf(b, sizeof(b), "%d dumped NSP(s) missing a ticket that CAN now be fetched:",
                              r.count);
                r.detail = b;
                for (const auto& c : cands) {
                    r.detail += "\n\u00b7 ";
                    r.detail += c.title.name.empty() ? c.nsp_path : c.title.name;
                }
            }
            return r;
        },
        [] (const ScanResult&) -> std::string {
            // Never invoked: run_in_background routes the confirmed op to
            // start_background_dump(TicketRepair) instead.
            return Lang::t("dump_fw_failed");
        },
        false,  // is_destructive = false (re-dump replaces only its own output)
        nullptr,
        true,    // run_in_background - threaded, minutes-long re-dumps
        1        // bg_kind 1 = ticket repair
    });
#endif

    // Alphabetical (case-insensitive). The menu is a maintenance catalog, not
    // a workflow: row order must not encode authoring order. Sorting m_ops
    // itself (rather than just the display rows) keeps m_pending indices -
    // and every op lookup through them - consistent with what is displayed.
    std::sort(m_ops.begin(), m_ops.end(),
              [](const Op& a, const Op& b) {
                  const size_t n = std::min(a.label.size(), b.label.size());
                  for (size_t i = 0; i < n; ++i) {
                      const int cx = std::tolower((unsigned char)a.label[i]);
                      const int cy = std::tolower((unsigned char)b.label[i]);
                      if (cx != cy) return cx < cy;
                  }
                  return a.label.size() < b.label.size();  // shorter prefix first
              });

    std::vector<Widgets::ListItem> rows;
    for (const auto& op : m_ops) {
        Widgets::ListItem row;
        row.label = op.label;
        rows.push_back(row);
    }
    m_list.set_items(std::move(rows));
}

#ifdef PLATFORM_SWITCH
// Scan sdmc:/switch/GarageNX/dumps for dumped NSPs with no .tik entry whose
// title id maps to a title that still exists AND has a common ticket now.
// Dumps whose title is gone or has no ticket are skipped: the op is a repair,
// not a cleanup.
std::vector<RepairCandidate> ToolsScreen::repair_scan() {
    std::vector<RepairCandidate> out;

    // Titles the console can still dump, indexed by application id.
    bool ok = false;
    std::vector<Core::Ncm::Title> titles = Core::Ncm::list_all(&ok);
    if (!ok) return out;  // fail closed: cannot confirm titles - find nothing
    std::unordered_map<uint64_t, const Core::Ncm::Title*> by_id;
    for (const auto& t : titles) by_id.emplace(t.program_id, &t);

    // Common tickets present NOW, by title id. A ticket existing means the
    // title is titlekey-protected and a re-dump can fetch it.
    std::unordered_set<uint64_t> ticketed;
    for (const auto& tik : Core::Es::list_common_tickets())
        ticketed.insert(tik.title_id);

    const std::string dir = "sdmc:/switch/GarageNX/dumps";
    bool dir_ok = false;
    for (const Fs::Entry& e : Fs::list(dir, &dir_ok)) {
        if (e.is_dir() || e.type != Fs::EntryType::File) continue;
        if (e.name.size() < 6 ||
            e.name.compare(e.name.size() - 4, 4, ".nsp") != 0) continue;
        const std::string path = Fs::join(dir, e.name);

        // Read the header, then the entry table, then just the .cnmt.nca
        // entry's data - never the whole multi-GB file.
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        uint8_t hdr[0x10];
        if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); continue; }
        const Tools::NspRepair::Header h = Tools::NspRepair::read_header(hdr, sizeof(hdr));
        if (!h.ok || h.header_size > (uint64_t)e.size) { fclose(f); continue; }

        std::vector<uint8_t> table((size_t)(h.header_size - 0x10));
        if (fread(table.data(), 1, table.size(), f) != table.size()) { fclose(f); continue; }
        const std::vector<Tools::NspRepair::EntryInfo> entries =
            Tools::NspRepair::read_entries(table.data(), table.size(),
                                           h.count, h.strtab_size);

        bool have_tik = false;
        const Tools::NspRepair::EntryInfo* cnmt = nullptr;
        for (const auto& en : entries) {
            if (Tools::NspRepair::is_tik_name(en.name))  have_tik = true;
            if (en.name.size() == 32 + 9 &&
                en.name.compare(en.name.size() - 9, 9, ".cnmt.nca") == 0)
                cnmt = &en;
        }
        if (have_tik || !cnmt) { fclose(f); continue; }

        std::vector<uint8_t> cnmt_data((size_t)cnmt->size);
        if (fseek(f, (long)(h.header_size + cnmt->data_off), SEEK_SET) != 0 ||
            fread(cnmt_data.data(), 1, cnmt_data.size(), f) != cnmt_data.size()) {
            fclose(f); continue;
        }
        fclose(f);

        const uint64_t title_id =
            Tools::NspRepair::title_id_from_cnmt_nca(cnmt_data.data(), cnmt_data.size());
        if (title_id == 0) continue;

        auto it = by_id.find(title_id);
        if (it == by_id.end()) continue;                 // title gone - skip
        if (ticketed.find(title_id) == ticketed.end()) continue;  // no ticket - skip
        out.push_back({ *it->second, path });
    }
    return out;
}

// Re-dump one candidate through the proven pipeline. dump_title_to_nsp()
// picks its own canonical output path (<id>_v<version>.nsp) and deletes its
// own output on failure, so a failed re-dump cannot leave a fresh corrupt
// file. The incomplete original is removed only after a successful re-dump
// that landed at a different path; when paths match, the pipeline already
// replaced it in place.
bool ToolsScreen::repair_one(const RepairCandidate& c, Core::Dump::Progress& progress,
                             std::string& out_path) {
    bool ok = Core::Dump::dump_title_to_nsp(c.title, Core::Keys::get(),
                                            progress, out_path);
    if (!ok) return false;
    if (out_path != c.nsp_path) remove(c.nsp_path.c_str());
    return true;
}
#endif  // PLATFORM_SWITCH

std::unique_ptr<Screen> ToolsScreen::select(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_ops.size())) return nullptr;

    if (m_ops[idx].push) return m_ops[idx].push(); // picker-style op - no scan/confirm flow

    m_pending_scan = m_ops[idx].scan();
    if (!m_pending_scan.any) {
        // Include the scan's reason when it has one (e.g. "could not read
        // parental-controls state") - "already tidy" alone would be a lie when
        // detection itself failed.
        std::string body = Lang::t("tools.nothing_to_do");
        if (!m_pending_scan.detail.empty()) body += "\n\n" + m_pending_scan.detail;
        Modal::show({ m_ops[idx].label,
                      body,
                      Modal::Kind::Info, Lang::t("modal.ok"), "" });
        m_pending = -1;
        return nullptr;
    }

    Modal::Kind kind = m_ops[idx].is_destructive ? Modal::Kind::Danger : Modal::Kind::Info;
    const char* confirm_label = m_ops[idx].is_destructive ? "tools.confirm_remove" : "modal.ok";
    const char* confirm_body = m_ops[idx].is_destructive ? "tools.confirm_body" : "tools.confirm_body_readonly";
    std::string body = m_pending_scan.detail + "\n" + Lang::t(confirm_body);
    Modal::show({ m_ops[idx].label, body,
                  kind,
                  Lang::t(confirm_label),
                  Lang::t("modal.cancel") });
    m_pending = idx;
    return nullptr;
}

void ToolsScreen::on_modal_result(int result) {
    if (m_pending < 0) return;
    const int idx = m_pending;
    m_pending = -1;

    if (static_cast<Modal::Result>(result) != Modal::Result::Confirmed) return;

    // Background ops never run on the UI thread - see Op::run_in_background.
    if (m_ops[idx].run_in_background) {
        if (m_ops[idx].bg_kind == 1) {
#ifdef PLATFORM_SWITCH
            // The scan ran seconds ago in select(); re-run it to capture the
            // candidate list for the worker (ScanResult carries counts, not
            // the candidates themselves).
            m_repair_list = repair_scan();
            if (!m_repair_list.empty())
                start_background_dump(BgOp::TicketRepair);
#endif
        } else {
            start_background_dump(BgOp::FirmwareDump);
        }
        return;
    }

    const std::string msg = m_ops[idx].run(m_pending_scan);
    Modal::show({ m_ops[idx].label, msg,
                  Modal::Kind::Info, Lang::t("modal.ok"), "" });
}

std::unique_ptr<Screen> ToolsScreen::update(bool& pop) {
    pop = false;

    // Poll the dump FIRST, every frame - this is what turns a background
    // operation into a finished one (gamecard.cpp pattern).
    const bool was_failed = m_fw_failed;
    poll_firmware_dump();

    // Failure notification: HALT. Any button returns to the previous screen.
    // Input on the frame the failure lands is ignored: a B press meant for
    // cancel must not also dismiss the notification it just caused.
    if (m_fw_failed) {
        if (was_failed && fw_any_button_pressed()) pop = true;
        return nullptr;
    }

    if (m_fw_dumping) {
        // B cancels; the dump checks progress.cancel between chunks and files.
        if (Input::pressed(Input::Button::B)) m_fw_progress.cancel.store(true);
        return nullptr;
    }

    if (Modal::is_active()) return nullptr;

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }
    if (m_list.handle_input()) return select(m_list.cursor());
    return nullptr;
}

void ToolsScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0, y = Layout::CONTENT_Y, w = Layout::SCREEN_W, h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    const SDL_Color fg  = Theme::get(Theme::Token::FgPrimary);
    const SDL_Color fg2 = Theme::get(Theme::Token::FgSecondary);

    // ── Live progress page (dump running) ────────────────────────────────
    // Updates land here the frame after each NCA completes: bytes_done,
    // ncas_done and current_file are atomics the worker bumps per file.
    if (m_fw_dumping) {
        const uint64_t total = m_fw_progress.bytes_total.load();
        const uint64_t done  = m_fw_progress.bytes_done.load();
        const float frac = total ? (float)((double)done / (double)total) : 0.f;

        std::string hdr = Lang::t("dump_fw_progress");
        char pct[8];
        snprintf(pct, sizeof(pct), "%d", (int)(frac * 100.f));
        size_t pos = hdr.find("{pct}");
        if (pos != std::string::npos) hdr.replace(pos, 5, pct);
        Renderer::draw_text(hdr, (int)Font::Size::Large, (int)Font::Weight::Bold,
                            (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 50, nullptr, nullptr, w);

        char bytes_line[96];
        snprintf(bytes_line, sizeof(bytes_line), "%s / %s",
                 Fs::format_size(done).c_str(), Fs::format_size(total).c_str());
        Renderer::draw_text(bytes_line, (int)Font::Size::Body, (int)Font::Weight::Regular,
                            (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 96, nullptr, nullptr, w);

        Widgets::draw_progress(x + Layout::MENU_INDENT_X, y + 132,
                               w - Layout::MENU_INDENT_X * 2, 14, frac);

        // Per-file line: how many NCAs are complete and the one in flight.
        char files[192];
        snprintf(files, sizeof(files), "%d / %d NCA(s)  ·  %s",
                 m_fw_progress.ncas_done.load(), m_fw_progress.ncas_total.load(),
                 m_fw_progress.current_file.c_str());
        Renderer::draw_text(files, (int)Font::Size::Small, (int)Font::Weight::Regular,
                            (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 164, nullptr, nullptr, w);

        std::vector<Widgets::ButtonHint> dh = { { "B", Lang::t("common.cancel") } };
        Widgets::draw_button_legend(x, y + h - 32, w, dh);
        return;
    }

    // ── Failure notification: HALT - any button returns to previous screen ──
    if (m_fw_failed) {
        Renderer::draw_text(Lang::t("tools.dump_firmware"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 50, nullptr, nullptr, w);

        Renderer::draw_text(m_fw_fail_msg, (int)Font::Size::Body,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans,
                            Theme::get(Theme::Token::AccentDanger),
                            x + Layout::MENU_INDENT_X, y + 110, nullptr, nullptr, w);

        Renderer::draw_text(Lang::t("dump_fw_press_any"), (int)Font::Size::Small,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + h - 48, nullptr, nullptr, w);
        return;
    }

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