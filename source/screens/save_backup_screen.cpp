// source/screens/save_backup_screen.cpp

#include "screens/save_backup_screen.hpp"

#include "core/save_backup.hpp"
#include "lang/localization.hpp"
#include "ui/backup_overlay.hpp"
#include "ui/font.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <SDL2/SDL.h>

#include <string>
#include <vector>

std::unique_ptr<Screen> SaveBackupScreen::update(bool& pop) {
    pop = false;

    if (Modal::is_active()) return nullptr;

    if (m_finished) { pop = true; return nullptr; }

    // Phase 0: do nothing, so draw() below gets one frame onto the display before
    // the blocking sweep starts. Same reason the automatic sweep waits for the
    // second loop iteration — otherwise the console sits on the previous screen
    // with no indication anything is happening.
    if (m_phase == 0) { m_phase = 1; return nullptr; }

    if (m_phase == 1) {
        m_phase = 2;

        // Blocking. It paints its own frames through the shared overlay, which is
        // the only way to show progress from an operation that owns this thread.
        m_backed_up = Core::SaveBackup::backup_all(UI::draw_backup_overlay);

        Modal::Options o;
        o.kind          = Modal::Kind::Info;
        o.confirm_label = Lang::t("common.ok");
        if (m_backed_up > 0) {
            o.title = Lang::t("backup_saves.done_title");
            // The count is CONCATENATED, never passed through a translated string
            // used as a printf format. A lang file is data; a translation that
            // said "%s" where the code passed an int would be undefined behaviour
            // on a console, triggered by nothing worse than a bad translation.
            // Everywhere else in this codebase the format is a literal and the
            // translated text is an argument — same rule here.
            o.body = std::to_string(m_backed_up) + " " +
                     Lang::t("backup_saves.done_body");
        } else {
            // Zero can mean "no saves on the console" or "every copy failed".
            // Saying "none were backed up" covers both without claiming success.
            o.title = Lang::t("backup_saves.none_title");
            o.body  = Lang::t("backup_saves.none_body");
        }
        Modal::show(o);
        return nullptr;
    }
    return nullptr;
}

void SaveBackupScreen::on_modal_result(int result) {
    (void)result;   // Info modal: dismissing it is the only outcome.
    m_finished = true;
}

void SaveBackupScreen::draw() {
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

    Renderer::draw_text(Lang::t("backup_saves.title"), (int)Font::Size::Large,
                        (int)Font::Weight::Bold, (int)Font::Family::Sans,
                        fg, x + Layout::MENU_INDENT_X, y + 40,
                        nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);

    Renderer::draw_text(Lang::t("backup_saves.working"), (int)Font::Size::Body,
                        (int)Font::Weight::Regular, (int)Font::Family::Sans,
                        fg2, x + Layout::MENU_INDENT_X, y + 92,
                        nullptr, nullptr, w - Layout::MENU_INDENT_X * 2);
}

std::string SaveBackupScreen::hint_string() const {
    return std::string();   // nothing to press; the operation runs and reports.
}
