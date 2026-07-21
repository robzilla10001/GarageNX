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

namespace {
// Format a duration in seconds as a compact ETA: "12s", "3m 05s", "1h 02m".
// Caps at a sane ceiling so a near-zero rate does not print an absurd number.
std::string format_eta(double seconds) {
    if (seconds < 0 || seconds > 359999) return "—";   // >~100h => not meaningful
    const int total = (int)(seconds + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    char buf[32];
    if (h > 0)      std::snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    else            std::snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}
} // namespace

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

    if (m_server && m_server->is_running()) {
        // Feed the meter the WIRE bytes of the actual file transfer, not
        // bytes_sent()+bytes_recv() — those include every MTP protocol exchange
        // (OpenSession, SendObjectInfo, responses), so sampling them makes the
        // data-phase average anchor the moment the host connects rather than when
        // a transfer begins. current_wire_recv() counts only file payload.
        m_rate.sample(m_server->current_wire_recv());
        // ...but only rebuild the on-screen strings once per second.
        const uint32_t now = SDL_GetTicks();
        if (now - m_last_latch_ms >= 1000) {
            m_last_latch_ms = now;
            refresh_latched_stats();
        }
    }

    if (Input::pressed(Input::Button::X)) {
        if (m_server && m_server->is_running()) { m_server->stop(); m_rate.reset(); }
        else { m_rate.reset(); start_server(); }
        // Clear the latched display so stale figures do not linger across a
        // stop/start.
        m_disp_sent = m_disp_recv = "0 B";
        m_disp_cur = m_disp_avg = m_disp_eta = "—";
        m_last_latch_ms = 0;
    }
    return nullptr;
}

void MTPScreen::refresh_latched_stats() {
    if (!m_server) return;

    m_disp_sent = Fs::format_size(m_server->bytes_sent());
    m_disp_recv = Fs::format_size(m_server->bytes_recv());

    const double cur = m_rate.bytes_per_sec();
    const double avg = m_rate.average_bytes_per_sec();
    m_disp_cur = cur > 0 ? (Fs::format_size((uint64_t)cur) + "/s") : "—";
    m_disp_avg = (m_rate.data_phase_started() && avg > 0)
                     ? (Fs::format_size((uint64_t)avg) + "/s") : "—";

    // ETA against WIRE bytes (compressed size crossing USB), not installed bytes.
    // "—" until the host has declared a size (SendObjectInfo) and we have a rate.
    const uint64_t wire_size = m_server->current_wire_size();
    const uint64_t wire_recv = m_server->current_wire_recv();
    if (wire_size > 0 && wire_recv <= wire_size && cur > 1.0) {
        const double remaining = (double)(wire_size - wire_recv);
        m_disp_eta = format_eta(remaining / cur);
    } else {
        m_disp_eta = "—";
    }
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

        // Five latched columns: bytes up, bytes down, current speed, average
        // speed, ETA. All rebuilt at 1Hz in refresh_latched_stats(), so the text
        // is legible and the text cache is not churned every frame.
        {
            const int kColW = 180;
            char f[64];

            std::snprintf(f, sizeof(f), "\u2191 %s", m_disp_sent.c_str());
            Widgets::draw_text(cx + 0 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "\u2193 %s", m_disp_recv.c_str());
            Widgets::draw_text(cx + 1 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "%s: %s",
                          Lang::t("mtp.speed_now").c_str(), m_disp_cur.c_str());
            Widgets::draw_text(cx + 2 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "%s: %s",
                          Lang::t("mtp.speed_avg").c_str(), m_disp_avg.c_str());
            Widgets::draw_text(cx + 3 * kColW, y, f,
                               Font::Size::Body, Font::Weight::Regular, Theme::Token::FgSecondary);

            std::snprintf(f, sizeof(f), "%s: %s",
                          Lang::t("mtp.eta").c_str(), m_disp_eta.c_str());
            Widgets::draw_text(cx + 4 * kColW, y, f,
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
