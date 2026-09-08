#pragma once
// source/services/save_surface.hpp
//
// THE shared Save Data surface. Everything a transport needs to present
//
//     /Save Data/<User>/<Title>/...
//
// lives here, so FTP, MTP and (later) HTTP cannot drift into different user
// lists, title labels, or mount policies. ONE implementation, several thin
// adapters (same anti-drift move as StorageCatalog and NspStream).
//
// THE MOUNT POLICY LIVES IN save_resolve(). Every request routes through it:
// a file-level path mounts its (user, title), releasing whatever was mounted
// before; anything shallower releases entirely.
//
// ── Synthetic paths (MTP) ────────────────────────────────────────────────────
// MTP hands the host an opaque u32 handle interned BY PATH. A real save path
// ("save:/slot1.dat") is not a safe handle key - "save:" means a different
// filesystem depending on what is mounted, so two titles' files would collide
// on one handle. MTP therefore interns save objects under a synthetic prefix
// carrying the full three-level identity:
//
//     savedata:/<User>/<Title>/<rest...>
//
// The part after the prefix is exactly the `rel` that sp_split_save() already
// parses, so no second parser exists. The prefix deliberately does NOT begin
// with "save:" - prefix-matching in StorageCatalog/mtp_storage_for_path would
// mistake it for a real mounted one.

#include "services/storage_paths.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Services {

// ── Synthetic path helpers (PURE - no libnx, host-testable) ──────────────────

inline const char* save_synth_prefix() { return "savedata:/"; }
inline size_t      save_synth_prefix_len() { return 10; }

/// True if `p` is a synthetic save path (an MTP handle key), NOT a mounted
/// "save:/..." path.
inline bool save_is_synthetic(const std::string& p) {
    return p.compare(0, save_synth_prefix_len(), save_synth_prefix()) == 0;
}

/// Build the synthetic path for a display-relative save path.
/// save_synth_path("Rob/Zelda [0100...]/slot1.dat")
///     -> "savedata:/Rob/Zelda [0100...]/slot1.dat"
inline std::string save_synth_path(const std::string& rel) {
    return std::string(save_synth_prefix()) + rel;
}

/// Inverse: strip the prefix, yielding the `rel` sp_split_save() consumes.
/// Returns "" for the storage root ("savedata:/") and for a non-synthetic path.
inline std::string save_synth_rel(const std::string& p) {
    if (!save_is_synthetic(p)) return std::string();
    return p.substr(save_synth_prefix_len());
}

/// True if the synthetic path names one of the two SYNTHESIZED levels - a user
/// folder or a title folder. Those are directories by construction, so a
/// transport can answer "is this a directory?" WITHOUT mounting anything.
///
/// This matters more than it looks: an MTP host asks for an ObjectInfo for every
/// object it lists, so answering the title level by mounting would mount and
/// unmount every save on the console just to browse one folder - exactly the
/// bulk-churn pattern the single-slot design exists to avoid.
inline bool save_synth_is_synthesized_dir(const std::string& p) {
    if (!save_is_synthetic(p)) return false;
    const std::string rel = save_synth_rel(p);
    if (rel.empty()) return true;                       // the storage root
    const SavePath sp = sp_split_save(rel);
    if (sp.level != SavePath::Level::Files) return true; // a user folder
    return sp.rest.empty();                              // a title folder
}

/// Is this concrete path the save MOUNT ROOT itself ("save:/") rather than
/// something inside it?
///
/// A title folder "/Save Data/<User>/<Title>" resolves to exactly this - fine
/// to LIST, but not a legal target for mutation: it is the filesystem, not a
/// file in it. Without this check, a client deleting a folder gets a confirm
/// prompt for an operation that must fail (you cannot rmdir a mount point).
inline bool save_is_mount_root(const std::string& vfs_path) {
    return vfs_path == "save:" || vfs_path == "save:/";
}

// ── Listings for the two synthesized levels ──────────────────────────────────
// Both RELEASE the mount before listing: by definition neither is inside a title
// folder, so the single-slot rule says nothing should stay mounted.

/// Level 1 - every registered user, as folder names. Empty (not an error) if the
/// account service is unavailable.
std::vector<std::string> save_user_names();

/// Level 2 - the titles this user has save data for, labelled "<Name> [APPID]"
/// (or "Title <APPID>" when the title is not installed, which is normal: save
/// data outlives an uninstalled game).
///
/// `block` controls how title-name resolution is obtained:
///   - true  (default): wait for the main loop to resolve names. Correct for a
///            TRANSPORT WORKER, whose wait the main loop can service.
///   - false: request resolution without blocking and label with whatever is
///            ready now. REQUIRED for a MAIN-THREAD caller: blocking there
///            parks the very loop that does the resolving. The screen re-labels
///            as names fill in over subsequent frames.
std::vector<std::string> save_title_labels(const std::string& user, bool block = true);

/// True if `label` is an id-only fallback ("Title <16 hex>") rather than a
/// resolved title name. The on-device screen uses this to know when to STOP
/// re-labelling - a prefix test would misfire on a real game actually named
/// "Title ...". Inline (pure) so the host test can link it.
inline bool save_label_is_unresolved(const std::string& label) {
    static const std::string prefix = "Title ";
    if (label.size() != prefix.size() + 16) return false;
    if (label.compare(0, prefix.size(), prefix) != 0) return false;
    for (size_t i = prefix.size(); i < label.size(); ++i) {
        const char ch = label[i];
        const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
        if (!hex) return false;
    }
    return true;
}

// ── The choke point ──────────────────────────────────────────────────────────

/// Apply the single-slot mount policy for one request and return the concrete
/// path to use.
///
/// Returns "save:/..." for a file-level request whose (user, title) mounted
/// successfully; returns "" for the synthesized levels and for any failure -
/// and in both of those cases the mount is RELEASED.
std::string save_resolve(const SavePath& sp);

/// Convenience for the synthetic-path callers: split and resolve in one step.
/// `synth` is a full "savedata:/..." path.
std::string save_resolve_synth(const std::string& synth);

/// Commit the mounted save IF `vfs_path` is one ("save:/..."), otherwise do
/// nothing and report success. Call this after any SUCCESSFUL mutation, on every
/// transport: a journalled save discards uncommitted writes at unmount, so
/// skipping it turns a reported success into silent data loss.
///
/// Taking the path rather than a bare flag means a caller cannot forget which
/// surface it just wrote to - a non-save path is simply a no-op.
bool save_commit_if_save_path(const std::string& vfs_path);

/// Wipe a mounted save: delete everything inside it, then commit, so the title
/// is left with an empty save rather than the save it had.
///
/// This is what "delete the title folder" MEANS for a save: the title folder is
/// the mount point, not a real directory, so it cannot be removed with rmdir;
/// transports route a delete of the save root here instead.
///
/// `mounted_root` must be the concrete "save:/" the request already resolved
/// to. Returns false if nothing is mounted, if a child could not be removed, or
/// if the commit failed - a false may mean the save is PARTIALLY emptied, which
/// is why the on-device path snapshots first.
bool save_wipe(const std::string& mounted_root);

/// DELETE the save record for (user, title_label) entirely, so the title leaves
/// the save list and the game creates a fresh save next launch. Distinct from
/// save_wipe(), which empties the contents but KEEPS the record.
///
/// Releases any mount first, looks up the save_data_id from the label the same
/// way save_resolve() matches a title, and calls the fs deletion primitive.
/// Returns false if the user or title is not found, or if deletion failed.
///
/// Like every destructive save op this must be gated by confirmation, and the
/// on-device path snapshots first - this function does NOT snapshot or confirm
/// itself, exactly as save_resolve()/save_wipe() do not.
bool save_delete_record(const std::string& user, const std::string& title_label);

/// Build the display label for a save: "<Name> [APPID]", or "Title <APPID>" when
/// the title is not installed. The SINGLE definition of that label -
/// save_title_labels(), save_resolve()'s matching, and auto-backup all use it.
/// Name resolution is cache-only, so callers must have primed the cache first.
std::string save_build_label(uint64_t application_id);

/// Enumerate every (user, title_label, application_id) live save on the console,
/// so callers that must act on all of them - auto-backup - do not re-derive the
/// same three-level walk. Primes the title-name cache (blocking) before labelling.
struct SaveRef {
    std::string user;
    std::string title_label;
    uint64_t    application_id = 0;
};
/// `pump`, when given, marks the caller as being ON THE MAIN THREAD: instead of
/// blocking for title names, this DRIVES the resolver itself and calls `pump`
/// between units so the caller can draw a frame. That distinction matters:
/// blocking on the main thread times out unresolved, and these labels become
/// BACKUP DIRECTORY NAMES persisted on the SD card. Pass nothing from a
/// transport worker, where blocking is correct.
std::vector<SaveRef> save_enumerate_all(
    const std::function<void()>& pump = nullptr);

/// Resolve a save for RESTORE, recreating the record first if it no longer
/// exists. Returns the mounted "save:/" root, or "" on failure.
///
/// Ordinary save_resolve() only matches saves in the LIVE list, so it cannot
/// mount a deleted save - exactly the one a delete-then-restore needs. The
/// application id comes from the backup's own label, the only surviving record
/// of it once the live save is gone.
std::string save_resolve_for_restore(const std::string& user,
                                     uint64_t application_id);

/// Release the mount unconditionally. For session teardown.
void save_surface_release();

} // namespace Services
