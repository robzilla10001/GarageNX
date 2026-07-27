#pragma once
// source/core/save_backup.hpp
//
// On-device save backup: copy a title's save data to the SD card.
//
// This is the SAFE half of 3e. Restore is the dangerous half and is deliberately
// not here yet — see the roadmap. Backup only ever READS the save and writes to
// the SD card, so the worst outcome is a wasted copy, and it is a prerequisite
// for doing restore safely: the pre-restore snapshot uses this exact code.
//
// Reading a save over FTP/MTP already works, so this is not the only way to get a
// save off the console. What it adds is a backup with no PC involved, and the
// rollback story that makes restore defensible.
//
// ── Layout ───────────────────────────────────────────────────────────────────
//
//     <backup_root>/<User>/<Title [APPID]>/<YYYYMMDD-HHMMSS>/...
//
// One directory per run, stamped so nothing is ever silently overwritten and the
// newest is obvious by name. backup_root comes from Config::Paths::save_backup.

#include "core/fs.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Core::SaveBackup {

// ── Pure naming (host-tested) ────────────────────────────────────────────────

/// Make one path component safe for the SD card's filesystem.
///
/// This is not cosmetic. Save folders are named after ACCOUNT NICKNAMES and GAME
/// TITLES, neither of which is constrained to anything: real titles contain ':'
/// ("Pokemon: Let's Go"), '?', '/', and trailing dots. FAT32 and exFAT reject
/// those outright, so an unsanitised name does not produce an ugly folder — it
/// produces a failed copy, on the exact titles a user is most likely to want
/// backed up.
///
/// Rules: illegal and control characters become '_'; leading/trailing dots and
/// spaces are stripped (FAT silently drops them, which makes a path the caller
/// built no longer the path that exists); an empty or fully-stripped result
/// becomes "unnamed"; length is capped so the total path stays workable.
inline std::string sanitize_component(const std::string& name, size_t max_len = 96) {
    static const std::string illegal = "\\/:*?\"<>|";

    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20 || illegal.find((char)c) != std::string::npos) {
            // Collapse runs so "A: B" does not become "A__B".
            if (!out.empty() && out.back() == '_') continue;
            out.push_back('_');
        } else {
            out.push_back((char)c);
        }
    }

    // FAT silently DROPS trailing dots and spaces, so a caller that built a path
    // ending in one would look for a directory that does not exist under that
    // name — the target and the created directory disagree, which is a horrible
    // failure to debug on a console. Strip them here instead.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ' || out.back() == '_'))
        out.pop_back();
    size_t first = 0;
    while (first < out.size() && (out[first] == ' ' || out[first] == '.' ||
                                  out[first] == '_')) ++first;
    out = out.substr(first);

    if (out.size() > max_len) {
        out.resize(max_len);
        // Truncation must not leave a half-written UTF-8 sequence: in a FILENAME
        // some tools reject the name outright rather than just drawing a box.
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0xC0) out.pop_back();
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    }

    return out.empty() ? std::string("unnamed") : out;
}

/// Build the destination directory for one backup run. `stamp` should come from
/// Core::DateTime::sortable_stamp_now(). Every component is sanitized.
inline std::string backup_dir_for(const std::string& backup_root,
                                  const std::string& user,
                                  const std::string& title_label,
                                  const std::string& stamp) {
    std::string root = backup_root;
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + sanitize_component(user)
                 + "/" + sanitize_component(title_label)
                 + "/" + sanitize_component(stamp);
}

/// The directory holding every backup of one title: the parent of the stamped
/// run directories.
inline std::string backup_title_dir(const std::string& backup_root,
                                    const std::string& user,
                                    const std::string& title_label) {
    std::string root = backup_root;
    while (!root.empty() && root.back() == '/') root.pop_back();
    return root + "/" + sanitize_component(user)
                 + "/" + sanitize_component(title_label);
}

/// Does this directory name look like one of our stamps ("20260712-143005")?
///
/// Used to filter what is listed as a restorable backup. A user's SD card is
/// their own: a stray folder, a half-extracted archive, a ".DS_Store" from a Mac
/// can all appear beside real backups. Offering one of those as a restore source
/// would replace a save with garbage, so anything not matching the exact shape is
/// simply not listed.
inline bool is_backup_stamp(const std::string& name) {
    if (name.size() != 15) return false;          // YYYYMMDD-HHMMSS
    for (size_t i = 0; i < name.size(); ++i) {
        if (i == 8) { if (name[i] != '-') return false; }
        else if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

/// Order stamps newest-first. Stamps are fixed-width and zero-padded, so
/// descending lexicographic order IS reverse chronological — which is the whole
/// reason sortable_stamp() exists rather than reusing log_stamp().
inline void sort_newest_first(std::vector<std::string>& stamps) {
    std::sort(stamps.begin(), stamps.end(), std::greater<std::string>());
}

/// Extract the application id from a title-label folder name. Handles BOTH forms
/// a backup directory can carry:
///
///   "<Name> [016 hex]"  — the normal, resolved form; id is the LAST bracket group
///   "Title 016 hex"     — the id-only fallback, written when the title's name was
///                         not resolvable at backup time (an uninstalled game, or
///                         a backup taken before the name cache had filled)
///
/// Returns 0 if neither form matches.
///
/// Pure, so it is host-tested. It matters for restoring a DELETED save: once the
/// live save is gone, this label — the folder name on the SD card — is the only
/// surviving record of which title the backup belongs to, and the id inside it is
/// what recreation needs. Parsing the LAST bracket group is deliberate: a game
/// name may itself contain brackets ("Ys [Memoire]"), but the id we appended is
/// always the final group.
///
/// The fallback form matters just as much for DISPLAY: a backup folder named
/// "Title 0100..." can be shown under its real game name by recovering the id here
/// and re-resolving, without renaming anything on disk.
inline uint64_t app_id_from_label(const std::string& title_label) {
    auto parse_hex16 = [](const std::string& hex, uint64_t& out) -> bool {
        if (hex.size() != 16) return false;
        uint64_t id = 0;
        for (char ch : hex) {
            id <<= 4;
            if (ch >= '0' && ch <= '9')      id |= (uint64_t)(ch - '0');
            else if (ch >= 'A' && ch <= 'F') id |= (uint64_t)(ch - 'A' + 10);
            else if (ch >= 'a' && ch <= 'f') id |= (uint64_t)(ch - 'a' + 10);
            else return false;
        }
        out = id;
        return true;
    };

    // Bracket form first — the normal case.
    const size_t close = title_label.rfind(']');
    if (close != std::string::npos) {
        const size_t open = title_label.rfind('[', close);
        if (open != std::string::npos) {
            uint64_t id = 0;
            if (parse_hex16(title_label.substr(open + 1, close - open - 1), id))
                return id;
        }
        return 0;
    }

    // Id-only fallback form: exactly "Title " + 16 hex.
    static const std::string prefix = "Title ";
    if (title_label.size() == prefix.size() + 16 &&
        title_label.compare(0, prefix.size(), prefix) == 0) {
        uint64_t id = 0;
        if (parse_hex16(title_label.substr(prefix.size()), id)) return id;
    }
    return 0;
}

/// Extract the calendar date (as a day-count since an arbitrary fixed epoch) from
/// a "YYYYMMDD-HHMMSS" stamp. Returns -1 if the stamp is malformed.
///
/// Pure and host-tested. Auto-backup staleness is measured in whole days, and
/// doing that by day-number avoids all the timezone and DST hazards of
/// subtracting two time_t values — the stamp is local wall-clock at backup time,
/// and "now" is local wall-clock, so comparing calendar days is exactly right and
/// hour-of-day never enters into it.
inline long stamp_day_number(const std::string& stamp) {
    if (!is_backup_stamp(stamp)) return -1;
    auto d = [&](int off, int len) {
        int v = 0;
        for (int i = 0; i < len; ++i) v = v * 10 + (stamp[off + i] - '0');
        return v;
    };
    const int y = d(0, 4), m = d(4, 2), day = d(6, 2);
    if (m < 1 || m > 12 || day < 1 || day > 31) return -1;
    // Days from a fixed civil epoch (Howard Hinnant's algorithm). Correct across
    // month and year boundaries and leap years; the absolute value is meaningless,
    // only differences matter.
    const int yy = y - (m <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)(era * 146097 + (int)doe - 719468);
}

/// Whole days between two stamps' calendar dates (newest positive if `now` is
/// later). Returns a large number if `newest` is malformed/empty, so a save with
/// no valid backup is always considered stale.
inline long days_between_stamps(const std::string& newest, const std::string& now) {
    const long a = stamp_day_number(newest);
    const long b = stamp_day_number(now);
    if (a < 0 || b < 0) return 1L << 30;   // treat as very stale
    return b - a;
}

/// Should a save whose newest backup is `newest_stamp` be auto-backed-up now,
/// given a threshold of `threshold_days`? `now_stamp` is today's stamp.
///
///   - threshold_days <= 0 disables auto-backup entirely (the default) — nothing
///     is ever stale, so nothing runs. This is the off switch.
///   - an empty/absent newest_stamp (no backup ever) is stale whenever
///     auto-backup is enabled at all.
///   - otherwise stale iff at least threshold_days have elapsed.
inline bool save_is_stale(const std::string& newest_stamp, const std::string& now_stamp,
                          int threshold_days) {
    if (threshold_days <= 0) return false;              // feature off
    if (newest_stamp.empty()) return true;              // never backed up
    return days_between_stamps(newest_stamp, now_stamp) >= (long)threshold_days;
}

/// Progress phases for the auto-backup sweep, so the UI can show what the
/// otherwise-silent sweep is doing. The slow part is usually NOT the copying —
/// it is Enumerating (priming the ncm title-name cache, which blocks), which is
/// exactly what makes the sweep look frozen without this.
enum class AutoPhase { Enumerating, BackingUp, Done };

struct AutoProgress {
    AutoPhase   phase = AutoPhase::Enumerating;
    int         current = 0;      // 1-based index of the save being backed up
    int         total   = 0;      // number of STALE saves to back up
    std::string title;            // label of the save currently being backed up
    int         backed_up = 0;    // succeeded so far
};

/// Auto-backup sweep: back up every LIVE save whose newest backup is older than
/// Config::Behavior::save_auto_backup_days (0 = off, the default).
///
/// Runs synchronously on the caller's thread and can touch many saves — the
/// single mount slot forces it sequential — so it is meant to be called ONCE at a
/// natural pause (app open), never per frame.
///
/// `on_progress` (optional) is invoked at each phase transition and before each
/// save is backed up, so the caller can REDRAW — the sweep blocks the thread, so
/// without the caller pumping a frame from this callback the screen freezes. It
/// must be cheap and must not re-enter the sweep.
///
/// Returns the number of saves backed up. Safe to call when off: returns 0
/// immediately.
int auto_backup_stale(const std::function<void(const AutoProgress&)>& on_progress
                          = nullptr);

/// Back up EVERY live save, regardless of when it was last backed up. This is the
/// MANUAL "back up my saves now" action.
///
/// Deliberately NOT the staleness sweep. A button that runs the stale policy would
/// do nothing at all when auto-backup is off (the default) or when nothing happens
/// to be stale — a control that silently no-ops is worse than no control, because
/// the user cannot tell "it worked" from "it ignored me". Pressing a button means
/// "do it now", so it does it.
///
/// Shares one sweep implementation with auto_backup_stale(); they differ only in
/// which saves they select, so neither can drift from the other on layout,
/// progress reporting, or error handling.
///
/// Same threading contract and same progress callback as the automatic sweep.
/// Returns the number of saves backed up.
int backup_all(const std::function<void(const AutoProgress&)>& on_progress = nullptr);

// ── The operations ───────────────────────────────────────────────────────────

/// Copy the save for (user, title_label) to a fresh stamped directory under the
/// configured backup root.
///
/// `title_label` is the same "<Name> [APPID]" string the transports show, so the
/// caller passes through exactly what the user saw.
///
/// Mounts the save through the shared choke point (Services::save_resolve), so
/// the single-slot rule and every other save invariant apply unchanged — this
/// does NOT open its own mount.
///
/// Runs synchronously and can take a while; call it on a worker thread and read
/// `progress` from the UI thread. Returns the destination directory on success,
/// or "" on failure (nothing mounted, no such title, copy failed).
std::string create(const std::string& user,
                   const std::string& title_label,
                   Fs::Progress& progress);

/// Stamped backup directory names for this title, newest first. Entries that do
/// not match the stamp shape are excluded — see is_backup_stamp().
std::vector<std::string> list(const std::string& user,
                              const std::string& title_label);

// ── Browsing the backup TREE itself (independent of live saves) ──────────────
//
// The Save Manager's normal hierarchy is driven by the LIVE saves on the console.
// That makes a backup unreachable the moment its save is deleted — the title
// vanishes from the list, taking the path to its backups with it, even though the
// backups sit untouched on the SD card. This is the "I deleted a save and cannot
// restore it" trap: the snapshot exists but nothing leads to it.
//
// These enumerate the backup DIRECTORY TREE directly, so a backup is reachable
// whether or not its live save still exists. Deletion no longer strands a backup.

/// User folder names that have at least one title with at least one backup.
std::vector<std::string> backup_users();

/// Title-label folder names under a user that contain at least one valid backup.
/// These are the sanitized labels as they were written at backup time.
std::vector<std::string> backup_titles(const std::string& user);

/// REPLACE the save for (user, title_label) with the contents of `backup_dir`.
///
/// THIS IS DESTRUCTIVE AND NOT REVERSIBLE BY ITSELF. It is a whole-save replace:
/// existing contents are deleted, then the backup is copied in. Per-file copying
/// would leave behind files that exist in the save but not in the backup, and for
/// many games a save carrying leftovers from a different point in time is
/// CORRUPT rather than merely stale — which is exactly why this is the dangerous
/// half of 3e and why callers must take a snapshot first.
///
/// Callers MUST have taken a pre-restore snapshot via create() and MUST have
/// shown the user where it went. This function does not take one itself: it would
/// then be silently doing two writes for one request, and a snapshot the user was
/// never told about is not a rollback they can use.
///
/// Commits once at the end, over the whole replace. A failed commit fails the
/// restore — a half-restored save reported as success is the worst outcome
/// available here.
///
/// Synchronous; same threading contract as create(). Returns true on success.
bool restore(const std::string& user,
             const std::string& title_label,
             const std::string& backup_dir,
             Fs::Progress& progress);

} // namespace Core::SaveBackup
