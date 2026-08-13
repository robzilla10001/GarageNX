#pragma once
// source/screens/network_browser.hpp
//
// The Browse Network CHOOSER: a list of the connections configured in
// config.json (network > shares). Selecting one prompts for an SMB password when
// needed, connects/mounts it through the shared choke point (Services::net_resolve),
// and — once mounted — opens the ordinary FileBrowserScreen on the "net:" root, so
// a network share reuses the whole file manager exactly as USB does.
//
// This screen owns only display state and the selection flow. All connection and
// mount policy lives in services/net_surface, so the chooser cannot drift from
// whatever a future transport does with the same connections.
//
// Passwords are prompt-per-session: entered here (masked), held in memory by
// Services::net_credentials(), never written to disk. A failed connect clears the
// held password so the next attempt re-prompts rather than silently refusing.

#include "screens/screen.hpp"
#include "ui/widgets.hpp"
#include "core/sleep_inhibit.hpp"

#include <memory>
#include <string>
#include <vector>

class NetworkBrowserScreen : public Screen {
public:
    NetworkBrowserScreen();

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    // Keep the console awake for the whole network session: this chooser stays on
    // the stack beneath the FileBrowserScreen it opens on net:/, so holding the
    // guard here inhibits auto-sleep/dimming through browsing AND long transfers —
    // matching FTP/HTTP/MTP, which each hold one for their session. (The idle timer
    // is what was cutting off SMB transfers mid-way.)
    Core::SleepInhibit::Guard m_stay_awake;

    // One selectable connection. `name` keys the config lookup and the credential
    // store; `label` is the display string built once by net_display_label().
    struct Row {
        std::string name;
        std::string label;
    };

    std::vector<Row> m_rows;   // empty => the "no connections" info row is shown
    Widgets::List    m_list;

    void rebuild();
    std::unique_ptr<Screen> open_selected();
};
