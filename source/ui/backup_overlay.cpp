// source/ui/backup_overlay.cpp

#include "ui/backup_overlay.hpp"

#include "lang/localization.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/renderer.hpp"
#include "ui/status_bar.hpp"
#include "ui/theme.hpp"
#include "ui/title_bar.hpp"
#include "ui/widgets.hpp"

#include <SDL2/SDL.h>

#include <cstdio>
#include <string>

namespace UI {

void draw_backup_overlay(const Core::SaveBackup::AutoProgress& pr) {
    // A self-contained frame: the sweep is blocking the main loop, so nothing else
    // will draw until it returns. Present each update so the console shows live
    // progress instead of appearing frozen.
    Renderer::begin_frame();
    TitleBar::draw();

    const int cw = 720, ch = 200;
    const int cx = (Layout::SCREEN_W - cw) / 2;
    const int cy = (Layout::SCREEN_H - ch) / 2;
    Theme::apply(Renderer::get(), Theme::Token::BgSurface);
    Renderer::fill_rect(cx, cy, cw, ch);

    // The enumerate phase gets its own wording. It is the SLOW step (priming the
    // ncm title-name cache), so a heading that only ever said "backing up" would
    // leave the longest stretch of the sweep unexplained.
    const std::string heading =
        pr.phase == Core::SaveBackup::AutoPhase::Enumerating
            ? Lang::t("auto_backup.working_scan")
            : Lang::t("auto_backup.working_title");
    SDL_Color fg = Theme::get(Theme::Token::FgPrimary);
    Renderer::draw_text(heading, (int)Font::Size::Large,
                        (int)Font::Weight::Bold, (int)Font::Family::Sans,
                        fg, cx + 32, cy + 28, nullptr, nullptr, cw - 64);

    if (pr.phase == Core::SaveBackup::AutoPhase::BackingUp && pr.total > 0) {
        SDL_Color fg2 = Theme::get(Theme::Token::FgSecondary);
        char line[96];
        std::snprintf(line, sizeof(line), "%d / %d", pr.current, pr.total);
        Renderer::draw_text(line, (int)Font::Size::Body,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans,
                            fg2, cx + 32, cy + 74, nullptr, nullptr, cw - 64);
        Renderer::draw_text(pr.title, (int)Font::Size::Small,
                            (int)Font::Weight::Regular, (int)Font::Family::Sans,
                            fg2, cx + 32, cy + 104, nullptr, nullptr, cw - 64);
        Widgets::draw_progress(cx + 32, cy + ch - 44, cw - 64, 14,
                               (float)pr.current / (float)pr.total);
    }

    StatusBar::draw();
    Renderer::end_frame();
}

} // namespace UI
