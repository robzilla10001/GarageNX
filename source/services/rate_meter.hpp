#pragma once
// source/services/rate_meter.hpp
// Smoothed transfer-rate meter shared by the service screens (FTP / HTTP / MTP).
//
// The service threads only maintain monotonically-increasing byte counters; they
// deliberately know nothing about time. A screen owns a RateMeter and feeds it
// the current cumulative total once per frame, and the meter turns those deltas
// into a stable bytes/sec figure. Keeping the meter on the UI side means no
// service needs a timer and no extra cross-thread state is introduced — the
// atomics the services already publish are the only contract.

#include <chrono>
#include <cstdint>

namespace Services {

class RateMeter {
public:
    // Feed the current cumulative byte total (sent + received, or either alone).
    // Safe to call every frame; the rate only recomputes once per window.
    void sample(uint64_t total_bytes);

    // Smoothed rate in bytes per second. Reads 0 when idle.
    double bytes_per_sec() const { return m_rate; }

    // Forget history (e.g. when the server is stopped and restarted).
    void reset();

private:
    using Clock = std::chrono::steady_clock;

    static constexpr int    kWindowMs      = 250;   // recompute interval
    static constexpr double kAlpha         = 0.35;  // EMA smoothing factor
    static constexpr int    kIdleToZero    = 4;     // idle windows (~1s) before snapping to 0

    bool              m_primed    = false;  // have a baseline sample
    bool              m_have_rate = false;  // have computed at least one rate
    int               m_idle      = 0;      // consecutive windows with no traffic
    uint64_t          m_last_bytes = 0;
    Clock::time_point m_last_t{};
    double            m_rate = 0.0;
};

} // namespace Services
