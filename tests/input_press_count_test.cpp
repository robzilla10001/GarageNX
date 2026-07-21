// tests/input_press_count_test.cpp
//
// Tests the tap-counting logic behind the file-browser navigation fix. The real
// Input::poll() needs SDL (not linkable on the host), so this reimplements the
// COUNTING CORE exactly as input.cpp does it and asserts the property that
// matters: N discrete button-down events in one frame yield a press count of N,
// not 1. That collapse (a bitmask OR losing repeated taps) was the "7 presses ->
// 5 lines" file-explorer drop.
//
// This is a logic mirror, not the shipping code — if input.cpp's counting
// changes, this must change with it. It exists to prove the algorithm, which is
// the part that can be subtly wrong; the SDL glue around it is trivial and
// hardware-verified separately.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// ── Mirror of input.cpp's per-frame counting core ────────────────────────────
// A frame's raw input: the button-down/up events drained from the queue, plus
// the polled held-state bitmasks (prev/curr) that cover held-across-frames and
// the analog stick (which emits no button events).
struct FrameInput {
    std::vector<std::pair<uint32_t, bool>> events;  // (button_mask, is_down)
    uint32_t held_prev = 0;
    uint32_t held_curr = 0;
};

static int bit_index(uint32_t mask) {
    int i = 0;
    while (i < 32 && !((mask >> i) & 1u)) ++i;
    return i;
}

// Produces per-button press counts exactly as input.cpp poll() now does.
static void compute_press_counts(const FrameInput& f, uint8_t out_count[32]) {
    std::memset(out_count, 0, 32);
    uint32_t event_pressed = 0;

    for (auto& [m, is_down] : f.events) {
        if (is_down) {
            event_pressed |= m;
            if (m) {
                const int idx = bit_index(m);
                if (out_count[idx] < 255) out_count[idx]++;   // COUNT, not OR
            }
        }
    }

    // Poll edge: a newly-held button (or analog direction) with no queue event
    // still counts as one press. Only when the queue didn't already count it.
    const uint32_t poll_edge = f.held_curr & ~f.held_prev;
    for (int i = 0; i < 32; ++i) {
        if ((poll_edge >> i) & 1u) {
            if (out_count[i] == 0) out_count[i] = 1;
        }
    }
}

static constexpr uint32_t DDOWN = (1u << 11);
static constexpr uint32_t DUP   = (1u << 10);
static constexpr uint32_t A_BTN = (1u << 0);

// ── The core claim: rapid taps are not collapsed ────────────────────────────
static void test_multiple_taps_in_one_frame_count_separately() {
    // Seven quick DDown taps (down+up each) landing in one stalled frame.
    FrameInput f;
    for (int i = 0; i < 7; ++i) {
        f.events.push_back({DDOWN, true});
        f.events.push_back({DDOWN, false});
    }
    // Button is up by poll time (tap ended), so held_curr has no DDown.
    uint8_t c[32];
    compute_press_counts(f, c);
    CHECK(c[bit_index(DDOWN)] == 7, "seven taps in one frame count as seven, not one");
    std::printf("  ok: seven sub-frame DDown taps count as seven presses\n");
}

// ── A single normal tap counts once (no double-count from poll+queue) ────────
static void test_single_tap_counts_once() {
    FrameInput f;
    f.events.push_back({DDOWN, true});
    // The tap is still held at poll time: both the queue and the poll edge see it.
    f.held_prev = 0;
    f.held_curr = DDOWN;
    uint8_t c[32];
    compute_press_counts(f, c);
    CHECK(c[bit_index(DDOWN)] == 1, "a tap seen by both queue and poll counts once, not twice");
    std::printf("  ok: single tap (queue + poll edge) counts once\n");
}

// ── Analog stick / held-across-frame: poll edge with no queue event ──────────
static void test_poll_edge_without_events_counts_once() {
    FrameInput f;                    // no button events at all (analog stick)
    f.held_prev = 0;
    f.held_curr = DDOWN;             // newly-held direction this frame
    uint8_t c[32];
    compute_press_counts(f, c);
    CHECK(c[bit_index(DDOWN)] == 1, "a poll-only edge (stick) counts as one press");
    std::printf("  ok: analog/poll-only edge counts as one press\n");
}

// ── A held button already down last frame produces NO new press ──────────────
static void test_sustained_hold_is_not_a_new_press() {
    FrameInput f;                    // no down-events; button held since before
    f.held_prev = DDOWN;
    f.held_curr = DDOWN;
    uint8_t c[32];
    compute_press_counts(f, c);
    CHECK(c[bit_index(DDOWN)] == 0, "a sustained hold is not counted as a fresh press");
    std::printf("  ok: sustained hold yields zero new presses (repeat handles it)\n");
}

// ── Independent buttons don't cross-contaminate ──────────────────────────────
static void test_counts_are_per_button() {
    FrameInput f;
    f.events.push_back({DDOWN, true});
    f.events.push_back({DDOWN, true});
    f.events.push_back({A_BTN, true});
    uint8_t c[32];
    compute_press_counts(f, c);
    CHECK(c[bit_index(DDOWN)] == 2, "DDown counted twice");
    CHECK(c[bit_index(A_BTN)] == 1, "A counted once");
    CHECK(c[bit_index(DUP)]   == 0, "DUp untouched");
    std::printf("  ok: per-button counts are independent\n");
}

int main() {
    std::printf("Input sub-frame tap counting (file-browser nav fix)\n");
    test_multiple_taps_in_one_frame_count_separately();
    test_single_tap_counts_once();
    test_poll_edge_without_events_counts_once();
    test_sustained_hold_is_not_a_new_press();
    test_counts_are_per_button();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
