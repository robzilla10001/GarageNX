#pragma once
// source/core/ntp.hpp
// NTP time synchronization. Queries an NTP server and sets the system clock.
// Modifies system state (the RTC/user clock), so it's an explicit operation.

#include <string>
#include <cstdint>

namespace Core::Ntp {

struct Result {
    bool     success = false;
    int64_t  server_time = 0;    // unix seconds reported by the server
    int64_t  offset_seconds = 0; // server_time - local (before adjustment)
    std::string error;           // human-readable error on failure
};

// Query the given NTP server (default: pool.ntp.org) and, on success, set the
// system clock to match. Blocking — call on a worker or during startup.
// timeout_ms bounds the network wait so a dead server can't hang startup.
Result sync(const std::string& server = "pool.ntp.org", int timeout_ms = 3000);

// Query only: get the server time and offset without changing the clock.
Result query(const std::string& server = "pool.ntp.org", int timeout_ms = 3000);

} // namespace Core::Ntp
