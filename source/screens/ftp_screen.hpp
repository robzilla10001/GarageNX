#pragma once
// source/screens/ftp_screen.hpp
// Status screen for the FTP service: shows the address to connect to, the login,
// live transfer counters, and a Start/Stop toggle. Owns the FtpServer and ties
// its lifetime to the screen (started on enter, stopped on exit).

#include "screens/screen.hpp"
#include "services/ftp_server.hpp"
#include "services/rate_meter.hpp"
#include "core/qr.hpp"
#include <memory>
#include <string>

class FTPScreen : public Screen {
public:
    FTPScreen() = default;

    void on_enter() override;
    void on_exit() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    std::string hint_string() const override;

private:
    void start_server();

    // Stats sampled every frame (accurate rate math), displayed values latched at
    // 1 Hz — same approach as the MTP screen: keeps the numbers legible and avoids
    // churning the text cache 60×/s, without throttling the render loop.
    void refresh_latched_stats();

    std::unique_ptr<Services::FtpServer> m_server;
    std::string m_ip = "0.0.0.0";
    uint16_t    m_port = 5000;
    bool        m_anon = true;
    std::string m_user;
    std::string m_pass;
    Services::RateMeter m_rate;   // sampled each frame from the server's byte counters
    Core::Qr::Code m_qr;          // cached; re-encoded only when the URL changes
    std::string    m_qr_url;      // URL m_qr was built from

    // Latched display strings, rebuilt at ~1Hz by refresh_latched_stats().
    uint32_t    m_last_latch_ms = 0;
    std::string m_disp_sent = "0 B";
    std::string m_disp_recv = "0 B";
    std::string m_disp_cur  = "—";   // current speed
    std::string m_disp_avg  = "—";   // average speed (install data phase)
    std::string m_disp_eta  = "—";   // ETA, "—" until wire size known
};
