// source/screens/menu_dispatch.hpp
//
// Shared menu model for the main menu and its submenus. One definition of every
// menu item, one place that turns an item into an action (push a screen, or set
// pop for exit/power). Both MainMenuScreen and the reusable SubMenuScreen use
// these, so a leaf item behaves identically wherever it appears and the big
// action switch isn't duplicated.

#pragma once

#include <memory>
#include <vector>

class Screen;

// Every selectable item. Leaf items map to an action (a screen or an exit/power
// effect). Submenu items (Browse/Connectivity/System/Exit) open a SubMenuScreen
// holding their own child items.
enum class MenuItem {
    // Top level leaves
    InstallCartridge,
    InstalledTitles,

    // Submenu openers
    BrowseMenu,
    ConnectivityMenu,
    SystemMenu,
    ExitMenu,

    // Browse submenu
    BrowseSD,
    BrowseSystemPartition,
    BrowseUserPartition,
    BrowseUSB,
    BrowseNetwork,
    Homebrew,
    Tickets,
    Saves,

    // Connectivity submenu
    MTP,
    FTP,
    HTTP,

    // System submenu
    Tools,
    Settings,
    ActivityLog,

    // Exit submenu
    ExitToHome,
    ExitToHBMenu,
    RestartToBootloader,
    Shutdown,
};

// A menu entry: the item and its language key (looked up via Lang::t()).
struct MenuEntry {
    MenuItem    id;
    const char* lang_key;
};

// The item lists for each submenu, in display order. Defined once here.
const std::vector<MenuEntry>& menu_top_level();
const std::vector<MenuEntry>& menu_browse_items();
const std::vector<MenuEntry>& menu_connectivity_items();
const std::vector<MenuEntry>& menu_system_items();
const std::vector<MenuEntry>& menu_exit_items();

// Is this item a submenu opener? If so, menu_open_submenu() returns its screen.
bool menu_is_submenu(MenuItem id);

// Whether an item should be shown given current runtime state (e.g. exit-to-hbmenu
// only under a loader). Pure enough to unit-test the leaf cases.
bool menu_item_visible(MenuItem id);

// Turn a selected item into an action:
//   - submenu opener  -> returns the SubMenuScreen for it
//   - leaf navigation -> returns the target screen
//   - exit/power leaf -> requests app quit (see menu_quit_requested) and returns null
// Shared by the main menu and every submenu.
std::unique_ptr<Screen> menu_activate(MenuItem id, bool& pop);

// Exit/power items (Exit to HOME/hbmenu) must quit the whole app, not merely pop
// the submenu they live in. They set this request; the main loop checks it and
// ends regardless of how deep in submenus the user was. (A plain screen pop only
// removes one screen, which for a submenu would just return to the top level.)
bool menu_quit_requested();
void menu_request_quit();
