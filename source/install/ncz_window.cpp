// source/install/ncz_window.cpp

#include "install/ncz_window.hpp"

#include <algorithm>
#include <cstring>

namespace Install {

NczWindow::NczWindow(size_t prefix_cap, size_t window_cap)
    : m_prefix_cap(prefix_cap), m_window_cap(window_cap) {
    m_prefix.resize(m_prefix_cap);
    m_ring.resize(m_window_cap);
    // The ring only ever holds offsets at or above the prefix, so the watermark
    // starts at the prefix boundary and rises from there.
    m_tail = m_prefix_cap;
}

// ── Ring mapping ─────────────────────────────────────────────────────────────
// Absolute offset o above the prefix lives at slot (o % m_window_cap). Valid
// only for o in [m_tail, m_write_pos); callers check that first.

void NczWindow::ring_write(uint64_t offset, const uint8_t* src, size_t n) {
    const size_t pos = static_cast<size_t>(offset % m_window_cap);
    const size_t first = std::min(n, m_window_cap - pos);
    std::memcpy(m_ring.data() + pos, src, first);
    if (n > first)
        std::memcpy(m_ring.data(), src + first, n - first);
}

void NczWindow::ring_read(uint64_t offset, uint8_t* dst, size_t n) const {
    const size_t pos = static_cast<size_t>(offset % m_window_cap);
    const size_t first = std::min(n, m_window_cap - pos);
    std::memcpy(dst, m_ring.data() + pos, first);
    if (n > first)
        std::memcpy(dst + first, m_ring.data(), n - first);
}

bool NczWindow::fail_locked(const std::string& why) {
    if (!m_failed) {
        m_failed = true;
        m_error = why;
    }
    m_cv_data.notify_all();
    m_cv_space.notify_all();
    return false;
}

// ── Producer ─────────────────────────────────────────────────────────────────

bool NczWindow::push(const uint8_t* data, size_t len) {
    std::unique_lock<std::mutex> lk(m_mtx);
    const uint8_t* src = data;
    size_t left = len;

    while (left > 0) {
        if (m_failed)
            return false;
        if (m_eof)
            return fail_locked("NczWindow: push after finish");

        // Below the prefix boundary the bytes are retained permanently, so there
        // is never back-pressure here.
        if (m_write_pos < m_prefix_cap) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, m_prefix_cap - m_write_pos));
            std::memcpy(m_prefix.data() + m_write_pos, src, n);
            m_write_pos += n;
            src += n;
            left -= n;
            m_cv_data.notify_all();
            continue;
        }

        // m_tail <= m_write_pos holds once past the prefix, so this cannot wrap.
        const uint64_t used = m_write_pos - m_tail;
        if (used >= m_window_cap) {
            m_cv_space.wait(lk);
            continue;
        }

        const size_t n = std::min(left, static_cast<size_t>(m_window_cap - used));
        ring_write(m_write_pos, src, n);
        m_write_pos += n;
        src += n;
        left -= n;
        m_cv_data.notify_all();
    }
    return true;
}

void NczWindow::finish() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_eof = true;
    m_cv_data.notify_all();
    m_cv_space.notify_all();
}

void NczWindow::abort(const std::string& why) {
    std::lock_guard<std::mutex> lk(m_mtx);
    fail_locked(why);
}

// ── Consumer ─────────────────────────────────────────────────────────────────

size_t NczWindow::read(uint64_t offset, void* buf, size_t len) {
    if (len == 0)
        return 0;

    std::unique_lock<std::mutex> lk(m_mtx);
    if (m_failed)
        return 0;

    const uint64_t end = offset + len;

    // Anything the request touches above the prefix must still be resident and
    // must fit the window. Both checks are design violations, not runtime
    // conditions: ncz.cpp reads strictly forward above the header region, and
    // never asks for more than one 1 MB stream chunk or one compressed block.
    // Failing loudly here is what keeps a wrong assumption from silently
    // decompressing garbage.
    if (end > m_prefix_cap) {
        const uint64_t ring_start = std::max<uint64_t>(offset, m_prefix_cap);
        if (ring_start < m_tail) {
            fail_locked("NczWindow: read below window watermark (backwards seek "
                        "outside the retained prefix)");
            return 0;
        }
        if (end - ring_start > m_window_cap) {
            fail_locked("NczWindow: read larger than window capacity");
            return 0;
        }
    }

    // safe_read() in ncz.cpp does not loop, so wait for the WHOLE request.
    // Release history we have already passed while waiting, or a full window
    // would deadlock against a producer that cannot advance.
    while (!m_failed && !m_eof && m_write_pos < end) {
        const uint64_t want = std::max<uint64_t>(m_prefix_cap, std::min(offset, m_write_pos));
        if (want > m_tail) {
            m_tail = want;
            m_cv_space.notify_all();
        }
        m_cv_data.wait(lk);
    }
    if (m_failed)
        return 0;

    const size_t n = (m_write_pos > offset)
                         ? static_cast<size_t>(std::min<uint64_t>(len, m_write_pos - offset))
                         : 0;

    uint8_t* out = static_cast<uint8_t*>(buf);
    size_t copied = 0;
    if (offset < m_prefix_cap) {
        const size_t p = static_cast<size_t>(std::min<uint64_t>(n, m_prefix_cap - offset));
        std::memcpy(out, m_prefix.data() + offset, p);
        copied = p;
    }
    if (copied < n)
        ring_read(offset + copied, out + copied, n - copied);

    const uint64_t want = std::max<uint64_t>(m_prefix_cap, offset + n);
    if (want > m_tail) {
        m_tail = want;
        m_cv_space.notify_all();
    }
    return n;
}

// ── Status ───────────────────────────────────────────────────────────────────

bool NczWindow::failed() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_failed;
}

std::string NczWindow::error() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_error;
}

uint64_t NczWindow::pushed() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_write_pos;
}

} // namespace Install
