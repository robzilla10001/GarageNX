// tests/rate_meter_test.cpp
//
// Tests the RateMeter data-phase average that backs the MTP screen's average
// speed and ETA. RateMeter reads steady_clock directly, so rather than refactor
// a working class to inject a clock, this drives it with real (short) sleeps and
// asserts the computed rate lands in a tolerance band. The point is to catch the
// logic errors that matter — the average anchoring at the first byte (not at
// reset), and re-anchoring after a counter reset — not to measure the clock.

#include "services/rate_meter.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Services;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ── Average is zero until the first byte moves ──────────────────────────────
static void test_average_zero_before_data() {
    RateMeter m;
    m.sample(0);
    sleep_ms(30);
    m.sample(0);            // still no bytes moved
    CHECK(!m.data_phase_started(), "data phase not started with no bytes");
    CHECK(m.average_bytes_per_sec() == 0.0, "average is zero before any data");
    std::printf("  ok: average is zero and phase not started before first byte\n");
}

// ── Pre-transfer idle is excluded from the average ──────────────────────────
static void test_idle_before_data_excluded() {
    RateMeter m;
    m.sample(0);
    sleep_ms(200);         // 200ms of idle waiting — must NOT drag the average
    m.sample(0);
    // Transfer runs: bytes climb over ~200ms. The screen samples every frame, so
    // model several samples across the data phase. The average anchors at the
    // first motion and measures forward from there, excluding the idle above.
    m.sample(0);
    sleep_ms(50);  m.sample(500000);    // first motion -> anchor here
    sleep_ms(50);  m.sample(1000000);
    sleep_ms(50);  m.sample(1500000);
    sleep_ms(50);  m.sample(2000000);
    CHECK(m.data_phase_started(), "phase started once bytes moved");
    const double avg = m.average_bytes_per_sec();
    // From the anchor (500000 @ t0) to the last sample (2000000 @ ~150ms):
    // 1,500,000 bytes / ~0.15s ~= 10 MB/s. If the 200ms idle were counted the
    // figure would be far lower, so the band proves the exclusion.
    CHECK(avg > 6.0e6 && avg < 16.0e6, "average reflects data phase, not idle wait");
    std::printf("  ok: pre-transfer idle excluded from the average (%.1f MB/s)\n",
                avg / 1e6);
}

// ── A counter reset (server restart) re-anchors the average ─────────────────
static void test_counter_reset_reanchors() {
    RateMeter m;
    m.sample(0);
    sleep_ms(50);
    m.sample(500000);
    CHECK(m.data_phase_started(), "phase started after first transfer");

    // Server restarts: counter drops back to 0.
    m.sample(0);
    CHECK(!m.data_phase_started(), "counter reset drops the data phase");

    // New transfer begins; average must measure only the new phase.
    sleep_ms(50);
    m.sample(0);
    sleep_ms(50);  m.sample(1000000);   // first motion post-reset -> re-anchor
    sleep_ms(50);  m.sample(2000000);
    sleep_ms(50);  m.sample(3000000);
    CHECK(m.data_phase_started(), "phase restarts on new data");
    const double avg = m.average_bytes_per_sec();
    // Anchor 1,000,000 @ t0 to 3,000,000 @ ~100ms => 2,000,000 / 0.1s ~= 20 MB/s.
    CHECK(avg > 12.0e6 && avg < 30.0e6, "average reflects only the post-reset phase");
    std::printf("  ok: counter reset re-anchors the average (%.1f MB/s)\n",
                avg / 1e6);
}

// ── reset() clears everything ───────────────────────────────────────────────
static void test_reset_clears() {
    RateMeter m;
    m.sample(0);
    sleep_ms(30);
    m.sample(100000);
    m.reset();
    CHECK(!m.data_phase_started(), "reset clears the data phase");
    CHECK(m.average_bytes_per_sec() == 0.0, "reset zeroes the average");
    CHECK(m.bytes_per_sec() == 0.0, "reset zeroes the instantaneous rate");
    std::printf("  ok: reset clears all rate state\n");
}

int main() {
    std::printf("RateMeter data-phase average (MTP stats rework)\n");
    test_average_zero_before_data();
    test_idle_before_data_excluded();
    test_counter_reset_reanchors();
    test_reset_clears();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
