#pragma once
// source/core/net.hpp
// Small network-identity helpers shared by the M6 services (FTP/HTTP/MTP) and
// their status screens. Requires socketInitialize() to have been called at
// startup (main.cpp). The IP is used for the on-screen address and the QR
// payload; nothing here opens listening sockets — that's the services' job.

#include <cstdint>
#include <string>

namespace Core::Net {

// Current LAN IPv4 as "a.b.c.d". Returns "0.0.0.0" when there is no network.
std::string current_ip();

// Best-effort device hostname (falls back to "GarageNX").
std::string hostname();

// Build a display/QR URL, e.g. link_url("ftp", ip, 5000) -> "ftp://192.168.1.5:5000/".
std::string link_url(const std::string& scheme, const std::string& ip, uint16_t port);

} // namespace Core::Net
