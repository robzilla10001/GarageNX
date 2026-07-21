// source/screens/main_menu.cpp
//
// The top-level menu. Since the menu reorg, this is a thin screen over the shared
// menu model (menu_dispatch): it shows the top-level entries (Install from
// Cartridge, Installed Titles, and the Browse/Connectivity/System/Exit submenu
// openers) and dispatches selections through menu_activate(), which either opens a
// submenu, pushes a leaf screen, or performs an exit/power action.

#include "screens/main_menu.hpp"
#include "screens/menu_dispatch.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include <SDL2/SDL.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

// Parallel id list: cursor index -> MenuItem for the currently shown rows.
static std::vector<MenuItem> s_id_map;

MainMenuScreen::MainMenuScreen() { rebuild_items(); }

void MainMenuScreen::on_enter() { rebuild_items(); }

void MainMenuScreen::rebuild_items() {
    std::vector<Widgets::ListItem> rows;
    s_id_map.clear();
    for (const auto& entry : menu_top_level()) {
        if (!menu_item_visible(entry.id)) continue;
        Widgets::ListItem row;
        row.label = Lang::t(entry.lang_key);
        rows.push_back(row);
        s_id_map.push_back(entry.id);
    }
    m_list.set_items(std::move(rows));
}

std::unique_ptr<Screen> MainMenuScreen::update(bool& pop) {
    pop = false;

    // B on the top level exits GarageNX (same clean path as the Exit submenu's
    // exit-to-home / exit-to-hbmenu items), so the user needn't open Exit to leave.
    if (Input::pressed(Input::Button::B)) {
#ifdef PLATFORM_SWITCH
        // Under a loader, arm hbmenu; as a real Application, clean-terminate to HOME.
        switch (appletGetAppletType()) {
            case AppletType_Application:
            case AppletType_SystemApplication:
                break;                       // clean exit -> HOME
            default:
                if (envHasNextLoad())
                    envSetNextLoad("sdmc:/hbmenu.nro", "sdmc:/hbmenu.nro");
                break;
        }
#endif
        pop = true;
        return nullptr;
    }

    if (m_list.handle_input()) {
        int idx = m_list.cursor();
        if (idx < 0 || idx >= static_cast<int>(s_id_map.size())) return nullptr;
        return menu_activate(s_id_map[idx], pop);
    }
    return nullptr;
}

void MainMenuScreen::draw() {
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

    m_list.draw(x, y, w, h - 36, style);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.open") },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
