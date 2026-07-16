// source/screens/title_test.cpp

#include "screens/title_test.hpp"
#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "core/fs.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <algorithm>

TitleTestScreen::TitleTestScreen() {}

void TitleTestScreen::on_enter() {
    run();
}

void TitleTestScreen::run() {
    // 1. Load keys. Block if unavailable (per design decision).
    auto kr = Core::Keys::load();
    m_keys_ok = kr.ok;
    m_keys_msg = Core::Keys::requirement_message();

    // 2. Enumerate titles regardless (enumeration doesn't need keys).
    bool ok = false;
    m_titles = Core::Ncm::list_all(&ok);

    m_app_count = m_patch_count = m_dlc_count = 0;
    for (auto& t : m_titles) {
        switch (t.type) {
            case Core::Ncm::TitleType::Application:  m_app_count++;   break;
            case Core::Ncm::TitleType::Patch:        m_patch_count++; break;
            case Core::Ncm::TitleType::AddOnContent: m_dlc_count++;   break;
            default: break;
        }
    }

    // 3. If keys are available, run the decrypt test on the first real USER
    //    game — not just the first Application-typed entry. System titles
    //    (qlaunch, sysmodules, etc.) are also typed "Application" by NCM but
    //    live in the low ID range and use the system key-area category, which
    //    our application-key decryptor doesn't handle. User games start at
    //    0x0100000000100000 and up.
    static constexpr uint64_t USER_GAME_ID_BASE = 0x0100000000100000ULL;
    if (m_keys_ok) {
        const Core::Ncm::Title* first_game = nullptr;
        for (auto& t : m_titles) {
            if (t.type == Core::Ncm::TitleType::Application &&
                t.program_id >= USER_GAME_ID_BASE) {
                first_game = &t;
                break;
            }
        }

        if (!first_game) {
            m_test_status = "No user games installed to test "
                            "(only system titles found).";
        } else {
            char idbuf[32];
            snprintf(idbuf, sizeof(idbuf), "%016llX",
                     (unsigned long long)first_game->program_id);

            auto cd = Core::Ncm::resolve_control(*first_game, Core::Keys::get(),
                                                 false /*no icon for the test*/);
            m_test_ran = true;
            if (cd.ok) {
                m_test_status = std::string("DECRYPT OK for ") + idbuf;
                m_test_name    = cd.name;
                m_test_version = cd.version;
            } else {
                m_test_status = std::string("DECRYPT FAILED for ") + idbuf
                              + "  [" + (cd.fail_reason.empty()
                                        ? "unknown" : cd.fail_reason) + "]";
            }
        }
    } else {
        m_test_status = "Keys unavailable — decrypt test skipped.";
    }
}

std::unique_ptr<Screen> TitleTestScreen::update(bool& pop) {
    pop = false;
    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    int visible = (Layout::CONTENT_H - 40) / 26;
    if (Input::repeat(Input::Button::DDown) &&
        m_scroll < (int)m_titles.size() - visible) m_scroll++;
    if (Input::repeat(Input::Button::DUp) && m_scroll > 0) m_scroll--;

    // A = re-run the test.
    if (Input::pressed(Input::Button::A)) run();
    return nullptr;
}

void TitleTestScreen::draw() {
    const int x = 0, y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W, h = Layout::CONTENT_H;

    SDL_Renderer* r = Renderer::get();
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    int cy = y + Layout::PAD_MD;
    const int lx = x + Layout::PAD_LG;

    // ── Keys status ────────────────────────────────────────────────────────────
    Widgets::draw_text(lx, cy, "Milestone 4 — Phase A validation",
                       Font::Size::Medium, Font::Weight::Bold,
                       Theme::Token::Accent);
    cy += 34;

    Widgets::draw_text(lx, cy,
                       std::string("Keys: ") + (m_keys_ok ? "LOADED" : "MISSING"),
                       Font::Size::Body, Font::Weight::Bold,
                       m_keys_ok ? Theme::Token::AccentOk : Theme::Token::AccentDanger);
    cy += 30;

    if (!m_keys_ok) {
        // Show the blocking "keys required" message (multi-line).
        std::string msg = m_keys_msg;
        size_t start = 0;
        while (start < msg.size()) {
            size_t nl = msg.find('\n', start);
            std::string line = msg.substr(start, nl == std::string::npos
                                          ? std::string::npos : nl - start);
            Widgets::draw_text(lx, cy, line, Font::Size::Small,
                               Font::Weight::Regular, Theme::Token::FgSecondary,
                               w - lx - Layout::PAD_LG);
            cy += 24;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        cy += 10;
    }

    // ── Enumeration summary ────────────────────────────────────────────────────
    char summary[128];
    snprintf(summary, sizeof(summary),
             "Titles: %d total  (apps %d, updates %d, dlc %d)",
             (int)m_titles.size(), m_app_count, m_patch_count, m_dlc_count);
    Widgets::draw_text(lx, cy, summary, Font::Size::Body, Font::Weight::Regular,
                       Theme::Token::FgPrimary);
    cy += 32;

    // ── Decrypt test result ────────────────────────────────────────────────────
    Widgets::draw_text(lx, cy, m_test_status, Font::Size::Small,
                       Font::Weight::Bold,
                       m_test_ran && !m_test_name.empty()
                         ? Theme::Token::AccentOk : Theme::Token::FgSecondary,
                       w - lx - Layout::PAD_LG);
    cy += 26;

    if (!m_test_name.empty()) {
        Widgets::draw_text(lx, cy, std::string("  Name:    ") + m_test_name,
                           Font::Size::Body, Font::Weight::Regular,
                           Theme::Token::FgPrimary, w - lx - Layout::PAD_LG);
        cy += 26;
        Widgets::draw_text(lx, cy, std::string("  Version: ") + m_test_version,
                           Font::Size::Small, Font::Weight::Regular,
                           Theme::Token::FgSecondary);
        cy += 26;
    }

    cy += 8;
    Theme::apply(r, Theme::Token::Border);
    Renderer::hline(lx, cy, w - lx - Layout::PAD_LG);
    cy += 10;

    // ── Title list (id / type / storage) ───────────────────────────────────────
    int visible = (y + h - 40 - cy) / 24;
    for (int i = m_scroll; i < (int)m_titles.size() && i < m_scroll + visible; ++i) {
        const auto& t = m_titles[i];
        const char* type_str =
            t.type == Core::Ncm::TitleType::Application  ? "APP" :
            t.type == Core::Ncm::TitleType::Patch        ? "UPD" :
            t.type == Core::Ncm::TitleType::AddOnContent ? "DLC" : "???";
        const char* store_str =
            t.storage == Core::Ncm::Storage::SdCard ? "SD" : "NAND";

        char line[128];
        snprintf(line, sizeof(line), "%016llX  %s  %-4s  %s",
                 (unsigned long long)t.meta_id, type_str, store_str,
                 Fs::format_size(t.size_bytes).c_str());
        Widgets::draw_text(lx, cy, line, Font::Size::Small,
                           Font::Weight::Regular, Theme::Token::FgSecondary);
        cy += 24;
    }

    // Legend
    std::vector<Widgets::ButtonHint> hints = {
        { "A", "Re-run test" },
        { "B", Lang::t("hints.back") },
    };
    Widgets::draw_button_legend(x, y + h - 30, w, hints);
}
