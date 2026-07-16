#pragma once
// source/core/fs.hpp
// Filesystem abstraction over POSIX. Works with libnx device-prefixed paths
// (sdmc:/, romfs:/) and standard PC paths. All browser file operations go
// through this — never call POSIX directly from UI code.
//
// Long-running operations (copy, move, recursive delete) run through the
// FileOp interface with progress reporting so the UI thread never blocks.

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <atomic>

namespace Fs {

// ─── Types ────────────────────────────────────────────────────────────────────

enum class EntryType { File, Directory, Unknown };

struct Entry {
    std::string name;          // filename only, no path
    EntryType   type = EntryType::Unknown;
    uint64_t    size = 0;      // bytes (0 for directories)

    // Convenience: lowercase extension without dot, e.g. "nsp", "json", "" for none
    std::string extension() const;

    bool is_dir() const { return type == EntryType::Directory; }
};

// ─── Directory listing ────────────────────────────────────────────────────────

/// List the contents of a directory.
/// Returns entries sorted: directories first (alphabetical), then files (alphabetical).
/// Does NOT include "." or "..". On error returns an empty vector and sets ok=false.
std::vector<Entry> list(const std::string& path, bool* ok = nullptr);

// ─── Path helpers ─────────────────────────────────────────────────────────────

/// Join a directory path and a name, handling separators and device prefixes.
/// join("sdmc:/switch", "app") → "sdmc:/switch/app"
/// join("sdmc:/", "switch")    → "sdmc:/switch"
std::string join(const std::string& dir, const std::string& name);

/// Return the parent directory of a path.
/// parent("sdmc:/switch/app") → "sdmc:/switch"
/// parent("sdmc:/")           → "sdmc:/"  (can't go above the device root)
std::string parent(const std::string& path);

/// Return the final component (file or dir name) of a path.
/// basename("sdmc:/switch/app.nro") → "app.nro"
std::string basename(const std::string& path);

/// True if the path is a device/mount root that can't be navigated above
/// (e.g. "sdmc:/", "romfs:/", "/").
bool is_root(const std::string& path);

// ─── Queries ──────────────────────────────────────────────────────────────────

bool exists(const std::string& path);
bool is_directory(const std::string& path);
uint64_t file_size(const std::string& path);

// ─── Simple (fast) operations ─────────────────────────────────────────────────
// These complete quickly and can be called directly on the UI thread.

bool make_directory(const std::string& path);
bool create_empty_file(const std::string& path);
bool rename(const std::string& from, const std::string& to);
bool remove_file(const std::string& path);
bool remove_directory_recursive(const std::string& path);  // can be slow — see FileOp

// ─── Conflict resolution ──────────────────────────────────────────────────────

enum class Conflict { Overwrite, Skip, Rename, Cancel };

/// Given a target path that already exists, produce a non-colliding variant.
/// resolve_rename("sdmc:/a/file.nsp") → "sdmc:/a/file_1.nsp" (or _2, etc.)
std::string resolve_rename(const std::string& path);

// ─── Long-running operations (background thread + progress) ────────────────────

/// Progress state for a running file operation. The worker thread updates these
/// atomically; the UI polls them each frame. No locking needed for display.
struct Progress {
    std::atomic<bool>     running{false};
    std::atomic<bool>     done{false};
    std::atomic<bool>     cancelled{false};
    std::atomic<bool>     success{false};
    std::atomic<uint64_t> bytes_total{0};
    std::atomic<uint64_t> bytes_done{0};
    std::atomic<int>      files_total{0};
    std::atomic<int>      files_done{0};

    // Current file being processed (guarded by a simple spinlock-free swap;
    // UI reads it best-effort, exact tearing is acceptable for a label).
    std::string current_file;

    // Request cancellation from the UI thread.
    void request_cancel() { cancelled.store(true); }

    float fraction() const {
        uint64_t t = bytes_total.load();
        if (t == 0) return 0.f;
        return static_cast<float>(bytes_done.load()) / static_cast<float>(t);
    }

    void reset() {
        running = false; done = false; cancelled = false; success = false;
        bytes_total = 0; bytes_done = 0; files_total = 0; files_done = 0;
        current_file.clear();
    }
};

/// Callback invoked when a destination path already exists during copy/move.
/// Called from the worker thread. Must return a Conflict decision.
/// The implementation typically blocks on a UI-thread-answered promise.
using ConflictResolver = std::function<Conflict(const std::string& dest_path)>;

/// Copy a file or directory (recursive) from src to dst_dir.
/// Runs synchronously on the calling thread — callers should run it on a worker.
/// Updates `progress` throughout. Returns true on success.
bool copy(const std::string& src,
          const std::string& dst_dir,
          Progress& progress,
          const ConflictResolver& resolver);

/// Move (copy then delete source). Same threading contract as copy().
bool move(const std::string& src,
          const std::string& dst_dir,
          Progress& progress,
          const ConflictResolver& resolver);

/// Recursively delete multiple paths. Same threading contract.
bool remove_many(const std::vector<std::string>& paths, Progress& progress);

// ─── Size formatting ──────────────────────────────────────────────────────────

/// Format a byte count as a human-readable string: "1.4 GB", "512 KB", "48 B".
std::string format_size(uint64_t bytes);

} // namespace Fs
