// source/services/save_surface.cpp
//
// Lifted from ftp_server.cpp's static helpers, which were the hardware-verified
// implementation (3d-b). The logic is unchanged — only its home moved, so MTP
// consumes the same code rather than a lookalike.

#include "services/save_surface.hpp"

#include <SDL2/SDL.h>
#include "services/storage_catalog.hpp"
#include "services/title_surface.hpp"
#include "core/save_mount.hpp"
#include "core/fs.hpp"

#include <cstdarg>
#include <cstdio>

namespace Services {

namespace {

// Failure breadcrumbs only. A save that will not mount, or a title folder that
// matches nothing, is rare and worth recording; per-request tracing is not.
void savelog(const char* fmt, ...) {
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/save.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    ::vfprintf(f, fmt, ap);
    va_end(ap);
    ::fputc('\n', f);
    ::fclose(f);
}

// "%016llX" of an application id — the part of a title folder name that actually
// identifies it, independent of the display name in front of it.
std::string app_id_hex(uint64_t application_id) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)application_id);
    return std::string(buf);
}

} // namespace

std::vector<std::string> save_user_names() {
    Core::SaveMount::release();          // not inside a title any more
    std::vector<std::string> out;
    for (const auto& u : Core::SaveMount::list_users())
        out.push_back(u.name);
    return out;
}

std::vector<std::string> save_title_labels(const std::string& user, bool block) {
    Core::SaveMount::release();
    std::vector<std::string> out;

    // Ensure titles are enumerated/resolved before labelling folders.
    // name_for_app() is cache-only by design (it must never do ncm work on a
    // transport thread), so without this the cache is empty unless the user
    // happened to browse Installed Titles first — and every folder falls back to
    // "Title <id>".
    //
    // The BLOCKING form is for transport workers, whose wait the main loop can
    // service. A MAIN-THREAD caller must NOT block: it would park the loop that
    // does the resolving and wait out the whole timeout (the ~10s first-browse
    // stall). It requests without blocking and labels with what is ready, then
    // re-labels on later frames as resolution catches up.
    if (block) (void)installed_titles_list();
    else       (void)installed_titles_request_nonblocking();

    for (const auto& u : Core::SaveMount::list_users()) {
        if (u.name != user) continue;
        for (const auto& e : Core::SaveMount::list_saves(u)) {
            // Shared builder, so this label always matches the one auto-backup and
            // save_resolve() use for the same title.
            out.push_back(save_build_label(e.application_id));
        }
        break;
    }
    return out;
}

std::string save_resolve(const SavePath& sp) {
    if (sp.level != SavePath::Level::Files) {
        Core::SaveMount::release();
        return std::string();
    }

    // Find the user and title this path names.
    for (const auto& u : Core::SaveMount::list_users()) {
        if (u.name != sp.user) continue;
        for (const auto& e : Core::SaveMount::list_saves(u)) {
            // The title folder is "<Name> [ID]"; match on the id, which is the
            // part that actually identifies it.
            if (sp.title.find(app_id_hex(e.application_id)) == std::string::npos)
                continue;
            if (!Core::SaveMount::ensure_mounted(u, e.application_id)) {
                savelog("  MOUNT FAILED app=%016llX",
                        (unsigned long long)e.application_id);
                return std::string();
            }
            return sp.rest.empty() ? std::string("save:/")
                                   : std::string("save:/") + sp.rest;
        }
    }
    savelog("  NO MATCH (user or title not found)");
    Core::SaveMount::release();
    return std::string();
}

std::string save_resolve_synth(const std::string& synth) {
    if (!save_is_synthetic(synth)) return std::string();
    return save_resolve(sp_split_save(save_synth_rel(synth)));
}

bool save_commit_if_save_path(const std::string& vfs_path) {
    const StorageSurface* s = StorageCatalog::find(StorageSurface::Id::Saves);
    const std::string root = s ? s->vfs_root : "save:";   // catalog owns the prefix
    if (vfs_path.compare(0, root.size(), root) != 0) return true;   // not a save
    const bool ok = Core::SaveMount::commit();
    if (!ok) savelog("  COMMIT FAILED path=%s", vfs_path.c_str());
    return ok;
}

bool save_wipe(const std::string& mounted_root) {
    if (!save_is_mount_root(mounted_root)) return false;   // caller error

    bool ok = false;
    std::vector<std::string> children;
    for (const auto& e : Fs::list(mounted_root, &ok))
        children.push_back(Fs::join(mounted_root, e.name));
    if (!ok) { savelog("  WIPE list failed"); return false; }

    for (const auto& child : children) {
        const bool removed = Fs::is_directory(child)
                                 ? Fs::remove_directory_recursive(child)   // NO-COMMIT: save_wipe commits once after the loop
                                 : Fs::remove_file(child);
        if (!removed) { savelog("  WIPE remove failed: %s", child.c_str()); return false; }
    }

    // Commit, for the same reason every other save mutation does: the journal
    // discards uncommitted deletions at unmount, so an uncommitted wipe would
    // "come back". A failed commit fails the wipe.
    if (!Core::SaveMount::commit()) { savelog("  WIPE commit failed"); return false; }
    return true;
}

bool save_delete_record(const std::string& user, const std::string& title_label) {
    Core::SaveMount::release();   // the record must not be mounted during deletion

    // Find the (user, save) exactly as save_resolve() does — match the user by
    // name and the title by the app id embedded in the label — but keep the
    // save_data_id, which is the handle deletion needs.
    for (const auto& u : Core::SaveMount::list_users()) {
        if (u.name != user) continue;
        for (const auto& e : Core::SaveMount::list_saves(u)) {
            if (title_label.find(app_id_hex(e.application_id)) == std::string::npos)
                continue;
            if (e.save_data_id == 0) {
                savelog("  DELETE no save_data_id app=%016llX",
                        (unsigned long long)e.application_id);
                return false;
            }
            const bool ok = Core::SaveMount::delete_save_record(e.save_data_id);
            if (!ok) savelog("  DELETE FAILED app=%016llX id=%016llX",
                             (unsigned long long)e.application_id,
                             (unsigned long long)e.save_data_id);
            return ok;
        }
    }
    savelog("  DELETE NO MATCH (user or title not found)");
    return false;
}

std::string save_build_label(uint64_t application_id) {
    const std::string idhex = app_id_hex(application_id);
    const std::string name  = installed_titles_name_for_app(application_id);
    return name.empty() ? std::string("Title ") + idhex
                        : name + " [" + idhex + "]";
}

std::vector<SaveRef> save_enumerate_all(const std::function<void()>& pump) {
    Core::SaveMount::release();

    if (pump) {
        // MAIN-THREAD caller. installed_titles_list() would block waiting for the
        // main loop to resolve names — but WE are that loop, so it would wait out
        // the full timeout and hand back id-only labels, which then get written to
        // disk as backup folder names. So do the loop's job instead: drive the
        // resolver directly and let the caller draw between units.
        //
        // Waits for names_resolved(), not merely enumerated(): "enumerated" is true
        // while names are still arriving one per tick, and a backup named from a
        // half-resolved cache is exactly the bug this replaced.
        installed_titles_request_nonblocking();
        const uint32_t deadline = SDL_GetTicks() + 20000;
        while (!installed_titles_names_resolved() &&
               (int32_t)(SDL_GetTicks() - deadline) < 0) {
            installed_titles_tick();
            pump();
        }
        // Falling out on the deadline is a degraded but honest outcome: labels are
        // id-based, which is also what an uninstalled title legitimately gets.
    } else {
        (void)installed_titles_list();   // transport worker: blocking is correct
    }

    std::vector<SaveRef> out;
    for (const auto& u : Core::SaveMount::list_users()) {
        for (const auto& e : Core::SaveMount::list_saves(u)) {
            SaveRef r;
            r.user           = u.name;
            r.application_id = e.application_id;
            r.title_label    = save_build_label(e.application_id);
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::string save_resolve_for_restore(const std::string& user,
                                     uint64_t application_id) {
    Core::SaveMount::release();

    for (const auto& u : Core::SaveMount::list_users()) {
        if (u.name != user) continue;

        // Recreate the record if a delete removed it. No-op-success when it still
        // exists, so this is safe on the common (undeleted) path too.
        if (!Core::SaveMount::ensure_save_exists(u, application_id)) {
            savelog("  RESTORE ensure_save_exists FAILED app=%016llX",
                    (unsigned long long)application_id);
            return std::string();
        }
        if (!Core::SaveMount::ensure_mounted(u, application_id)) {
            savelog("  RESTORE mount FAILED app=%016llX",
                    (unsigned long long)application_id);
            return std::string();
        }
        return std::string("save:/");
    }
    savelog("  RESTORE no such user: %s", user.c_str());
    return std::string();
}

void save_surface_release() {
    Core::SaveMount::release();
}

} // namespace Services
