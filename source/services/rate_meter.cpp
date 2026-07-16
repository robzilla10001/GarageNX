// source/services/rate_meter.cpp

#include "services/rate_meter.hpp"

namespace Services {

void RateMeter::reset() {
    m_primed     = false;
    m_have_rate  = false;
    m_idle       = 0;
    m_last_bytes = 0;
    m_rate       = 0.0;
}

void RateMeter::sample(uint64_t total_bytes) {
    const auto now = Clock::now();

    // First call just establishes the baseline; a rate needs two samples.
    if (!m_primed) {
        m_primed     = true;
        m_last_bytes = total_bytes;
        m_last_t     = now;
        return;
    }

    // The counters only ever increase while a server runs, but restarting the
    // server resets them to 0. Treat any decrease as a fresh baseline rather
    // than underflowing the delta.
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

} // namespace Services
