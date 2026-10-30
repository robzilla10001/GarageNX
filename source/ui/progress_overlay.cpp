// source/ui/progress_overlay.cpp

#include "ui/progress_overlay.hpp"

#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/renderer.hpp"
#include "ui/status_bar.hpp"
#include "ui/theme.hpp"
#include "ui/title_bar.hpp"
#include "ui/widgets.hpp"

namespace UI {

void draw_progress_overlay(const std::string& heading, const std::string& detail,
                           float fraction) {
    Renderer::begin_frame();
    TitleBar::draw();

    const int cw = 720, ch = 200;
    const int cx = (Layout::SCREEN_W - cw) / 2;
    const int cy = (Layout::SCREEN_H - ch) / 2;
    Theme::apply(Renderer::get(), Theme::Token::BgSurface);
    Renderer::fill_rect(cx, cy, cw, ch);

    SDL_Color fg = Theme::get(Theme::Token::FgPrimary);
    Renderer::draw_text(heading, (int)Font::Size::Large, (int)Font::Weight::Bold,
                        (int)Font::Family::Sans, fg, cx + 32, cy + 28,
                        nullptr, nullptr, cw - 64);
    if (!detail.empty()) {
        SDL_Color fg2 = Theme::get(Theme::Token::FgSecondary);
        Renderer::draw_text(detail, (int)Font::Size::Body, (int)Font::Weight::Regular,
                            (int)Font::Family::Sans, fg2, cx + 32, cy + 80,
                            nullptr, nullptr, cw - 64);
    }
    if (fraction >= 0.f)
        Widgets::draw_progress(cx + 32, cy + ch - 44, cw - 64, 14, fraction);

    StatusBar::draw();
    Renderer::end_frame();
}

} // namespace UI
