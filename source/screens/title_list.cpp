// source/screens/title_list.cpp

#include "screens/title_list.hpp"
#include "screens/title_detail.hpp"
#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "core/fs.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <cstdio>
#include <algorithm>

TitleListScreen::TitleListScreen() {}

TitleListScreen::~TitleListScreen() {
    free_icons();
}

void TitleListScreen::free_icons() {
    for (auto& r : m_rows) {
        if (r.icon) { SDL_DestroyTexture(r.icon); r.icon = nullptr; }
    }
}

void TitleListScreen::on_enter() {
    // on_enter() fires every time this screen becomes active — including when we
    // pop back from TitleDetail. Only build the list on the FIRST entry; on
    // return we keep the already-decrypted rows so we don't re-decrypt every
    // title's Control NCA again. The cache is discarded only when this screen is
    // destroyed (i.e. when the user leaves the title list entirely).
    if (m_phase == Phase::Ready || m_phase == Phase::Loading) {
        // Already loaded — but if a child screen changed the installed set
        // (e.g. deleted a title), rebuild instead of showing a stale list.
        if (Core::Ncm::titles_dirty()) {
            Core::Ncm::clear_titles_dirty();
            free_icons();
            m_rows.clear();
            m_cursor = 0;
            m_scroll = 0;
            m_phase = Phase::CheckKeys;   // fall through to reload below
        } else {
            return;  // reuse cached rows
        }
    }
    if (m_phase == Phase::NoKeys) {
        return;  // keys were missing; nothing changed
    }

    // First entry (Phase::CheckKeys): load keys and kick off enumeration.
    auto kr = Core::Keys::load();
    if (!kr.ok) {
        m_phase = Phase::NoKeys;
        m_keys_msg = Core::Keys::requirement_message();
        return;
    }
    begin_load();
}

void TitleListScreen::begin_load() {
    // Enumerate + group. Applications only in the main list; updates/DLC attach
    // to their app inside each group.
    bool ok = false;
    auto all = Core::Ncm::list_all(&ok);
    m_pending = Core::Ncm::group_by_application(all);

    m_load_total = (int)m_pending.size();
    m_load_done  = 0;
    m_rows.clear();
    m_phase = Phase::Loading;
}

SDL_Texture* TitleListScreen::decode_icon(const std::vector<uint8_t>& jpeg) {
    if (jpeg.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromConstMem(jpeg.data(), (int)jpeg.size());
    if (!rw) return nullptr;
    SDL_Surface* surf = IMG_Load_RW(rw, 1 /*free rw*/);
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(Renderer::get(), surf);
    SDL_FreeSurface(surf);
    return tex;
}

void TitleListScreen::resolve_next() {
    if (m_pending.empty()) { m_phase = Phase::Ready; return; }

    Core::Ncm::TitleGroup g = std::move(m_pending.front());
    m_pending.erase(m_pending.begin());

    Row row;
    row.group = g;

    // Decrypt the app's Control NCA for name + icon.
    auto cd = Core::Ncm::resolve_control(g.app, Core::Keys::get(), true /*icon*/);
    if (cd.ok && !cd.name.empty()) {
        row.name    = cd.name;
        row.version = cd.version;
        row.icon    = decode_icon(cd.icon_jpeg);
    } else {
        // Fall back to the program id if the control couldn't be read, and keep
        // the failure reason so we can diagnose which titles fail and why.
        char idbuf[24];
        snprintf(idbuf, sizeof(idbuf), "%016llX",
                 (unsigned long long)g.app.program_id);
        row.name = std::string(idbuf) + "  [" +
                   (cd.fail_reason.empty() ? "unknown" : cd.fail_reason) + "]";
    }

    m_rows.push_back(std::move(row));
    m_load_done++;

    if (m_pending.empty()) {
        // Sort finished list alphabetically by name (case-insensitive).
        std::sort(m_rows.begin(), m_rows.end(),
            [](const Row& a, const Row& b) {
                std::string la = a.name, lb = b.name;
                std::transform(la.begin(), la.end(), la.begin(), ::tolower);
                std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
                return la < lb;
            });
        m_phase = Phase::Ready;
    }
}

int TitleListScreen::visible_rows() const {
    return (Layout::CONTENT_H - 40) / Layout::LIST_ROW_H_LG;
}

std::unique_ptr<Screen> TitleListScreen::update(bool& pop) {
    pop = false;

    if (m_phase == Phase::NoKeys) {
        if (Input::pressed(Input::Button::B)) pop = true;
        return nullptr;
    }

    if (m_phase == Phase::Loading) {
        // Resolve a few per frame so the progress bar advances smoothly without
        // freezing input handling entirely.
        for (int i = 0; i < 2 && m_phase == Phase::Loading; ++i)
            resolve_next();
        // Allow bailing out mid-load.
        if (Input::pressed(Input::Button::B)) pop = true;
        return nullptr;
    }

    // Ready
    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    int vis = visible_rows();
    static Widgets::WrapNav s_nav;
    const bool nav_down = Input::repeat(Input::Button::DDown);
    const bool nav_up   = Input::repeat(Input::Button::DUp);
    int new_cursor = s_nav.step(m_cursor, (int)m_rows.size(), nav_down, nav_up);
    if (new_cursor != m_cursor) {
        m_cursor = new_cursor;
        if (m_cursor < m_scroll)        m_scroll = m_cursor;
        if (m_cursor >= m_scroll + vis) m_scroll = m_cursor - vis + 1;
        if (m_scroll < 0)               m_scroll = 0;
    }

    // A → open TitleDetail for the selected app.
    if (Input::pressed(Input::Button::A) && m_cursor < (int)m_rows.size()) {
        return std::make_unique<TitleDetailScreen>(m_rows[m_cursor].group,
                                                   m_rows[m_cursor].name);
    }

    return nullptr;
}

void TitleListScreen::draw() {
    const int x = 0, y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W, h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    // ── Keys missing ───────────────────────────────────────────────────────────
    if (m_phase == Phase::NoKeys) {
        int cy = y + Layout::PAD_LG;
        Widgets::draw_text(x + Layout::PAD_LG, cy, "Keys required",
                           Font::Size::Medium, Font::Weight::Bold,
                           Theme::Token::AccentDanger);
        cy += 40;
        // Multi-line message.
        std::string msg = m_keys_msg;
        size_t start = 0;
        while (start < msg.size()) {
            size_t nl = msg.find('\n', start);
            std::string line = msg.substr(start,
                nl == std::string::npos ? std::string::npos : nl - start);
            Widgets::draw_text(x + Layout::PAD_LG, cy, line, Font::Size::Small,
                               Font::Weight::Regular, Theme::Token::FgSecondary,
                               w - Layout::PAD_LG * 2);
            cy += 24;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        std::vector<Widgets::ButtonHint> hints = {{ "B", Lang::t("hints.back") }};
        Widgets::draw_button_legend(x, y + h - 30, w, hints);
        return;
    }

    // ── Loading ────────────────────────────────────────────────────────────────
    if (m_phase == Phase::Loading) {
        int bx = (w - 480) / 2, by = y + h / 2 - 40;
        Widgets::draw_text(bx, by, "Reading installed titles…",
                           Font::Size::Medium, Font::Weight::Bold,
                           Theme::Token::FgPrimary);
        char cnt[64];
        snprintf(cnt, sizeof(cnt), "%d / %d", m_load_done, m_load_total);
        Widgets::draw_text(bx, by + 34, cnt, Font::Size::Small,
                           Font::Weight::Regular, Theme::Token::FgSecondary);
        float frac = m_load_total ? (float)m_load_done / m_load_total : 0.f;
        Widgets::draw_progress(bx, by + 60, 480, 16, frac);
        return;
    }

    // ── Ready: title rows ──────────────────────────────────────────────────────
    const int row_h = Layout::LIST_ROW_H_LG;
    int vis = visible_rows();
    int cy = y + 4;

    // Reserve a right-hand column for the full-size highlighted-app icon. The
    // list text stops before this so nothing runs under the icon.
    const int icon_full = 256;                       // full-size icon edge
    const int icon_pad  = Layout::PAD_LG;
    const int icon_x    = w - icon_full - icon_pad;  // left edge of icon column
    const int list_right = icon_x - Layout::PAD_LG;  // text must end here

    for (int i = m_scroll; i < (int)m_rows.size() && i < m_scroll + vis; ++i) {
        const Row& row = m_rows[i];
        bool focused = (i == m_cursor);

        // Row background for the focused row (spans only the list area).
        if (focused) {
            Theme::apply(r, Theme::Token::BgElevated);
            Renderer::fill_rect(x, cy, list_right - x, row_h);
            Theme::apply(r, Theme::Token::Accent);
            Renderer::fill_rect(x, cy, 3, row_h);  // focus bar
        }

        int text_x = x + Layout::PAD_LG;

        // Name (primary) — standard size, no inline icon.
        Widgets::draw_text(text_x, cy + 8, row.name,
                           Font::Size::Body, Font::Weight::Bold,
                           Theme::Token::FgPrimary, list_right - text_x - 160);

        // Second line: version + updates/DLC counts.
        char sub[96];
        int nupd = (int)row.group.updates.size();
        int ndlc = (int)row.group.dlc.size();
        snprintf(sub, sizeof(sub), "v%s   %d update%s · %d DLC",
                 row.version.empty() ? "?" : row.version.c_str(),
                 nupd, nupd == 1 ? "" : "s", ndlc);
        Widgets::draw_text(text_x, cy + 28, sub,
                           Font::Size::Tiny, Font::Weight::Regular,
                           Theme::Token::FgSecondary, list_right - text_x - 160);

        // Right side of the row (within the list area): storage + size.
        const char* store = (row.group.app.storage == Core::Ncm::Storage::SdCard)
                            ? "SD" : "NAND";
        char right[64];
        snprintf(right, sizeof(right), "%s  ·  %s", store,
                 Fs::format_size(row.group.app.size_bytes).c_str());
        {
            SDL_Color c = Theme::get(Theme::Token::FgSecondary);
            SDL_Surface* s = Font::render(right, Font::Size::Small,
                                          Font::Weight::Regular, c);
            if (s) { Renderer::blit_right(s, x, cy + 16, list_right - Layout::PAD_MD, 24);
                     SDL_FreeSurface(s); }
        }

        cy += row_h;
    }

    // ── Full-size icon of the highlighted app, bottom-right ─────────────────────
    if (m_cursor >= 0 && m_cursor < (int)m_rows.size()) {
        const Row& sel = m_rows[m_cursor];
        int iy = y + h - icon_full - 40;   // sit above the button legend
        if (iy < y + Layout::PAD_MD) iy = y + Layout::PAD_MD;

        if (sel.icon) {
            SDL_Rect dst { icon_x, iy, icon_full, icon_full };
            SDL_RenderCopy(r, sel.icon, nullptr, &dst);
            // Subtle frame around the icon.
            Theme::apply(r, Theme::Token::Border);
            Renderer::draw_rect(icon_x, iy, icon_full, icon_full);
        } else {
            // Placeholder box when no icon decoded.
            Theme::apply(r, Theme::Token::BgSurface);
            Renderer::fill_rect(icon_x, iy, icon_full, icon_full);
            Theme::apply(r, Theme::Token::Border);
            Renderer::draw_rect(icon_x, iy, icon_full, icon_full);
        }
    }

    // Count + scroll position header line at bottom.
    char pos[64];
    snprintf(pos, sizeof(pos), "%d applications", (int)m_rows.size());
    Widgets::draw_text(x + Layout::PAD_MD, y + h - 28, pos,
                       Font::Size::Tiny, Font::Weight::Regular,
                       Theme::Token::FgDisabled);

    std::vector<Widgets::ButtonHint> hints = {
        { "A", "Details" },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 30, w, hints);
}
