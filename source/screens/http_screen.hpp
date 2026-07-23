#pragma once
// source/screens/http_screen.hpp
// Status screen for the HTTP service: shows the address to connect to, live
// transfer counters, and a Start/Stop toggle. Owns the HttpServer and ties
// its lifetime to the screen (started on enter, stopped on exit).

#include "screens/screen.hpp"
#include "core/sleep_inhibit.hpp"
#include "services/http_server.hpp"
#include "services/rate_meter.hpp"
#include "core/qr.hpp"
#include <memory>
#include <string>

class HTTPScreen : public Screen {
public:
    HTTPScreen() = default;

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

    std::unique_ptr<Services::HttpServer> m_server;
    std::string m_ip = "0.0.0.0";
    uint16_t    m_port = 8080;
    bool        m_allow_upload = true;
    Services::RateMeter m_rate;   // sampled each frame from the server's byte counters
    Core::Qr::Code m_qr;          // cached; re-encoded only when the URL changes
    std::string    m_qr_url;      // URL m_qr was built from
};
