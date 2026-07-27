// source/screens/activity_log.cpp

#include "screens/activity_log.hpp"

#include "lang/localization.hpp"
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

// "12h 34m", "34m", "—" when pdm has no record. Deliberately not seconds: a
// playtime column that ticks in seconds invites reading precision that the
// underlying data does not have.
std::string fmt_playtime(uint64_t seconds, bool valid) {
    if (!valid) return "\u2014";
    if (seconds < 60) return "<1m";
    const uint64_t h = seconds / 3600;
    const uint64_t m = (seconds % 3600) / 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%lluh %02llum",
                             (unsigned long long)h, (unsigned long long)m);
    else       std::snprintf(buf, sizeof(buf), "%llum", (unsigned long long)m);
    return buf;
}

} // namespace

void ActivityLogScreen::build_rows() {
    // Gathering draws its own frames: it blocks this thread, so nothing else will.
    auto pump = [] {
        Renderer::begin_frame();
        TitleBar::draw();
        SDL_Color fg = Theme::get(Theme::Token::FgPrimary);
        Renderer::draw_text(Lang::t("activity_log.loading"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            Layout::MENU_INDENT_X, Layout::CONTENT_Y + 60,
                            nullptr, nullptr, Layout::SCREEN_W - Layout::MENU_INDENT_X * 2);
        StatusBar::draw();
        Renderer::end_frame();
    };

    m_rows = Core::Activity::title_play_stats(pump);

    std::vector<Widgets::ListItem> items;
    items.reserve(m_rows.size());
    for (const auto& r : m_rows) {
        Widgets::ListItem it;
        it.label = r.title_label;
        char meta[64];
        if (r.valid)
            std::snprintf(meta, sizeof(meta), "%s  \u00b7  %u\u00d7",
                          fmt_playtime(r.playtime_seconds, true).c_str(), r.launches);
        else
            std::snprintf(meta, sizeof(meta), "%s", Lang::t("activity_log.never").c_str());
        it.meta = meta;
        items.push_back(std::move(it));
    }
    m_list.set_items(std::move(items));
}

std::unique_ptr<Screen> ActivityLogScreen::update(bool& pop) {
    pop = false;

    // Phase 0 lets draw() put a frame on screen before the blocking gather.
    if (m_phase == 0) { m_phase = 1; return nullptr; }
    if (m_phase == 1) { m_phase = 2; build_rows(); return nullptr; }

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }
    m_list.handle_input();
    return nullptr;
}

void ActivityLogScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    if (m_phase < 2) {
        SDL_Color fg = Theme::get(Theme::Token::FgPrimary);
        Renderer::draw_text(Lang::t("activity_log.loading"), (int)Font::Size::Large,
                            (int)Font::Weight::Bold, (int)Font::Family::Sans, fg,
                            x + Layout::MENU_INDENT_X, y + 60,
                            nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
        return;
    }

    if (m_rows.empty()) {
        SDL_Color fg2 = Theme::get(Theme::Token::FgSecondary);
        Renderer::draw_text(Lang::t("activity_log.empty"), (int)Font::Size::Body,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans, fg2,
                            x + Layout::MENU_INDENT_X, y + 60,
                            nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
    } else {
        Widgets::ListStyle style;
        style.row_height    = Layout::MENU_ITEM_H;
        style.indent_x      = Layout::MENU_INDENT_X;
        style.show_checkbox = false;
        style.show_dividers = true;
        m_list.draw(x, y, w, h - 36, style);
    }

    std::vector<Widgets::ButtonHint> hints = { { "B", Lang::t("hints.back") } };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
