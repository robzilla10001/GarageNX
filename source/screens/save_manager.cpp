// source/screens/save_manager.cpp

#include "screens/save_manager.hpp"
#include "screens/file_browser.hpp"
#include "core/save_backup.hpp"
#include "services/title_surface.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "services/save_surface.hpp"
#include "services/storage_paths.hpp"
#include "ui/font.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"

#include <SDL2/SDL.h>

SaveManagerScreen::SaveManagerScreen() {
    reload();
}

void SaveManagerScreen::on_enter() {
    // Coming back from the file browser, the mount may have moved on. Reloading
    // the current level re-establishes the right state — and at Users/Titles the
    // shared listing helpers release the mount, which is what makes "navigate
    // away from a title and it unloads" true here too.
    reload();
}

std::string SaveManagerScreen::display_label(const std::string& entry) const {
    // Only backup TITLE folders get rewritten for display. Everything else — user
    // names, stamps, live save labels — is already what it should be.
    if (m_level != Level::BackupTitles) return entry;
    if (!Services::save_label_is_unresolved(entry)) return entry;

    // An id-only folder name ("Title 0100..."). The directory on disk keeps that
    // name — renaming it would break every path already recorded and is not ours
    // to do — but the id inside it is enough to show the real game name now that
    // the cache can resolve it.
    const uint64_t app = Core::SaveBackup::app_id_from_label(entry);
    if (app == 0) return entry;
    const std::string better = Services::save_build_label(app);
    return Services::save_label_is_unresolved(better) ? entry : better;
}

void SaveManagerScreen::reload() {
    if (m_level == Level::Users) {
        m_entries = Services::save_user_names();
        // Pin the backup-tree entry at the top, always present so orphaned
        // backups are reachable even when there are no live saves at all. It is
        // identified positionally (idx 0), never by string-matching its label.
        m_entries.insert(m_entries.begin(), Lang::t("save_manager.manage_backups"));
    } else if (m_level == Level::BackupUsers) {
        m_entries = Core::SaveBackup::backup_users();
    } else if (m_level == Level::BackupTitles) {
        m_entries = Core::SaveBackup::backup_titles(m_user);
        // Ask for title names so id-only folder names can be DISPLAYED under their
        // real game name. Non-blocking for the usual reason (this is the main
        // thread); update() re-labels as names arrive.
        Services::installed_titles_request_nonblocking();
        m_titles_pending = !Services::installed_titles_names_resolved();
    } else if (m_level == Level::Titles) {
        // Non-blocking: this runs on the MAIN THREAD, so blocking would park the
        // loop that resolves title names and stall ~10s (the first-browse bug).
        // Labels come back with whatever is resolved; update() re-labels as the
        // rest fill in.
        m_entries = Services::save_title_labels(m_user, /*block=*/false);
        m_titles_pending = !Services::installed_titles_enumerated();
    } else {
        m_entries = Core::SaveBackup::list(m_user, m_title);
    }

    std::vector<Widgets::ListItem> rows;
    rows.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        Widgets::ListItem it;
        it.label = display_label(e);
        rows.push_back(std::move(it));
    }
    m_list.set_items(std::move(rows));
}

void SaveManagerScreen::start_backup(const std::string& title_label) {
    // Synchronous, matching FileBrowserScreen's copy/delete: this repo has no
    // threaded-op harness yet, and inventing one here would make a save backup
    // the first threaded UI operation in the project. Saves are small — kilobytes
    // to a few megabytes — so the freeze is brief. When the harness lands (M6),
    // this joins it alongside the file browser rather than ahead of it.
    m_progress.reset();
    m_op_active = true;
    m_op_label  = Lang::t("save_manager.op_backing_up");
    m_status.clear();

    const std::string dest =
        Core::SaveBackup::create(m_user, title_label, m_progress);

    m_progress.done = true;
    m_op_active     = false;

    Modal::Options o;
    if (dest.empty()) {
        o.kind  = Modal::Kind::Info;
        o.title = Lang::t("save_manager.backup_failed_title");
        o.body  = Lang::t("save_manager.backup_failed_body");
        m_status = Lang::t("save_manager.backup_failed_title");
    } else {
        o.kind  = Modal::Kind::Info;
        o.title = Lang::t("save_manager.backup_done_title");
        // The destination is a long path with no spaces — it relies on the
        // modal's mid-token wrapping to stay inside the box.
        o.body  = Lang::t("save_manager.backup_done_body") + "\n\n" + dest;
        m_status = dest;
    }
    o.confirm_label = Lang::t("common.ok");
    Modal::show(o);
}

void SaveManagerScreen::begin_restore(const std::string& stamp) {
    // Pre-restore snapshot protects the CURRENT save from being overwritten. When
    // restoring from the BACKUP TREE the current save may not exist (it was
    // deleted — that is why we are here), so there is nothing to snapshot and
    // create() would fail. Skip it in that case; the restore recreates the save.
    m_pending_snapshot.clear();
    if (!m_in_backup_tree) {
        // Snapshot FIRST, before asking. The confirmation can then name a path
        // that already exists rather than promising one, and a user who says no
        // has cost nothing but a copy. If the snapshot fails we do not ask at all.
        m_progress.reset();
        m_op_active = true;
        m_op_label  = Lang::t("save_manager.op_snapshotting");
        m_pending_snapshot = Core::SaveBackup::create(m_user, m_title, m_progress);
        m_op_active = false;

        if (m_pending_snapshot.empty()) {
            Modal::Options o;
            o.kind          = Modal::Kind::Info;
            o.title         = Lang::t("save_manager.snapshot_failed_title");
            o.body          = Lang::t("save_manager.snapshot_failed_body");
            o.confirm_label = Lang::t("common.ok");
            Modal::show(o);
            m_pending_restore_dir.clear();
            return;
        }
    }

    m_pending_restore_dir = Core::SaveBackup::backup_title_dir(
        Config::get().paths.save_backup, m_user, m_title) + "/" + stamp;

    Modal::Options o;
    o.kind  = Modal::Kind::Danger;      // hold-to-confirm, not a single press
    o.title = Lang::t("save_manager.restore_confirm_title");
    o.body  = Lang::t("save_manager.restore_confirm_body") + "\n\n"
            + m_title + "  <-  " + stamp;
    // Only mention a snapshot when one was actually taken.
    if (!m_pending_snapshot.empty())
        o.body += "\n\n" + Lang::t("save_manager.restore_snapshot_note") + "\n"
                + m_pending_snapshot;
    o.confirm_label = Lang::t("save_manager.restore");
    o.cancel_label  = Lang::t("common.cancel");
    Modal::show(o);
}

void SaveManagerScreen::begin_wipe(const std::string& title_label) {
    // Snapshot FIRST, same as restore: a wipe is destructive and this is the
    // rollback. If the snapshot fails, do not offer the wipe.
    m_progress.reset();
    m_op_active = true;
    m_op_label  = Lang::t("save_manager.op_snapshotting");
    m_pending_wipe_snapshot = Core::SaveBackup::create(m_user, title_label, m_progress);
    m_op_active = false;

    if (m_pending_wipe_snapshot.empty()) {
        Modal::Options o;
        o.kind          = Modal::Kind::Info;
        o.title         = Lang::t("save_manager.snapshot_failed_title");
        o.body          = Lang::t("save_manager.snapshot_failed_body");
        o.confirm_label = Lang::t("common.ok");
        Modal::show(o);
        return;
    }

    m_pending_wipe_title = title_label;
    Modal::Options o;
    o.kind  = Modal::Kind::Danger;
    o.title = Lang::t("save_manager.wipe_confirm_title");
    o.body  = Lang::t("save_manager.wipe_confirm_body") + "\n\n" + title_label
            + "\n\n" + Lang::t("save_manager.restore_snapshot_note") + "\n"
            + m_pending_wipe_snapshot;
    o.confirm_label = Lang::t("save_manager.wipe");
    o.cancel_label  = Lang::t("common.cancel");
    Modal::show(o);
}

void SaveManagerScreen::begin_delete(const std::string& title_label) {
    m_progress.reset();
    m_op_active = true;
    m_op_label  = Lang::t("save_manager.op_snapshotting");
    m_pending_delete_snapshot = Core::SaveBackup::create(m_user, title_label, m_progress);
    m_op_active = false;

    if (m_pending_delete_snapshot.empty()) {
        Modal::Options o;
        o.kind          = Modal::Kind::Info;
        o.title         = Lang::t("save_manager.snapshot_failed_title");
        o.body          = Lang::t("save_manager.snapshot_failed_body");
        o.confirm_label = Lang::t("common.ok");
        Modal::show(o);
        return;
    }

    m_pending_delete_title = title_label;
    Modal::Options o;
    o.kind  = Modal::Kind::Danger;
    o.title = Lang::t("save_manager.delete_confirm_title");
    o.body  = Lang::t("save_manager.delete_confirm_body") + "\n\n" + title_label
            + "\n\n" + Lang::t("save_manager.restore_snapshot_note") + "\n"
            + m_pending_delete_snapshot;
    o.confirm_label = Lang::t("save_manager.delete");
    o.cancel_label  = Lang::t("common.cancel");
    Modal::show(o);
}

void SaveManagerScreen::finish_delete() {
    const std::string title    = m_pending_delete_title;
    const std::string snapshot = m_pending_delete_snapshot;
    m_pending_delete_title.clear();
    m_pending_delete_snapshot.clear();
    if (title.empty()) return;

    m_progress.reset();
    m_op_active = true;
    m_op_label  = Lang::t("save_manager.op_deleting");
    const bool ok = Services::save_delete_record(m_user, title);
    m_op_active = false;

    Modal::Options o;
    o.kind          = Modal::Kind::Info;
    o.confirm_label = Lang::t("common.ok");
    if (ok) {
        o.title = Lang::t("save_manager.delete_done_title");
        o.body  = Lang::t("save_manager.delete_done_body");
        // The title is gone; the current Titles listing is stale. Drop back to
        // the user level, whose listing is still valid, rather than show a list
        // with a hole in it or a title that no longer exists.
        m_level = Level::Users;
        m_user.clear();
        m_status.clear();
        reload();
    } else {
        o.title = Lang::t("save_manager.delete_failed_title");
        o.body  = Lang::t("save_manager.delete_failed_body") + "\n\n" + snapshot;
        m_status = snapshot;
    }
    Modal::show(o);
}

void SaveManagerScreen::finish_wipe() {
    const std::string title    = m_pending_wipe_title;
    const std::string snapshot = m_pending_wipe_snapshot;
    m_pending_wipe_title.clear();
    m_pending_wipe_snapshot.clear();
    if (title.empty()) return;

    Services::SavePath sp;
    sp.level = Services::SavePath::Level::Files;
    sp.user  = m_user;
    sp.title = title;
    const std::string root = Services::save_resolve(sp);

    m_progress.reset();
    m_op_active = true;
    m_op_label  = Lang::t("save_manager.op_wiping");
    const bool ok = !root.empty() && Services::save_wipe(root);
    m_op_active = false;

    Modal::Options o;
    o.kind          = Modal::Kind::Info;
    o.confirm_label = Lang::t("common.ok");
    if (ok) {
        o.title  = Lang::t("save_manager.wipe_done_title");
        o.body   = Lang::t("save_manager.wipe_done_body");
        m_status = Lang::t("save_manager.wipe_done_title");
    } else {
        o.title  = Lang::t("save_manager.wipe_failed_title");
        o.body   = Lang::t("save_manager.restore_failed_body") + "\n\n" + snapshot;
        m_status = snapshot;
    }
    Modal::show(o);
}

void SaveManagerScreen::finish_restore() {
    const std::string src      = m_pending_restore_dir;
    const std::string snapshot = m_pending_snapshot;
    m_pending_restore_dir.clear();
    m_pending_snapshot.clear();
    if (src.empty()) return;

    m_progress.reset();
    m_op_active = true;
    m_op_label  = Lang::t("save_manager.op_restoring");

    const bool ok = Core::SaveBackup::restore(m_user, m_title, src, m_progress);
    m_op_active = false;

    Modal::Options o;
    o.kind          = Modal::Kind::Info;
    o.confirm_label = Lang::t("common.ok");
    if (ok) {
        o.title  = Lang::t("save_manager.restore_done_title");
        o.body   = Lang::t("save_manager.restore_done_body");
        m_status = Lang::t("save_manager.restore_done_title");
    } else {
        // On failure the save may be partially replaced. Point at the snapshot if
        // there is one; an orphaned restore has none (nothing to protect), so say
        // the shorter thing instead of appending an empty path.
        o.title  = Lang::t("save_manager.restore_failed_title");
        o.body   = snapshot.empty()
                     ? Lang::t("save_manager.restore_failed_nobackup")
                     : Lang::t("save_manager.restore_failed_body") + "\n\n" + snapshot;
        m_status = snapshot;
    }
    Modal::show(o);
}

std::unique_ptr<Screen> SaveManagerScreen::update(bool& pop) {
    pop = false;

    // While title names are still resolving (main loop does one per frame),
    // rebuild the labels each frame so ids turn into names as they arrive. Cheap:
    // it is a cache read per row, no ncm work. Stop once enumeration is complete
    // AND every row has a real name, so this is not a permanent per-frame rebuild.
    if (m_level == Level::Titles && m_titles_pending) {
        const std::vector<std::string> relabelled =
            Services::save_title_labels(m_user, /*block=*/false);
        if (relabelled != m_entries) {
            m_entries = relabelled;
            std::vector<Widgets::ListItem> rows;
            rows.reserve(m_entries.size());
            for (const auto& e : m_entries) {
                Widgets::ListItem it; it.label = display_label(e);
                rows.push_back(std::move(it));
            }
            m_list.update_items(std::move(rows));   // keeps the cursor put
        }
        // Done when enumeration finished and nothing is still id-only.
        bool any_idonly = false;
        for (const auto& e : m_entries)
            if (Services::save_label_is_unresolved(e)) { any_idonly = true; break; }
        if (Services::installed_titles_enumerated() && !any_idonly)
            m_titles_pending = false;
    }

    // Backup folders keep their on-disk names, so m_entries never changes here —
    // only the DISPLAYED label does, as names resolve. Rebuild the rows rather
    // than the entries.
    if (m_level == Level::BackupTitles && m_titles_pending) {
        std::vector<Widgets::ListItem> rows;
        rows.reserve(m_entries.size());
        for (const auto& e : m_entries) {
            Widgets::ListItem it; it.label = display_label(e);
            rows.push_back(std::move(it));
        }
        m_list.update_items(std::move(rows));
        if (Services::installed_titles_names_resolved()) m_titles_pending = false;
    }

    // The modal owns input while it is up, and an operation blocks everything.
    if (Modal::is_active() || m_op_active) return nullptr;

    if (Input::pressed(Input::Button::B)) {
        if (m_level == Level::Backups) {
            // Return to whichever tree we descended through.
            if (m_in_backup_tree) {
                m_level = Level::BackupTitles;
                m_title.clear();
            } else {
                m_level = Level::Titles;
                m_title.clear();
            }
            m_status.clear();
            reload();
            return nullptr;
        }
        if (m_level == Level::BackupTitles) {
            m_level = Level::BackupUsers;
            m_user.clear();
            m_status.clear();
            reload();
            return nullptr;
        }
        if (m_level == Level::BackupUsers) {
            m_in_backup_tree = false;
            m_level = Level::Users;
            m_user.clear();
            m_status.clear();
            reload();
            return nullptr;
        }
        if (m_level == Level::Titles) {
            // Back to the user list. reload() calls save_user_names(), which
            // releases the mount — leaving a title's save mounted after the user
            // has navigated out of it would hold a limited resource for nothing.
            m_level = Level::Users;
            m_user.clear();
            m_status.clear();
            reload();
            return nullptr;
        }
        pop = true;
        return nullptr;
    }

    const int idx = m_list.cursor();
    const bool have = idx >= 0 && idx < static_cast<int>(m_entries.size());

    // Minus wipes the highlighted save (clears its CONTENTS, keeps the record).
    if (have && m_level == Level::Titles && Input::pressed(Input::Button::Minus)) {
        begin_wipe(m_entries[idx]);
        return nullptr;
    }

    // Plus DELETES the save record entirely — the title leaves the list and the
    // game makes a fresh save next launch. The most destructive action here, so
    // it snapshots first like the others and returns to the user level after.
    if (have && m_level == Level::Titles && Input::pressed(Input::Button::Plus)) {
        begin_delete(m_entries[idx]);
        return nullptr;
    }

    // X backs up the highlighted save. Asked for even though a backup is safe:
    // it writes to the SD card and the user should see WHERE before it happens.
    if (have && m_level == Level::Titles && Input::pressed(Input::Button::X)) {
        m_pending_backup = m_entries[idx];
        Modal::Options o;
        o.kind          = Modal::Kind::Confirm;
        o.title         = Lang::t("save_manager.backup_confirm_title");
        o.body          = Lang::t("save_manager.backup_confirm_body") + "\n\n" +
                          m_pending_backup;
        o.confirm_label = Lang::t("save_manager.backup");
        o.cancel_label  = Lang::t("common.cancel");
        Modal::show(o);
        return nullptr;
    }

    // Y lists this title's backups, from which one can be restored.
    if (have && m_level == Level::Titles && Input::pressed(Input::Button::Y)) {
        m_title = m_entries[idx];
        m_level = Level::Backups;
        m_status.clear();
        reload();
        if (m_entries.empty()) {
            Modal::Options o;
            o.kind          = Modal::Kind::Info;
            o.title         = Lang::t("save_manager.no_backups_title");
            o.body          = Lang::t("save_manager.no_backups_body");
            o.confirm_label = Lang::t("common.ok");
            Modal::show(o);
        }
        return nullptr;
    }

    if (m_list.handle_input() && have) {
        if (m_level == Level::Backups) {
            begin_restore(m_entries[idx]);
            return nullptr;
        }
        if (m_level == Level::BackupUsers) {
            m_user  = m_entries[idx];
            m_level = Level::BackupTitles;
            m_status.clear();
            reload();
            return nullptr;
        }
        if (m_level == Level::BackupTitles) {
            // Enter this title's backups. m_title drives SaveBackup::list().
            m_title = m_entries[idx];
            m_level = Level::Backups;
            m_status.clear();
            reload();
            if (m_entries.empty()) {
                Modal::Options o;
                o.kind = Modal::Kind::Info;
                o.title = Lang::t("save_manager.no_backups_title");
                o.body  = Lang::t("save_manager.no_backups_body");
                o.confirm_label = Lang::t("common.ok");
                Modal::show(o);
            }
            return nullptr;
        }
        if (m_level == Level::Users) {
            // The pinned first row opens the backup tree; every other row is a
            // live user. idx 0 is always the sentinel (reload() pins it there).
            if (idx == 0) {
                m_in_backup_tree = true;
                m_level  = Level::BackupUsers;
                m_user.clear();
                m_status.clear();
                reload();
                if (m_entries.empty()) {
                    Modal::Options o;
                    o.kind = Modal::Kind::Info;
                    o.title = Lang::t("save_manager.no_backups_title");
                    o.body  = Lang::t("save_manager.no_backups_any_body");
                    o.confirm_label = Lang::t("common.ok");
                    Modal::show(o);
                    // Fall back to the live user list rather than sit on an empty
                    // backup-users screen.
                    m_in_backup_tree = false;
                    m_level = Level::Users;
                    reload();
                }
                return nullptr;
            }
            m_user  = m_entries[idx];
            m_level = Level::Titles;
            m_status.clear();
            reload();
            return nullptr;
        }

        // Enter the save itself. save_resolve() mounts the named (user, title)
        // through the shared choke point and hands back the concrete root.
        Services::SavePath sp;
        sp.level = Services::SavePath::Level::Files;
        sp.user  = m_user;
        sp.title = m_entries[idx];
        const std::string root = Services::save_resolve(sp);
        if (root.empty()) {
            Modal::Options o;
            o.kind          = Modal::Kind::Info;
            o.title         = Lang::t("save_manager.mount_failed_title");
            o.body          = Lang::t("save_manager.mount_failed_body");
            o.confirm_label = Lang::t("common.ok");
            Modal::show(o);
            return nullptr;
        }
        return std::unique_ptr<Screen>(
            new FileBrowserScreen(root, m_entries[idx]));
    }
    return nullptr;
}

void SaveManagerScreen::on_modal_result(int result) {
    if (!m_pending_delete_title.empty()) {
        if (static_cast<Modal::Result>(result) == Modal::Result::Confirmed) {
            finish_delete();
        } else {
            m_status = m_pending_delete_snapshot;   // keep the snapshot, say where
            m_pending_delete_title.clear();
            m_pending_delete_snapshot.clear();
        }
        return;
    }

    if (!m_pending_wipe_title.empty()) {
        if (static_cast<Modal::Result>(result) == Modal::Result::Confirmed) {
            finish_wipe();
        } else {
            // Cancelled: keep the snapshot (a real backup) and say where it is.
            m_status = m_pending_wipe_snapshot;
            m_pending_wipe_title.clear();
            m_pending_wipe_snapshot.clear();
        }
        return;
    }

    // A restore confirmation takes precedence: it is the only modal here whose
    // answer performs a destructive action.
    if (!m_pending_restore_dir.empty()) {
        if (static_cast<Modal::Result>(result) == Modal::Result::Confirmed) {
            finish_restore();
        } else {
            // Cancelled. The snapshot stays on the SD card — it is a real backup
            // and deleting it to tidy up would throw away something the user may
            // want, for no benefit.
            m_status = m_pending_snapshot;
            m_pending_restore_dir.clear();
            m_pending_snapshot.clear();
        }
        return;
    }

    if (m_pending_backup.empty()) return;
    const std::string title = m_pending_backup;
    m_pending_backup.clear();
    if (static_cast<Modal::Result>(result) != Modal::Result::Confirmed) return;
    start_backup(title);
}

void SaveManagerScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x      = Layout::MENU_INDENT_X;
    style.show_checkbox = false;
    style.show_dividers = true;

    const int list_h = h - 36 - (m_status.empty() ? 0 : 24);
    m_list.draw(x, y, w, list_h, style);

    if (!m_status.empty()) {
        SDL_Color c = Theme::get(Theme::Token::FgSecondary);
        Renderer::draw_text(m_status, (int)Font::Size::Small,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans,
                            c, x + Layout::MENU_INDENT_X, y + list_h + 4,
                            nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
    }

    std::vector<Widgets::ButtonHint> hints;
    if (m_level == Level::Users || m_level == Level::BackupUsers ||
        m_level == Level::BackupTitles) {
        hints = { { "A", Lang::t("hints.open") }, { "B", Lang::t("hints.back") } };
    } else if (m_level == Level::Titles) {
        hints = { { "A", Lang::t("save_manager.browse") },
                  { "X", Lang::t("save_manager.backup") },
                  { "Y", Lang::t("save_manager.restore") },
                  { "-", Lang::t("save_manager.wipe") },
                  { "+", Lang::t("save_manager.delete") },
                  { "B", Lang::t("hints.back") } };
    } else {
        hints = { { "A", Lang::t("save_manager.restore") },
                  { "B", Lang::t("hints.back") } };
    }
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
