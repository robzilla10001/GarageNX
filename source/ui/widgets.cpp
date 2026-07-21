// source/ui/widgets.cpp

#include "ui/widgets.hpp"
#include "ui/renderer.hpp"
#include "ui/layout.hpp"
#include "ui/input.hpp"
#include <algorithm>
#include <cstring>

namespace Widgets {

// ─── Delayed round-robin navigation ───────────────────────────────────────────

int WrapNav::step(int cursor, int count, bool down, bool up, uint32_t delay_ms) {
    if (count <= 0) return cursor;
    if (count == 1) { edge_dir = 0; return 0; }

    const uint32_t now  = SDL_GetTicks();
    const int      last = count - 1;

    if (down) {
        if (cursor < last) { edge_dir = 0; return cursor + 1; }
        // At the bottom: arm on first contact, wrap once sustained past delay.
        if (edge_dir != 1)                    { edge_dir = 1; edge_since = now; }
        else if (now - edge_since >= delay_ms) { edge_dir = 0; return 0; }
    } else if (up) {
        if (cursor > 0) { edge_dir = 0; return cursor - 1; }
        if (edge_dir != -1)                    { edge_dir = -1; edge_since = now; }
        else if (now - edge_since >= delay_ms) { edge_dir = 0; return last; }
    }
    // Neither moved, or still arming at an edge: hold position. We intentionally
    // do NOT disarm when the button is released, so a hold that reaches the edge
    // and a later deliberate press both count toward the same wrap timer.
    return cursor;
}

// ─── List ─────────────────────────────────────────────────────────────────────

void List::set_items(std::vector<ListItem> items) {
    m_items        = std::move(items);
    m_cursor       = m_items.empty() ? -1 : 0;
    m_scroll_offset = 0;
    m_wrap.reset();
}

void List::update_items(std::vector<ListItem> items) {
    int prev_cursor = m_cursor;
    m_items = std::move(items);
    m_cursor = std::min(prev_cursor, static_cast<int>(m_items.size()) - 1);
    if (m_cursor < 0 && !m_items.empty()) m_cursor = 0;
    m_wrap.reset();
}

bool List::handle_input() {
    if (m_items.empty()) return false;

    // Discrete taps: one step per counted press, so fast tapping during a stalled
    // frame is not collapsed to a single move (the file-browser drop). Held
    // navigation still comes through repeat(). We take the max of "taps this
    // frame" and "repeat fired" rather than summing, so a held press that also
    // shows one poll-edge press does not double-step.
    const int  down_taps = Input::press_count(Input::Button::DDown);
    const int  up_taps   = Input::press_count(Input::Button::DUp);
    const bool down_rep  = Input::repeat(Input::Button::DDown);
    const bool up_rep    = Input::repeat(Input::Button::DUp);

    const int down_steps = std::max(down_taps, down_rep ? 1 : 0);
    const int up_steps   = std::max(up_taps,   up_rep   ? 1 : 0);

    for (int i = 0; i < down_steps; ++i)
        m_cursor = m_wrap.step(m_cursor, static_cast<int>(m_items.size()), true, false);
    for (int i = 0; i < up_steps; ++i)
        m_cursor = m_wrap.step(m_cursor, static_cast<int>(m_items.size()), false, true);

    if (Input::pressed(Input::Button::A)) {
        return true;
    }
    if (Input::pressed(Input::Button::X)) {
        toggle_selection();
    }
    if (Input::pressed(Input::Button::Y)) {
        // If any are selected, deselect all; otherwise select all
        bool any = !selected_indices().empty();
        if (any) deselect_all(); else select_all();
    }

    return false;
}

void List::draw(int x, int y, int w, int h, const ListStyle& style) {
    if (m_items.empty()) {
        // Draw a centered "(empty)" placeholder
        SDL_Surface* surf = Font::render("(empty)", Font::Size::Body,
                                         Font::Weight::Regular,
                                         Theme::get(Theme::Token::FgDisabled));
        Renderer::blit_centered(surf, x, y, w, h);
        SDL_FreeSurface(surf);
        return;
    }

    int visible_rows = h / style.row_height;
    ensure_visible(m_cursor, visible_rows);

    SDL_Renderer* r = Renderer::get();
    int row_y = y;

    for (int i = m_scroll_offset;
         i < static_cast<int>(m_items.size()) && row_y < y + h;
         ++i, row_y += style.row_height)
    {
        auto& item = m_items[i];
        bool focused = (i == m_cursor);

        // Row background
        if (focused) {
            Theme::apply(r, Theme::Token::BgElevated);
            Renderer::fill_rect(x, row_y, w, style.row_height);
            // Left accent bar for focused row
            Theme::apply(r, Theme::Token::Accent);
            Renderer::fill_rect(x, row_y, 3, style.row_height);
        } else if (item.is_selected) {
            SDL_Color sel = Theme::get(Theme::Token::Accent);
            sel.a = 40;
            SDL_SetRenderDrawColor(r, sel.r, sel.g, sel.b, sel.a);
            Renderer::fill_rect(x, row_y, w, style.row_height);
        }

        // Divider
        if (style.show_dividers && row_y > y) {
            Theme::apply(r, Theme::Token::Border);
            Renderer::hline(x, row_y, w);
        }

        int text_x = x + style.indent_x;

        // Checkbox
        if (style.show_checkbox) {
            Theme::apply(r, item.is_selected ? Theme::Token::Accent : Theme::Token::Border);
            Renderer::draw_rect(text_x, row_y + (style.row_height - 14) / 2, 14, 14);
            if (item.is_selected) {
                Theme::apply(r, Theme::Token::Accent);
                Renderer::fill_rect(text_x + 3, row_y + (style.row_height - 8) / 2, 8, 8);
            }
            text_x += 22;
        }

        // Label
        Theme::Token label_color = item.is_disabled
            ? Theme::Token::FgDisabled
            : (focused ? Theme::Token::FgPrimary : Theme::Token::FgPrimary);

        SDL_Color lc = Theme::get(label_color);
        int label_max_w = w - (text_x - x) - (item.meta.empty() ? style.indent_x : 120);

        // Label — cached: rasterised + uploaded once, reused every frame.
        int lh = 0, lw = 0;
        Renderer::measure_text(item.label, (int)Font::Size::Body,
                               (int)(focused ? Font::Weight::Bold : Font::Weight::Regular),
                               (int)Font::Family::Sans, lc, &lw, &lh);
        if (lh > 0) {
            Renderer::draw_text(item.label, (int)Font::Size::Body,
                                (int)(focused ? Font::Weight::Bold : Font::Weight::Regular),
                                (int)Font::Family::Sans, lc,
                                text_x, row_y + (style.row_height - lh) / 2,
                                nullptr, nullptr, label_max_w);
        }

        // Meta (right-aligned) — cached.
        if (!item.meta.empty()) {
            SDL_Color mc = Theme::get(Theme::Token::FgSecondary);
            int mw = 0, mh = 0;
            Renderer::measure_text(item.meta, (int)Font::Size::Small,
                                   (int)Font::Weight::Regular, (int)Font::Family::Sans,
                                   mc, &mw, &mh);
            if (mh > 0) {
                const int mx = x + w - style.indent_x - mw;
                Renderer::draw_text(item.meta, (int)Font::Size::Small,
                                    (int)Font::Weight::Regular, (int)Font::Family::Sans,
                                    mc, mx, row_y + (style.row_height - mh) / 2);
            }
        }
    }
}

void List::set_cursor(int idx) {
    m_cursor = std::max(0, std::min(idx, static_cast<int>(m_items.size()) - 1));
    m_wrap.reset();
}

void List::toggle_selection() {
    if (m_cursor >= 0 && m_cursor < static_cast<int>(m_items.size())) {
        m_items[m_cursor].is_selected = !m_items[m_cursor].is_selected;
    }
}

void List::select_all() {
    for (auto& item : m_items) item.is_selected = true;
}

void List::deselect_all() {
    for (auto& item : m_items) item.is_selected = false;
}

std::vector<int> List::selected_indices() const {
    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(m_items.size()); ++i) {
        if (m_items[i].is_selected) result.push_back(i);
    }
    return result;
}

void List::clamp_cursor() {
    if (m_items.empty()) { m_cursor = -1; return; }
    m_cursor = std::max(0, std::min(m_cursor, static_cast<int>(m_items.size()) - 1));
}

void List::ensure_visible(int idx, int visible_rows) {
    if (idx < m_scroll_offset)
        m_scroll_offset = idx;
    if (idx >= m_scroll_offset + visible_rows)
        m_scroll_offset = idx - visible_rows + 1;
    m_scroll_offset = std::max(0, m_scroll_offset);
}

// ─── Toggle ───────────────────────────────────────────────────────────────────

bool draw_toggle(int x, int y, bool value, bool focused) {
    SDL_Renderer* r = Renderer::get();
    constexpr int W = 44, H = 24;

    // Track background
    SDL_Color track = value
        ? Theme::get(Theme::Token::Accent)
        : Theme::get(Theme::Token::Border);
    SDL_SetRenderDrawColor(r, track.r, track.g, track.b, 255);
    Renderer::fill_rect(x, y, W, H);

    // Thumb
    int thumb_x = value ? x + W - H + 2 : x + 2;
    Theme::apply(r, Theme::Token::FgPrimary);
    Renderer::fill_rect(thumb_x, y + 2, H - 4, H - 4);

    // Focus ring
    if (focused) {
        Theme::apply(r, Theme::Token::Accent);
        Renderer::draw_rect(x - 2, y - 2, W + 4, H + 4);
    }

    return value; // caller updates on A press
}

// ─── Progress bar ─────────────────────────────────────────────────────────────

void draw_progress(int x, int y, int w, int h, float fraction) {
    SDL_Renderer* r = Renderer::get();
    fraction = std::max(0.0f, std::min(1.0f, fraction));

    // Track
    Theme::apply(r, Theme::Token::BgElevated);
    Renderer::fill_rect(x, y, w, h);

    // Fill
    int fill_w = static_cast<int>(w * fraction);
    if (fill_w > 0) {
        Theme::apply(r, Theme::Token::Accent);
        Renderer::fill_rect(x, y, fill_w, h);
    }

    // Border
    Theme::apply(r, Theme::Token::Border);
    Renderer::draw_rect(x, y, w, h);
}

// ─── Battery indicator ────────────────────────────────────────────────────────

void draw_battery(int x, int y, int w, int h, float fraction, ChargeState state) {
    SDL_Renderer* r = Renderer::get();
    fraction = std::max(0.0f, std::min(1.0f, fraction));

    // Body
    int body_w = w - 4;  // leave room for the nub
    Theme::apply(r, Theme::Token::Border);
    Renderer::draw_rect(x, y, body_w, h);

    // Nub
    int nub_h = h / 2;
    int nub_y = y + (h - nub_h) / 2;
    Theme::apply(r, Theme::Token::Border);
    Renderer::fill_rect(x + body_w, nub_y, 4, nub_h);

    // Fill color
    Theme::Token fill_color;
    if (state == ChargeState::Charging) {
        fill_color = Theme::Token::AccentOk;
    } else if (fraction < 0.15f) {
        fill_color = Theme::Token::AccentDanger;
    } else if (fraction < 0.30f) {
        fill_color = Theme::Token::AccentWarn;
    } else {
        fill_color = Theme::Token::AccentOk;
    }

    int fill_w = static_cast<int>((body_w - 4) * fraction);
    if (fill_w > 0) {
        Theme::apply(r, fill_color);
        Renderer::fill_rect(x + 2, y + 2, fill_w, h - 4);
    }
}

// ─── Button legend ────────────────────────────────────────────────────────────

void draw_button_legend(int x, int y, int w, const std::vector<ButtonHint>& hints) {
    // Draw right-to-left, right-aligned within the given width
    int cursor_x = x + w - Layout::PAD_MD;

    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        // Label
        SDL_Color lc = Theme::get(Theme::Token::FgSecondary);
        SDL_Surface* label_surf = Font::render(it->label, Font::Size::Small,
                                                Font::Weight::Regular, lc);
        if (label_surf) {
            cursor_x -= label_surf->w;
            Renderer::blit(label_surf, cursor_x, y + 2);
            SDL_FreeSurface(label_surf);
        }

        cursor_x -= Layout::PAD_XS;

        // Button pill
        SDL_Color ac = Theme::get(Theme::Token::Accent);
        SDL_Surface* btn_surf = Font::render(it->button, Font::Size::Small,
                                              Font::Weight::Bold, ac);
        if (btn_surf) {
            cursor_x -= btn_surf->w + Layout::PAD_XS * 2;
            // Pill background
            SDL_SetRenderDrawColor(Renderer::get(), ac.r, ac.g, ac.b, 40);
            Renderer::fill_rect(cursor_x - Layout::PAD_XS,
                                y + 1,
                                btn_surf->w + Layout::PAD_XS * 2,
                                btn_surf->h + 2);
            Renderer::blit(btn_surf, cursor_x, y + 2);
            SDL_FreeSurface(btn_surf);
        }

        cursor_x -= Layout::PAD_MD;
    }
}

// ─── Section header ───────────────────────────────────────────────────────────

void draw_section_header(int x, int y, int w, const std::string& text) {
    SDL_Color tc = Theme::get(Theme::Token::FgSecondary);
    SDL_Surface* surf = Font::render(text, Font::Size::Small, Font::Weight::Bold, tc);
    if (surf) {
        Renderer::blit(surf, x + Layout::PAD_MD, y + Layout::PAD_SM);
        SDL_FreeSurface(surf);
    }
    Theme::apply(Renderer::get(), Theme::Token::Border);
    Renderer::hline(x, y + Layout::LIST_ROW_H - 1, w);
}

// ─── Text helper ──────────────────────────────────────────────────────────────

int draw_text(int x, int y,
              const std::string& text,
              Font::Size size,
              Font::Weight weight,
              Theme::Token color,
              int max_width)
{
    // Routed through the renderer's text-texture cache: rasterise + upload once,
    // reuse across frames. This helper is called many times per frame by the
    // file browser (title, path, hints, per-pane detail lines); the old body
    // rasterised and destroyed a texture on every call, which is what made that
    // screen stutter and drop inputs.
    SDL_Color c = Theme::get(color);
    int w = 0, h = 0;
    Renderer::draw_text(text, (int)size, (int)weight, (int)Font::Family::Sans,
                        c, x, y, &w, &h, max_width);
    return w;
}



// ─── QR code ──────────────────────────────────────────────────────────────────

void draw_qr(int x, int y, int box_px, const Core::Qr::Code& code) {
    if (!code.ok() || box_px <= 0) return;

    SDL_Renderer* r = Renderer::get();

    // The spec requires a 4-module light margin; without it many scanners will
    // not lock on. Budget it inside the box the caller gave us.
    constexpr int kQuiet = 4;
    const int total_modules = code.size + kQuiet * 2;

    // Floor to a whole pixel per module: fractional scaling produces uneven
    // module widths, which is exactly what makes a QR hard to decode.
    const int scale = box_px / total_modules;
    if (scale < 1) return;   // box too small to render honestly

    const int drawn = total_modules * scale;
    const int ox = x + (box_px - drawn) / 2;   // centre the crisp result
    const int oy = y + (box_px - drawn) / 2;

    // White field (quiet zone included), then black modules. Not themed: a QR
    // inverted on a dark theme does not scan.
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    Renderer::fill_rect(ox, oy, drawn, drawn);

    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    for (int my = 0; my < code.size; my++) {
        for (int mx = 0; mx < code.size; mx++) {
            if (!code.at(mx, my)) continue;
            Renderer::fill_rect(ox + (mx + kQuiet) * scale,
                                oy + (my + kQuiet) * scale,
                                scale, scale);
        }
    }
}

} // namespace Widgets
