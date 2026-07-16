// source/ui/title_bar.cpp

#include "ui/title_bar.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include <SDL2/SDL.h>

namespace TitleBar {

static Info s_info;

void set(const Info& info) {
    s_info = info;
}

void draw() {
    SDL_Renderer* r = Renderer::get();

    // Background
    Theme::apply(r, Theme::Token::TitleBarBg);
    Renderer::fill_rect(0, 0, Layout::SCREEN_W, Layout::TITLE_BAR_H);

    // Bottom border
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(0, Layout::TITLE_BAR_H - 1, Layout::SCREEN_W);

    int cy = (Layout::TITLE_BAR_H - static_cast<int>(Font::Size::Body)) / 2;

    // App name (left, bold, accent color)
    SDL_Color accent = Theme::get(Theme::Token::Accent);
    SDL_Surface* name_surf = Font::render(APP_NAME, Font::Size::Body,
                                           Font::Weight::Bold, accent);
    if (name_surf) {
        Renderer::blit(name_surf, Layout::PAD_MD, cy);
        SDL_FreeSurface(name_surf);
    }

    // Version string (center-right, secondary)
    std::string ver_str =
        "v" + s_info.app_version +
        "  |  FW " + s_info.fw_version +
        "  |  SDK " + s_info.sdk_version;

    SDL_Color sc = Theme::get(Theme::Token::FgSecondary);
    SDL_Surface* ver_surf = Font::render(ver_str, Font::Size::Small,
                                          Font::Weight::Regular, sc);
    if (ver_surf) {
        Renderer::blit_right(ver_surf,
                              0, 0,
                              Layout::SCREEN_W - Layout::PAD_MD,
                              Layout::TITLE_BAR_H);
        SDL_FreeSurface(ver_surf);
    }
}

} // namespace TitleBar
