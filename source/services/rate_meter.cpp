// source/services/rate_meter.cpp

#include "services/rate_meter.hpp"

namespace Services {

void RateMeter::reset() {
    m_primed     = false;
    m_have_rate  = false;
    m_idle       = 0;
    m_last_bytes = 0;
    m_rate       = 0.0;

    m_avg_started    = false;
    m_avg_base_bytes = 0;
    m_avg_last_bytes = 0;

    m_prev_primed = false;
    m_prev_bytes  = 0;
}

void RateMeter::sample(uint64_t total_bytes) {
    const auto now = Clock::now();

    // ── Data-phase tracking (every sample, independent of the rate window) ──
    // m_prev_bytes mirrors the true counter every call, so reset detection and
    // first-byte anchoring are precise regardless of the coarse rate window.
    if (!m_prev_primed) {
        m_prev_primed = true;
        m_prev_bytes  = total_bytes;
    } else if (total_bytes < m_prev_bytes) {
        // Counter went backwards: server restarted. Drop the average so it
        // re-anchors at the next real byte.
        m_avg_started = false;
        m_prev_bytes  = total_bytes;
    } else {
        if (!m_avg_started && total_bytes > m_prev_bytes) {
            // First byte of a transfer: anchor at now with the current total, so
            // the average measures strictly forward and excludes pre-transfer
            // idle. (m_last_t is windowed and would drag idle in.)
            m_avg_started    = true;
            m_avg_base_bytes = total_bytes;
            m_avg_base_t     = now;
            m_avg_last_bytes = total_bytes;
            m_avg_last_t     = now;
        } else if (m_avg_started) {
            m_avg_last_bytes = total_bytes;
            m_avg_last_t     = now;
        }
        m_prev_bytes = total_bytes;
    }

    // ── Instantaneous (smoothed) rate — windowed at kWindowMs ──────────────
    // First call just establishes the baseline; a rate needs two samples.
    if (!m_primed) {
        m_primed     = true;
        m_last_bytes = total_bytes;
        m_last_t     = now;
        return;
    }

    // A counter reset also rebaselines the instantaneous rate.
    if (total_bytes < m_last_bytes) {
        m_last_bytes = total_bytes;
        m_last_t     = now;
        m_have_rate  = false;
        m_idle       = 0;
        m_rate       = 0.0;
        return;
    }

    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_t).count();
    if (ms < kWindowMs) return;   // not enough elapsed time to be meaningful yet

    const uint64_t delta = total_bytes - m_last_bytes;
    const double instant = static_cast<double>(delta) * 1000.0 / static_cast<double>(ms);

    if (delta == 0) {
        // Decay while idle, but snap to a clean zero after ~1s of no traffic so a
        // finished transfer doesn't leave a phantom speed on screen.
        if (++m_idle >= kIdleToZero) {
            m_rate      = 0.0;
            m_have_rate = false;
        } else {
            m_rate = (1.0 - kAlpha) * m_rate;
        }
    } else {
        m_idle = 0;
        if (!m_have_rate) {
            m_rate      = instant;   // seed, so the first reading isn't damped from zero
            m_have_rate = true;
        } else {
            m_rate = kAlpha * instant + (1.0 - kAlpha) * m_rate;
        }
    }

    m_last_bytes = total_bytes;
    m_last_t     = now;
}

double RateMeter::average_bytes_per_sec() const {
    if (!m_avg_started) return 0.0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        m_avg_last_t - m_avg_base_t).count();
    if (ms <= 0) return 0.0;
    const uint64_t moved = m_avg_last_bytes - m_avg_base_bytes;
    return static_cast<double>(moved) * 1000.0 / static_cast<double>(ms);
}

} // namespace Services
