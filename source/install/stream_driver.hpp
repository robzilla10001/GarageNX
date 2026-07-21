// source/install/stream_driver.hpp
//
// Transport-agnostic driver for a streaming install. This is the KEYSTONE that
// lets MTP, FTP, and HTTP share one install path instead of copy-pasting the
// loop (and re-learning the 4c teardown/UAF lessons) three times.
//
// The install ENGINE (StreamInstaller: begin/feed/finish/abort) was already
// transport-agnostic. What was welded to MTP was the DRIVER LOOP around it:
//   - correcting the payload size from the container's own PFS0/HFS0 table when
//     the transport can't express a 64-bit size (MTP's 32-bit cap),
//   - overlapping the reads with the placeholder writes (OverlapBuffer),
//   - the load-bearing teardown ORDER on cancel/error (quiesce the overlap
//     worker BEFORE abort(), or feed() runs against a freed decompress window —
//     the cross-thread UAF we chased for six rounds).
// All of that lives here now, driven by an injected byte source. The transport
// keeps only what is genuinely its own: framing (e.g. MTP's 12-byte data-
// container header) and unwedging a half-finished transfer (drain).
//
// TESTABILITY: because the byte source, cancel predicate, and drain are all
// callbacks, the whole driver runs on the host against a synthetic in-memory
// stream — no USB, no sockets. See tests/stream_driver_test.cpp.

#pragma once

#include "install/stream_installer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>   // ssize_t (do not rely on transitive visibility)

namespace Install {

// Wire accounting the driver publishes as bytes are consumed, so a screen can
// show speed/ETA. The transport owns the atomics; the driver just updates them.
// (Kept as plain callbacks rather than atomic refs so the core has no threading
// assumptions of its own — the transport decides how these are stored.)
struct WireSink {
    // Called once when the transfer's true size becomes known (0 = still unknown;
    // the UI shows "—"). May be called more than once as the estimate refines
    // (declared size → container table). Always the best-known total.
    std::function<void(uint64_t size)> set_size = [](uint64_t) {};
    // Called with each chunk of file payload actually consumed (not transport
    // framing). The transport typically fetch_adds this into a counter.
    std::function<void(uint64_t bytes)> add_recv = [](uint64_t) {};
};

// Everything the driver needs from the transport, injected. The driver never
// names MTP/FTP/HTTP; each transport fills these in.
struct StreamSource {
    // Fill up to `n` bytes into `buf`; return bytes read. 0 means the stream
    // ended (EOF / ZLP / socket close). A negative return means a read error.
    // MTP: wraps ep_read. FTP/HTTP: wraps ::recv on the data/socket fd.
    std::function<ssize_t(uint8_t* buf, size_t n)> read;

    // Cooperative cancel. MTP/FTP/HTTP each pass their own should_stop().
    std::function<bool()> stop = [] { return false; };

    // Unwedge a transfer abandoned mid-stream. MTP must drain leftover USB bytes
    // so the next command doesn't parse file data as a header; a socket transport
    // can leave this empty and just close the connection. `remaining` is the
    // driver's best estimate of unconsumed bytes.
    std::function<void(uint64_t remaining)> drain = [](uint64_t) {};

    // Scratch buffer for reads. The transport owns it (MTP reuses its endpoint
    // buffer). Must be at least a few KiB; larger is better for throughput.
    uint8_t* buffer      = nullptr;
    size_t   buffer_size = 0;
};

// The first payload chunk, already stripped of any transport framing by the
// adapter (e.g. MTP removes its 12-byte header before calling). May be empty if
// the transport delivers pure payload from the first read.
struct FirstChunk {
    const uint8_t* data = nullptr;
    size_t         size = 0;
};

// Result of a driver run, so the transport can log/report without re-deriving it.
enum class DriveResult {
    Ok,          // finish() succeeded
    Cancelled,   // stop() went true mid-stream; cleaned up
    ShortRead,   // stream ended before payload was satisfied
    FeedError,   // installer rejected a chunk (bad container, OOM bound, etc.)
    FinishError, // all bytes fed but finish() failed (register/commit)
};

// Drive an install to completion over an already-begun StreamInstaller.
//
// Preconditions: `inst.begin(name, declared_size)` has already been called by
// the transport (begin needs the filename, which is transport-specific to obtain).
// `declared_size` is the transport's best size; `size_exact` is true only when it
// came from a real 64-bit declaration (not MTP's 32-bit field). `first` is the
// initial payload chunk after framing removal.
//
// The driver handles size correction, the overlap buffer, the feed loop, and all
// teardown ordering. It does NOT save logs or reset the installer pointer — the
// transport owns the StreamInstaller's lifetime and does that after this returns.
DriveResult drive(StreamInstaller& inst,
                  const StreamSource& src,
                  const FirstChunk& first,
                  uint64_t declared_size,
                  bool size_exact,
                  const WireSink& wire);

} // namespace Install
