// source/services/confirmation_broker.hpp
//
// Cross-transport NAND safety: when a client (FTP/MTP/HTTP, on a worker thread)
// asks to mutate a guarded surface, the worker BLOCKS here while a modal is shown
// ON THE CONSOLE. The user answers on-device; the worker resumes with the result.
// The context switch (PC -> console -> PC) is itself the safety mechanism.
//
// This class is the pure, host-testable state machine behind that. It owns NO UI
// and NO libnx — the main loop drives it (poll for a pending request, show a
// modal, resolve it); the worker thread only calls ask() and waits. Keeping it
// pure means the whole request/resolve/timeout/shutdown state machine is unit-
// tested off-device, exactly like RateMeter and StreamDriver.
//
// Design decisions (agreed):
//   - ONE pending request at a time. A second ask() while one is pending is
//     AUTO-DENIED immediately (no queue): if the user is paying attention and
//     acting deliberately, concurrent guarded ops shouldn't be happening; deny is
//     the safe answer.
//   - TIMEOUT auto-deny: a request nobody answers (user walked away) resolves to
//     Denied after a timeout so the worker can't hang forever.
//   - SHUTDOWN auto-deny: if the app/servers are tearing down while a request is
//     pending, the worker is released with Denied — never left parked on a UI
//     that's going away. This is the teardown discipline from the cancel crash,
//     applied up front.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace Services {

enum class ConfirmResult { Allowed, Denied };

// A single confirmation request the UI should show. Copyable snapshot handed to
// the main thread; the worker keeps waiting regardless.
struct ConfirmRequest {
    uint64_t    id = 0;          // monotonic; identifies this request to resolve()
    std::string transport;       // "FTP" / "MTP" / "HTTP" — who is asking
    std::string operation;       // "delete", "write", "rename", ...
    std::string target;          // the path/object being mutated
};

class ConfirmationBroker {
public:
    // Default: never time out on its own unless a positive timeout is given to
    // ask(). Shutdown always releases waiters.
    ConfirmationBroker() = default;

    // WORKER THREAD: request confirmation and block until answered, timed out, a
    // second request pre-empts (auto-deny), or shutdown. `timeout_ms == 0` means
    // no timeout (wait until answered or shutdown). Returns the result.
    ConfirmResult ask(const std::string& transport,
                      const std::string& operation,
                      const std::string& target,
                      uint32_t timeout_ms = 0);

    // MAIN THREAD: is a request waiting to be shown? Fills `out` and returns true.
    bool pending(ConfirmRequest& out);

    // MAIN THREAD: resolve the currently-pending request. `id` must match the
    // pending request (a stale id — e.g. the request already timed out — is
    // ignored). Wakes the blocked worker.
    void resolve(uint64_t id, ConfirmResult result);

    // ANY THREAD: release all waiters with Denied and refuse further asks until
    // reset(). Called on server stop / app exit so no worker is left parked.
    void shutdown();

    // Re-arm after a shutdown() (e.g. server restarted). Clears the shut flag.
    void reset();

    // For the UI/host tests: is there an unresolved request right now?
    bool has_pending();

private:
    std::mutex              m_mtx;
    std::condition_variable m_cv;

    bool          m_shut       = false;
    bool          m_busy       = false;   // a request is pending/unresolved
    uint64_t      m_next_id    = 1;
    uint64_t      m_cur_id     = 0;
    ConfirmRequest m_cur;
    bool          m_resolved   = false;
    ConfirmResult m_result     = ConfirmResult::Denied;
};

} // namespace Services
