// source/screens/tickets.cpp

#include "screens/tickets.hpp"

#include "lang/localization.hpp"
#include "services/save_surface.hpp"
#include "services/title_surface.hpp"
#include "ui/font.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/renderer.hpp"
#include "ui/status_bar.hpp"
#include "ui/theme.hpp"
#include "ui/title_bar.hpp"

#include <SDL2/SDL.h>

#include <cstdio>

namespace {

std::string rights_id_hex(const std::array<uint8_t, 0x10>& rid) {
    char buf[0x21];
    for (int i = 0; i < 0x10; ++i)
        std::snprintf(buf + i * 2, 3, "%02X", rid[(size_t)i]);
    return std::string(buf, 0x20);
}

} // namespace

void TicketsScreen::build_rows() {
    // Resolving title names blocks; drive the resolver rather than waiting on it,
    // and draw while working. Same contract as the Activity Log and the backup
    // sweep — this runs on the main thread, so it must do the main loop's job.
    auto pump = [] {
        Renderer::begin_frame();
        TitleBar::draw();
        SDL_Color fg = Theme::get(Theme::Token::FgPrimary);
        Renderer::draw_text(Lang::t("tickets.loading"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            Layout::MENU_INDENT_X, Layout::CONTENT_Y + 60,
                            nullptr, nullptr, Layout::SCREEN_W - Layout::MENU_INDENT_X * 2);
        StatusBar::draw();
        Renderer::end_frame();
    };

    Services::installed_titles_request_nonblocking();
    const uint32_t deadline = SDL_GetTicks() + 20000;
    while (!Services::installed_titles_names_resolved() &&
           (int32_t)(SDL_GetTicks() - deadline) < 0) {
        Services::installed_titles_tick();
        pump();
    }

    m_tickets = Core::Es::list_common_tickets();

    std::vector<Widgets::ListItem> items;
    items.reserve(m_tickets.size());
    for (const auto& t : m_tickets) {
        Widgets::ListItem it;
        // save_build_label resolves to "<Name> [APPID]", or the id fallback when
        // the title is not installed — which is common and correct here: a ticket
        // can outlive the title it belongs to.
        const std::string label = Services::save_build_label(t.title_id);
        it.label = Services::save_label_is_unresolved(label)
                       ? rights_id_hex(t.rights_id)   // no name: show the real thing
                       : label;
        it.meta  = Services::save_label_is_unresolved(label)
                       ? Lang::t("tickets.not_installed")
                       : std::string();
        items.push_back(std::move(it));
    }
    m_list.set_items(std::move(items));
}

std::unique_ptr<Screen> TicketsScreen::update(bool& pop) {
    pop = false;
    if (m_phase == 0) { m_phase = 1; return nullptr; }
    if (m_phase == 1) { m_phase = 2; build_rows(); return nullptr; }

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }
    m_list.handle_input();
    return nullptr;
}

void TicketsScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    SDL_Color fg  = Theme::get(Theme::Token::FgPrimary);
    SDL_Color fg2 = Theme::get(Theme::Token::FgSecondary);

    if (m_phase < 2) {
        Renderer::draw_text(Lang::t("tickets.loading"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 60,
                            nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
        return;
    }

    if (m_tickets.empty()) {
        Renderer::draw_text(Lang::t("tickets.empty"), (int)Font::Size::Body,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 60,
                            nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
    } else {
        char hdr[64];
        std::snprintf(hdr, sizeof(hdr), "%s: %zu",
                      Lang::t("tickets.count").c_str(), m_tickets.size());
        Renderer::draw_text(hdr, (int)Font::Size::Small, (int)Font::Weight::Regular,
                            (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 8, nullptr, nullptr, w);

        Widgets::ListStyle style;
        style.row_height    = Layout::MENU_ITEM_H;
        style.indent_x      = Layout::MENU_INDENT_X;
        style.show_checkbox = false;
        style.show_dividers = true;
        m_list.draw(x, y + 32, w, h - 32 - 36, style);
    }

    std::vector<Widgets::ButtonHint> hints = { { "B", Lang::t("hints.back") } };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
