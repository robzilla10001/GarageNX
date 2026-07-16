// source/core/fs.cpp

#include "core/fs.hpp"
#include <SDL2/SDL.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace Fs {

// ─── Entry helpers ────────────────────────────────────────────────────────────

std::string Entry::extension() const {
    auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return "";
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// ─── Path helpers ─────────────────────────────────────────────────────────────

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == ':') return dir + name;
    return dir + "/" + name;
}

bool is_root(const std::string& path) {
    // Plain filesystem root
    if (path == "/") return true;
    // Device-prefixed root: ends with ":/" and has no path component after it,
    // e.g. "sdmc:/", "romfs:/", "user:/"
    auto colon = path.find(':');
    if (colon != std::string::npos) {
        // Everything after the colon should be just "/" or empty
        std::string after = path.substr(colon + 1);
        if (after == "/" || after.empty()) return true;
    }
    return false;
}

std::string parent(const std::string& path) {
    if (is_root(path)) return path;

    // Strip a trailing slash (unless it's the root slash we already handled)
    std::string p = path;
    if (p.size() > 1 && p.back() == '/') p.pop_back();

    auto slash = p.rfind('/');
    if (slash == std::string::npos) return p;

    // If the slash is immediately after a device colon ("sdmc:/x"), the parent
    // is the device root "sdmc:/".
    if (slash > 0 && p[slash - 1] == ':') {
        return p.substr(0, slash + 1);
    }

    // Don't strip below "sdmc:/" — if what remains ends in ':' add the slash back
    std::string result = p.substr(0, slash);
    if (!result.empty() && result.back() == ':') result += "/";
    return result.empty() ? "/" : result;
}

std::string basename(const std::string& path) {
    std::string p = path;
    if (p.size() > 1 && p.back() == '/') p.pop_back();
    auto slash = p.rfind('/');
    if (slash == std::string::npos) return p;
    return p.substr(slash + 1);
}

// ─── Queries ──────────────────────────────────────────────────────────────────

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

uint64_t file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

// ─── Directory listing ────────────────────────────────────────────────────────

std::vector<Entry> list(const std::string& path, bool* ok) {
    std::vector<Entry> entries;

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        SDL_Log("Fs::list — cannot open %s", path.c_str());
        if (ok) *ok = false;
        return entries;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        Entry e;
        e.name = name;

        // Prefer d_type when available; fall back to stat if unknown.
        std::string full = join(path, name);
        bool is_dir_flag = false;

#ifdef _DIRENT_HAVE_D_TYPE
        if (ent->d_type == DT_DIR)       { is_dir_flag = true;  }
        else if (ent->d_type == DT_REG)  { is_dir_flag = false; }
        else {
            // DT_UNKNOWN — stat it
            struct stat st;
            if (stat(full.c_str(), &st) == 0) is_dir_flag = S_ISDIR(st.st_mode);
        }
#else
        struct stat st;
        if (stat(full.c_str(), &st) == 0) is_dir_flag = S_ISDIR(st.st_mode);
#endif

        if (is_dir_flag) {
            e.type = EntryType::Directory;
            e.size = 0;
        } else {
            e.type = EntryType::File;
            struct stat st;
            e.size = (stat(full.c_str(), &st) == 0)
                     ? static_cast<uint64_t>(st.st_size) : 0;
        }

        entries.push_back(std::move(e));
    }
    closedir(dir);

    // Sort: directories first, then files, each alphabetical (case-insensitive)
    auto casecmp = [](const std::string& a, const std::string& b) {
        return std::lexicographical_compare(
            a.begin(), a.end(), b.begin(), b.end(),
            [](unsigned char x, unsigned char y) {
                return std::tolower(x) < std::tolower(y);
            });
    };
    std::sort(entries.begin(), entries.end(),
        [&](const Entry& a, const Entry& b) {
            if (a.is_dir() != b.is_dir()) return a.is_dir();  // dirs first
            return casecmp(a.name, b.name);
        });

    if (ok) *ok = true;
    return entries;
}

// ─── Simple operations ────────────────────────────────────────────────────────

bool make_directory(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == 0) return true;
    return exists(path) && is_directory(path);  // already exists is fine
}

bool create_empty_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool rename(const std::string& from, const std::string& to) {
    return ::rename(from.c_str(), to.c_str()) == 0;
}

bool remove_file(const std::string& path) {
    return ::remove(path.c_str()) == 0;
}

bool remove_directory_recursive(const std::string& path) {
    bool ok = true;
    auto entries = list(path, &ok);
    if (!ok) return false;

    for (auto& e : entries) {
        std::string child = join(path, e.name);
        if (e.is_dir()) {
            if (!remove_directory_recursive(child)) ok = false;
        } else {
            if (!remove_file(child)) ok = false;
        }
    }
    // Remove the now-empty directory
    if (rmdir(path.c_str()) != 0) ok = false;
    return ok;
}

// ─── Conflict resolution ──────────────────────────────────────────────────────

std::string resolve_rename(const std::string& path) {
    // Split into stem and extension so "file.nsp" → "file_1.nsp"
    std::string dir  = parent(path);
    std::string name = basename(path);

    std::string stem = name, ext;
    auto dot = name.rfind('.');
    if (dot != std::string::npos && dot != 0) {
        stem = name.substr(0, dot);
        ext  = name.substr(dot);   // includes the dot
    }

    for (int i = 1; i < 10000; ++i) {
        std::string candidate = join(dir, stem + "_" + std::to_string(i) + ext);
        if (!exists(candidate)) return candidate;
    }
    return path;  // give up (extremely unlikely)
}

// ─── Byte-copy helper ─────────────────────────────────────────────────────────

// Copy a single regular file, updating progress. Returns false on error/cancel.
static bool copy_file_bytes(const std::string& src, const std::string& dst,
                             Progress& progress) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) { SDL_Log("Fs::copy — cannot read %s", src.c_str()); return false; }

    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { SDL_Log("Fs::copy — cannot write %s", dst.c_str()); fclose(in); return false; }

    // 1 MB buffer — good throughput on the Switch's SD without hogging memory.
    constexpr size_t BUF = 1024 * 1024;
    std::vector<char> buffer(BUF);

    bool ok = true;
    size_t n;
    while ((n = fread(buffer.data(), 1, BUF, in)) > 0) {
        if (progress.cancelled.load()) { ok = false; break; }
        if (fwrite(buffer.data(), 1, n, out) != n) {
            SDL_Log("Fs::copy — write error on %s", dst.c_str());
            ok = false;
            break;
        }
        progress.bytes_done.fetch_add(n);
    }

    fclose(in);
    fclose(out);

    if (!ok) remove_file(dst);  // clean up partial file on failure/cancel
    return ok;
}

// ─── Recursive size / count (for progress totals) ─────────────────────────────

static void accumulate_totals(const std::string& path, bool is_dir,
                               uint64_t& bytes, int& files) {
    if (!is_dir) {
        bytes += file_size(path);
        files += 1;
        return;
    }
    bool ok = true;
    auto entries = list(path, &ok);
    if (!ok) return;
    for (auto& e : entries) {
        accumulate_totals(join(path, e.name), e.is_dir(), bytes, files);
    }
}

// ─── Recursive copy core ──────────────────────────────────────────────────────

static bool copy_recursive(const std::string& src, const std::string& dst,
                            bool src_is_dir, Progress& progress,
                            const ConflictResolver& resolver) {
    if (progress.cancelled.load()) return false;

    if (!src_is_dir) {
        // File. Handle conflict if destination exists.
        std::string target = dst;
        if (exists(target)) {
            Conflict c = resolver ? resolver(target) : Conflict::Overwrite;
            switch (c) {
                case Conflict::Skip:      progress.files_done.fetch_add(1); return true;
                case Conflict::Cancel:    progress.request_cancel();        return false;
                case Conflict::Rename:    target = resolve_rename(target);  break;
                case Conflict::Overwrite: break;
            }
        }
        progress.current_file = basename(src);
        bool ok = copy_file_bytes(src, target, progress);
        progress.files_done.fetch_add(1);
        return ok;
    }

    // Directory. Create it at the destination, then recurse.
    if (!make_directory(dst)) {
        SDL_Log("Fs::copy — cannot create dir %s", dst.c_str());
        return false;
    }

    bool ok = true;
    bool listed = true;
    auto entries = list(src, &listed);
    if (!listed) return false;

    for (auto& e : entries) {
        if (progress.cancelled.load()) return false;
        std::string child_src = join(src, e.name);
        std::string child_dst = join(dst, e.name);
        if (!copy_recursive(child_src, child_dst, e.is_dir(), progress, resolver))
            ok = false;
    }
    return ok;
}

// ─── Public long-running ops ──────────────────────────────────────────────────

bool copy(const std::string& src, const std::string& dst_dir,
          Progress& progress, const ConflictResolver& resolver) {
    progress.reset();
    progress.running = true;

    bool src_is_dir = is_directory(src);

    // Compute totals up front for an accurate progress bar.
    uint64_t total_bytes = 0;
    int total_files = 0;
    accumulate_totals(src, src_is_dir, total_bytes, total_files);
    progress.bytes_total = total_bytes;
    progress.files_total = total_files;

    std::string dst = join(dst_dir, basename(src));

    bool ok = copy_recursive(src, dst, src_is_dir, progress, resolver);

    progress.success = ok && !progress.cancelled.load();
    progress.done    = true;
    progress.running = false;
    return progress.success;
}

bool move(const std::string& src, const std::string& dst_dir,
          Progress& progress, const ConflictResolver& resolver) {
    // Fast path: same-filesystem rename. Try it first; if it works, we're done
    // instantly with no byte copying. rename() fails across devices (EXDEV),
    // in which case we fall back to copy + delete.
    std::string dst = join(dst_dir, basename(src));

    if (!exists(dst) && rename(src, dst)) {
        progress.reset();
        progress.success = true;
        progress.done = true;
        return true;
    }

    // Slow path: copy then remove source.
    bool ok = copy(src, dst_dir, progress, resolver);
    if (ok && !progress.cancelled.load()) {
        if (is_directory(src)) remove_directory_recursive(src);
        else                   remove_file(src);
    }
    return ok;
}

bool remove_many(const std::vector<std::string>& paths, Progress& progress) {
    progress.reset();
    progress.running = true;

    // Count files for progress
    uint64_t total_bytes = 0;
    int total_files = 0;
    for (auto& p : paths) {
        accumulate_totals(p, is_directory(p), total_bytes, total_files);
    }
    progress.files_total = total_files;
    progress.bytes_total = total_bytes;

    bool ok = true;
    for (auto& p : paths) {
        if (progress.cancelled.load()) { ok = false; break; }
        progress.current_file = basename(p);
        if (is_directory(p)) {
            if (!remove_directory_recursive(p)) ok = false;
        } else {
            progress.bytes_done.fetch_add(file_size(p));
            if (!remove_file(p)) ok = false;
        }
        progress.files_done.fetch_add(1);
    }

    progress.success = ok && !progress.cancelled.load();
    progress.done    = true;
    progress.running = false;
    return progress.success;
}

// ─── Size formatting ──────────────────────────────────────────────────────────

std::string format_size(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024ULL) {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

} // namespace Fs
