#pragma once
// source/services/overlap_buffer.hpp
//
// Overlaps a slow producer with a slow consumer.
//
// The transfer paths read a chunk from USB, then write it to storage, then read
// the next — strictly alternating, so the bulk endpoint sits idle for the whole
// write and the storage sits idle for the whole read. Measured on hardware that
// costs about half the achievable rate: ~18 MB/s installing versus ~36 MB/s for
// a plain copy over the same link.
//
// This hands the caller one of two page-aligned buffers to fill while a worker
// thread drains the other, so the two overlap. usb:ds requires page-aligned DMA
// memory, which is why the buffers are owned here and lent out by pointer
// rather than copied through.
//
// Deliberately transport-agnostic: the consumer is a callback, so the same
// class serves an NCM placeholder write, an fwrite to the SD card, or a future
// FTP/HTTP path. Thread primitive follows NetworkService — libnx Thread on
// Switch, std::thread on PC.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#else
#include <thread>
#endif

namespace Services {

class OverlapBuffer {
public:
    /// Consumes one chunk. Returns false to abort the transfer; the failure is
    /// latched and surfaced from submit()/flush().
    using Sink = std::function<bool(const uint8_t* data, size_t len)>;

    OverlapBuffer(size_t buf_size, Sink sink);
    ~OverlapBuffer();

    OverlapBuffer(const OverlapBuffer&) = delete;
    OverlapBuffer& operator=(const OverlapBuffer&) = delete;

    /// False if the buffers or worker could not be created; the caller should
    /// fall back to a direct, non-overlapped path.
    bool valid() const { return m_valid; }

    /// The buffer to fill next. Blocks until the worker releases it.
    /// Returns nullptr once the sink has failed.
    uint8_t* acquire();

    /// Publish `len` bytes of the acquired buffer to the worker and return
    /// immediately. False once the sink has failed.
    bool submit(size_t len);

    /// Wait for all published work to drain. False if the sink ever failed.
    bool flush();

    bool failed() const { return m_failed.load(); }

    size_t buffer_size() const { return m_size; }

private:
    void worker_loop();
#ifdef PLATFORM_SWITCH
    static void thread_entry(void* self);
#endif

    size_t   m_size = 0;
    Sink     m_sink;
    uint8_t* m_buf[2] = {nullptr, nullptr};
    int      m_fill = 0;      // index the caller is currently filling

    std::mutex              m_m;
    std::condition_variable m_cv_work;   // worker waits for a chunk
    std::condition_variable m_cv_done;   // caller waits for a buffer to free up
    int    m_busy_idx = -1;   // index the worker holds, -1 when idle
    size_t m_busy_len = 0;
    bool   m_stop = false;

    std::atomic<bool> m_failed{false};
    bool m_valid = false;
    bool m_thread_started = false;

#ifdef PLATFORM_SWITCH
    Thread m_thread{};
#else
    std::thread m_thread;
#endif
};

} // namespace Services
