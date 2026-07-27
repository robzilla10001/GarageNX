// source/ui/modal.cpp

#include "ui/modal.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/input.hpp"
#include "ui/widgets.hpp"
#include <SDL2/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

namespace Modal {

// ─── State ────────────────────────────────────────────────────────────────────

static bool    s_active  = false;
static Options s_opts;
static int     s_focus   = 0;   // 0 = confirm button, 1 = cancel button
static Result  s_result  = Result::Pending;

// Hold-to-confirm state for Danger modals: the confirm button requires holding
// A for HOLD_SECONDS rather than a single press, adding friction to
// irreversible actions (file deletes, etc.).
// Body wrapped ONCE in show(), not per frame. Wrapping measures many trial
// substrings, and the body cannot change while a modal is up, so re-wrapping
// every frame would be pure waste on the main thread — the same thread the
// transports block on waiting for a confirmation.
static std::vector<std::string> s_body_lines;

static uint32_t s_hold_start = 0;   // SDL_GetTicks when the A-hold began (0=idle)
static float    s_hold_frac  = 0.f;
static constexpr float MODAL_HOLD_SECONDS = 1.5f;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static Theme::Token accent_for_kind(Kind kind) {
    switch (kind) {
        case Kind::Warning: return Theme::Token::AccentWarn;
        case Kind::Danger:  return Theme::Token::AccentDanger;
        default:            return Theme::Token::Accent;
    }
}

static void draw_button(int x, int y, int w, int h,
                         const std::string& label,
                         bool focused, Theme::Token color)
{
    SDL_Renderer* r = Renderer::get();

    if (focused) {
        Theme::apply(r, color);
        Renderer::fill_rect(x, y, w, h);
        SDL_Color tc = Theme::get(Theme::Token::BgBase);
        SDL_Surface* surf = Font::render(label, Font::Size::Body,
                                          Font::Weight::Bold, tc);
        if (surf) { Renderer::blit_centered(surf, x, y, w, h); SDL_FreeSurface(surf); }
    } else {
        Theme::apply(r, Theme::Token::Border);
        Renderer::draw_rect(x, y, w, h);
        SDL_Color tc = Theme::get(color);
        SDL_Surface* surf = Font::render(label, Font::Size::Body,
                                          Font::Weight::Regular, tc);
        if (surf) { Renderer::blit_centered(surf, x, y, w, h); SDL_FreeSurface(surf); }
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void show(const Options& opts) {
    s_opts   = opts;

    // Wrap here rather than at draw time. NOTE the measurement path: wrapping
    // uses Font::measure_width (pure TTF metrics), NOT Renderer::measure_text,
    // even though the latter also returns a width. measure_text populates the
    // TEXT TEXTURE CACHE as a side effect, and wrapping measures dozens of
    // throwaway substrings ("NAND (Sys", "NAND (Syst", ...) that will never be
    // drawn. Routing wrapping through it would fill the cache with garbage and
    // evict the entries that are actually on screen. They are not duplicates of
    // each other; do not "unify" them.
    const int body_w = Layout::MODAL_W - Layout::PAD_LG * 2;
    s_body_lines = Font::wrap(opts.body, body_w, Font::Size::Body,
                              Font::Weight::Regular);

    // Clamp a body long enough to overflow the SCREEN, which would otherwise
    // draw a modal taller than the display and hide its own buttons — leaving no
    // way to answer it, and for a broker confirmation, a transport blocked until
    // the timeout.
    const int line_h   = static_cast<int>(Font::Size::Body) + 6;
    const int chrome_h = Layout::PAD_LG
                       + static_cast<int>(Font::Size::Large) + Layout::PAD_MD
                       + Layout::PAD_LG + 44 + Layout::PAD_MD;
    const int max_lines = std::max(
        1, (Layout::SCREEN_H - Layout::PAD_LG * 4 - chrome_h) / line_h);
    if (static_cast<int>(s_body_lines.size()) > max_lines) {
        s_body_lines.resize(max_lines);
        s_body_lines.back() += "...";
    }

    s_active = true;
    s_focus  = 0;
    s_result = Result::Pending;
    s_hold_start = 0;
    s_hold_frac  = 0.f;
}

bool is_active() { return s_active; }

void dismiss() {
    s_active = false;
    s_result = Result::Cancelled;
}

Result update_and_draw() {
    if (!s_active) return Result::Pending;

    SDL_Renderer* r = Renderer::get();

    // ── Dim overlay ──────────────────────────────────────────────────────────
    SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
    Renderer::fill_rect(0, 0, Layout::SCREEN_W, Layout::SCREEN_H);

    // Height follows the WRAPPED line count (computed in show()). This used to
    // count only explicit '\n' and render each stretch as ONE line, so any body
    // longer than the modal ran off the box and off the screen. It was marked a
    // placeholder; it became a real bug the moment a modal carried a sentence
    // rather than a label. The write-confirmation modals were exposed to the same
    // thing without anyone hitting it yet — they show FILE PATHS, which contain
    // no spaces and so need the mid-token break too.
    const int line_h = static_cast<int>(Font::Size::Body) + 6;
    int body_h = static_cast<int>(s_body_lines.size()) * line_h;
    int modal_h = std::max(Layout::MODAL_MIN_H,
                            Layout::PAD_LG                           // top pad
                            + static_cast<int>(Font::Size::Large) + Layout::PAD_MD  // title
                            + body_h + Layout::PAD_LG                // body
                            + 44 + Layout::PAD_MD);                  // buttons + bottom pad
    int modal_y = (Layout::SCREEN_H - modal_h) / 2;

    // ── Background ───────────────────────────────────────────────────────────
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(Layout::MODAL_X, modal_y, Layout::MODAL_W, modal_h);

    // Top accent line
    Theme::apply(r, accent_for_kind(s_opts.kind));
    Renderer::fill_rect(Layout::MODAL_X, modal_y, Layout::MODAL_W, 3);

    // Border
    Theme::apply(r, Theme::Token::Border);
    Renderer::draw_rect(Layout::MODAL_X, modal_y, Layout::MODAL_W, modal_h);

    int cx = Layout::MODAL_X + Layout::PAD_LG;
    int cy = modal_y + Layout::PAD_LG;

    // ── Title ─────────────────────────────────────────────────────────────────
    {
        SDL_Color tc = Theme::get(Theme::Token::FgPrimary);
        SDL_Surface* surf = Font::render(s_opts.title, Font::Size::Large,
                                          Font::Weight::Bold, tc);
        if (surf) { Renderer::blit(surf, cx, cy); SDL_FreeSurface(surf); }
        cy += static_cast<int>(Font::Size::Large) + Layout::PAD_MD;
    }

    // ── Body ─────────────────────────────────────────────────────────────────
    {
        SDL_Color bc = Theme::get(Theme::Token::FgSecondary);
        for (const auto& line : s_body_lines) {
            if (!line.empty()) {
                SDL_Surface* surf = Font::render(line, Font::Size::Body,
                                                  Font::Weight::Regular, bc);
                if (surf) { Renderer::blit(surf, cx, cy); SDL_FreeSurface(surf); }
            }
            cy += line_h;      // an empty line still advances: blank = spacing
        }
        cy += Layout::PAD_LG;
    }

    // ── Buttons ───────────────────────────────────────────────────────────────
    Theme::Token accent = accent_for_kind(s_opts.kind);
    bool two_buttons = (s_opts.kind != Kind::Info);
    int btn_y = modal_y + modal_h - 44 - Layout::PAD_MD;

    if (two_buttons) {
        int btn_w = (Layout::MODAL_W - Layout::PAD_LG * 3) / 2;

        bool is_danger = (s_opts.kind == Kind::Danger);

        // Confirm button label: for Danger, show hold progress inline.
        draw_button(cx,              btn_y, btn_w, 40,
                    s_opts.confirm_label, s_focus == 0, accent);
        draw_button(cx + btn_w + Layout::PAD_LG, btn_y, btn_w, 40,
                    s_opts.cancel_label,  s_focus == 1, Theme::Token::FgSecondary);

        // For Danger modals, draw a hold-progress bar above the buttons.
        if (is_danger && s_focus == 0) {
            int bar_y = btn_y - 16;
            Widgets::draw_progress(cx, bar_y, Layout::MODAL_W - Layout::PAD_LG * 2,
                                   8, s_hold_frac);
        }

        // Navigation + cancel are the same for all kinds.
        if (Input::pressed(Input::Button::DLeft) || Input::pressed(Input::Button::DRight)) {
            s_focus = 1 - s_focus;
            s_hold_start = 0; s_hold_frac = 0.f;   // reset hold if focus moves
        }
        if (Input::pressed(Input::Button::B)) {
            s_active = false;
            return Result::Cancelled;
        }

        if (is_danger && s_focus == 0) {
            // HOLD A to confirm a destructive action.
            if (Input::held(Input::Button::A)) {
                uint32_t now = SDL_GetTicks();
                if (s_hold_start == 0) s_hold_start = now;
                s_hold_frac = (now - s_hold_start) / 1000.0f / MODAL_HOLD_SECONDS;
                if (s_hold_frac >= 1.0f) {
                    s_active = false;
                    return Result::Confirmed;
                }
            } else {
                s_hold_start = 0; s_hold_frac = 0.f;   // released early → reset
            }
        } else {
            // Non-danger (or cancel focused): single press.
            if (Input::pressed(Input::Button::A)) {
                s_active = false;
                return s_focus == 0 ? Result::Confirmed : Result::Cancelled;
            }
        }
    } else {
        // Info — single OK button
        int btn_w = 120;
        int btn_x = Layout::MODAL_X + (Layout::MODAL_W - btn_w) / 2;
        draw_button(btn_x, btn_y, btn_w, 40, s_opts.confirm_label, true, accent);

        if (Input::pressed(Input::Button::A) || Input::pressed(Input::Button::B)) {
            s_active = false;
            return Result::Confirmed;
        }
    }

    return Result::Pending;
}

} // namespace Modal
