#pragma once
// source/services/save_surface.hpp
//
// THE shared Save Data surface. Everything a transport needs to present
//
//     /Save Data/<User>/<Title>/...
//
// lives here, so FTP, MTP and (later) HTTP cannot drift into different user
// lists, different title labels, or — the dangerous one — different mount
// policies. This is the same anti-drift move as StorageCatalog for storages and
// NspStream for NSP building: ONE implementation, several thin adapters.
//
// Extracted from ftp_server.cpp when MTP became the second consumer. FTP's
// version was hardware-verified first, so the extraction unifies onto the proven
// implementation rather than the other way round (cf. dump.cpp -> NspStream).
//
// THE MOUNT POLICY LIVES IN save_resolve(). Every request routes through it, and
// it applies the single-slot rule for that request: a file-level path mounts its
// (user, title), releasing whatever was mounted before; anything shallower
// releases entirely. That is what makes "navigate away from a title and it
// unloads" true for any transport without any of them tracking navigation state.
//
// ── Synthetic paths (MTP) ────────────────────────────────────────────────────
// FTP re-resolves a full display path on every command, so it never needs to
// name a save object except by that path. MTP does: it hands the host an opaque
// u32 handle that must stay valid for the session, and handles are interned
// BY PATH. A real save path ("save:/slot1.dat") is NOT a safe handle key,
// because "save:" means a different filesystem depending on what is mounted —
// two titles' files would collide on one handle and the host would silently get
// the wrong bytes.
//
// So MTP interns save objects under a synthetic prefix that carries the full
// three-level identity:
//
//     savedata:/<User>/<Title>/<rest...>
//
// The part after the prefix is exactly the `rel` that sp_split_save() already
// parses, so no second parser exists (and none can drift). The concrete
// "save:/..." path is derived at the moment of use, via save_resolve().
//
// The prefix deliberately does NOT begin with "save:" — StorageCatalog::
// surface_for_vfs() and mtp_storage_for_path() match a mount prefix with a plain
// string compare, and a synthetic path that matched "save:" would be mistaken
// for a real mounted one by the write guard.

#include "services/storage_paths.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Services {

// ── Synthetic path helpers (PURE — no libnx, host-testable) ──────────────────

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

/// True if the synthetic path names one of the two SYNTHESIZED levels — a user
/// folder or a title folder. Those are directories by construction, so a
/// transport can answer "is this a directory?" WITHOUT mounting anything.
///
/// This matters more than it looks: an MTP host asks for an ObjectInfo for every
/// object it lists, so answering the title level by mounting would mount and
/// unmount every save on the console just to browse one folder — exactly the
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
/// The distinction matters because a title folder in the display hierarchy —
/// "/Save Data/<User>/<Title>" — resolves to exactly this. It is a perfectly good
/// path to LIST, which is why browsing works, but it is not a legal target for
/// delete, rename or any other mutation: it is the filesystem, not a file in it.
///
/// Without this check a client that deletes a folder by deleting its contents and
/// then the folder itself — which is what an ordinary FTP or MTP client does —
/// gets prompted to confirm removing the title folder, the user approves, and the
/// operation fails anyway because you cannot rmdir a mount point. A confirmation
/// dialog for something that cannot succeed is worse than no dialog: it teaches
/// people that approving these prompts does nothing.
inline bool save_is_mount_root(const std::string& vfs_path) {
    return vfs_path == "save:" || vfs_path == "save:/";
}

// ── Listings for the two synthesized levels ──────────────────────────────────
// Both RELEASE the mount before listing: by definition neither is inside a title
// folder, so the single-slot rule says nothing should stay mounted.

/// Level 1 — every registered user, as folder names. Empty (not an error) if the
/// account service is unavailable.
std::vector<std::string> save_user_names();

/// Level 2 — the titles this user has save data for, labelled "<Name> [APPID]"
/// (or "Title <APPID>" when the title is not installed, which is normal: save
/// data outlives an uninstalled game).
///
/// `block` controls how title-name resolution is obtained:
///   - true  (default): wait for the main loop to resolve names, exactly like the
///            Installed Titles listing. Correct for a TRANSPORT WORKER, whose
///            wait the main loop can service.
///   - false: request resolution without blocking and label with whatever is
///            ready now, falling back to "Title <id>" for the rest. REQUIRED for
///            a MAIN-THREAD caller (the on-device screen): blocking there parks
///            the very loop that does the resolving, so it waits out the full
///            timeout and returns unresolved anyway. The screen re-labels as
///            names fill in over subsequent frames.
std::vector<std::string> save_title_labels(const std::string& user, bool block = true);

/// True if `label` is an id-only fallback ("Title <16 hex>") rather than a
/// resolved title name. The on-device screen uses this to know when to STOP
/// re-labelling — a prefix test would misfire on a real game actually named
/// "Title ...", pinning the screen in a permanent per-frame rebuild.
///
/// Inline (pure, no libnx) so the host test can link it, like the other helpers
/// in this header.
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
/// successfully; returns "" for the synthesized levels and for any failure —
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
/// surface it just wrote to — a non-save path is simply a no-op.
bool save_commit_if_save_path(const std::string& vfs_path);

/// Wipe a mounted save: delete everything inside it, then commit, so the title
/// is left with an empty save rather than the save it had.
///
/// This is what "delete the title folder" MEANS for a save. The title folder is
/// synthesized — it is the mount point, not a real directory — so it cannot be
/// removed with rmdir, and a client that tries (by clearing the contents and then
/// removing the folder) gets a confusing failure on the final step. Rather than
/// refuse the intent, transports route a delete of the save root here.
///
/// `mounted_root` must be the concrete "save:/" the request already resolved to;
/// the caller has therefore already mounted the right (user, title) through the
/// shared choke point. Returns false if nothing is mounted, if a child could not
/// be removed, or if the commit failed — and a false means the save may be
/// PARTIALLY emptied, exactly like a failed restore, which is why the on-device
/// path snapshots first.
bool save_wipe(const std::string& mounted_root);

/// DELETE the save record for (user, title_label) entirely, so the title leaves
/// the save list and the game creates a fresh save next launch. Distinct from
/// save_wipe(), which empties the contents but KEEPS the record.
///
/// Releases any mount first (deletion is by fs record id, and the record cannot
/// be mounted during deletion), looks up the save_data_id from the label the same
/// way save_resolve() matches a title, and calls the fs deletion primitive.
///
/// Returns false if the user or title is not found, or if deletion failed.
///
/// Like every destructive save op this must be gated by confirmation, and the
/// on-device path snapshots first — but this function does NOT snapshot or
/// confirm itself, exactly as save_resolve()/save_wipe() do not: doing so would
/// hide a second write behind a call that reads like one primitive, and a
/// snapshot the caller never surfaced is not a rollback anyone can use.
bool save_delete_record(const std::string& user, const std::string& title_label);

/// Build the display label for a save: "<Name> [APPID]", or "Title <APPID>" when
/// the title is not installed. This is the SINGLE definition of that label —
/// save_title_labels(), save_resolve()'s matching, and auto-backup all use it, so
/// a label built in one place always matches a label matched in another. (Getting
/// this even slightly inconsistent would make auto-backup write to a directory the
/// manual restore path could not find.)
///
/// Name resolution is cache-only (installed_titles_name_for_app), so callers that
/// need resolved names must have primed the cache first.
std::string save_build_label(uint64_t application_id);

/// Enumerate every (user, title_label, application_id) live save on the console,
/// so callers that must act on all of them — auto-backup — do not re-derive the
/// same three-level walk. Primes the title-name cache (blocking) before labelling.
struct SaveRef {
    std::string user;
    std::string title_label;
    uint64_t    application_id = 0;
};
/// `pump`, when given, marks the caller as being ON THE MAIN THREAD. Instead of
/// blocking for title names — which parks the very loop that resolves them — this
/// DRIVES the resolver itself (installed_titles_tick) and calls `pump` between
/// units so the caller can draw a frame.
///
/// That distinction is not cosmetic here. Blocking on the main thread times out
/// with names unresolved, and this function's labels become BACKUP DIRECTORY
/// NAMES: a save backed up while names were unresolved is written to
/// "Title 0100000000010000/" and stays that way forever. The label is not a
/// display string, it is persisted state, so it has to be right the first time.
///
/// Pass nothing from a transport worker, where blocking is correct and the main
/// loop is free to service the wait.
std::vector<SaveRef> save_enumerate_all(
    const std::function<void()>& pump = nullptr);

/// Resolve a save for RESTORE, recreating the record first if it no longer
/// exists. Returns the mounted "save:/" root, or "" on failure.
///
/// Ordinary save_resolve() only matches saves in the LIVE list, so it cannot
/// mount a deleted save — which is exactly the one a delete-then-restore needs.
/// This looks the user up by name, ensures the save exists (creating it for the
/// given application id if a delete removed it), then mounts. The application id
/// comes from the backup's own label, which is the only surviving record of it
/// once the live save is gone.
std::string save_resolve_for_restore(const std::string& user,
                                     uint64_t application_id);

/// Release the mount unconditionally. For session teardown.
void save_surface_release();

} // namespace Services
