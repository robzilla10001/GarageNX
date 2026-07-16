#pragma once
// source/screens/mtp_screen.hpp
// Status screen for the MTP service. Unlike FTP/HTTP there is no address to
// show and nothing to scan — the host is whatever is on the other end of the
// USB cable — so this reports connection and session state instead.

#include "screens/screen.hpp"
#include "services/mtp_server.hpp"
#include "services/rate_meter.hpp"
#include <memory>
#include <string>

class MTPScreen : public Screen {
public:
    MTPScreen() = default;

    void on_enter() override;
    void on_exit() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    std::string hint_string() const override;

private:
    void start_server();

    std::unique_ptr<Services::MtpServer> m_server;
    Services::RateMeter m_rate;   // sampled each frame from the server's byte counters
};
