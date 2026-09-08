#pragma once
// source/install/ncz_window.hpp
//
// Adapts a push-only byte stream (MTP/USB, later FTP/HTTP) to the pull-by-offset
// ReadFn that NczDecompressor requires. Slice 4b.
//
// Why this is not a pipe. NczDecompressor reads by absolute offset and re-reads
// the NCZ header region more than once, out of order (get_decompressed_size()
// then decompress() both walk offset 0..compressed_start). A plain
// producer/consumer pipe cannot serve that without rewinding.
//
// The shape that works: retain a bounded PREFIX of the entry in RAM and serve
// every re-read from it, then serve everything above the prefix from a sliding
// window that blocks until the producer has pushed that far. The re-read region
// ends at compressed_start; for a 14 GiB NCA at the typical 1 MB block exponent
// that is ~74 KB, and the pathological small-block case ~3.6 MB. kDefaultPrefix
// is 8 MB so every real container clears it with margin, and a read that
// escapes the prefix fails loudly rather than silently returning wrong bytes.
//
// Read contract. ncz.cpp's safe_read() is `fn(off, buf, len) == len` - it does
// NOT loop on short reads. read() therefore returns a short count ONLY at
// end-of-stream or on failure, and must serve a request that straddles the
// prefix/window seam in a single call.
//
// Threading. Producer (MTP thread) calls push()/finish(); consumer (the
// decompression thread) calls read(). Both block on each other and both wake on
// abort(). Sync is std::mutex/std::condition_variable - no libnx, following
// OverlapBuffer (hardware-validated). Builds and tests off-device unchanged.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Install {

class NczWindow {
public:
    /// Prefix retained for the lifetime of the entry. See sizing note above.
    static constexpr size_t kDefaultPrefix = 8u * 1024 * 1024;
    /// Sliding window above the prefix. Must exceed the largest single read
    /// ncz.cpp issues (1 MB stream chunk; one compressed block otherwise).
    static constexpr size_t kDefaultWindow = 8u * 1024 * 1024;

    NczWindow(size_t prefix_cap = kDefaultPrefix, size_t window_cap = kDefaultWindow);

    NczWindow(const NczWindow&) = delete;
    NczWindow& operator=(const NczWindow&) = delete;

    // ── Producer side (MTP thread) ───────────────────────────────────────────

    /// Append the next bytes of the entry. Blocks while the window is full,
    /// i.e. until the consumer has read far enough to release space. Returns
    /// false once the window has failed or been aborted.
    bool push(const uint8_t* data, size_t len);

    /// No more bytes will be pushed. Pending and subsequent reads return short
    /// rather than blocking forever.
    void finish();

    /// Abandon the transfer; unblocks both sides. The first reason latches.
    void abort(const std::string& why);

    // ── Consumer side (decompression thread) ─────────────────────────────────

    /// Pull-by-offset read matching Install::ReadFn. Blocks until `len` bytes at
    /// `offset` are available. Returns `len`, or fewer only at end-of-stream or
    /// on failure. Reading below the retained prefix is always legal; reading
    /// below the window watermark is a design violation and fails the window.
    size_t read(uint64_t offset, void* buf, size_t len);

    // ── Status ───────────────────────────────────────────────────────────────

    bool failed() const;
    std::string error() const;

    /// Total bytes pushed so far.
    uint64_t pushed() const;

private:
    /// Absolute offset -> ring slot. Bytes at [m_tail, m_write_pos) above the
    /// prefix live at (offset % m_window_cap).
    void ring_write(uint64_t offset, const uint8_t* src, size_t n);
    void ring_read(uint64_t offset, uint8_t* dst, size_t n) const;

    /// Latch a failure and wake everyone. Caller holds m_mtx.
    bool fail_locked(const std::string& why);

    const size_t m_prefix_cap;
    const size_t m_window_cap;

    mutable std::mutex      m_mtx;
    std::condition_variable m_cv_data;   // consumer waits for bytes
    std::condition_variable m_cv_space;  // producer waits for window space

    std::vector<uint8_t> m_prefix;  // [0, m_prefix_cap), retained for the entry
    std::vector<uint8_t> m_ring;    // sliding window above the prefix

    uint64_t m_write_pos = 0;  // total bytes pushed
    uint64_t m_tail = 0;       // lowest offset still retained in the ring;
                               // starts at m_prefix_cap and only ever rises
    bool        m_eof = false;
    bool        m_failed = false;
    std::string m_error;
};

} // namespace Install
