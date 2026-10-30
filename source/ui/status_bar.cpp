// source/ui/status_bar.cpp

#include "ui/status_bar.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "core/app_exit.hpp"
#include "lang/localization.hpp"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>

namespace StatusBar {

static Info s_info;

void set(const Info& info) { s_info = info; }

static std::string storage_str(float free_gb, float total_gb) {
    char buf[64];
    float used = total_gb - free_gb;
    int pct = (total_gb > 0.f)
        ? static_cast<int>(100.f * used / total_gb)
        : 0;
    // Use MB if total is less than 1 GB
    if (total_gb < 1.f) {
        snprintf(buf, sizeof(buf), "%.0f/%.0fMB (%d%%)",
                 free_gb * 1024.f, total_gb * 1024.f, pct);
    } else {
        snprintf(buf, sizeof(buf), "%.1f/%.0fGB (%d%%)",
                 free_gb, total_gb, pct);
    }
    return buf;
}

void draw() {
    SDL_Renderer* r = Renderer::get();

    // Background
    Theme::apply(r, Theme::Token::StatusBarBg);
    Renderer::fill_rect(0, Layout::STATUS_BAR_Y, Layout::SCREEN_W, Layout::STATUS_BAR_H);

    // Top border
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(0, Layout::STATUS_BAR_Y, Layout::SCREEN_W);

    // Applet-mode notice, just above the bar. Applet mode (hbmenu/Sphaira album
    // launch) gives the app far less memory than an Application, so the user
    // should know why a large operation might fail. Drawn here because the bar is
    // painted after the screen, so this lands on top of the content area's foot.
    static const bool applet_mode = Core::in_applet_mode();
    if (applet_mode) {
        // Two lines of near-equal width, broken at a space (never mid-word), so the
        // notice stays narrow enough not to cover the content above it.
        const std::string msg = Lang::t("status.applet_mode");
        std::string l1 = msg, l2;
        size_t best = std::string::npos, best_diff = msg.size();
        for (size_t i = 0; i < msg.size(); ++i) {
            if (msg[i] != ' ') continue;
            const size_t d = i > msg.size() - i - 1 ? i - (msg.size() - i - 1)
                                                    : (msg.size() - i - 1) - i;
            if (d < best_diff) { best_diff = d; best = i; }
        }
        if (best != std::string::npos) { l1 = msg.substr(0, best); l2 = msg.substr(best + 1); }

        const SDL_Color red = Theme::get(Theme::Token::AccentDanger);
        SDL_Surface* s1 = Font::render(l1, Font::Size::Small, Font::Weight::Bold, red);
        SDL_Surface* s2 = l2.empty() ? nullptr
                                     : Font::render(l2, Font::Size::Small, Font::Weight::Bold, red);
        if (s1) {
            const int lh = s1->h + 1;
            const int bh = lh * (s2 ? 2 : 1) + 4;
            const int bw = std::max(s1->w, s2 ? s2->w : 0) + Layout::PAD_MD * 2;
            const int by = Layout::STATUS_BAR_Y - bh - 2;
            Theme::apply(r, Theme::Token::BgBase);
            Renderer::fill_rect(0, by, bw, bh);
            Renderer::blit(s1, Layout::PAD_MD, by + 2);
            if (s2) Renderer::blit(s2, Layout::PAD_MD, by + 2 + lh);
        }
        if (s1) SDL_FreeSurface(s1);
        if (s2) SDL_FreeSurface(s2);
    }

    int cy     = Layout::STATUS_BAR_Y
               + (Layout::STATUS_BAR_H - static_cast<int>(Font::Size::Small)) / 2;
    int x      = Layout::STATUS_PAD_X;
    SDL_Color sc = Theme::get(Theme::Token::FgSecondary);

    // ── SD card ───────────────────────────────────────────────────────────────
    {
        std::string label = "SD: " + storage_str(s_info.sd_free_gb, s_info.sd_total_gb);
        SDL_Surface* surf = Font::render(label, Font::Size::Small,
                                          Font::Weight::Regular, sc);
        if (surf) {
            Renderer::blit(surf, x, cy);
            x += surf->w + Layout::PAD_LG;
            SDL_FreeSurface(surf);
        }
    }

    // ── NAND ──────────────────────────────────────────────────────────────────
    {
        std::string label = "NAND: " + storage_str(s_info.nand_free_gb, s_info.nand_total_gb);
        SDL_Surface* surf = Font::render(label, Font::Size::Small,
                                          Font::Weight::Regular, sc);
        if (surf) {
            Renderer::blit(surf, x, cy);
            x += surf->w + Layout::PAD_LG;
            SDL_FreeSurface(surf);
        }
    }

    // ── SoC temperature ───────────────────────────────────────────────────────
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f°C", s_info.soc_temp_c);
        SDL_Surface* surf = Font::render(buf, Font::Size::Small,
                                          Font::Weight::Regular, sc);
        if (surf) {
            Renderer::blit(surf, x, cy);
            x += surf->w + Layout::PAD_LG;
            SDL_FreeSurface(surf);
        }
    }

    // ── Right side: clock + battery ───────────────────────────────────────────
    int rx = Layout::SCREEN_W - Layout::STATUS_PAD_X;

    // Battery icon
    {
        using CS = Widgets::ChargeState;
        CS state = s_info.is_charging ? CS::Charging : CS::Discharging;
        int batt_y = Layout::STATUS_BAR_Y + (Layout::STATUS_BAR_H - Layout::BATTERY_BAR_H) / 2;
        rx -= (Layout::BATTERY_BAR_W + 4);
        Widgets::draw_battery(rx, batt_y,
                               Layout::BATTERY_BAR_W, Layout::BATTERY_BAR_H,
                               s_info.battery_pct, state);
    }

    rx -= Layout::PAD_SM;

    // Battery percentage text
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%",
                 static_cast<int>(s_info.battery_pct * 100.f));
        SDL_Surface* surf = Font::render(buf, Font::Size::Small,
                                          Font::Weight::Regular, sc);
        if (surf) {
            rx -= surf->w;
            Renderer::blit(surf, rx, cy);
            rx -= Layout::PAD_MD;
            SDL_FreeSurface(surf);
        }
    }

    // Charging indicator
    if (s_info.is_charging) {
        SDL_Color ok = Theme::get(Theme::Token::AccentOk);
        SDL_Surface* surf = Font::render("▲", Font::Size::Small,
                                          Font::Weight::Regular, ok);
        if (surf) {
            rx -= surf->w;
            Renderer::blit(surf, rx, cy);
            rx -= Layout::PAD_SM;
            SDL_FreeSurface(surf);
        }
    } else {
        SDL_Color warn = Theme::get(Theme::Token::FgSecondary);
        SDL_Surface* surf = Font::render("▼", Font::Size::Small,
                                          Font::Weight::Regular, warn);
        if (surf) {
            rx -= surf->w;
            Renderer::blit(surf, rx, cy);
            rx -= Layout::PAD_SM;
            SDL_FreeSurface(surf);
        }
    }

    // Clock
    if (!s_info.clock_str.empty()) {
        SDL_Surface* surf = Font::render(s_info.clock_str, Font::Size::Small,
                                          Font::Weight::Regular, sc);
        if (surf) {
            rx -= surf->w + Layout::PAD_MD;
            Renderer::blit(surf, rx, cy);
            SDL_FreeSurface(surf);
        }
    }
}

} // namespace StatusBar
