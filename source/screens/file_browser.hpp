#pragma once
// source/screens/file_browser.hpp
// Ranger-style three-column file browser. Reused across all filesystem contexts
// (SD, NAND, USB, network-backed) by pointing it at a different root path.
//
// Layout (default):   nav/breadcrumb | listing | details
// Layout (split):      nav | source listing | destination listing
//                          | source details  | operation context
//
// Buttons:
//   A       open file / enter directory
//   B       up one level (or exit at root)
//   X       toggle selection
//   Y       mark all / deselect all
//   +       context menu
//   -       toggle split view
//   L/R     (in split) switch active pane

#include "screens/screen.hpp"
#include "core/fs.hpp"
#include "core/keys.hpp"
#include "install/installer.hpp"
#include "ui/widgets.hpp"
#include <string>
#include <vector>
#include <memory>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

class FileBrowserScreen : public Screen {
public:
    // root: the starting directory (e.g. "sdmc:/"). title: shown in the nav column.
    FileBrowserScreen(std::string root, std::string title);

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

private:
    // ── One navigable pane ──────────────────────────────────────────────────
    struct Pane {
        std::string        path;
        std::vector<Fs::Entry> entries;
        Widgets::List      list;
        bool               ok = true;
        std::vector<int>   cursor_stack;   // saved cursor per level, for B (back)

        // reset_cursor=true moves the selector to the top (use when the path
        // changes, e.g. opening a folder). Default preserves the cursor, which
        // is what a same-directory refresh after a file operation wants.
        void reload(bool reset_cursor = false);
        std::string selected_path() const;   // full path of the cursor entry
        const Fs::Entry* selected_entry() const;
    };

    std::string m_root;
    std::string m_title;

    Pane m_left;      // primary pane (source in split mode)
    Pane m_right;     // destination pane (only used in split mode)
    bool m_split = false;
    bool m_right_active = false;   // in split mode, which pane has focus

    // Clipboard for copy/cut across navigation
    enum class ClipMode { None, Copy, Cut };
    ClipMode                 m_clip_mode = ClipMode::None;
    std::vector<std::string> m_clipboard;

    // ── Context menu ──────────────────────────────────────────────────────────
    bool                     m_menu_open = false;
    Widgets::List            m_menu_list;
    std::vector<int>         m_menu_action_ids;   // maps menu row → action id

    // ── Active file operation (background thread) ──────────────────────────────
    // Milestone 2 runs ops synchronously via a simple worker; wired to a modal.
    // (Threading harness lands with services in a later milestone; for now the
    //  progress struct is updated inline and shown, which is acceptable for SD.)
    Fs::Progress             m_progress;
    bool                     m_op_active = false;
    std::string              m_op_label;

    // ── Pending actions ─────────────────────────────────────────────────────
    // Some actions can't complete inline (they need a modal answer or must push
    // a screen, which only update() can return). They're stashed here and
    // processed at the top of the next update().
    std::vector<std::string> m_pending_delete;   // awaiting delete confirmation
    std::string              m_pending_view_path; // open in viewer next frame
    bool                     m_pending_view_hex = false;

    // ── Install pipeline (Milestone 5) ──────────────────────────────────────
    enum class InstallMode { None, Installing, Result };
    InstallMode              m_install_mode = InstallMode::None;
    Install::Progress        m_install_progress;
    std::string              m_install_path;     // source file being installed
    Core::Ncm::Storage       m_install_storage = Core::Ncm::Storage::SdCard;
    bool                     m_install_result_ok = false;
    int                      m_install_log_scroll = 0;    // scroll offset in Result mode
    bool                     m_install_log_saved  = false; // persisted-to-disk guard
#ifdef PLATFORM_SWITCH
    Thread                   m_install_thread{};
#endif
    bool                     m_install_thread_active = false;

    void start_install(const std::string& path, Core::Ncm::Storage storage);
    void poll_install();
    static void install_thread_fn(void* arg);
    void draw_install_overlay();
    void save_install_log();   // persist the verbose log to a timestamped file

    Pane& active_pane() { return (m_split && m_right_active) ? m_right : m_left; }

    void open_context_menu();
    void open_install_menu();   // A on an installable file: Install to SD / NAND
    void handle_context_action(int action_id);

    void do_paste(bool cut);
    void do_delete_selected();
    void do_rename();
    void do_new_dir();
    void do_new_file();

    void draw_pane(Pane& pane, int x, int y, int w, int h, bool active, bool details);
    void draw_details(const Pane& pane, int x, int y, int w, int h);
    void draw_nav_column(int x, int y, int w, int h);
    void draw_context_menu();

    std::vector<std::string> selected_or_cursor(Pane& pane);
};
