// source/screens/ftp_screen.cpp

#include "screens/ftp_screen.hpp"
#include "core/net.hpp"
#include "core/fs.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"

#include <cstdio>

void FTPScreen::start_server() {
    const auto& ftp = Config::get().ftp;
    m_port = ftp.server_port ? ftp.server_port : 5000;
    m_anon = ftp.allow_anonymous;
    m_user = ftp.login_user;
    m_pass = ftp.login_pass;

    m_server = std::make_unique<Services::FtpServer>(
        m_port, m_anon, m_user, m_pass, "sdmc:/");
    m_server->start();
}

void FTPScreen::on_enter() {
    m_ip = Core::Net::current_ip();
    start_server();
}

void FTPScreen::on_exit() {
    m_server.reset();   // destructor stops + joins the worker thread
}

std::unique_ptr<Screen> FTPScreen::update(bool& pop) {
    pop = false;

    // Refresh the IP each frame (it can appear after Wi-Fi associates).
    m_ip = Core::Net::current_ip();

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    // Feed the rate meter from the server's cumulative counters. The service
    // thread publishes bytes only; turning that into a speed is the UI's job.
    if (m_server && m_server->is_running())
        m_rate.sample(m_server->bytes_sent() + m_server->bytes_recv());

    // X toggles the server.
    if (Input::pressed(Input::Button::X)) {
        if (m_server && m_server->is_running()) { m_server->stop(); m_rate.reset(); }
        else { m_rate.reset(); start_server(); }
    }
    return nullptr;
}

void FTPScreen::draw() {
    const int cx = 60;
    int y = Layout::CONTENT_Y + 40;

    Widgets::draw_text(cx, y, Lang::t("ftp.title"),
                       Font::Size::Large, Font::Weight::Bold, Theme::Token::FgPrimary);
    y += 46;

    // Status line with a colored state word.
    const Services::Status st = m_server ? m_server->status() : Services::Status::Stopped;
    Theme::Token stc = Theme::Token::FgSecondary;
    if (st == Services::Status::Running)      stc = Theme::Token::AccentOk;
    else if (st == Services::Status::Error)   stc = Theme::Token::AccentDanger;

    int w = Widgets::draw_text(cx, y, Lang::t("ftp.status"),
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
    Widgets::draw_text(cx + w + 8, y, Services::status_str(st),
                       Font::Size::Body, Font::Weight::Bold, stc);
    y += 40;

    const bool running = (st == Services::Status::Running);

    if (running) {
        // Address to connect to.
        const std::string url = Core::Net::link_url("ftp", m_ip, m_port);
        Widgets::draw_text(cx, y, Lang::t("ftp.address"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        Widgets::draw_text(cx + 160, y, url,
                           Font::Size::Medium, Font::Weight::Bold, Theme::Token::AccentOk);
        y += 34;

        // Login.
        Widgets::draw_text(cx, y, Lang::t("ftp.login"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        char cred[128];
        if (m_anon) std::snprintf(cred, sizeof(cred), "anonymous  (%s)", Lang::t("ftp.anon_ok").c_str());
        else        std::snprintf(cred, sizeof(cred), "%s / %s", m_user.c_str(), m_pass.c_str());
        Widgets::draw_text(cx + 160, y, cred,
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgPrimary);
        y += 34;

        // Live counters + current transfer speed.
        //
        // Each field is drawn at its own fixed column rather than as one
        // concatenated string: the values change width constantly (1.2 MB ->
        // 12.5 MB, 0 B/s -> 4.5 MB/s), and a single string would let every
        // growth shove the fields to its right around. Fixed columns keep them
        // still.
        {
            const int kColW = 180;
            char f[64];

            std::snprintf(f, sizeof(f), "%s: %d",
                          Lang::t("ftp.clients").c_str(), m_server->client_count());
            Widgets::draw_text(cx + 0 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "\u2191 %s",
                          Fs::format_size(m_server->bytes_sent()).c_str());
            Widgets::draw_text(cx + 1 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "\u2193 %s",
                          Fs::format_size(m_server->bytes_recv()).c_str());
            Widgets::draw_text(cx + 2 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "%s/s",
                          Fs::format_size((uint64_t)m_rate.bytes_per_sec()).c_str());
            Widgets::draw_text(cx + 3 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        }
        y += 44;

        // Scannable address. Encoding is cheap but not free, and the URL only
        // changes when the IP or port does, so cache it rather than re-encoding
        // every frame.
        if (url != m_qr_url) { m_qr = Core::Qr::encode(url); m_qr_url = url; }

        const int qs = 252;   // ~50% larger; v2 code = 33 modules incl. quiet zone -> 7px/module
        if (m_qr.ok()) {
            Widgets::draw_qr(cx, y, qs, m_qr);
        } else {
            // Fall back to the plain URL rather than showing a broken panel.
            Theme::apply(Renderer::get(), Theme::Token::BgSurface);
            Renderer::fill_rect(cx, y, qs, qs);
            Theme::apply(Renderer::get(), Theme::Token::Border);
            Renderer::draw_rect(cx, y, qs, qs);
        }
        Widgets::draw_text(cx + qs + 24, y + qs / 2 - 8, url,
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgPrimary);

        // AGPLv3 section 13: users interacting with this service over the
        // network must be offered its corresponding source.
        Widgets::draw_text(cx, y + qs + 16, std::string("Source: ") + APP_SOURCE_URL,
                           Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
    } else if (st == Services::Status::Error) {
        Widgets::draw_text(cx, y, m_server ? m_server->last_error() : std::string("error"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::AccentDanger);
    } else {
        Widgets::draw_text(cx, y, Lang::t("ftp.stopped_hint"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
    }
}

std::string FTPScreen::hint_string() const {
    const bool running = m_server && m_server->is_running();
    return running ? Lang::t("ftp.hint_running") : Lang::t("ftp.hint_stopped");
}
