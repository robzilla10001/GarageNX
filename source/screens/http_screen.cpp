// source/screens/http_screen.cpp

#include "screens/http_screen.hpp"
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

void HTTPScreen::start_server() {
    const auto& http = Config::get().http;
    m_port         = http.server_port ? http.server_port : 8080;
    m_allow_upload = http.allow_upload;

    m_server = std::make_unique<Services::HttpServer>(
        m_port, m_allow_upload, "sdmc:/");
    m_server->start();
}

void HTTPScreen::on_enter() {
    m_ip = Core::Net::current_ip();
    start_server();
}

void HTTPScreen::on_exit() {
    m_server.reset();   // destructor stops + joins the worker thread
}

std::unique_ptr<Screen> HTTPScreen::update(bool& pop) {
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

void HTTPScreen::draw() {
    const int cx = 60;
    int y = Layout::CONTENT_Y + 40;

    Widgets::draw_text(cx, y, Lang::t("http.title"),
                       Font::Size::Large, Font::Weight::Bold, Theme::Token::FgPrimary);
    y += 46;

    // Status line with a colored state word.
    const Services::Status st = m_server ? m_server->status() : Services::Status::Stopped;
    Theme::Token stc = Theme::Token::FgSecondary;
    if (st == Services::Status::Running)      stc = Theme::Token::AccentOk;
    else if (st == Services::Status::Error)   stc = Theme::Token::AccentDanger;

    int w = Widgets::draw_text(cx, y, Lang::t("http.status"),
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
    Widgets::draw_text(cx + w + 8, y, Services::status_str(st),
                       Font::Size::Body, Font::Weight::Bold, stc);
    y += 40;

    const bool running = (st == Services::Status::Running);

    if (running) {
        // Address to connect to.
        const std::string url = Core::Net::link_url("http", m_ip, m_port);
        Widgets::draw_text(cx, y, Lang::t("http.address"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        Widgets::draw_text(cx + 160, y, url,
                           Font::Size::Medium, Font::Weight::Bold, Theme::Token::AccentOk);
        y += 34;

        // Live counters + current transfer speed.
        //
        // Fixed columns per field (see FTPScreen): the values change width as
        // they grow, and one concatenated string would make each field jitter
        // whenever the field to its left got wider.
        {
            const int kColW = 180;
            char f[64];

            std::snprintf(f, sizeof(f), "%s: %d",
                          Lang::t("http.requests").c_str(), m_server->request_count());
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
        Widgets::draw_text(cx, y, Lang::t("http.stopped_hint"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
    }
}

std::string HTTPScreen::hint_string() const {
    const bool running = m_server && m_server->is_running();
    return running ? Lang::t("http.hint_running") : Lang::t("http.hint_stopped");
}
