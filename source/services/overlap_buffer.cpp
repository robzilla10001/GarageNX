// source/services/overlap_buffer.cpp

#include "services/overlap_buffer.hpp"

#include <cstdlib>

#ifdef PLATFORM_SWITCH
#include <malloc.h>
#endif

namespace Services {

namespace {
constexpr size_t kAlign = 0x1000;   // usb:ds wants page-aligned DMA buffers

void* aligned_block(size_t bytes) {
#ifdef PLATFORM_SWITCH
    return memalign(kAlign, bytes);
#else
    // aligned_alloc requires a size that is a multiple of the alignment.
    const size_t rounded = ((bytes + kAlign - 1) / kAlign) * kAlign;
    return std::aligned_alloc(kAlign, rounded);
#endif
}
} // namespace

OverlapBuffer::OverlapBuffer(size_t buf_size, Sink sink)
    : m_size(buf_size), m_sink(std::move(sink)) {
    if (m_size == 0 || !m_sink) return;

    for (int i = 0; i < 2; i++) {
        m_buf[i] = (uint8_t*)aligned_block(m_size);
        if (!m_buf[i]) {   // caller falls back to the direct path
            for (int j = 0; j < 2; j++) { std::free(m_buf[j]); m_buf[j] = nullptr; }
            return;
        }
    }

#ifdef PLATFORM_SWITCH
    if (R_FAILED(threadCreate(&m_thread, &OverlapBuffer::thread_entry, this,
                              nullptr, 0x8000, 0x2C, -2))) {
        for (int j = 0; j < 2; j++) { std::free(m_buf[j]); m_buf[j] = nullptr; }
        return;
    }
    if (R_FAILED(threadStart(&m_thread))) {
        threadClose(&m_thread);
        for (int j = 0; j < 2; j++) { std::free(m_buf[j]); m_buf[j] = nullptr; }
        return;
    }
#else
    m_thread = std::thread(&OverlapBuffer::worker_loop, this);
#endif
    m_thread_started = true;
    m_valid = true;
}

void OverlapBuffer::quiesce() {
    if (!m_thread_started) return;
    {
        std::lock_guard<std::mutex> lk(m_m);
        m_stop = true;
    }
    m_cv_work.notify_all();
#ifdef PLATFORM_SWITCH
    // Blocks until worker_loop() returns. If the worker is mid-sink (the slow
    // half, run outside the lock) this waits for that sink call to finish — which
    // is the whole point: after this returns, the sink provably will not run
    // again, so state the sink closes over can be torn down safely.
    threadWaitForExit(&m_thread);
    threadClose(&m_thread);
#else
    if (m_thread.joinable()) m_thread.join();
#endif
    m_thread_started = false;
    m_valid = false;   // no more work may be submitted
}

OverlapBuffer::~OverlapBuffer() {
    quiesce();
    for (int i = 0; i < 2; i++) { std::free(m_buf[i]); m_buf[i] = nullptr; }
}

#ifdef PLATFORM_SWITCH
void OverlapBuffer::thread_entry(void* self) {
    static_cast<OverlapBuffer*>(self)->worker_loop();
}
#endif

void OverlapBuffer::worker_loop() {
    for (;;) {
        int    idx = -1;
        size_t len = 0;
        {
            std::unique_lock<std::mutex> lk(m_m);
            m_cv_work.wait(lk, [this] { return m_stop || m_busy_idx >= 0; });
            if (m_stop && m_busy_idx < 0) return;
            idx = m_busy_idx;
            len = m_busy_len;
        }

        // Run the sink outside the lock: it is the slow half, and holding the
        // mutex across it would serialise exactly what we are trying to overlap.
        bool ok = true;
        if (idx >= 0 && len > 0) ok = m_sink(m_buf[idx], len);
        if (!ok) m_failed.store(true);

        {
            std::lock_guard<std::mutex> lk(m_m);
            m_busy_idx = -1;
            m_busy_len = 0;
        }
        m_cv_done.notify_all();
    }
}

uint8_t* OverlapBuffer::acquire() {
    if (!m_valid) return nullptr;
    std::unique_lock<std::mutex> lk(m_m);
    // Wait until the worker is not holding the buffer we intend to fill.
    m_cv_done.wait(lk, [this] { return m_failed.load() || m_busy_idx != m_fill; });
    if (m_failed.load()) return nullptr;
    return m_buf[m_fill];
}

bool OverlapBuffer::submit(size_t len) {
    if (!m_valid) return false;
    if (m_failed.load()) return false;
    if (len == 0) return true;   // nothing to hand over; keep the same buffer

    {
        std::unique_lock<std::mutex> lk(m_m);
        // Only one chunk may be in flight, so wait for the previous to land.
        m_cv_done.wait(lk, [this] { return m_failed.load() || m_busy_idx < 0; });
        if (m_failed.load()) return false;
        m_busy_idx = m_fill;
        m_busy_len = len;
    }
    m_cv_work.notify_one();

    m_fill = 1 - m_fill;   // caller now fills the other buffer
    return true;
}

bool OverlapBuffer::flush() {
    if (!m_valid) return false;
    std::unique_lock<std::mutex> lk(m_m);
    m_cv_done.wait(lk, [this] { return m_failed.load() || m_busy_idx < 0; });
    return !m_failed.load();
}

} // namespace Services
