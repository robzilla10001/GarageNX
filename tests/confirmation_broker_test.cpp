// tests/confirmation_broker_test.cpp
//
// Tests the ConfirmationBroker state machine with REAL threads (run under TSan in
// CI). A worker thread calls ask() and blocks; the "main" thread polls pending()
// and resolves — mirroring how a transport worker and the UI loop interact.
//
// Covers the four behaviours that matter for the NAND safety model:
//   - happy path: allow and deny both flow back to the worker
//   - timeout: an unanswered request auto-denies so the worker can't hang
//   - shutdown: tearing down while a request is pending releases the worker with
//     Denied (the cancel-crash teardown discipline)
//   - concurrency: a second request while one is pending is auto-denied (no queue)

#include "services/confirmation_broker.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace Services;
using namespace std::chrono_literals;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// Spin until the broker has a pending request (or give up after ~2s).
static bool wait_pending(ConfirmationBroker& b) {
    for (int i = 0; i < 2000; ++i) {
        if (b.has_pending()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// ── Allow flows back to the worker ──────────────────────────────────────────
static void test_allow() {
    ConfirmationBroker b;
    std::atomic<int> result{-1};
    std::thread worker([&] {
        result = (int)b.ask("FTP", "delete", "bis_user:/x");
    });
    CHECK(wait_pending(b), "request becomes pending");
    ConfirmRequest r;
    CHECK(b.pending(r), "pending() returns the request");
    CHECK(r.transport == "FTP" && r.operation == "delete", "request fields carried");
    b.resolve(r.id, ConfirmResult::Allowed);
    worker.join();
    CHECK(result == (int)ConfirmResult::Allowed, "worker received Allowed");
    std::printf("  ok: allow flows back to the worker\n");
}

// ── Deny flows back ─────────────────────────────────────────────────────────
static void test_deny() {
    ConfirmationBroker b;
    std::atomic<int> result{-1};
    std::thread worker([&] { result = (int)b.ask("MTP", "write", "bis_system:/y"); });
    CHECK(wait_pending(b), "request pending");
    ConfirmRequest r; b.pending(r);
    b.resolve(r.id, ConfirmResult::Denied);
    worker.join();
    CHECK(result == (int)ConfirmResult::Denied, "worker received Denied");
    std::printf("  ok: deny flows back to the worker\n");
}

// ── Timeout auto-denies ─────────────────────────────────────────────────────
static void test_timeout() {
    ConfirmationBroker b;
    auto t0 = std::chrono::steady_clock::now();
    ConfirmResult r = b.ask("HTTP", "delete", "bis_user:/z", /*timeout_ms*/100);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(r == ConfirmResult::Denied, "unanswered request auto-denies");
    CHECK(elapsed >= 90ms, "waited about the timeout, not instant");
    CHECK(!b.has_pending(), "slot cleared after timeout");
    std::printf("  ok: timeout auto-denies and clears the slot\n");
}

// ── Shutdown releases a blocked worker with Denied ──────────────────────────
static void test_shutdown_releases() {
    ConfirmationBroker b;
    std::atomic<int> result{-1};
    std::thread worker([&] { result = (int)b.ask("FTP", "rmdir", "bis_user:/d"); });
    CHECK(wait_pending(b), "request pending before shutdown");
    b.shutdown();                       // tear down while pending
    worker.join();
    CHECK(result == (int)ConfirmResult::Denied, "shutdown releases worker as Denied");
    // Further asks are denied immediately while shut.
    CHECK(b.ask("FTP", "delete", "x") == ConfirmResult::Denied, "ask denied while shut");
    b.reset();
    std::printf("  ok: shutdown releases blocked worker, denies further asks\n");
}

// ── Second request while one pending is auto-denied (no queue) ──────────────
static void test_concurrent_autodeny() {
    ConfirmationBroker b;
    std::atomic<int> first{-1};
    std::thread w1([&] { first = (int)b.ask("FTP", "delete", "a"); });
    CHECK(wait_pending(b), "first request pending");
    // Second ask from another worker must return immediately as Denied.
    ConfirmResult second = b.ask("MTP", "delete", "b");
    CHECK(second == ConfirmResult::Denied, "second concurrent request auto-denied");
    // First is still pending and can be resolved normally.
    ConfirmRequest r; CHECK(b.pending(r), "first still pending after second denied");
    CHECK(r.target == "a", "the pending one is the first request");
    b.resolve(r.id, ConfirmResult::Allowed);
    w1.join();
    CHECK(first == (int)ConfirmResult::Allowed, "first resolves normally");
    std::printf("  ok: second concurrent request auto-denied, first unaffected\n");
}

// ── Stale resolve is ignored ────────────────────────────────────────────────
static void test_stale_resolve_ignored() {
    ConfirmationBroker b;
    // Resolve an id that was never issued — must not corrupt state.
    b.resolve(999, ConfirmResult::Allowed);
    CHECK(!b.has_pending(), "stale resolve on empty broker is a no-op");

    std::atomic<int> result{-1};
    std::thread worker([&] { result = (int)b.ask("FTP", "write", "x", 500); });
    CHECK(wait_pending(b), "pending");
    b.resolve(12345, ConfirmResult::Allowed);   // wrong id
    CHECK(b.has_pending(), "wrong-id resolve ignored, still pending");
    ConfirmRequest r; b.pending(r);
    b.resolve(r.id, ConfirmResult::Denied);      // correct id
    worker.join();
    CHECK(result == (int)ConfirmResult::Denied, "only the correct id resolves");
    std::printf("  ok: stale/wrong-id resolve is ignored\n");
}

int main() {
    std::printf("ConfirmationBroker (cross-thread NAND confirm)\n");
    test_allow();
    test_deny();
    test_timeout();
    test_shutdown_releases();
    test_concurrent_autodeny();
    test_stale_resolve_ignored();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
