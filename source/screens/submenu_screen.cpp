// source/screens/submenu_screen.cpp

#include "screens/submenu_screen.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/layout.hpp"
#include "ui/input.hpp"

SubMenuScreen::SubMenuScreen(std::string title, const std::vector<MenuEntry>& items)
    : m_title(std::move(title)), m_items(items) {
    rebuild_items();
}

void SubMenuScreen::on_enter() {
    rebuild_items();
}

void SubMenuScreen::rebuild_items() {
    // Filter to visible items, then feed the list. m_items keeps the parallel
    // id list so a cursor index maps back to a MenuItem.
    std::vector<MenuEntry> visible;
    std::vector<Widgets::ListItem> rows;
    for (const auto& e : m_items) {
        if (!menu_item_visible(e.id)) continue;
        visible.push_back(e);
        Widgets::ListItem row;
        row.label = Lang::t(e.lang_key);
        rows.push_back(row);
    }
    m_items = std::move(visible);
    m_list.set_items(std::move(rows));
}

std::unique_ptr<Screen> SubMenuScreen::update(bool& pop) {
    pop = false;

    // B backs out of the submenu to the parent (main menu).
    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    if (m_list.handle_input()) {
        int idx = m_list.cursor();
        if (idx < 0 || idx >= static_cast<int>(m_items.size())) return nullptr;

        // Dispatch through the shared activator. A leaf that sets pop (an exit or
        // power action) must propagate that pop up so the app actually exits —
        // popping only THIS submenu would just return to the main menu. So we
        // return the child screen if any; if the action set pop, we keep it set.
        bool child_pop = false;
        auto next = menu_activate(m_items[idx].id, child_pop);
        if (next) return next;      // navigation: push the leaf screen
        if (child_pop) { pop = true; return nullptr; }  // exit/power: propagate
        return nullptr;             // not-yet-implemented leaf: stay put
    }
    return nullptr;
}

void SubMenuScreen::draw() {
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
