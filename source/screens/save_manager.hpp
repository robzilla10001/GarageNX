#pragma once
// source/screens/save_manager.hpp
//
// On-device save manager: browse users, then that user's titles, then either
// open the save in the file browser or back it up to the SD card.
//
// This is what MenuItem::Saves has been stubbed to for the whole project.
//
// ── What it reuses, and why that matters ─────────────────────────────────────
// Nothing here re-implements save handling. The two synthesized levels come from
// Services::save_user_names() / save_title_labels(), and entering a title mounts
// through Services::save_resolve() — the same choke point FTP and MTP use, so the
// single-slot mount rule holds across the on-device path too. A screen that
// opened its own mount would be the second implementation of the one thing that
// must not have two.
//
// Browsing reuses FileBrowserScreen pointed at "save:/", which is exactly what it
// was built for ("reused across all filesystem contexts by pointing it at a
// different root").
//
// ── Restore ──────────────────────────────────────────────────────────────────
// Restore REPLACES a save and cannot be undone by itself, so the flow is built
// around the rollback rather than around the action:
//
//   1. A pre-restore snapshot is taken FIRST, before anything is asked.
//   2. The confirmation names that snapshot's path, so the user can see the way
//      back exists before deciding — not afterwards, when it would be a
//      consolation rather than a choice.
//   3. Only then is the save replaced.
//
// Taking the snapshot before the user has agreed means a cancelled restore has
// cost one wasted copy to the SD card. That is the right trade: a few seconds and
// a few megabytes against a confirmation that would otherwise have to say "a
// backup will be taken" and hope.
//
// NO ConfirmationBroker HERE. The broker exists so a TRANSPORT-initiated write
// has to be approved on the console — the PC-to-console round trip IS the safety
// property. A restore started from this screen already has the user in front of
// the console, so the Danger modal is that gate. Routing it through the broker
// would be a second dialog for one decision, and two dialogs for one decision is
// how people learn to dismiss both.

#include "screens/screen.hpp"
#include "core/fs.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

class SaveManagerScreen : public Screen {
public:
    SaveManagerScreen();

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

private:
    // Two parallel drill-downs share this one screen:
    //  - LIVE saves:   Users -> Titles -> Backups (restore/backup/wipe/delete)
    //  - BACKUP TREE:  BackupUsers -> BackupTitles -> Backups (restore only)
    // The backup tree is reachable independently of live saves — a deleted save's
    // title is gone from the live list, but its backups are still on the SD card,
    // and this is the only path back to them. Entered via a pinned row at the top
    // of the Users list.
    enum class Level { Users, Titles, Backups, BackupUsers, BackupTitles };

    Level                    m_level = Level::Users;
    std::string              m_user;              // valid at Level::Titles

    // True while restoring from the BACKUP TREE (Level::Backup* path), so the
    // Backups level knows to return to BackupTitles rather than live Titles, and
    // restore uses the label-derived app id even for a live title.
    bool                     m_in_backup_tree = false;
    std::vector<std::string> m_entries;           // names or title labels
    Widgets::List            m_list;

    // A backup awaiting its confirmation. Held across frames because the modal
    // resolves later than the press that raised it.
    std::string              m_pending_backup;

    // Restore in flight: the backup directory chosen, and the snapshot taken
    // before asking. Both are held across frames because the modal resolves
    // later than the press, and the snapshot path is shown again on failure —
    // when it is the only thing that matters.
    std::string              m_pending_restore_dir;
    std::string              m_pending_snapshot;
    std::string              m_title;             // valid at Level::Backups

    // Wipe in flight: the title, and the snapshot taken before asking. Same
    // rollback-first shape as restore — wiping a save is destructive, so the
    // on-device path takes a backup first and names it in the confirmation.
    std::string              m_pending_wipe_title;
    std::string              m_pending_wipe_snapshot;

    // Delete in flight: title + pre-delete snapshot. Delete removes the record so
    // the title vanishes from the list — the screen returns to the user level
    // afterwards, since staying on a title that no longer exists makes no sense.
    std::string              m_pending_delete_title;
    std::string              m_pending_delete_snapshot;

    // Operation state, matching FileBrowserScreen's shape so both screens report
    // progress the same way.
    Fs::Progress             m_progress;
    bool                     m_op_active = false;
    std::string              m_op_label;
    std::string              m_status;            // result line under the list

    // True while title names are still resolving on the main loop, so update()
    // knows to keep re-labelling the list. Cleared once every row has a real name.
    bool                     m_titles_pending = false;

    void reload();

    /// The label SHOWN for an entry. Differs from the entry itself only for backup
    /// title folders whose on-disk name is the id-only fallback: those display
    /// under the real game name while the folder keeps its recorded name, so no
    /// path that was ever written down stops working.
    std::string display_label(const std::string& entry) const;
    void start_backup(const std::string& title_label);
    void begin_restore(const std::string& stamp);
    void finish_restore();
    void begin_wipe(const std::string& title_label);
    void finish_wipe();
    void begin_delete(const std::string& title_label);
    void finish_delete();
};
