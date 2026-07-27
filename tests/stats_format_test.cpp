// tests/stats_format_test.cpp
//
// The shared ETA formatter used by the MTP, FTP and HTTP status screens.
//
// This was two byte-identical file-local copies before HTTP needed a third. It is
// pure, and its whole job is boundary behaviour — rollover at 60s and 3600s, and
// refusing to print numbers that are technically true but useless. Those are
// exactly the cases you cannot check by glancing at a console during a transfer.

#include "ui/stats_format.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static const char* kDash = "\u2014";

static void test_seconds() {
    CHECK(UI::format_eta(0) == "0s", "zero");
    CHECK(UI::format_eta(1) == "1s", "one second");
    CHECK(UI::format_eta(45) == "45s", "under a minute");
    CHECK(UI::format_eta(59) == "59s", "just under a minute");
    std::printf("  ok: seconds\n");
}

static void test_minute_rollover() {
    // The boundary: 60s must become "1m 00s", not "60s".
    CHECK(UI::format_eta(60) == "1m 00s", "exactly one minute");
    CHECK(UI::format_eta(61) == "1m 01s", "seconds zero-padded");
    CHECK(UI::format_eta(187) == "3m 07s", "padding holds above ten minutes' worth");
    CHECK(UI::format_eta(3599) == "59m 59s", "just under an hour");
    std::printf("  ok: minute rollover and padding\n");
}

static void test_hour_rollover() {
    CHECK(UI::format_eta(3600) == "1h 00m", "exactly one hour");
    CHECK(UI::format_eta(7500) == "2h 05m", "hours with padded minutes");
    // Above an hour, seconds are dropped deliberately — they are noise at that
    // scale and would make the column jitter every frame.
    CHECK(UI::format_eta(3659) == "1h 00m", "seconds not shown past an hour");
    std::printf("  ok: hour rollover\n");
}

static void test_rounding() {
    // Rounds to nearest rather than truncating, so a 0.6s estimate is not "0s".
    CHECK(UI::format_eta(0.6) == "1s", "rounds up");
    CHECK(UI::format_eta(0.4) == "0s", "rounds down");
    CHECK(UI::format_eta(59.7) == "1m 00s", "rounding can cross the minute boundary");
    std::printf("  ok: rounding\n");
}

// Anything not worth showing becomes an em dash. A negative value comes from
// arithmetic on a stalled transfer; a huge one means the rate is so near zero the
// number is noise. "2847h" would be true and useless.
static void test_refuses_meaningless_values() {
    CHECK(UI::format_eta(-1) == kDash, "negative");
    CHECK(UI::format_eta(-0.001) == kDash, "slightly negative");
    CHECK(UI::format_eta(360000) == kDash, "beyond the cap");
    CHECK(UI::format_eta(1e12) == kDash, "absurd");
    CHECK(UI::format_eta(359999) != kDash, "just inside the cap still prints");

    // NaN must not slip through as a formatted number. It arises the moment a
    // rate of exactly zero is divided into a remaining byte count, which happens
    // on the first frame of every transfer.
    CHECK(UI::format_eta(std::nan("")) == kDash, "NaN");
    CHECK(UI::format_eta(INFINITY) == kDash, "infinity");
    std::printf("  ok: refuses meaningless values\n");
}

int main() {
    std::printf("stats_format_test\n");
    test_seconds();
    test_minute_rollover();
    test_hour_rollover();
    test_rounding();
    test_refuses_meaningless_values();
    std::printf("stats_format_test: %d checks passed\n", g_checks);
    return 0;
}
