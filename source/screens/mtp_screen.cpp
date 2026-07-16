// source/screens/mtp_screen.cpp

#include "screens/mtp_screen.hpp"
#include "core/fs.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"

#include <algorithm>
#include <cstdio>

void MTPScreen::start_server() {
    m_server = std::make_unique<Services::MtpServer>();
    m_server->start();
}

void MTPScreen::on_enter() { start_server(); }

void MTPScreen::on_exit() {
    m_server.reset();   // destructor stops + joins the worker thread
}

std::unique_ptr<Screen> MTPScreen::update(bool& pop) {
    pop = false;

    if (Input::pressed(Input::Button::B)) { pop = true; return nullptr; }

    if (m_server && m_server->is_running())
        m_rate.sample(m_server->bytes_sent() + m_server->bytes_recv());

    if (Input::pressed(Input::Button::X)) {
        if (m_server && m_server->is_running()) { m_server->stop(); m_rate.reset(); }
        else { m_rate.reset(); start_server(); }
    }
    return nullptr;
}

void MTPScreen::draw() {
    const int cx = 60;
    int y = Layout::CONTENT_Y + 40;

    Widgets::draw_text(cx, y, Lang::t("mtp.title"),
                       Font::Size::Large, Font::Weight::Bold, Theme::Token::FgPrimary);
    y += 46;

    const Services::Status st = m_server ? m_server->status() : Services::Status::Stopped;
    Theme::Token stc = Theme::Token::FgSecondary;
    if (st == Services::Status::Running)    stc = Theme::Token::AccentOk;
    else if (st == Services::Status::Error) stc = Theme::Token::AccentDanger;

    int w = Widgets::draw_text(cx, y, Lang::t("mtp.status"),
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
    Widgets::draw_text(cx + w + 8, y, Services::status_str(st),
                       Font::Size::Body, Font::Weight::Bold, stc);
    y += 40;

    if (st == Services::Status::Running) {
        // USB connection state — the thing the user actually needs to know,
        // since there is no address to type anywhere.
        const bool connected = m_server->host_connected();
        w = Widgets::draw_text(cx, y, Lang::t("mtp.host"),
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        Widgets::draw_text(cx + 160, y,
                           Lang::t(connected ? "mtp.host_connected" : "mtp.host_waiting"),
                           Font::Size::Medium, Font::Weight::Bold,
                           connected ? Theme::Token::AccentOk : Theme::Token::FgSecondary);
        y += 34;

        w = Widgets::draw_text(cx, y, Lang::t("mtp.session"),
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        Widgets::draw_text(cx + 160, y,
                           Lang::t(m_server->session_open() ? "mtp.session_open" : "mtp.session_none"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgPrimary);
        y += 44;

        // Fixed columns per field, matching the FTP/HTTP screens.
        {
            const int kColW = 180;
            char f[64];
            std::snprintf(f, sizeof(f), "%s: %d",
                          Lang::t("mtp.requests").c_str(), m_server->request_count());
            Widgets::draw_text(cx + 0 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
            std::snprintf(f, sizeof(f), "\u2191 %s", Fs::format_size(m_server->bytes_sent()).c_str());
            Widgets::draw_text(cx + 1 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
            std::snprintf(f, sizeof(f), "\u2193 %s", Fs::format_size(m_server->bytes_recv()).c_str());
            Widgets::draw_text(cx + 2 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
            std::snprintf(f, sizeof(f), "%s/s", Fs::format_size((uint64_t)m_rate.bytes_per_sec()).c_str());
            Widgets::draw_text(cx + 3 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
        }
        y += 44;

        // Surface the installer. A stream install has no overlay of its own, so
        // without this a failure is just an opaque host-side error.
        const auto& ip = m_server->install_progress();
        const std::string msg = ip.message;
        if (m_server->installing() || !msg.empty()) {
            const auto lines = ip.log_snapshot();
            const int show = (int)std::min<size_t>(lines.size(), 4);
            for (int i = 0; i < show; i++) {
                const std::string& raw = lines[lines.size() - show + i];
                const bool err = raw.rfind("ERROR", 0) == 0;
                std::string ln = raw;
                if (ln.size() > 92) ln = ln.substr(0, 89) + "...";
                Widgets::draw_text(cx, y, ln, Font::Size::Small, Font::Weight::Regular,
                                   err ? Theme::Token::AccentDanger : Theme::Token::FgSecondary);
                y += 20;
            }
            if (!msg.empty()) {
                Widgets::draw_text(cx, y, msg, Font::Size::Small, Font::Weight::Bold,
                                   ip.success.load() ? Theme::Token::AccentOk
                                                     : Theme::Token::AccentDanger);
                y += 24;
            }
        } else {
            Widgets::draw_text(cx, y, Lang::t("mtp.hint_connect"),
                               Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
            y += 28;
        }

        // AGPLv3 section 13: users interacting with this service must be
        // offered its corresponding source.
        Widgets::draw_text(cx, y, std::string("Source: ") + APP_SOURCE_URL,
                           Font::Size::Small, Font::Weight::Regular, Theme::Token::FgSecondary);
    } else if (st == Services::Status::Error) {
        Widgets::draw_text(cx, y, m_server ? m_server->last_error() : std::string("error"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::AccentDanger);
    } else {
        Widgets::draw_text(cx, y, Lang::t("mtp.stopped_hint"),
                           Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);
    }
}

std::string MTPScreen::hint_string() const {
    const bool running = m_server && m_server->is_running();
    return running ? Lang::t("mtp.hint_running") : Lang::t("mtp.hint_stopped");
}
