// tests/overlap_buffer_test.cpp
//
// Tests for OverlapBuffer, with a specific target: the teardown-ordering race
// behind the MTP cancel crash (Atmosphère 2347-0018, a newlib BadReent fault).
//
// The crash path is: user cancels a streaming NSZ install; recv_install's loop
// exits on should_stop(); it calls m_install->abort(), which joins the
// decompression worker and frees the decompress window — WHILE the OverlapBuffer
// worker thread may still be inside the sink, which is m_install->feed() and
// touches that same window. Two workers, uncoordinated teardown, cross-thread
// use-after-free.
//
// This suite cannot instantiate a StreamInstaller on the host (it needs ncm), so
// it reproduces the SHAPE with a stand-in sink that touches heap state a
// concurrent "abort" tears down. The claim under test is narrow and exact:
//
//   quiesce() guarantees the worker will not enter the sink again, and blocks
//   until any in-flight sink call has returned.
//
// If that holds, ordering quiesce() before the teardown that frees the sink's
// state closes the race. Built under TSan (see CMakeLists), an ordering bug here
// surfaces as a reported data race, not just a lucky pass. Run it many times.

#include "services/overlap_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

using Services::OverlapBuffer;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static constexpr size_t kBuf = 64 * 1024;

// ── 1. Straight-line correctness ─────────────────────────────────────────────
// Every submitted byte reaches the sink, once, in order.
static void test_all_bytes_arrive_in_order() {
    std::vector<uint8_t> got;
    OverlapBuffer ov(kBuf, [&](const uint8_t* p, size_t n) {
        got.insert(got.end(), p, p + n);
        return true;
    });
    CHECK(ov.valid(), "buffer constructs");

    uint64_t next = 0;
    std::vector<uint8_t> expect;
    for (int chunk = 0; chunk < 200; ++chunk) {
        uint8_t* dst = ov.acquire();
        CHECK(dst != nullptr, "acquire");
        const size_t n = 1 + (chunk * 37) % (kBuf - 1);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t b = (uint8_t)(next++ & 0xFF);
            dst[i] = b;
            expect.push_back(b);
        }
        CHECK(ov.submit(n), "submit");
    }
    CHECK(ov.flush(), "flush");
    CHECK(got == expect, "every byte arrived once, in order");
    std::printf("  ok: all bytes arrive once, in order, across 200 chunks\n");
}

// ── 2. Sink failure latches and surfaces ─────────────────────────────────────
static void test_sink_failure_is_latched() {
    std::atomic<int> calls{0};
    OverlapBuffer ov(kBuf, [&](const uint8_t*, size_t) {
        return ++calls < 3;   // fail on the third chunk
    });
    CHECK(ov.valid(), "constructs");

    bool saw_failure = false;
    for (int i = 0; i < 50 && !saw_failure; ++i) {
        uint8_t* dst = ov.acquire();
        if (!dst) { saw_failure = true; break; }   // acquire returns null post-failure
        if (!ov.submit(16)) { saw_failure = true; break; }
    }
    CHECK(saw_failure || !ov.flush(), "a failing sink is surfaced to the caller");
    CHECK(ov.failed(), "failure is latched");
    std::printf("  ok: sink failure latches and surfaces\n");
}

// ── 3. THE ONE THAT MATTERS: quiesce() fences the sink against teardown ───────
//
// A sink that reads and writes heap state owned by a unique_ptr. A second thread
// cancels — quiesce()s the buffer, then frees the state — exactly as
// recv_install's cancel path quiesce()s then abort()s. The invariant: no sink
// call touches the state after it is freed. Under TSan a violation is a reported
// race; under ASan a use-after-free. Both fail the run.
static void test_quiesce_fences_sink_before_state_is_freed() {
    for (int trial = 0; trial < 40; ++trial) {
        struct Window { std::atomic<uint64_t> bytes{0}; bool alive = true; };
        auto win = std::make_unique<Window>();
        Window* raw = win.get();

        std::atomic<bool> touched_after_free{false};

        OverlapBuffer ov(kBuf, [&](const uint8_t*, size_t n) {
            // Stand-in for m_install->feed()/push(): touches window state. If the
            // worker runs this after the window is freed, `alive` is false (or the
            // read itself is a UAF, which the sanitizers catch directly).
            if (!raw->alive) { touched_after_free.store(true); return false; }
            raw->bytes.fetch_add(n);
            // A little work, so the cancel below can land mid-sink some of the time.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            return raw->alive;
        });
        CHECK(ov.valid(), "constructs");

        // Producer: push chunks until cancelled.
        std::atomic<bool> stop{false};
        std::thread producer([&] {
            while (!stop.load()) {
                uint8_t* dst = ov.acquire();
                if (!dst) break;
                if (!ov.submit(4096)) break;
            }
        });

        // Let a few chunks get in flight, then cancel the way recv_install does:
        // quiesce FIRST (worker provably parked), THEN tear down the state.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        stop.store(true);
        ov.quiesce();          // (1) after this the sink cannot run again
        raw->alive = false;    // (2) safe: no sink call is or will be in progress
        win.reset();           // (3) free — must not race any sink call

        producer.join();
        CHECK(!touched_after_free.load(), "sink never ran against freed window state");
    }
    std::printf("  ok: quiesce() fences the sink before dependent state is freed (40 trials)\n");
}

// ── 4. quiesce() is idempotent and destructor-safe ───────────────────────────
static void test_quiesce_is_idempotent() {
    std::atomic<int> calls{0};
    {
        OverlapBuffer ov(kBuf, [&](const uint8_t*, size_t) { ++calls; return true; });
        CHECK(ov.valid(), "constructs");
        uint8_t* dst = ov.acquire();
        CHECK(dst, "acquire");
        CHECK(ov.submit(128), "submit");
        ov.quiesce();
        ov.quiesce();          // second call must be a no-op, not a double-join
        CHECK(!ov.valid(), "invalid after quiesce");
        // Destructor runs here and must not double-join.
    }
    std::printf("  ok: quiesce() is idempotent and safe before the destructor\n");
}

// ── 5. quiesce() blocks until an in-flight sink call returns ─────────────────
// The sharp edge of the invariant: if the worker is mid-sink when quiesce() is
// called, quiesce() must not return until that call finishes.
static void test_quiesce_waits_for_in_flight_sink() {
    std::atomic<bool> in_sink{false};
    std::atomic<bool> sink_returned{false};
    std::atomic<bool> quiesce_returned{false};

    OverlapBuffer ov(kBuf, [&](const uint8_t*, size_t) {
        in_sink.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        sink_returned.store(true);
        return true;
    });
    CHECK(ov.valid(), "constructs");

    uint8_t* dst = ov.acquire();
    CHECK(dst, "acquire");
    CHECK(ov.submit(4096), "submit");

    // Wait until the worker is demonstrably inside the sink.
    for (int i = 0; i < 1000 && !in_sink.load(); ++i)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    CHECK(in_sink.load(), "worker entered the sink");

    ov.quiesce();
    quiesce_returned.store(true);

    // If quiesce() returned before the sink did, the fence is a lie.
    CHECK(sink_returned.load(), "quiesce() did not return until the in-flight sink finished");
    CHECK(quiesce_returned.load(), "quiesce() returned");
    std::printf("  ok: quiesce() blocks until an in-flight sink call returns\n");
}

int main() {
    std::printf("OverlapBuffer\n");
    test_all_bytes_arrive_in_order();
    test_sink_failure_is_latched();
    test_quiesce_fences_sink_before_state_is_freed();
    test_quiesce_is_idempotent();
    test_quiesce_waits_for_in_flight_sink();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
