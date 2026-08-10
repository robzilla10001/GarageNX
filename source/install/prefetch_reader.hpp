#pragma once

// PrefetchReader — wraps a pull-by-offset ReadFn with a background thread that
// reads the source sequentially ahead of the consumer into a bounded queue of
// chunks. A consumer that reads mostly sequentially (the NSZ/NSP install streams)
// then overlaps its own CPU/SD work (decompress, AES re-encrypt, WritePlaceHolder)
// with network I/O instead of alternating serially with it.
//
// Threading contract: ONLY the worker thread ever calls the underlying ReadFn, so
// a non-thread-safe source (e.g. the libsmb2 connection behind the net: devoptab)
// is touched by exactly one thread. The consumer only ever touches the in-memory
// queue under the mutex. The caller must ensure nothing else uses the same source
// for the lifetime of the PrefetchReader (during an install the browser is blocked,
// so this holds).
//
// Random access is supported: a read at an unexpected offset repositions the
// worker (dropping buffered data). The install does a few small header reads before
// streaming; those cost a reposition each, then the bulk streams sequentially.

#include "install/installer.hpp"   // Install::ReadFn

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <deque>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <utility>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Install {

class PrefetchReader {
public:
    PrefetchReader(ReadFn under, uint64_t total,
                   size_t chunk = 8u << 20, size_t depth = 4)
        : m_under(std::move(under)), m_total(total),
          m_chunk(chunk ? chunk : (1u << 20)), m_depth(depth ? depth : 1) {
#ifdef PLATFORM_SWITCH
        // Pin the reader to a dedicated core (2) so it runs truly in parallel with
        // the install thread's decompress/encrypt/write. With the portable default
        // the worker tended to be scheduled only while the consumer was blocked,
        // which defeats read-ahead. If the thread can't start, fall back to reading
        // straight through on the caller (correct, just not overlapped).
        if (R_SUCCEEDED(threadCreate(&m_nx, &PrefetchReader::nx_entry, this,
                                     nullptr, 128 * 1024, 0x2C, 2))
            && R_SUCCEEDED(threadStart(&m_nx))) {
            m_nx_started = true;
        } else {
            m_direct = true;
        }
#else
        m_thread = std::thread([this] { worker(); });
#endif
    }

    ~PrefetchReader() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stop = true;
        }
        m_cv_notfull.notify_all();
        m_cv_notempty.notify_all();
#ifdef PLATFORM_SWITCH
        if (m_nx_started) { threadWaitForExit(&m_nx); threadClose(&m_nx); }
#else
        if (m_thread.joinable()) m_thread.join();
#endif

        // Diagnostics: if reads mostly had to wait and the queue never filled, the
        // worker isn't getting ahead (parallelism problem); if maxq reaches depth
        // and waits are low, the overlap is working and the link is just the limit.
        if (FILE* tf = std::fopen("sdmc:/switch/GarageNX/logs/install_timing.log", "a")) {
            std::fprintf(tf, "  prefetch: reads=%llu waited=%llu (%.0f%%) readMB=%.0f maxq=%zu/%zu\n",
                         (unsigned long long)m_dbg_reads, (unsigned long long)m_dbg_waits,
                         m_dbg_reads ? (100.0 * (double)m_dbg_waits / (double)m_dbg_reads) : 0.0,
                         m_dbg_bytes / (1024.0 * 1024.0), m_dbg_maxq, m_depth);
            std::fclose(tf);
        }
    }

    PrefetchReader(const PrefetchReader&) = delete;
    PrefetchReader& operator=(const PrefetchReader&) = delete;

    // Consumer read: copies up to `len` bytes at absolute offset `off`. Returns the
    // number of bytes copied; a short/zero return means EOF or an underlying error
    // (matching the ReadFn contract the install already handles).
    size_t read(uint64_t off, void* buf, size_t len) {
        if (m_direct) return m_under(off, buf, len);   // worker unavailable
        std::unique_lock<std::mutex> lk(m_mtx);

        if (off != m_serve_pos) {
            // Seek: restart the worker at `off` and drop what we had buffered.
            m_reposition    = true;
            m_reposition_to = off;
            m_queue.clear();
            m_serve_pos = off;
            m_error = false;
            m_cv_notfull.notify_all();
        }

        uint8_t* out = static_cast<uint8_t*>(buf);
        size_t copied = 0;
        ++m_dbg_reads;
        if (m_queue.empty()) ++m_dbg_waits;   // will block below = worker behind
        while (copied < len) {
            m_cv_notempty.wait(lk, [this] {
                return m_stop || m_error || !m_queue.empty() ||
                       (m_eof && m_serve_pos >= m_read_pos);
            });
            if (m_stop || m_error) break;
            if (m_queue.empty()) break;   // EOF, nothing more at serve_pos

            Chunk& c = m_queue.front();
            // Invariant: the front chunk contains serve_pos (contiguous, front-first
            // consumption; a reposition refills from serve_pos).
            const size_t avail = c.data.size() - c.consumed;
            const size_t take  = std::min(avail, len - copied);
            std::memcpy(out + copied, c.data.data() + c.consumed, take);
            c.consumed  += take;
            copied      += take;
            m_serve_pos += take;
            if (c.consumed == c.data.size()) {
                m_queue.pop_front();
                m_cv_notfull.notify_one();
            }
        }
        return copied;
    }

private:
    struct Chunk { uint64_t off = 0; std::vector<uint8_t> data; size_t consumed = 0; };

    void worker() {
        std::vector<uint8_t> tmp(m_chunk);
        std::unique_lock<std::mutex> lk(m_mtx);
        for (;;) {
            m_cv_notfull.wait(lk, [this] {
                return m_stop || m_reposition ||
                       (!m_eof && !m_error && m_queue.size() < m_depth && m_read_pos < m_total);
            });
            if (m_stop) return;
            if (m_reposition) {
                m_reposition = false;
                m_read_pos = m_reposition_to;
                m_eof = false;
                m_error = false;
                m_queue.clear();
                continue;
            }

            const uint64_t pos  = m_read_pos;
            const size_t   want = static_cast<size_t>(std::min<uint64_t>(m_chunk, m_total - pos));

            lk.unlock();
            const size_t got = m_under(pos, tmp.data(), want);   // the only source access
            lk.lock();

            if (m_stop) return;
            if (m_reposition) continue;      // consumer sought while we were reading
            if (got == 0) { m_error = true; m_cv_notempty.notify_all(); continue; }

            Chunk c;
            c.off = pos;
            c.data.assign(tmp.begin(), tmp.begin() + got);
            m_queue.push_back(std::move(c));
            m_read_pos = pos + got;
            m_dbg_bytes += got;
            if (m_queue.size() > m_dbg_maxq) m_dbg_maxq = m_queue.size();
            if (m_read_pos >= m_total) m_eof = true;
            m_cv_notempty.notify_one();
        }
    }

    ReadFn   m_under;
    uint64_t m_total;
    size_t   m_chunk;
    size_t   m_depth;

    std::mutex              m_mtx;
    std::condition_variable m_cv_notfull;
    std::condition_variable m_cv_notempty;
    std::deque<Chunk>       m_queue;

    uint64_t m_read_pos     = 0;   // next offset the worker will read
    uint64_t m_serve_pos    = 0;   // next offset the consumer expects
    uint64_t m_reposition_to = 0;
    bool     m_reposition   = false;
    bool     m_stop         = false;
    bool     m_eof          = false;
    bool     m_error        = false;

    // Diagnostics (worker + consumer, guarded by m_mtx).
    uint64_t m_dbg_reads = 0, m_dbg_waits = 0, m_dbg_bytes = 0;
    size_t   m_dbg_maxq  = 0;

    bool m_direct = false;   // true => no worker; read() calls m_under directly
#ifdef PLATFORM_SWITCH
    ::Thread m_nx{};
    bool     m_nx_started = false;
    static void nx_entry(void* p) { static_cast<PrefetchReader*>(p)->worker(); }
#else
    std::thread m_thread;
#endif
};

}  // namespace Install
