// source/screens/title_detail.cpp

#include "screens/title_detail.hpp"
#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "core/fs.hpp"
#include "core/title_ops.hpp"
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

TitleDetailScreen::TitleDetailScreen(Core::Ncm::TitleGroup group, std::string name)
    : m_group(std::move(group)), m_name(std::move(name)) {}

TitleDetailScreen::~TitleDetailScreen() {
    if (m_icon) SDL_DestroyTexture(m_icon);
}

void TitleDetailScreen::on_enter() {
    load_icon();
}

void TitleDetailScreen::load_icon() {
    // Re-decrypt the app's control for a larger icon + version (cheap: one NCA).
    auto cd = Core::Ncm::resolve_control(m_group.app, Core::Keys::get(), true);
    if (cd.ok) {
        m_version = cd.version;
        if (!cd.icon_jpeg.empty()) {
            SDL_RWops* rw = SDL_RWFromConstMem(cd.icon_jpeg.data(),
                                               (int)cd.icon_jpeg.size());
            if (rw) {
                SDL_Surface* surf = IMG_Load_RW(rw, 1);
                if (surf) {
                    m_icon = SDL_CreateTextureFromSurface(Renderer::get(), surf);
                    SDL_FreeSurface(surf);
                }
            }
        }
    }
}

std::unique_ptr<Screen> TitleDetailScreen::update(bool& pop) {
    pop = false;

    switch (m_mode) {
        case Mode::Browsing: {
            if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

            int total_rows = (int)(m_group.updates.size() + m_group.dlc.size());
            int visible = 6;
            if (Input::repeat(Input::Button::DDown) && m_scroll < total_rows - visible)
                m_scroll++;
            if (Input::repeat(Input::Button::DUp) && m_scroll > 0)
                m_scroll--;

            // + opens the action menu (currently just Delete).
            if (Input::pressed(Input::Button::Plus)) {
                m_mode = Mode::ActionMenu;
            }
            return nullptr;
        }

        case Mode::ActionMenu: {
            if (Input::pressed(Input::Button::B)) { m_mode = Mode::Browsing; return nullptr; }
            static Widgets::WrapNav s_action_nav;
            m_action_cursor = s_action_nav.step(
                m_action_cursor, 3,
                Input::repeat(Input::Button::DDown),
                Input::repeat(Input::Button::DUp));
            if (Input::pressed(Input::Button::A)) {
                if (m_action_cursor == 0) {
                    start_dump();                 // Dump to SD
                } else if (m_action_cursor == 1) {
                    start_move();                 // Move SD<->NAND
                } else {
                    m_mode = Mode::ConfirmDelete; // Delete
                    m_hold_progress = 0.f;
                    m_hold_start = 0;
                }
            }
            return nullptr;
        }

        case Mode::ConfirmDelete: {
            // Cancel with B at any time.
            if (Input::pressed(Input::Button::B)) {
                m_mode = Mode::Browsing;
                m_hold_start = 0;
                m_hold_progress = 0.f;
                return nullptr;
            }
            // Hold A to fill the progress, measured in real time (wall clock) so
            // the duration is correct regardless of frame rate. Release resets.
            if (Input::held(Input::Button::A)) {
                uint32_t now = SDL_GetTicks();
                if (m_hold_start == 0) m_hold_start = now;   // began holding
                float elapsed = (now - m_hold_start) / 1000.0f;
                m_hold_progress = elapsed / HOLD_SECONDS;
                if (m_hold_progress >= 1.0f) {
                    m_hold_progress = 1.0f;
                    m_mode = Mode::Deleting;
                }
            } else {
                m_hold_start = 0;        // released early — reset
                m_hold_progress = 0.f;
            }
            return nullptr;
        }

        case Mode::Deleting: {
            // Perform the delete once, then show the result.
            do_delete();
            m_mode = Mode::Result;
            return nullptr;
        }

        case Mode::Result: {
            // Any button dismisses. If the delete succeeded, pop back to the
            // list (which rebuilds and will no longer show this title).
            if (Input::pressed(Input::Button::A) ||
                Input::pressed(Input::Button::B)) {
                if (m_result_ok) { pop = true; }
                else { m_mode = Mode::Browsing; }
            }
            return nullptr;
        }

        case Mode::Dumping: {
            // B requests cancel; the worker checks the flag and stops cleanly.
            if (Input::pressed(Input::Button::B)) m_dump.cancel.store(true);
            poll_dump();
            return nullptr;
        }

        case Mode::DumpResult: {
            if (Input::pressed(Input::Button::A) ||
                Input::pressed(Input::Button::B)) {
                m_mode = Mode::Browsing;
            }
            return nullptr;
        }

        case Mode::Moving: {
            if (Input::pressed(Input::Button::B)) m_move.cancel.store(true);
            poll_move();
            return nullptr;
        }

        case Mode::MoveResult: {
            if (Input::pressed(Input::Button::A) ||
                Input::pressed(Input::Button::B)) {
                // A successful move changes storage → refresh the list on return.
                if (m_result_ok) { Core::Ncm::mark_titles_dirty(); pop = true; }
                else m_mode = Mode::Browsing;
            }
            return nullptr;
        }
    }
    return nullptr;
}

// ─── Move ────────────────────────────────────────────────────────────────────

void TitleDetailScreen::move_thread_fn(void* arg) {
    auto* self = static_cast<TitleDetailScreen*>(arg);
    Core::TitleOps::move_title(self->m_group.app, self->m_move);
}

void TitleDetailScreen::start_move() {
    m_move.reset();
    m_mode = Mode::Moving;
#ifdef PLATFORM_SWITCH
    if (R_SUCCEEDED(threadCreate(&m_move_thread, move_thread_fn, this, nullptr,
                                 0x20000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&m_move_thread))) {
            m_move_thread_active = true;
            return;
        }
        threadClose(&m_move_thread);
    }
    move_thread_fn(this);
    poll_move();
#else
    move_thread_fn(this);
#endif
}

void TitleDetailScreen::poll_move() {
    if (!m_move.done.load()) return;
#ifdef PLATFORM_SWITCH
    if (m_move_thread_active) {
        threadWaitForExit(&m_move_thread);
        threadClose(&m_move_thread);
        m_move_thread_active = false;
    }
#endif
    m_result_ok  = m_move.success.load();
    m_result_msg = m_move.message.empty()
                 ? (m_result_ok ? "Move complete" : "Move failed")
                 : m_move.message;
    m_mode = Mode::MoveResult;
}

// ─── Dump ────────────────────────────────────────────────────────────────────

void TitleDetailScreen::dump_thread_fn(void* arg) {
    auto* self = static_cast<TitleDetailScreen*>(arg);
    Core::Dump::dump_title_to_nsp(self->m_group.app, Core::Keys::get(),
                                  self->m_dump, self->m_dump_out_path);
}

void TitleDetailScreen::start_dump() {
    m_dump.reset();
    m_mode = Mode::Dumping;
#ifdef PLATFORM_SWITCH
    // Run the dump on a background thread so the UI keeps refreshing the bar.
    // Priority slightly below the main thread; 128 KB stack is plenty (buffers
    // are heap-allocated inside the dump).
    if (R_SUCCEEDED(threadCreate(&m_dump_thread, dump_thread_fn, this, nullptr,
                                 0x20000, 0x2C, -2))) {
        if (R_SUCCEEDED(threadStart(&m_dump_thread))) {
            m_dump_thread_active = true;
            return;
        }
        threadClose(&m_dump_thread);
    }
    // Thread failed to start — fall back to a synchronous dump.
    dump_thread_fn(this);
    poll_dump();
#else
    dump_thread_fn(this);
#endif
}

void TitleDetailScreen::poll_dump() {
    if (!m_dump.done.load()) return;

#ifdef PLATFORM_SWITCH
    if (m_dump_thread_active) {
        threadWaitForExit(&m_dump_thread);
        threadClose(&m_dump_thread);
        m_dump_thread_active = false;
    }
#endif
    m_result_ok  = m_dump.success.load();
    m_result_msg = m_dump.message.empty()
                 ? (m_result_ok ? "Dump complete" : "Dump failed")
                 : m_dump.message;
    m_mode = Mode::DumpResult;
}

void TitleDetailScreen::do_delete() {
    auto res = Core::TitleOps::delete_application_completely(
        m_group.app.program_id);
    m_result_ok  = res.ok;
    m_result_msg = res.message;
    m_deleted    = res.ok;
    if (res.ok) Core::Ncm::mark_titles_dirty();
}

void TitleDetailScreen::draw() {
    const int x = 0, y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W, h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    // ── Full-size icon, bottom-right (matches the TitleList layout) ─────────────
    const int icon_full = 256;
    const int icon_pad  = Layout::PAD_LG;
    const int icon_x    = w - icon_full - icon_pad;
    int       icon_y    = y + h - icon_full - 40;   // above the button legend
    if (icon_y < y + Layout::PAD_MD) icon_y = y + Layout::PAD_MD;

    if (m_icon) {
        SDL_Rect dst { icon_x, icon_y, icon_full, icon_full };
        SDL_RenderCopy(r, m_icon, nullptr, &dst);
        Theme::apply(r, Theme::Token::Border);
        Renderer::draw_rect(icon_x, icon_y, icon_full, icon_full);
    } else {
        Theme::apply(r, Theme::Token::BgSurface);
        Renderer::fill_rect(icon_x, icon_y, icon_full, icon_full);
        Theme::apply(r, Theme::Token::Border);
        Renderer::draw_rect(icon_x, icon_y, icon_full, icon_full);
    }

    // Text content must stop before the icon column.
    const int content_right = icon_x - Layout::PAD_LG;

    // ── Header: name + core facts (top-left) ────────────────────────────────────
    int tx = x + Layout::PAD_LG;
    int hy = y + Layout::PAD_LG;

    Widgets::draw_text(tx, hy, m_name, Font::Size::Title, Font::Weight::Bold,
                       Theme::Token::FgPrimary, content_right - tx);

    char idline[48];
    snprintf(idline, sizeof(idline), "%016llX",
             (unsigned long long)m_group.app.program_id);
    Widgets::draw_text(tx, hy + 40, idline, Font::Size::Small,
                       Font::Weight::Regular, Theme::Token::FgSecondary);

    char facts[128];
    const char* store = (m_group.app.storage == Core::Ncm::Storage::SdCard)
                        ? "SD card" : "System memory";
    snprintf(facts, sizeof(facts), "v%s   ·   %s   ·   %s",
             m_version.empty() ? "?" : m_version.c_str(),
             Fs::format_size(m_group.app.size_bytes).c_str(), store);
    Widgets::draw_text(tx, hy + 66, facts, Font::Size::Small,
                       Font::Weight::Regular, Theme::Token::FgSecondary,
                       content_right - tx);

    // Divider under the header.
    int cy = hy + 96 + Layout::PAD_SM;
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(x + Layout::PAD_LG, cy, content_right - (x + Layout::PAD_LG));
    cy += Layout::PAD_MD;

    // ── Updates section ────────────────────────────────────────────────────────
    auto section_header = [&](const char* label, int count) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "%s (%d)", label, count);
        Widgets::draw_text(x + Layout::PAD_LG, cy, hdr, Font::Size::Small,
                           Font::Weight::Bold, Theme::Token::Accent);
        cy += 28;
    };

    auto draw_sub = [&](const Core::Ncm::Title& t, const char* tag) {
        char line[128];
        const char* store2 = (t.storage == Core::Ncm::Storage::SdCard) ? "SD" : "NAND";
        snprintf(line, sizeof(line), "  %s  %016llX  v%u  %s  %s",
                 tag, (unsigned long long)t.meta_id, t.version, store2,
                 Fs::format_size(t.size_bytes).c_str());
        Widgets::draw_text(x + Layout::PAD_LG, cy, line, Font::Size::Small,
                           Font::Weight::Regular, Theme::Token::FgSecondary,
                           content_right - (x + Layout::PAD_LG));
        cy += 24;
    };

    section_header("Updates", (int)m_group.updates.size());
    if (m_group.updates.empty()) {
        Widgets::draw_text(x + Layout::PAD_LG * 2, cy, "None installed",
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgDisabled);
        cy += 24;
    } else {
        for (auto& u : m_group.updates) draw_sub(u, "UPD");
    }
    cy += Layout::PAD_SM;

    section_header("DLC", (int)m_group.dlc.size());
    if (m_group.dlc.empty()) {
        Widgets::draw_text(x + Layout::PAD_LG * 2, cy, "None installed",
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgDisabled);
        cy += 24;
    } else {
        for (auto& d : m_group.dlc) draw_sub(d, "DLC");
    }

    // Legend.
    std::vector<Widgets::ButtonHint> hints = {
        { "+", "Options" },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 30, w, hints);

    // ── Overlays for the delete flow ────────────────────────────────────────────
    auto dim = [&](int alpha) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, alpha);
        Renderer::fill_rect(0, y, w, h);
    };

    auto panel = [&](int pw, int ph) -> SDL_Rect {
        SDL_Rect box { (w - pw) / 2, y + (h - ph) / 2, pw, ph };
        Theme::apply(r, Theme::Token::BgSurface);
        Renderer::fill_rect(box.x, box.y, box.w, box.h);
        Theme::apply(r, Theme::Token::Border);
        Renderer::draw_rect(box.x, box.y, box.w, box.h);
        return box;
    };

    if (m_mode == Mode::ActionMenu) {
        dim(150);
        SDL_Rect box = panel(360, 210);
        Theme::apply(r, Theme::Token::Accent);
        Renderer::fill_rect(box.x, box.y, box.w, 3);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           "Actions", Font::Size::Medium,
                           Font::Weight::Bold, Theme::Token::FgPrimary);
        // Move target label reflects current storage.
        bool on_sd = (m_group.app.storage == Core::Ncm::Storage::SdCard);
        const char* move_label = on_sd ? "Move to System memory"
                                       : "Move to SD card";
        const char* opts[3] = { "Dump to SD (NSP)", move_label, "Delete" };
        for (int i = 0; i < 3; ++i) {
            int oy = box.y + 52 + i * 34;
            if (i == m_action_cursor) {
                Theme::apply(r, Theme::Token::BgElevated);
                Renderer::fill_rect(box.x + 8, oy - 4, box.w - 16, 30);
                Theme::apply(r, Theme::Token::Accent);
                Renderer::fill_rect(box.x + 8, oy - 4, 3, 30);
            }
            Widgets::draw_text(box.x + Layout::PAD_LG, oy, opts[i],
                               Font::Size::Body, Font::Weight::Regular,
                               i == 2 ? Theme::Token::AccentDanger
                                      : Theme::Token::FgPrimary);
        }
        std::vector<Widgets::ButtonHint> h2 = {
            { "A", "Select" }, { "B", "Cancel" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
    else if (m_mode == Mode::Dumping) {
        dim(180);
        SDL_Rect box = panel(480, 170);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           "Dumping to NSP…", Font::Size::Medium,
                           Font::Weight::Bold, Theme::Token::FgPrimary);
        // Current file + NCA count.
        char sub[128];
        snprintf(sub, sizeof(sub), "%s   (%d / %d)",
                 m_dump.current_file.c_str(),
                 m_dump.ncas_done.load(), m_dump.ncas_total.load());
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 48, sub,
                           Font::Size::Tiny, Font::Weight::Regular,
                           Theme::Token::FgSecondary, box.w - Layout::PAD_LG*2);
        // Progress bar + byte counts.
        Widgets::draw_progress(box.x + Layout::PAD_LG, box.y + 78,
                               box.w - Layout::PAD_LG*2, 18, m_dump.fraction());
        char bytes[96];
        double done_gb = m_dump.bytes_done.load() / (1024.0*1024*1024);
        double tot_gb  = m_dump.bytes_total.load() / (1024.0*1024*1024);
        snprintf(bytes, sizeof(bytes), "%.2f / %.2f GB", done_gb, tot_gb);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 104, bytes,
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgSecondary);
        std::vector<Widgets::ButtonHint> h2 = { { "B", "Cancel" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
    else if (m_mode == Mode::DumpResult) {
        dim(180);
        SDL_Rect box = panel(640, 160);
        Theme::apply(r, m_result_ok ? Theme::Token::AccentOk
                                    : Theme::Token::AccentDanger);
        Renderer::fill_rect(box.x, box.y, box.w, 3);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           m_result_ok ? "Dump complete" : "Dump failed",
                           Font::Size::Medium, Font::Weight::Bold,
                           m_result_ok ? Theme::Token::AccentOk
                                       : Theme::Token::AccentDanger);
        // Full message at small size across the wide panel so nothing truncates.
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 52, m_result_msg,
                           Font::Size::Tiny, Font::Weight::Regular,
                           Theme::Token::FgPrimary, box.w - Layout::PAD_LG*2);
        if (m_result_ok) {
            Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 80,
                               "Saved to switch/GarageNX/dumps/",
                               Font::Size::Tiny, Font::Weight::Regular,
                               Theme::Token::FgDisabled, box.w - Layout::PAD_LG*2);
        }
        std::vector<Widgets::ButtonHint> h2 = { { "A", "OK" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
    else if (m_mode == Mode::Moving) {
        dim(180);
        SDL_Rect box = panel(480, 170);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           "Moving…", Font::Size::Medium,
                           Font::Weight::Bold, Theme::Token::FgPrimary);
        char sub[128];
        snprintf(sub, sizeof(sub), "%s   %s   (%d / %d)",
                 m_move.stage.c_str(), m_move.current_file.c_str(),
                 m_move.ncas_done.load(), m_move.ncas_total.load());
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 48, sub,
                           Font::Size::Tiny, Font::Weight::Regular,
                           Theme::Token::FgSecondary, box.w - Layout::PAD_LG*2);
        Widgets::draw_progress(box.x + Layout::PAD_LG, box.y + 78,
                               box.w - Layout::PAD_LG*2, 18, m_move.fraction());
        char bytes[96];
        double d = m_move.bytes_done.load() / (1024.0*1024*1024);
        double t = m_move.bytes_total.load() / (1024.0*1024*1024);
        snprintf(bytes, sizeof(bytes), "%.2f / %.2f GB", d, t);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 104, bytes,
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgSecondary);
        std::vector<Widgets::ButtonHint> h2 = { { "B", "Cancel" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
    else if (m_mode == Mode::MoveResult) {
        dim(180);
        SDL_Rect box = panel(460, 150);
        Theme::apply(r, m_result_ok ? Theme::Token::AccentOk
                                    : Theme::Token::AccentDanger);
        Renderer::fill_rect(box.x, box.y, box.w, 3);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           m_result_ok ? "Move complete" : "Move failed",
                           Font::Size::Medium, Font::Weight::Bold,
                           m_result_ok ? Theme::Token::AccentOk
                                       : Theme::Token::AccentDanger);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 52, m_result_msg,
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgSecondary, box.w - Layout::PAD_LG*2);
        if (!m_result_ok) {
            Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 78,
                               "Source left intact.",
                               Font::Size::Tiny, Font::Weight::Regular,
                               Theme::Token::FgDisabled);
        }
        std::vector<Widgets::ButtonHint> h2 = { { "A", "OK" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
    else if (m_mode == Mode::ConfirmDelete) {
        dim(180);
        SDL_Rect box = panel(440, 200);
        Theme::apply(r, Theme::Token::AccentDanger);
        Renderer::fill_rect(box.x, box.y, box.w, 3);

        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           "Hold A to delete", Font::Size::Medium,
                           Font::Weight::Bold, Theme::Token::AccentDanger);
        // Show the resolved name so the user sees exactly what's being removed.
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 50, m_name,
                           Font::Size::Body, Font::Weight::Bold,
                           Theme::Token::FgPrimary, box.w - Layout::PAD_LG * 2);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 78,
                           "This cannot be undone.", Font::Size::Small,
                           Font::Weight::Regular, Theme::Token::FgSecondary);

        // Hold progress bar.
        int bx = box.x + Layout::PAD_LG;
        int bw = box.w - Layout::PAD_LG * 2;
        Widgets::draw_progress(bx, box.y + 120, bw, 18, m_hold_progress);

        std::vector<Widgets::ButtonHint> h2 = { { "B", "Cancel" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
    else if (m_mode == Mode::Deleting) {
        dim(180);
        SDL_Rect box = panel(360, 100);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_LG,
                           "Deleting…", Font::Size::Medium, Font::Weight::Bold,
                           Theme::Token::FgPrimary);
    }
    else if (m_mode == Mode::Result) {
        dim(180);
        SDL_Rect box = panel(400, 140);
        Theme::apply(r, m_result_ok ? Theme::Token::AccentOk
                                    : Theme::Token::AccentDanger);
        Renderer::fill_rect(box.x, box.y, box.w, 3);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + Layout::PAD_MD,
                           m_result_ok ? "Deleted" : "Delete failed",
                           Font::Size::Medium, Font::Weight::Bold,
                           m_result_ok ? Theme::Token::AccentOk
                                       : Theme::Token::AccentDanger);
        Widgets::draw_text(box.x + Layout::PAD_LG, box.y + 52, m_result_msg,
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgSecondary, box.w - Layout::PAD_LG * 2);
        std::vector<Widgets::ButtonHint> h2 = { { "A", "OK" } };
        Widgets::draw_button_legend(box.x, box.y + box.h - 28, box.w, h2);
    }
}
