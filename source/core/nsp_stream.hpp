#pragma once
// source/core/nsp_stream.hpp
//
// A title presented as a sequential byte stream of a complete NSP (PFS0), built on
// the fly from the installed NCAs. This is what lets a PC client download an
// installed game over FTP/MTP/HTTP the way DBI does — no temporary file, nothing
// buffered, a 15 GB title costs a few MB of RAM.
//
// WHY A STREAM: FTP RETR, MTP GetObject and HTTP GET all read sequentially from
// offset 0, so one pull interface serves every transport. Random access is
// deliberately NOT supported — a virtual NSP is for downloading, not seeking.
//
// THREADING: an instance is owned and used by ONE transport worker thread. It
// holds its ncm sessions open for its lifetime, which is the same shape as the
// installer (which already does ncm work on the transport thread successfully).
// This is NOT the pattern that crashed the title listing — that was 126 rapid
// session open/close pairs plus decryption, in a loop, racing the main thread.
//
// The SD-card dump (Core::Dump::dump_title_to_nsp) is a thin wrapper over this, so
// there is exactly ONE implementation of NSP construction to keep correct.

#include "core/keys.hpp"
#include "core/ncm.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Core {
namespace NspStream {

class Source;

/// Open a title as an NSP byte stream. Returns null on failure (with `error` set).
/// `total_size()` is exact and known up front — it is what a transport advertises
/// as the file size, so it must be computed before the first byte is sent.
std::unique_ptr<Source> open(const Ncm::Title& title,
                             const Keys::Keyset& keys,
                             std::string* error = nullptr);

class Source {
public:
    virtual ~Source() = default;

    /// Exact total byte count of the NSP this will produce.
    virtual uint64_t total_size() const = 0;

    /// Read the next chunk sequentially. Returns bytes written into `buf` (0 at
    /// end of stream), or a negative value on a read error. Short reads are normal
    /// — callers loop until 0 or total_size() bytes have been consumed.
    virtual int64_t read(void* buf, size_t len) = 0;

    /// Bytes handed out so far.
    virtual uint64_t position() const = 0;

    /// Progress introspection, so a UI can show "file 3 of 7 — abcd.nca". The SD
    /// dump reports these; a transport ignores them.
    /// Human-readable note about ticket/certificate handling ("n/a" for standard
    /// crypto). Worth surfacing: a titlekey title without its ticket will not
    /// install, and that is invisible otherwise.
    virtual std::string note() const = 0;

    virtual size_t      file_count()     const = 0;
    virtual size_t      current_index()  const = 0;
    virtual std::string current_name()   const = 0;
};

} // namespace NspStream
} // namespace Core
