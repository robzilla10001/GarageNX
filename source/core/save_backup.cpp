// source/core/save_backup.cpp

#include "core/save_backup.hpp"
#include "core/datetime.hpp"
#include "config/config.hpp"
#include "services/save_surface.hpp"
#include "services/storage_paths.hpp"
#include "core/save_mount.hpp"

#include <algorithm>
#include <cctype>

namespace Core::SaveBackup {

std::string create(const std::string& user,
                   const std::string& title_label,
                   Fs::Progress& progress) {
    // Resolve through the SHARED choke point so the single-slot mount rule and
    // every other save invariant apply unchanged. Opening a mount here would be a
    // second implementation of the one thing that must not have two.
    Services::SavePath sp;
    sp.level = Services::SavePath::Level::Files;
    sp.user  = user;
    sp.title = title_label;
    sp.rest.clear();

    const std::string src = Services::save_resolve(sp);
    if (src.empty()) {
        progress.done = true;
        progress.success = false;
        return std::string();
    }

    const std::string dest = backup_dir_for(Config::get().paths.save_backup,
                                            user, title_label,
                                            Core::DateTime::sortable_stamp_now());

    // mkdir does not create intermediates, so build the tree top-down — same
    // reason main.cpp's ensure_directories() does.
    {
        std::string acc;
        size_t i = 0;
        // Keep any "device:" prefix intact as the first segment.
        const size_t colon = dest.find(":/");
        if (colon != std::string::npos) { acc = dest.substr(0, colon + 2); i = colon + 2; }
        for (; i <= dest.size(); ++i) {
            if (i == dest.size() || dest[i] == '/') {
                if (!acc.empty() && acc.back() != '/') Fs::make_directory(acc);  // NO-COMMIT: backup tree on SD
                if (i == dest.size()) break;
                acc += '/';
            } else {
                acc += dest[i];
            }
        }
        Fs::make_directory(dest);  // NO-COMMIT: backup destination on SD
    }

    if (!Fs::is_directory(dest)) {
        progress.done = true;
        progress.success = false;
        return std::string();
    }

    // Copy the CONTENTS of the save, not the mount root itself: Fs::copy(src,
    // dst_dir) would otherwise create a folder named after "save:" inside dest.
    bool ok = true;
    bool list_ok = false;
    for (const auto& e : Fs::list(src, &list_ok)) {
        const std::string child = Fs::join(src, e.name);
        // Never prompt: a backup writes only into a directory we just created
        // with a unique timestamp, so a conflict here would mean something is
        // very wrong. Skipping is safer than silently overwriting.
        if (!Fs::copy(child, dest, progress,
                      [](const std::string&) { return Fs::Conflict::Skip; })) {
            ok = false;
            break;
        }
    }
    if (!list_ok) ok = false;

    progress.done    = true;
    progress.success = ok;
    return ok ? dest : std::string();
}

namespace {

// One sweep, two policies. `stale_only` picks between the automatic schedule and
// the manual "everything, now". Keeping this single means the two cannot drift on
// layout, progress reporting, or failure handling — only on which saves they pick.
int sweep_impl(bool stale_only, int threshold,
               const std::function<void(const AutoProgress&)>& on_progress) {
    AutoProgress prog;

    // Phase 1: enumerate. This is the SLOW step — save_enumerate_all() primes the
    // ncm name cache by blocking, which is what made the sweep look frozen. Tell
    // the UI first so the "please wait" is on screen BEFORE the block, not after.
    prog.phase = AutoPhase::Enumerating;
    if (on_progress) on_progress(prog);

    // Pass a pump: both sweeps run on the MAIN THREAD, so enumeration must drive
    // the name resolver rather than block on it. The pump redraws the overlay, so
    // the (genuinely slow) enumerate phase animates instead of freezing — and,
    // critically, the labels come back RESOLVED, because they are about to become
    // backup directory names on disk.
    const std::vector<Services::SaveRef> all = Services::save_enumerate_all(
        on_progress ? std::function<void()>([&] { on_progress(prog); })
                    : std::function<void()>());
    const std::string now = Core::DateTime::sortable_stamp_now();

    // Decide the set up front, so the progress bar has a real total instead of
    // counting up to an unknown end.
    std::vector<Services::SaveRef> todo;
    for (const auto& s : all) {
        if (!stale_only) { todo.push_back(s); continue; }
        const std::vector<std::string> existing = list(s.user, s.title_label);
        const std::string newest = existing.empty() ? std::string() : existing.front();
        if (save_is_stale(newest, now, threshold)) todo.push_back(s);
    }

    // Phase 2: back up, reporting before each so the UI can name the current save
    // and redraw. The redraw happens in the caller's callback.
    prog.phase = AutoPhase::BackingUp;
    prog.total = (int)todo.size();
    for (size_t i = 0; i < todo.size(); ++i) {
        prog.current = (int)i + 1;
        prog.title   = todo[i].title_label;
        if (on_progress) on_progress(prog);

        Fs::Progress p;
        const std::string dest = create(todo[i].user, todo[i].title_label, p);
        if (!dest.empty()) ++prog.backed_up;
    }

    prog.phase = AutoPhase::Done;
    if (on_progress) on_progress(prog);
    return prog.backed_up;
}

} // namespace

int auto_backup_stale(const std::function<void(const AutoProgress&)>& on_progress) {
    const int threshold = Config::get().behavior.save_auto_backup_days;
    if (threshold <= 0) return 0;                 // feature off — the default
    return sweep_impl(/*stale_only=*/true, threshold, on_progress);
}

int backup_all(const std::function<void(const AutoProgress&)>& on_progress) {
    // No threshold and no config gate: this is an explicit user action.
    return sweep_impl(/*stale_only=*/false, 0, on_progress);
}

std::vector<std::string> list(const std::string& user,
                              const std::string& title_label) {
    const std::string dir = backup_title_dir(Config::get().paths.save_backup,
                                             user, title_label);
    std::vector<std::string> stamps;
    bool ok = false;
    for (const auto& e : Fs::list(dir, &ok)) {
        if (e.type != Fs::EntryType::Directory) continue;
        if (!is_backup_stamp(e.name)) continue;   // not ours; never offer it
        stamps.push_back(e.name);
    }
    sort_newest_first(stamps);
    return stamps;
}

std::vector<std::string> backup_users() {
    const std::string root = []{
        std::string r = Config::get().paths.save_backup;
        while (!r.empty() && r.back() == '/') r.pop_back();
        return r;
    }();

    std::vector<std::string> out;
    bool ok = false;
    for (const auto& e : Fs::list(root, &ok)) {
        if (e.type != Fs::EntryType::Directory) continue;
        // Only list a user folder that actually has a title with a backup, so an
        // empty or stray directory does not appear as a restorable user.
        if (!backup_titles(e.name).empty()) out.push_back(e.name);
    }
    return out;
}

std::vector<std::string> backup_titles(const std::string& user) {
    std::string root = Config::get().paths.save_backup;
    while (!root.empty() && root.back() == '/') root.pop_back();
    const std::string user_dir = root + "/" + sanitize_component(user);

    std::vector<std::string> out;
    bool ok = false;
    for (const auto& e : Fs::list(user_dir, &ok)) {
        if (e.type != Fs::EntryType::Directory) continue;
        // Only a title folder that holds at least one valid stamp is offered;
        // this reuses list()'s stamp filter, so junk directories are excluded.
        if (!list(user, e.name).empty()) out.push_back(e.name);
    }
    return out;
}

bool restore(const std::string& user,
             const std::string& title_label,
             const std::string& backup_dir,
             Fs::Progress& progress) {
    // Validate the SOURCE before touching the save. Deleting the save and then
    // discovering the backup is unreadable would be the worst possible ordering,
    // and it is the ordering you get for free if you do not check first.
    bool src_ok = false;
    const auto src_entries = Fs::list(backup_dir, &src_ok);
    if (!src_ok || !Fs::is_directory(backup_dir)) {
        progress.done = true; progress.success = false;
        return false;
    }

    // Refuse an EMPTY backup. Restoring one would delete the save and copy
    // nothing back, leaving the title with no save at all — and an empty backup
    // directory is far more likely to mean an interrupted or damaged backup than
    // a user who genuinely wants to erase their progress. If someone really does
    // want that, deleting the save in the file browser is the honest way to ask
    // for it, and it does not look like a restore that quietly did nothing.
    if (src_entries.empty()) {
        progress.done = true; progress.success = false;
        return false;
    }

    // Resolve the destination via the RESTORE resolver, which recreates the save
    // record if a delete removed it. Plain save_resolve() only matches live
    // saves, so it would fail here for exactly the deleted-then-restore case this
    // path has to support. The application id comes from the backup's own label —
    // once the live save is gone, that label is the only place it survives.
    const uint64_t app_id = app_id_from_label(title_label);
    if (app_id == 0) {
        progress.done = true; progress.success = false;
        return false;
    }
    const std::string dst = Services::save_resolve_for_restore(user, app_id);
    if (dst.empty()) {
        progress.done = true; progress.success = false;
        return false;
    }

    // ── Delete the existing contents ─────────────────────────────────────────
    // The save ROOT itself is not removed — "save:" is a mount point, and
    // removing it would be removing the filesystem rather than its contents.
    bool dst_ok = false;
    std::vector<std::string> existing;
    for (const auto& e : Fs::list(dst, &dst_ok))
        existing.push_back(Fs::join(dst, e.name));
    if (!dst_ok) {
        progress.done = true; progress.success = false;
        return false;
    }
    // NO-COMMIT: restore() commits ONCE over the whole replace, further down.
    if (!existing.empty() && !Fs::remove_many(existing, progress)) {
        // The save is now PARTIALLY EMPTIED. The caller's pre-restore snapshot is
        // the only way back, which is why taking one is a precondition and not a
        // suggestion.
        progress.done = true; progress.success = false;
        return false;
    }

    // ── Copy the backup in ───────────────────────────────────────────────────
    bool ok = true;
    for (const auto& e : src_entries) {
        const std::string child = Fs::join(backup_dir, e.name);
        // Overwrite: the destination was just cleared, so anything present is
        // either a leftover from a failed delete or a name collision within the
        // backup itself. Skipping would silently produce a partial restore.
        if (!Fs::copy(child, dst, progress,
                      [](const std::string&) { return Fs::Conflict::Overwrite; })) {
            ok = false;
            break;
        }
    }

    // ── Commit ONCE, over the whole replace ──────────────────────────────────
    // Per-file commits would make each intermediate state durable, which is not
    // what "replace this save" means. A failed commit fails the restore: the
    // journal will discard the writes at unmount, so reporting success would be
    // reporting a save that is about to revert to a half-deleted state.
    if (ok && !Core::SaveMount::commit()) ok = false;

    progress.done    = true;
    progress.success = ok;
    return ok;
}

} // namespace Core::SaveBackup
