#pragma once
// source/core/save_mount.hpp
//
// Save data as a browseable three-level surface:
//
//     /Save Data/<User>/<Title>/...
//
// Save data is NOT a filesystem that can be mounted once. Each title's save is a
// separate filesystem mounted per (user, application) pair, so the catalog's
// "one vfs_root" model does not describe it — the top two levels are synthesized
// from account + save-data enumeration, and only the leaf is a real mount.
//
// MOUNT LIFECYCLE — SINGLE SLOT (user's chosen design):
// At most ONE save is mounted at a time. Touching a path under a different title
// unmounts the current one and mounts that; touching anything that is not inside a
// title's folder releases the mount entirely. So browsing
//   /Save Data/User1/Title1  ->  /Save Data/User1  ->  /Save Data/User1/Title2
// mounts Title1, releases it, then mounts Title2.
//
// Why single-slot: the system allows only a limited number of mounted save
// filesystems, and a client that browses quickly could otherwise exhaust them. One
// slot cannot leak more than one mount, and the failure mode is a remount rather
// than an exhausted handle table.

#include <cstdint>
#include <string>
#include <vector>

namespace Core {
namespace SaveMount {

struct User {
    uint64_t    uid_lo = 0;      // AccountUid is 128-bit; kept split so this
    uint64_t    uid_hi = 0;      // header needs no libnx types
    std::string name;            // profile nickname, sanitized for a filename
};

struct SaveEntry {
    uint64_t application_id = 0;
    uint64_t save_data_id   = 0;   // the fs record id — DELETION is by this, not
                                   // by application_id, so it must be carried here
    uint64_t size_bytes     = 0;   // best-effort; 0 if unknown
};

// A save-data record carrying its owning uid, unscoped to any already-known
// live user — the counterpart list_saves() cannot provide, since it only ever
// shows what a KNOWN User has. Exists for exactly one purpose: finding saves
// whose uid does not match anything list_users() currently returns, i.e. an
// account was deleted but its save data was not (the OS does not do this
// automatically).
struct OrphanCandidateSave {
    uint64_t uid_lo         = 0;
    uint64_t uid_hi         = 0;
    uint64_t application_id = 0;
    uint64_t save_data_id   = 0;
};

/// All registered users on the console. Empty if the account service is
/// unavailable — callers should show an empty folder, not an error.
///
/// `ok`, if provided, is set to false when the underlying accountListAllUsers
/// call itself failed — as opposed to genuinely succeeding with zero users.
/// Existing callers that only ever display the list can ignore this (hence the
/// default nullptr); a caller that would treat "empty" as license to delete
/// something (Tools: saves of deleted users) cannot safely ignore it — an empty
/// list on failure must not be read as "no live users, so everything else is
/// orphaned."
std::vector<User> list_users(bool* ok = nullptr);

/// Titles that have save data for this user.
std::vector<SaveEntry> list_saves(const User& u);

/// Every ACCOUNT save-data record on the console, regardless of which user (or
/// former user) owns it — unlike list_saves(), which only ever shows what a
/// known, currently-live User has. Same fsOpenSaveDataInfoReader /
/// FsSaveDataType_Account walk list_saves() already uses (hardware-verified via
/// Save Manager), just without the per-user filter, and carrying uid instead of
/// discarding it.
std::vector<OrphanCandidateSave> list_all_account_saves();

/// CREATE an account save-data record for (user, application_id) if it does not
/// already exist, so a DELETED save can be restored into. No-op-success if the
/// save already exists.
///
/// This is the counterpart to delete_save_record and, like it, is the kind of fs
/// call §5.4 says to verify against a real header — which cannot be done in the
/// build sandbox. It is therefore isolated here, takes explicit ids, logs its
/// Result, and the definition records the fallback spellings to try. It does NOT
/// mount; the caller mounts afterward as usual.
///
/// The size fields use conservative defaults; the OS rounds/enforces per title.
/// Returns true if the save exists after the call (created, or already present).
bool ensure_save_exists(const User& u, uint64_t application_id);

/// DELETE a save-data record entirely — not its contents, the record itself, so
/// the title stops appearing in the save list and the game creates a fresh save
/// next launch. This is the "remove" that "wipe" is not.
///
/// Deletion is BY save_data_id, which is why SaveEntry carries it: the id is the
/// only stable handle to the exact record. Deleting by application_id would be
/// ambiguous — nothing in the fs API takes it for deletion — and guessing is
/// exactly what 5.4 forbids on this console.
///
/// The save MUST NOT be mounted when this is called: deleting the filesystem out
/// from under a live mount is undefined. Callers release first.
///
/// This is the single most destructive fs call in the app, so it is isolated
/// here, takes an explicit id (never a "current title" implicit), and logs its
/// Result. Signature confirmed against a real libnx fs.h (line 513:
/// `Result fsDeleteSaveDataFileSystemBySaveDataSpaceId(FsSaveDataSpaceId
/// save_data_space_id, u64 saveID); ///< [2.0.0+]`) and separately hardware-
/// verified working via Save Manager's own Delete button — both confirmations
/// happened after this was first written "on knowledge", so this note replaces
/// the original "UNVERIFIED CALL" flag rather than deleting that history
/// silently: it WAS unverified when written, it no longer is.
bool delete_save_record(uint64_t save_data_id);

/// Mount this user's save for `application_id` as "save:", releasing whatever was
/// mounted before. Returns false if it cannot be mounted (no save, or fs error).
/// Calling it for the already-mounted pair is cheap and does not remount.
bool ensure_mounted(const User& u, uint64_t application_id);

/// Flush pending changes to the mounted save so they SURVIVE the unmount.
///
/// This is not optional bookkeeping. A Switch save filesystem is journalled:
/// writes made through "save:" live in the journal and are DISCARDED at unmount
/// unless they are committed. Without this, a file copied into a save appears to
/// write successfully, lists correctly while the mount is live, and is simply
/// gone the next time the title is opened — a silent data loss with a success
/// reported to the client. Every save manager on this platform commits for the
/// same reason.
///
/// Returns false if nothing is mounted or the commit failed; a false here means
/// the write did NOT persist and the caller should report failure, not success.
bool commit();

/// Unmount whatever is mounted. Safe to call when nothing is.
void release();

/// True if `save:` currently holds this exact (user, application) pair.
bool is_mounted(const User& u, uint64_t application_id);

} // namespace SaveMount
} // namespace Core
