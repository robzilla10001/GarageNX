// source/install/stream_driver.cpp

#include "install/stream_driver.hpp"
#include "services/overlap_buffer.hpp"

#include <algorithm>

namespace Install {

DriveResult drive(StreamInstaller& inst,
                  const StreamSource& src,
                  const FirstChunk& first,
                  uint64_t declared_size,
                  bool size_exact,
                  const WireSink& wire) {

    static constexpr uint64_t kUnknown = UINT64_MAX;

    // ── Resolve the payload size ────────────────────────────────────────────
    // Precedence, highest first:
    //   1. A real 64-bit declaration from the transport (size_exact) — the only
    //      value that describes the TRANSFER rather than the container contents.
    //   2. The container's own PFS0/HFS0 table, exact and 64-bit, known once the
    //      first chunk has been parsed by the installer.
    //   3. The transport's declared size, even if it may be a 32-bit cap.
    // (3) is the starting guess; (1)/(2) refine it below.
    uint64_t payload = (declared_size == 0 || declared_size == 0xFFFFFFFFu)
                           ? kUnknown : declared_size;
    if (size_exact && declared_size > 0) payload = declared_size;

    // ── Feed the first (already de-framed) chunk ────────────────────────────
    uint64_t fed = 0;
    {
        size_t take = first.size;
        if (payload != kUnknown && take > payload) take = (size_t)payload;
        if (take > 0 && !inst.feed(first.data, take)) {
            src.drain(payload != kUnknown && payload > take ? payload - take : 0);
            return DriveResult::FeedError;
        }
        fed += take;
        wire.add_recv(take);
    }

    // Now that the installer has seen the header, the container table may give a
    // better (or the only) size. The exact declaration still wins if present.
    if (!size_exact && inst.container_size() > 0) {
        payload = inst.container_size();
    }
    if (payload != kUnknown) wire.set_size(payload);

    // ── Overlap reads with placeholder writes ───────────────────────────────
    // The sink is inst.feed(); flush() fences it before finish() runs. feed() is
    // only touched by the overlap worker within this call, so no extra locking.
    Services::OverlapBuffer ov(src.buffer_size, [&](const uint8_t* p, size_t n) {
        return inst.feed(p, n);
    });

    while ((payload == kUnknown || fed < payload) && !src.stop()) {
        uint8_t* dst = ov.valid() ? ov.acquire() : src.buffer;
        if (!dst) break;                                    // installer failed

        const ssize_t r = src.read(dst, src.buffer_size);
        if (r < 0) break;                                   // read error
        if (r == 0) break;                                  // EOF / ZLP ends data
        size_t got = (size_t)r;

        // Late size discovery: no declaration, table now known.
        if (payload == kUnknown && inst.container_size() > 0) {
            payload = inst.container_size();
            wire.set_size(payload);
        }

        size_t take = got;
        if (payload != kUnknown && fed + take > payload)
            take = (size_t)(payload - fed);                 // ignore ZLP/padding

        if (ov.valid() && !ov.submit(take)) {
            ov.quiesce();
            inst.abort();
            src.drain(payload != kUnknown && payload > fed ? payload - fed : 0);
            return DriveResult::FeedError;
        }
        if (!ov.valid() && !inst.feed(dst, take)) {
            ov.quiesce();
            inst.abort();
            src.drain(payload != kUnknown && payload > fed + take ? payload - fed - take : 0);
            return DriveResult::FeedError;
        }
        fed += take;
        wire.add_recv(take);

        if (inst.complete()) { payload = fed; break; }      // every entry consumed
    }

    // ── Teardown — ORDER IS LOAD-BEARING (see stream_driver.hpp) ─────────────
    // The overlap worker must be fully parked before abort() destroys anything
    // feed()/push() touches, or that's the cross-thread UAF from 4c. quiesce()
    // is idempotent and safe on the direct (ov invalid) path.
    const bool cancelled = src.stop();
    if (cancelled) {
        ov.quiesce();
        inst.abort();
        src.drain(payload != kUnknown && payload > fed ? payload - fed : 0);
        return DriveResult::Cancelled;
    }

    if (ov.valid() && !ov.flush()) {   // fence: worker done before finish()
        ov.quiesce();
        inst.abort();
        src.drain(payload != kUnknown && payload > fed ? payload - fed : 0);
        return DriveResult::FeedError;
    }

    if (payload != kUnknown && fed != payload) {
        inst.abort();
        return DriveResult::ShortRead;
    }

    return inst.finish() ? DriveResult::Ok : DriveResult::FinishError;
}

} // namespace Install
