#pragma once
// source/screens/settings_screen.hpp
//
// ONE parameterized settings screen, built from a row list — the same choice
// SubMenuScreen made for submenus, and for the same reason: a screen per section
// would be six near-identical classes that drift apart the first time one of them
// gets a fix.
//
// Round 1 handles the two row kinds that need no text entry: Toggle and Submenu.
// Number and string rows (ports, credentials, SSID) arrive with the software
// keyboard in round 2 — see the roadmap. Adding them is a new Kind and a new
// branch, not a new screen.
//
// ── Persistence ──────────────────────────────────────────────────────────────
// A row writes straight into Config::get_mutable() and marks the settings dirty.
// The write to DISK happens in on_exit(), only when dirty.
//
// Note that push() calls on_exit() on the parent screen, so this is really "flush
// pending changes at any navigation boundary", not "save when the user leaves
// settings". That is the safer reading anyway: a change cannot be lost by an
// unexpected exit, and Config::save() is non-destructive, so an extra write costs
// nothing but a file rewrite.

#include "screens/screen.hpp"
#include "ui/widgets.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class SettingsScreen : public Screen {
public:
    struct Row {
        enum class Kind { Toggle, Submenu, Choice, Text, Number };

        Kind        kind = Kind::Toggle;
        std::string label;

        // Toggle
        std::function<bool()>     get;
        std::function<void(bool)> set;

        // Submenu
        std::function<std::unique_ptr<Screen>()> open;

        // Choice — opens a list picker. Used where a handful of fixed values reads
        // better than free entry.
        std::function<int()>    choice_get;
        std::function<void(int)> choice_set;
        std::vector<int>         choice_values;
        std::vector<std::string> choice_labels;

        // Text — opens the system keyboard (swkbd). The keyboard is BLOCKING: it
        // hands control to the OS overlay and returns on confirm/cancel, which is
        // why it is only ever invoked from a button press and never from draw().
        std::function<std::string()>            text_get;
        std::function<void(const std::string&)> text_set;
        bool        text_secret = false;   // display as dots (passwords/tokens)
        int         text_max_len = 255;
        bool        text_allow_empty = true;

        // Number — numeric keyboard, clamped to [num_min, num_max]. Clamping is
        // done here rather than trusted to the entry, because a port of 0 or 99999
        // is accepted by the keyboard and only fails much later at bind() time.
        std::function<int()>    num_get;
        std::function<void(int)> num_set;
        int         num_min = 0;
        int         num_max = 65535;
        std::string num_suffix;            // e.g. "s" for seconds

        /// Turning this ON requires an on-device confirmation. Turning it OFF
        /// never does — removing access is not the dangerous direction.
        /// This row is the "reset everything" action rather than a setting. It
        /// borrows the Toggle kind's confirmation machinery but performs a reset
        /// instead of setting a value.
        bool        is_reset_action = false;

        bool        confirm_on_enable = false;
        std::string confirm_title;
        std::string confirm_body;
    };

    SettingsScreen(std::string title, std::vector<Row> rows);

    void on_enter() override;
    void on_exit() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

    /// The root of the settings tree. This is what the System menu opens.
    static std::unique_ptr<Screen> root();

private:
    std::string      m_title;
    std::vector<Row> m_rows;
    Widgets::List    m_list;

    /// Row awaiting a confirmation modal, or -1. Held across frames because the
    /// modal resolves on a later frame than the press that raised it.
    int              m_pending_row = -1;

    // In-screen picker for Choice rows. A list-select is a different shape from
    // the shared confirm/cancel Modal (it returns a chosen index, needs list
    // navigation), so rather than overload Modal and every call site, Choice rows
    // open this self-contained overlay — the same in-screen-selection approach the
    // Save Manager uses. -1 = no picker open.
    int              m_picker_row = -1;
    Widgets::List    m_picker_list;

    void rebuild_rows();
    void apply_toggle(int idx, bool value);
    void open_picker(int row_idx);
    void draw_picker();
};

namespace Settings {

/// True if a setting changed since the last successful write.
bool dirty();

/// Mark settings changed. Any code that mutates Config outside this screen must
/// call this, or the change will sit in memory and never reach the disk.
void mark_dirty();

/// Write config if dirty. Clears the flag only on a SUCCESSFUL write, so a
/// failed save is retried at the next boundary rather than silently dropped.
/// Also re-runs the config-gated mounts, so enabling a NAND surface takes effect
/// immediately instead of at the next launch.
bool flush();

} // namespace Settings
