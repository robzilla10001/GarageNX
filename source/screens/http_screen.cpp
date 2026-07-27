// source/screens/http_screen.cpp

#include "screens/http_screen.hpp"
#include "ui/stats_format.hpp"
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

    if (m_server && m_server->is_running()) {
        // Feed the meter the WIRE bytes of the active install, NOT
        // bytes_sent+bytes_recv. Those totals now include the web UI's own
        // traffic — the HTML page on every load and a JSON listing on every
        // navigation and status poll — so using them would report page chatter as
        // install throughput. Before B2 that distinction barely mattered; with a
        // browser client polling every 5s it would be actively misleading.
        // When no install is running current_wire_recv() is 0 and the meter idles.
        m_rate.sample(m_server->current_wire_recv());
        const uint32_t now = SDL_GetTicks();
        if (now - m_last_latch_ms >= 1000) {
            m_last_latch_ms = now;
            refresh_latched_stats();
        }
    }

    // X toggles the server.
    if (Input::pressed(Input::Button::X)) {
        if (m_server && m_server->is_running()) { m_server->stop(); m_rate.reset(); }
        else { m_rate.reset(); start_server(); }
        m_disp_sent = m_disp_recv = "0 B";
        m_disp_cur = m_disp_avg = m_disp_eta = "\u2014";
        m_last_latch_ms = 0;
    }
    return nullptr;
}

void HTTPScreen::refresh_latched_stats() {
    if (!m_server) return;
    m_disp_sent = Fs::format_size(m_server->bytes_sent());
    m_disp_recv = Fs::format_size(m_server->bytes_recv());

    const double cur = m_rate.bytes_per_sec();
    const double avg = m_rate.average_bytes_per_sec();
    m_disp_cur = cur > 0 ? (Fs::format_size((uint64_t)cur) + "/s") : "\u2014";
    m_disp_avg = (m_rate.data_phase_started() && avg > 0)
                     ? (Fs::format_size((uint64_t)avg) + "/s") : "\u2014";

    // ETA against WIRE bytes. Content-Length gives HTTP an exact size from the
    // first byte, so unlike FTP this is meaningful immediately rather than only
    // once the container table has been read.
    const uint64_t wire_size = m_server->current_wire_size();
    const uint64_t wire_recv = m_server->current_wire_recv();
    if (wire_size > 0 && wire_recv <= wire_size && cur > 1.0)
        m_disp_eta = UI::format_eta((double)(wire_size - wire_recv) / cur);
    else
        m_disp_eta = "\u2014";
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

            std::snprintf(f, sizeof(f), "\u2191 %s", m_disp_sent.c_str());
            Widgets::draw_text(cx + 1 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "\u2193 %s", m_disp_recv.c_str());
            Widgets::draw_text(cx + 2 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            // Same stat set and the same lang keys as MTP and FTP — one column
            // layout across all three transports, so the pages read identically.
            std::snprintf(f, sizeof(f), "%s: %s",
                          Lang::t("mtp.speed_now").c_str(), m_disp_cur.c_str());
            Widgets::draw_text(cx + 3 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "%s: %s",
                          Lang::t("mtp.speed_avg").c_str(), m_disp_avg.c_str());
            Widgets::draw_text(cx + 4 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "%s: %s",
                          Lang::t("mtp.eta").c_str(), m_disp_eta.c_str());
            Widgets::draw_text(cx + 5 * kColW, y, f,
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
