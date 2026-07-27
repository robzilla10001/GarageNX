#pragma once
// source/services/save_write.hpp
//
// Filesystem mutations that COMMIT when they touched a save.
//
// ── Why this exists ──────────────────────────────────────────────────────────
// A Switch save filesystem is journalled: writes are discarded at unmount unless
// fsdevCommitDevice() runs. So every mutation of a save path must be followed by a
// commit, and "must be followed by" was, until now, a thing fourteen call sites
// each remembered separately across four files.
//
// That is not a rule, it is a hope, and it has failed twice already:
//   * FileBrowserScreen went its whole life without committing — correct, because
//     it only ever saw SD and NAND — until the Save Manager pointed it at
//     "save:/" and an on-device delete started silently reverting.
//   * Writing THIS header found two more that were still live: do_new_dir() and
//     do_new_file() create a folder/file in the active pane and never commit, so
//     making a folder inside a save quietly vanished at unmount.
//
// Fourteen call sites remembering independently is a bug waiting for the
// fifteenth. These wrappers make the commit part of the operation instead, and
// tests/save_commit_discipline_test.cpp mechanically fails the build if a raw
// Fs:: mutation reappears in a file that can see save paths.
//
// ── Using them ───────────────────────────────────────────────────────────────
// Call SaveWrite::* instead of Fs::* for any mutation whose path COULD be a save.
// The commit is a no-op (returning success) on every other surface, so there is no
// need to know in advance which surface you are on — which is the entire point,
// since that is exactly the thing callers get wrong.
//
// If a mutation genuinely cannot touch a save — writing a log file to a fixed SD
// path, say — call Fs:: directly and mark the line with a trailing
//     // NOT-A-SAVE: <reason>
// comment. The discipline test accepts that marker and rejects anything else, so
// the exemption is explicit and reviewable rather than silent.

#include "core/fs.hpp"
#include "services/save_surface.hpp"

#include <string>
#include <vector>

namespace Services::SaveWrite {

/// Create a directory, committing if it landed in a save.
inline bool make_directory(const std::string& path) {
    if (!Fs::make_directory(path)) return false;
    return save_commit_if_save_path(path);
}

/// Create an empty file, committing if it landed in a save.
inline bool create_empty_file(const std::string& path) {
    if (!Fs::create_empty_file(path)) return false;
    return save_commit_if_save_path(path);
}

/// Rename/move within a filesystem. Commits against the DESTINATION, which is the
/// side that gained data; when both sides are in the same save that is the same
/// commit, and when they are not, the source side is handled by its own surface.
inline bool rename(const std::string& from, const std::string& to) {
    if (!Fs::rename(from, to)) return false;
    return save_commit_if_save_path(to);
}

inline bool remove_file(const std::string& path) {
    if (!Fs::remove_file(path)) return false;
    return save_commit_if_save_path(path);
}

inline bool remove_directory_recursive(const std::string& path) {
    if (!Fs::remove_directory_recursive(path)) return false;
    return save_commit_if_save_path(path);
}

/// Batch delete. One commit for the batch: a batch always comes from a single
/// pane, so its entries share a surface.
inline bool remove_many(const std::vector<std::string>& paths, Fs::Progress& progress) {
    const bool ok = Fs::remove_many(paths, progress);
    if (paths.empty()) return ok;
    // Commit even when the removal reported failure: a PARTIAL delete still
    // changed the save, and leaving those changes uncommitted would revert some
    // of them at unmount, which is a stranger state than either outcome.
    const bool committed = save_commit_if_save_path(paths.front());
    return ok && committed;
}

/// Commit after a write this module did not perform — a streamed upload, or any
/// sequence that wrote through a FILE* it owns. Call once the bytes are down.
inline bool after_write(const std::string& path) {
    return save_commit_if_save_path(path);
}

} // namespace Services::SaveWrite
