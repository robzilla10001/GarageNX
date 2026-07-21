#pragma once
// source/screens/submenu_screen.hpp
//
// One reusable screen for every submenu (Browse, Connectivity, System, Exit).
// Constructed with a title and an item list; it renders the list, dispatches
// selections through the shared menu_activate(), and B pops back to the parent.
// Using a single parameterized screen keeps every submenu's look and behaviour
// identical and avoids four near-duplicate classes.

#include "screens/screen.hpp"
#include "screens/menu_dispatch.hpp"
#include "ui/widgets.hpp"

#include <string>
#include <vector>

class SubMenuScreen : public Screen {
public:
    SubMenuScreen(std::string title, const std::vector<MenuEntry>& items);

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    std::string           m_title;
    std::vector<MenuEntry> m_items;   // visible items only
    Widgets::List         m_list;

    void rebuild_items();
};
