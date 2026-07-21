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

    // Smoothed (instantaneous) rate in bytes per second. Reads 0 when idle.
    double bytes_per_sec() const { return m_rate; }

    // Average rate over the active data phase: total bytes moved since the first
    // non-zero sample, divided by wall time since that first byte. Excludes the
    // idle period before the transfer began, so it answers "how fast has this
    // transfer actually been" rather than being dragged down by pre-transfer
    // waiting. Reads 0 until the data phase has started.
    double average_bytes_per_sec() const;

    // True once at least one byte has moved (the data phase has begun). Until
    // then the average and any ETA built on it are meaningless.
    bool data_phase_started() const { return m_avg_started; }

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

    // Data-phase average: anchored at the first byte moved, not at reset(), so
    // pre-transfer idle is excluded.
    bool              m_avg_started    = false;
    uint64_t          m_avg_base_bytes = 0;     // byte total at first-byte anchor
    Clock::time_point m_avg_base_t{};           // wall time at first-byte anchor
    uint64_t          m_avg_last_bytes = 0;     // most recent total (for the numerator)
    Clock::time_point m_avg_last_t{};           // most recent sample time

    // Every-sample byte total, used for reset detection and first-byte anchoring.
    // Distinct from m_last_bytes, which is the instantaneous-rate window baseline
    // and only advances once per kWindowMs — too coarse for these decisions.
    bool              m_prev_primed = false;
    uint64_t          m_prev_bytes  = 0;
};

} // namespace Services
