#pragma once
// source/screens/mtp_screen.hpp
// Status screen for the MTP service. Unlike FTP/HTTP there is no address to
// show and nothing to scan — the host is whatever is on the other end of the
// USB cable — so this reports connection and session state instead.

#include "screens/screen.hpp"
#include "core/sleep_inhibit.hpp"
#include "services/mtp_server.hpp"
#include "services/rate_meter.hpp"
#include <memory>
#include <string>
#include <cstdint>   // uint32_t (latch timestamp)

class MTPScreen : public Screen {
public:
    MTPScreen() = default;

    void on_enter() override;
    void on_exit() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    std::string hint_string() const override;

private:
    // Held for this screen's whole lifetime: while a Connectivity page is
    // open the console must not auto-sleep, or an in-flight transfer is cut
    // off and the client disconnects. RAII, so leaving the page always
    // restores normal sleep behaviour.
    Core::SleepInhibit::Guard m_stay_awake;

    void start_server();

    // The stats are sampled every frame (so the rate math stays accurate) but the
    // *displayed* figures are latched once per second. Refreshing text every frame
    // makes the numbers strobe unreadably and needlessly churns the text cache;
    // latching keeps them legible without throttling the render loop itself
    // (throttling the loop would starve input — the whole point of the render fix).
    void refresh_latched_stats();

    std::unique_ptr<Services::MtpServer> m_server;
    Services::RateMeter m_rate;   // sampled each frame from the server's byte counters

    // Latched display strings, rebuilt at ~1Hz by refresh_latched_stats().
    uint32_t    m_last_latch_ms = 0;
    std::string m_disp_sent  = "0 B";
    std::string m_disp_recv  = "0 B";
    std::string m_disp_cur   = "—";     // current speed
    std::string m_disp_avg   = "—";     // average speed (data phase)
    std::string m_disp_eta   = "—";     // ETA, "—" until wire size known
};
