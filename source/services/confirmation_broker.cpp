// source/services/confirmation_broker.cpp

#include "services/confirmation_broker.hpp"

namespace Services {

ConfirmResult ConfirmationBroker::ask(const std::string& transport,
                                      const std::string& operation,
                                      const std::string& target,
                                      uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lk(m_mtx);

    // Refuse while shut down (teardown in progress) — deny immediately.
    if (m_shut) return ConfirmResult::Denied;

    // One at a time: a second request while one is pending is auto-denied.
    if (m_busy) return ConfirmResult::Denied;

    // Publish this request for the main thread to pick up.
    const uint64_t my_id = m_next_id++;
    m_busy     = true;
    m_resolved = false;
    m_cur_id   = my_id;
    m_cur      = ConfirmRequest{ my_id, transport, operation, target };
    m_cv.notify_all();   // wake pending()/UI if it's waiting

    // Wait until resolved, shut down, or (if requested) timed out.
    auto is_done = [&] { return m_resolved || m_shut; };
    if (timeout_ms == 0) {
        m_cv.wait(lk, is_done);
    } else {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        if (!m_cv.wait_until(lk, deadline, is_done)) {
            // Timed out with no answer: auto-deny and clear the slot so the next
            // request can proceed. Guard against a resolve() that raced in.
            if (!m_resolved) {
                m_result   = ConfirmResult::Denied;
                m_resolved = true;
            }
        }
    }

    const ConfirmResult r = m_shut ? ConfirmResult::Denied : m_result;

    // Clear the slot (only if this call still owns it).
    if (m_cur_id == my_id) {
        m_busy   = false;
        m_cur_id = 0;
    }
    return r;
}

bool ConfirmationBroker::pending(ConfirmRequest& out) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_busy && !m_resolved && !m_shut) {
        out = m_cur;
        return true;
    }
    return false;
}

void ConfirmationBroker::resolve(uint64_t id, ConfirmResult result) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // Only resolve the currently-pending request; a stale id (already timed out or
    // superseded) is ignored so a late answer can't affect a new request.
    if (m_busy && !m_resolved && m_cur_id == id) {
        m_result   = result;
        m_resolved = true;
        m_cv.notify_all();
    }
}

void ConfirmationBroker::shutdown() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_shut = true;
    m_cv.notify_all();   // release any blocked worker with Denied
}

void ConfirmationBroker::reset() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_shut     = false;
    m_busy     = false;
    m_resolved = false;
    m_cur_id   = 0;
}

bool ConfirmationBroker::has_pending() {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_busy && !m_resolved && !m_shut;
}

} // namespace Services
