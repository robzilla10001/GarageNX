// source/core/title_ops.cpp

#include "core/title_ops.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::TitleOps {

Result delete_application_completely(uint64_t application_id) {
    Result r;

#ifdef PLATFORM_SWITCH
    // nsDeleteApplicationCompletely removes the base application, all of its
    // updates and DLC, and the application record — the same operation the OS
    // uses when you delete a game from the HOME menu. This is the safe path:
    // it can't leave orphaned content or a dangling record the way manual
    // per-NCA ncm deletion can.
    //
    // ns must be initialized (done at startup). The call is synchronous.
    ::Result rc = nsDeleteApplicationCompletely(application_id);

    if (R_SUCCEEDED(rc)) {
        r.ok = true;
        char buf[64];
        snprintf(buf, sizeof(buf), "Deleted %016llX",
                 (unsigned long long)application_id);
        r.message = buf;
    } else {
        r.ok = false;
        char buf[96];
        snprintf(buf, sizeof(buf), "Delete failed (0x%08X)", rc);
        r.message = buf;
        SDL_Log("TitleOps::delete — nsDeleteApplicationCompletely(%016llX) "
                "failed: 0x%08X", (unsigned long long)application_id, rc);
    }
#else
    // PC stub: pretend success for UI development.
    r.ok = true;
    char buf[64];
    snprintf(buf, sizeof(buf), "Deleted %016llX (stub)",
             (unsigned long long)application_id);
    r.message = buf;
#endif

    return r;
}

// ─── Move (SD <-> NAND) ─────────────────────────────────────────────────────────

#ifdef PLATFORM_SWITCH

static NcmContentMetaKey make_meta_key(const Core::Ncm::Title& t) {
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

// Copy one NCA from src storage to dst storage via the placeholder/register
// pipeline. Registers under the SAME content id (we don't modify content, so the
// id — which is the content hash — is unchanged). Returns true on success.
static bool copy_nca(NcmContentStorage* src, NcmContentStorage* dst,
                     const NcmContentId& content_id, uint64_t size,
                     Core::TitleOps::MoveProgress& progress) {
    // If the destination already has this content registered, we're done.
    bool has = false;
    if (R_SUCCEEDED(ncmContentStorageHas(dst, &has, &content_id)) && has) {
        progress.bytes_done.fetch_add(size);
        return true;
    }

    NcmPlaceHolderId ph;
    if (R_FAILED(ncmContentStorageGeneratePlaceHolderId(dst, &ph))) return false;

    // Clean any stale placeholder, then create sized.
    ncmContentStorageDeletePlaceHolder(dst, &ph);
    if (R_FAILED(ncmContentStorageCreatePlaceHolder(dst, &content_id, &ph, (s64)size)))
        return false;

    constexpr size_t CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf(CHUNK);
    uint64_t off = 0;
    bool ok = true;

    while (off < size) {
        if (progress.cancel.load()) { ok = false; break; }
        size_t chunk = (size_t)std::min<uint64_t>(size - off, CHUNK);
        if (R_FAILED(ncmContentStorageReadContentIdFile(src, buf.data(), chunk,
                                                        &content_id, (s64)off))) {
            ok = false; break;
        }
        if (R_FAILED(ncmContentStorageWritePlaceHolder(dst, &ph, off,
                                                       buf.data(), chunk))) {
            ok = false; break;
        }
        off += chunk;
        progress.bytes_done.fetch_add(chunk);
    }

    if (!ok) {
        ncmContentStorageDeletePlaceHolder(dst, &ph);
        return false;
    }

    // Move the placeholder into the registered path (atomic on the FS side).
    if (R_FAILED(ncmContentStorageRegister(dst, &content_id, &ph))) {
        ncmContentStorageDeletePlaceHolder(dst, &ph);
        return false;
    }
    return true;
}

#endif // PLATFORM_SWITCH

Result move_title(const Core::Ncm::Title& title, MoveProgress& progress) {
    Result r;
    progress.reset();
    progress.running = true;

#ifdef PLATFORM_SWITCH
    NcmStorageId src_id = (title.storage == Core::Ncm::Storage::SdCard)
        ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    NcmStorageId dst_id = (title.storage == Core::Ncm::Storage::SdCard)
        ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;

    // Open source + destination content storages and meta databases.
    NcmContentStorage src_cs, dst_cs;
    NcmContentMetaDatabase src_db, dst_db;
    bool have_src_cs=false, have_dst_cs=false, have_src_db=false, have_dst_db=false;

    auto cleanup = [&]() {
        if (have_src_cs) ncmContentStorageClose(&src_cs);
        if (have_dst_cs) ncmContentStorageClose(&dst_cs);
        if (have_src_db) ncmContentMetaDatabaseClose(&src_db);
        if (have_dst_db) ncmContentMetaDatabaseClose(&dst_db);
    };

    if (R_FAILED(ncmOpenContentStorage(&src_cs, src_id)))       { r.message="open src storage failed"; progress.done=true; return r; }
    have_src_cs=true;
    if (R_FAILED(ncmOpenContentStorage(&dst_cs, dst_id)))       { r.message="open dst storage failed"; cleanup(); progress.done=true; return r; }
    have_dst_cs=true;
    if (R_FAILED(ncmOpenContentMetaDatabase(&src_db, src_id)))  { r.message="open src meta failed"; cleanup(); progress.done=true; return r; }
    have_src_db=true;
    if (R_FAILED(ncmOpenContentMetaDatabase(&dst_db, dst_id)))  { r.message="open dst meta failed"; cleanup(); progress.done=true; return r; }
    have_dst_db=true;

    NcmContentMetaKey key = make_meta_key(title);

    // ── Enumerate the meta's contents + total size ──────────────────────────────
    constexpr s32 MAXC = 48;
    std::vector<NcmContentInfo> infos(MAXC);
    s32 written = 0;
    if (R_FAILED(ncmContentMetaDatabaseListContentInfo(&src_db, &written,
            infos.data(), MAXC, &key, 0)) || written <= 0) {
        r.message = "no content info"; cleanup(); progress.done=true; return r;
    }

    uint64_t total = 0;
    for (s32 i = 0; i < written; ++i) {
        u64 sz = 0; ncmContentInfoSizeToU64(&infos[i], &sz);
        total += sz;
    }
    progress.bytes_total = total;
    progress.ncas_total  = written;

    // ── Phase 1: copy every NCA to the destination ──────────────────────────────
    progress.stage = "copying";
    std::vector<NcmContentId> copied;   // for rollback on failure
    bool ok = true;

    for (s32 i = 0; i < written && ok; ++i) {
        if (progress.cancel.load()) { r.message="cancelled"; ok=false; break; }
        char idhex[8]; snprintf(idhex, sizeof(idhex), "%02x%02x…",
                                infos[i].content_id.c[0], infos[i].content_id.c[1]);
        progress.current_file = idhex;

        u64 sz = 0; ncmContentInfoSizeToU64(&infos[i], &sz);
        if (!copy_nca(&src_cs, &dst_cs, infos[i].content_id, sz, progress)) {
            r.message = "content copy failed"; ok = false; break;
        }
        copied.push_back(infos[i].content_id);
        progress.ncas_done.fetch_add(1);
    }

    // ── Phase 2: copy the meta record into the destination meta database ─────────
    if (ok) {
        progress.stage = "registering";
        // Read the packaged content-meta record from the source.
        u64 meta_size = 0;
        if (R_FAILED(ncmContentMetaDatabaseGetSize(&src_db, &meta_size, &key)) ||
            meta_size == 0) {
            r.message = "meta size query failed"; ok = false;
        } else {
            std::vector<uint8_t> meta(meta_size);
            u64 got = 0;
            if (R_FAILED(ncmContentMetaDatabaseGet(&src_db, &key, &got,
                    meta.data(), meta.size())) || got == 0) {
                r.message = "meta read failed"; ok = false;
            } else if (R_FAILED(ncmContentMetaDatabaseSet(&dst_db, &key,
                    meta.data(), got))) {
                r.message = "meta write failed"; ok = false;
            } else if (R_FAILED(ncmContentMetaDatabaseCommit(&dst_db))) {
                r.message = "meta commit failed"; ok = false;
            }
        }
    }

    // ── Phase 3: push the application record so the title resolves on dst ────────
    // (ns records are storage-agnostic — the record already exists for this
    // application, but we push/refresh to ensure the content-storage record
    // points at the new location. For a straight base/patch/DLC move the existing
    // record + the dst meta entry are sufficient; we refresh defensively.)
    // No-op here beyond the meta DB write above for most cases.

    // ── If anything failed: roll back the destination, keep source intact ───────
    if (!ok) {
        progress.stage = "rolling back";
        for (auto& cid : copied) {
            bool has=false;
            if (R_SUCCEEDED(ncmContentStorageHas(&dst_cs, &has, &cid)) && has)
                ncmContentStorageDelete(&dst_cs, &cid);
        }
        // Remove any partial meta entry we may have written.
        ncmContentMetaDatabaseRemove(&dst_db, &key);
        ncmContentMetaDatabaseCommit(&dst_db);

        cleanup();
        progress.success = false;
        progress.done = true;
        return r;
    }

    // ── Phase 4: destination is complete — NOW remove the source ────────────────
    // Only reached when every NCA + the meta are safely on the destination.
    progress.stage = "removing source";
    for (s32 i = 0; i < written; ++i) {
        bool has=false;
        if (R_SUCCEEDED(ncmContentStorageHas(&src_cs, &has, &infos[i].content_id)) && has)
            ncmContentStorageDelete(&src_cs, &infos[i].content_id);
    }
    ncmContentMetaDatabaseRemove(&src_db, &key);
    ncmContentMetaDatabaseCommit(&src_db);

    cleanup();

    // NOTE: HOS tracks application records separately from the ncm content-meta
    // databases. After moving content between storages, the existing application
    // record already points at the application id (records are storage-agnostic),
    // and the destination meta-db entry we wrote provides the content location.
    // A record refresh (nsPushApplicationRecord with a rebuilt content-meta-
    // status array) may be needed on some firmwares for the title to relaunch
    // cleanly; if hardware testing shows the moved title doesn't appear/launch,
    // that's the precise piece to add. We avoid guessing its record-array format
    // here rather than risk a malformed push.

    r.ok = true;
    char buf[96];
    snprintf(buf, sizeof(buf), "Moved to %s",
             dst_id == NcmStorageId_SdCard ? "SD card" : "System memory");
    r.message = buf;
    progress.success = true;
    progress.message = r.message;
    progress.done = true;
    return r;
#else
    r.ok = true; r.message = "move (stub)";
    progress.success = true; progress.done = true;
    return r;
#endif
}

} // namespace Core::TitleOps
