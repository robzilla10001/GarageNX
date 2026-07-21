// source/install/stream_installer.cpp

#include "install/stream_installer.hpp"

#include "install/ncz.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <atomic>       // abort() idempotency guard

#ifdef PLATFORM_SWITCH
#include <SDL2/SDL.h>
#endif

namespace Install {

namespace {

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

constexpr size_t kPfs0HeaderSize = 0x10;
constexpr size_t kPfs0EntrySize  = 0x18;

// ── XCI / HFS0 ───────────────────────────────────────────────────────────────
// An HFS0 header has the same shape as a PFS0's (magic, count, string-table
// size) but its entries are 0x40 rather than 0x18: the extra space is a
// SHA-256, a hash region size, and reserved bytes. These figures and the XCI
// header offsets below match xci_reader.cpp, which is hardware-validated.
constexpr size_t   kHfs0HeaderSize = 0x10;
constexpr size_t   kHfs0EntrySize  = 0x40;
constexpr uint64_t kXciMagicOff    = 0x100;   // "HEAD"
constexpr size_t   kXciHeadLen     = 0x38;    // 0x100..0x137 — magic at +0, root offset at +0x30
constexpr size_t   kXciRootOffAt   = 0x30;    // 0x130, relative to the collection at 0x100
constexpr uint32_t kMaxHfs0Files   = 1024;    // matches XciReader::parse_hfs0

// A string table holds `count` NUL-terminated names and nothing else, so it
// cannot legitimately exceed count * (255 + 1) — 255 being the conventional
// filename ceiling, and every name that actually appears here (a 32-hex NCA id
// plus a suffix, or a partition name like "secure") far shorter than that.
//
// Both entry counts are already bounded above, so deriving the string table's
// bound from the count bounds the whole allocation. Without it a host-chosen
// string-table size of 0xFFFFFFFF reserves 4 GiB and takes the process out via
// bad_alloc — a crash, on a console, from a bad file. Deriving rather than
// picking a flat constant means a three-file container gets a three-file bound.
constexpr uint32_t kMaxNameBytes = 256;

/// Case-insensitive suffix test. The MTP host controls the filename's case, and
/// a host that sends GAME.XCI must not be routed to the PFS0 front-end.
bool ends_with_ci(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++) {
        const char a = (char)tolower((unsigned char)s[s.size() - n + i]);
        if (a != suffix[i]) return false;
    }
    return true;
}

} // namespace

StreamInstaller::Format StreamInstaller::format_from_name(const std::string& name) {
    if (ends_with_ci(name, ".xci") || ends_with_ci(name, ".xcz")) return Format::Xci;
    return Format::Pfs0;
}

StreamInstaller::StreamInstaller(Core::Ncm::Storage storage,
                                 const Core::Keys::Keyset& keys,
                                 Progress& progress)
    : m_storage(storage), m_keys(keys), m_progress(progress) {}

StreamInstaller::~StreamInstaller() { abort(); }

bool StreamInstaller::fail(const std::string& why) {
    m_phase = Phase::Failed;
    m_error = why;
    m_progress.message = why;
    m_progress.push_log("ERROR: " + why);
    return false;
}

bool StreamInstaller::begin(const std::string& filename, uint64_t total_size) {
    m_filename = filename;
    m_total    = total_size;
    m_pos      = 0;
    m_cur      = 0;
    m_entry_open = false;
    m_blob.clear();
    m_entries.clear();
    m_small.clear();
    m_container_size = 0;

    m_progress.reset();
    m_progress.running = true;
    m_progress.bytes_total = total_size;
    m_progress.push_log("Stream install: " + filename);
    m_progress.push_log(std::string("Target: ") +
        (m_storage == Core::Ncm::Storage::SdCard ? "SD card" : "internal (NAND)"));

    // Arm the first collection. A stream carries no format marker before its
    // first bytes, so the front-end is chosen from the name — the MTP gate has
    // already established that the extension is one we accept. An unrecognised
    // name falls to PFS0, which then fails on the magic, exactly as before.
    m_format = format_from_name(filename);
    if (m_format == Format::Xci) {
        m_progress.push_log("Container: XCI/XCZ (gamecard image)");
        if (!want(kXciMagicOff, kXciHeadLen, Step::XciHead)) return false;
    } else {
        if (!want(0, kPfs0HeaderSize, Step::Pfs0Header)) return false;
    }

#ifdef PLATFORM_SWITCH
    const NcmStorageId sid = (m_storage == Core::Ncm::Storage::SdCard)
                           ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    if (R_FAILED(ncmOpenContentStorage(&m_cs, sid)))
        return fail("Could not open destination content storage");
    m_have_cs = true;
#endif
    return true;
}

// ─── The collector ───────────────────────────────────────────────────────────
//
// One rule: collect N bytes at absolute offset X, then run step S, discarding
// everything in between. PFS0 is two contiguous steps; XCI (4c) is four
// scattered ones over the same machinery.

bool StreamInstaller::want(uint64_t off, size_t len, Step step) {
    // A stream cannot rewind. Asking for bytes that have already gone past is a
    // caller bug — a step that computed an offset wrongly — not a runtime
    // condition, so it is refused here rather than silently mis-collected.
    if (off < m_pos)
        return fail("Internal: collector asked to rewind to an earlier offset");

    m_want_off = off;
    m_want_len = len;
    m_step     = step;
    m_blob.clear();
    m_blob.reserve(len);
    m_phase = Phase::Collect;
    return true;
}

void StreamInstaller::classify(StreamEntry& se) {
    se.is_ncz      = ends_with(se.name, ".ncz");
    se.is_nca      = ends_with(se.name, ".nca") || se.is_ncz;
    se.is_cnmt_nca = ends_with(se.name, ".cnmt.nca") || ends_with(se.name, ".cnmt.ncz");
    se.is_tik      = ends_with(se.name, ".tik");
    se.is_cert     = ends_with(se.name, ".cert");
}

bool StreamInstaller::run_step() {
    switch (m_step) {
        case Step::Pfs0Header:      return parse_pfs0_header();
        case Step::Pfs0Table:       return parse_pfs0_table();
        case Step::XciHead:         return parse_xci_head();
        case Step::XciRootHeader:   return parse_xci_root_header();
        case Step::XciRootTable:    return parse_xci_root_table();
        case Step::XciSecureHeader: return parse_xci_secure_header();
        case Step::XciSecureTable:  return parse_xci_secure_table();
    }
    return fail("Internal: unknown collector step");
}

// ─── PFS0 front-end ──────────────────────────────────────────────────────────

bool StreamInstaller::parse_pfs0_header() {
    if (std::memcmp(m_blob.data(), "PFS0", 4) != 0)
        return fail("Not an NSP (bad PFS0 magic)");

    // Stashed, not re-read later: by the time the table is parsed, m_blob holds
    // the table, not this header. See the members' comment in the header file.
    m_ent_count = rd32(m_blob.data() + 4);
    m_str_size  = rd32(m_blob.data() + 8);
    if (m_ent_count == 0 || m_ent_count > 4096)
        return fail("PFS0: implausible file count");

    // Both fields are host-supplied and feed straight into a reserve(). See
    // kMaxNameBytes: this is the only thing standing between a corrupt NSP and
    // a 4 GiB allocation.
    if (m_str_size > (uint64_t)m_ent_count * kMaxNameBytes)
        return fail("PFS0: implausible string table size");

    // The table follows the header immediately.
    const size_t table_size = (size_t)m_ent_count * kPfs0EntrySize + m_str_size;
    return want(kPfs0HeaderSize, table_size, Step::Pfs0Table);
}

bool StreamInstaller::parse_pfs0_table() {
    const uint32_t count    = m_ent_count;
    const uint32_t sts      = m_str_size;
    const size_t   names_at = (size_t)count * kPfs0EntrySize;

    // Data begins right after the header, entry table and string table — which
    // is exactly where this collection ends.
    const uint64_t data_start = m_want_off + m_want_len;

    m_entries.clear();
    m_entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = m_blob.data() + (size_t)i * kPfs0EntrySize;
        StreamEntry se;
        se.abs_off = data_start + rd64(e);
        se.size    = rd64(e + 8);
        const uint32_t noff = rd32(e + 16);
        if (noff >= sts) return fail("PFS0: name offset out of range");

        // The string table is host-supplied: scan for the terminator without
        // running past the table (strnlen is not available on this toolchain).
        const char*  np     = (const char*)m_blob.data() + names_at + noff;
        const size_t maxlen = sts - noff;
        size_t nlen = 0;
        while (nlen < maxlen && np[nlen] != '\0') nlen++;
        se.name.assign(np, nlen);
        if (se.name.empty()) return fail("PFS0: empty filename");

        classify(se);
        m_entries.push_back(std::move(se));
    }

    // A PFS0's last entry ends at the file's end, so its table yields the exact
    // container size. This is the one part of the tail that is format-specific:
    // an XCI continues past `secure` into gamecard padding and must report 0.
    uint64_t csz = data_start;
    for (const auto& e : m_entries)
        if (e.abs_end() > csz) csz = e.abs_end();

    return finalize_entries(csz);
}

// ─── XCI front-end (slice 4c) ────────────────────────────────────────────────
//
// Five collections, all forward, everything between them discarded by
// Phase::Collect as it passes: the RSA signature, the rest of the gamecard
// header, and the update/normal partitions all drop on the floor for free.
//
// Offsets follow xci_reader.cpp's parse(), which is hardware-validated. Note
// that the root HFS0 offset is at 0x130 and NOT at 0x120, which is the gamecard
// IV — an unreferenced struct in xci_reader.cpp documented 0x120 for a long
// time; slice 4c deleted it. If you are checking this against a reference,
// check it against parse(), not against anything's field list.

bool StreamInstaller::parse_xci_head() {
    if (std::memcmp(m_blob.data(), "HEAD", 4) != 0)
        return fail("Not an XCI (bad HEAD magic at 0x100)");

    const uint64_t root_off = rd64(m_blob.data() + kXciRootOffAt);

    // Host-supplied. want() would already refuse a backwards offset, but a wild
    // forward one is worse: it would skip silently to the end of a multi-GB
    // transfer before failing. m_total is trustworthy here because the MTP gate
    // refuses an XCI whose size the host could not declare exactly.
    if (m_total > 0 && root_off >= m_total)
        return fail("XCI: root HFS0 offset lies past the end of the image");

    m_progress.push_log("XCI: root HFS0 at 0x" + [&]{
        char b[24]; std::snprintf(b, sizeof(b), "%llX", (unsigned long long)root_off); return std::string(b);
    }());
    return want(root_off, kHfs0HeaderSize, Step::XciRootHeader);
}

bool StreamInstaller::parse_hfs0_header(Step table_step, const char* what) {
    if (std::memcmp(m_blob.data(), "HFS0", 4) != 0)
        return fail(std::string("XCI: bad HFS0 magic in the ") + what + " partition");

    m_ent_count = rd32(m_blob.data() + 4);
    m_str_size  = rd32(m_blob.data() + 8);
    if (m_ent_count == 0 || m_ent_count > kMaxHfs0Files)
        return fail(std::string("XCI: implausible file count in the ") + what + " partition");

    // As on the PFS0 path — see kMaxNameBytes.
    if (m_str_size > (uint64_t)m_ent_count * kMaxNameBytes)
        return fail(std::string("XCI: implausible string table size in the ") + what + " partition");

    // count is bounded above, so this cannot overflow a 64-bit width; the cast
    // to size_t below is safe for the same reason.
    const uint64_t table_size = (uint64_t)m_ent_count * kHfs0EntrySize + m_str_size;

    // The table follows its header immediately.
    return want(m_want_off + kHfs0HeaderSize, (size_t)table_size, table_step);
}

bool StreamInstaller::decode_hfs0_table(uint64_t data_start, std::vector<StreamEntry>& out) {
    const uint32_t count    = m_ent_count;
    const uint32_t sts      = m_str_size;
    const size_t   names_at = (size_t)count * kHfs0EntrySize;

    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        // HFS0 entry: u64 data_offset, u64 data_size, u32 name_offset,
        // u32 hash_region_size, u64 reserved, u8[0x20] sha256.
        const uint8_t* e = m_blob.data() + (size_t)i * kHfs0EntrySize;
        StreamEntry se;
        se.abs_off = data_start + rd64(e);
        se.size    = rd64(e + 8);
        const uint32_t noff = rd32(e + 16);
        if (noff >= sts) return fail("XCI: name offset out of range");

        // Same reasoning as the PFS0 path: the string table is host-supplied,
        // so scan for the terminator without running past the table.
        const char*  np     = (const char*)m_blob.data() + names_at + noff;
        const size_t maxlen = sts - noff;
        size_t nlen = 0;
        while (nlen < maxlen && np[nlen] != '\0') nlen++;
        se.name.assign(np, nlen);
        if (se.name.empty()) return fail("XCI: empty filename");

        classify(se);
        out.push_back(std::move(se));
    }
    return true;
}

bool StreamInstaller::parse_xci_root_header() {
    return parse_hfs0_header(Step::XciRootTable, "root");
}

bool StreamInstaller::parse_xci_root_table() {
    // The root partition table's data region begins where this collection ends.
    const uint64_t root_data_start = m_want_off + m_want_len;

    std::vector<StreamEntry> parts;
    if (!decode_hfs0_table(root_data_start, parts)) return false;

    const StreamEntry* secure = nullptr;
    for (const auto& p : parts)
        if (p.name == "secure") { secure = &p; break; }
    if (!secure)
        return fail("XCI: no 'secure' partition in the root HFS0");

    // secure->abs_off is already absolute and >= root_data_start >= m_pos, so
    // this is always a forward seek — update/normal, if they precede it,
    // discard themselves in Phase::Collect.
    m_secure_abs = secure->abs_off;
    m_progress.push_log("XCI: " + std::to_string(parts.size()) +
                        " partition(s); secure is " + std::to_string(secure->size) + " bytes");
    return want(m_secure_abs, kHfs0HeaderSize, Step::XciSecureHeader);
}

bool StreamInstaller::parse_xci_secure_header() {
    return parse_hfs0_header(Step::XciSecureTable, "secure");
}

bool StreamInstaller::parse_xci_secure_table() {
    const uint64_t data_start = m_want_off + m_want_len;

    if (!decode_hfs0_table(data_start, m_entries)) return false;

    // container_size() is 0 for XCI, deliberately. A PFS0's last entry ends at
    // the file's end, so its table yields an exact size; a gamecard image
    // continues past `secure` into padding that belongs to the transfer but to
    // no entry. Inferring a length from the entries would come up short, which
    // does not fail an install — it leaves unread bytes in the endpoint and
    // desyncs the session. The host's declared size is the only authority, and
    // the MTP gate refuses an XCI whose size the host cannot declare exactly.
    return finalize_entries(0);
}


// ─── Format-independent tail ─────────────────────────────────────────────────

bool StreamInstaller::finalize_entries(uint64_t container_size) {
    // The stream can only go forwards, so the entries must be walked in the
    // order their bytes actually arrive — the table is not required to be sorted.
    std::sort(m_entries.begin(), m_entries.end(),
              [](const StreamEntry& a, const StreamEntry& b) { return a.abs_off < b.abs_off; });

    // .ncz entries are handled by the NczWindow path in begin_entry(); the 4a
    // blanket rejection is gone. Keys are NOT ambient — an NSZ cannot be
    // decompressed without them, so refuse up front rather than partway through
    // a multi-gigabyte transfer.
    bool has_ncz = false;
    for (const auto& e : m_entries)
        if (e.is_ncz) { has_ncz = true; break; }
    if (has_ncz && !m_keys.has_header_key) {
        // has_header_key on the keyset we were HANDED, rather than the global
        // Core::Keys::available(): that predicate answers for whatever load()
        // last cached, which is not necessarily this reference. header_key is
        // the actual precondition — get_decompressed_size() decrypts the NCA
        // header to recover the size for a stream NCZ.
        //
        // Name the container the user actually sent. Both formats reach here
        // by the same route — a compressed entry with no key to read it — but
        // a message that says NSZ to someone installing an XCZ reads as a bug
        // in us, and this text is the only thing they will see.
        const char* what = (m_format == Format::Xci) ? "XCZ" : "NSZ";
        const std::string why = Core::Keys::requirement_message();
        return fail(why.empty()
            ? std::string(what) + " install needs prod.keys (header_key) — none are loaded"
            : std::string(what) + " install needs prod.keys: " + why);
    }

    m_container_size = container_size;

    int ncas = 0;
    for (const auto& e : m_entries) if (e.is_nca) ncas++;
    m_progress.ncas_total = ncas;
    m_progress.push_log("Container has " + std::to_string(m_entries.size()) +
                        " file(s), " + std::to_string(ncas) + " NCA(s)");
    m_progress.stage = "installing";
    m_phase = Phase::Data;
    return true;
}

// ─── Per-entry streaming ─────────────────────────────────────────────────────

bool StreamInstaller::begin_entry() {
    StreamEntry& e = m_entries[m_cur];
    m_entry_skip = false;
    m_tee.clear();

#ifndef PLATFORM_SWITCH
    // Off-device there is no ncm, but the window and worker still run so the
    // decompression path stays exercisable.
    if (e.is_ncz && !ncz_begin()) return false;
#endif

#ifdef PLATFORM_SWITCH
    if (e.is_nca) {
        if (!content_id_from_name(e.name, m_cid))
            return fail("Bad NCA filename: " + e.name);

        bool has = false;
        if (R_SUCCEEDED(ncmContentStorageHas(&m_cs, &has, &m_cid)) && has) {
            m_entry_skip = true;   // already installed; still tee it if small
            m_progress.push_log("[skip] " + e.name + " already installed");
        } else if (e.is_ncz) {
            // The placeholder must be sized to the DECOMPRESSED NCA, which is
            // only knowable after get_decompressed_size() has read the NCZ
            // header — and not a byte of it has arrived yet. So creation moves
            // to the worker, which blocks in NczWindow::read() until it has
            // enough. Nothing is created on this thread for an .ncz entry.
            char lb[160];
            std::snprintf(lb, sizeof(lb), "[%d/%d] %s (%.1f MB compressed)",
                          m_progress.ncas_done.load() + 1, m_progress.ncas_total.load(),
                          e.name.c_str(), (double)e.size / (1024.0 * 1024.0));
            m_progress.push_log(lb);
            if (!ncz_begin()) return false;
        } else {
            if (R_FAILED(ncmContentStorageGeneratePlaceHolderId(&m_cs, &m_ph)))
                return fail("GeneratePlaceHolderId failed");
            ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
            if (R_FAILED(ncmContentStorageCreatePlaceHolder(&m_cs, &m_cid, &m_ph, (s64)e.size)))
                return fail("CreatePlaceHolder failed for " + e.name);
            m_ph_open = true;
            m_ph_off  = 0;
            char lb[160];
            std::snprintf(lb, sizeof(lb), "[%d/%d] %s (%.1f MB)",
                          m_progress.ncas_done.load() + 1, m_progress.ncas_total.load(),
                          e.name.c_str(), (double)e.size / (1024.0 * 1024.0));
            m_progress.push_log(lb);
        }
    }
#endif
    m_entry_open = true;
    return true;
}

bool StreamInstaller::write_entry(const uint8_t* data, size_t len) {
    StreamEntry& e = m_entries[m_cur];

    // An .ncz is pushed into the window instead of written through: the worker
    // pulls from it, decompresses, and writes the placeholder itself. push()
    // blocks while the window is full, which is how a slow decompressor applies
    // back-pressure to USB.
    if (e.is_ncz && !m_entry_skip) {
        if (!m_ncz_win || !m_ncz_win->push(data, len)) {
            ncz_join(false);
            return fail(m_ncz_error.empty() ? "NSZ: decompression stopped" : m_ncz_error);
        }
    }
#ifdef PLATFORM_SWITCH
    else if (e.is_nca && !m_entry_skip) {
        if (R_FAILED(ncmContentStorageWritePlaceHolder(&m_cs, &m_ph, m_ph_off,
                                                       (void*)data, len)))
            return fail("WritePlaceHolder failed for " + e.name);
        m_ph_off += len;
    }
#endif

    // The small entries are what install() will read back from RAM. For a
    // .cnmt.ncz these are the COMPRESSED bytes — correct, because finish() sets
    // ce.is_ncz and install() decompresses it on the way in, exactly as it does
    // for a local NSZ. It needs random access, and RAM gives it that.
    if (e.is_cnmt_nca || e.is_tik || e.is_cert)
        m_tee.insert(m_tee.end(), data, data + len);

    // Container bytes, not decompressed bytes: bytes_total is the container size.
    m_progress.bytes_done.fetch_add(len);
    return true;
}

bool StreamInstaller::end_entry() {
    StreamEntry& e = m_entries[m_cur];

    // Every byte is in: close the window so the worker sees end-of-stream, then
    // join it. The join must complete before Register touches the placeholder.
    if (e.is_ncz && !m_entry_skip) {
        ncz_join(true);
        if (!m_ncz_error.empty()) return fail(m_ncz_error);
    }

#ifdef PLATFORM_SWITCH
    if (e.is_nca && !m_entry_skip) {
        if (R_FAILED(ncmContentStorageRegister(&m_cs, &m_cid, &m_ph))) {
            ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
            m_ph_open = false;
            return fail("Register failed for " + e.name);
        }
        m_ph_open = false;   // ownership passed to ncm
        m_progress.push_log("      registered");
    }
    if (e.is_nca) m_progress.ncas_done.fetch_add(1);
#endif

    if (e.is_cnmt_nca || e.is_tik || e.is_cert)
        m_small.emplace_back(e.name, std::move(m_tee));
    m_tee.clear();

    m_entry_open = false;
    m_entry_skip = false;
    return true;
}

size_t StreamInstaller::feed_data(const uint8_t* data, size_t len) {
    // Past the last entry: trailing padding, nothing left to do.
    if (m_cur >= m_entries.size()) { m_phase = Phase::Done; return len; }

    StreamEntry& e = m_entries[m_cur];

    // Gap/alignment padding before this entry's data starts.
    if (m_pos < e.abs_off) {
        const uint64_t gap = e.abs_off - m_pos;
        return (size_t)std::min<uint64_t>(gap, len);
    }

    if (!m_entry_open && !begin_entry()) return 0;

    const uint64_t left = e.abs_end() - m_pos;
    const size_t   take = (size_t)std::min<uint64_t>(left, len);
    if (take > 0 && !write_entry(data, take)) return 0;

    if (m_pos + take >= e.abs_end()) {
        if (!end_entry()) return 0;
        m_cur++;
        if (m_cur >= m_entries.size()) m_phase = Phase::Done;
    }
    return take;
}

bool StreamInstaller::feed(const uint8_t* data, size_t len) {
    while (len > 0) {
        switch (m_phase) {
            case Phase::Collect: {
                // Bytes before the window belong to no step: drop them as they
                // pass. PFS0's two collections are contiguous so this never
                // fires; XCI's are not, and it is how the gamecard header, the
                // root HFS0 and the update/normal partitions discard themselves.
                if (m_pos < m_want_off) {
                    const size_t drop =
                        (size_t)std::min<uint64_t>(m_want_off - m_pos, len);
                    data += drop; len -= drop; m_pos += drop;
                    break;
                }
                const size_t take = std::min(m_want_len - m_blob.size(), len);
                m_blob.insert(m_blob.end(), data, data + take);
                data += take; len -= take; m_pos += take;
                if (m_blob.size() == m_want_len && !run_step()) return false;
                break;
            }
            case Phase::Data: {
                const size_t used = feed_data(data, len);
                if (used == 0) return false;   // feed_data already recorded why
                data += used; len -= used; m_pos += used;
                break;
            }
            case Phase::Done:
                return true;   // ignore any trailing bytes
            case Phase::Failed:
                return false;
        }
    }
    return m_phase.load() != Phase::Failed;
}

// ─── Finish: hand off to the validated installer ─────────────────────────────

bool StreamInstaller::finish() {
    if (m_phase.load() == Phase::Failed) return false;
    if (m_phase.load() != Phase::Done && m_phase.load() != Phase::Data)
        return fail("Transfer ended before the container was complete");

    m_progress.push_log("Transfer complete; registering title...");

    // Rebuild the entry list for install(). Every NCA is already registered, so
    // install() takes its "already installed" path and never calls read() on
    // them; only the small entries are actually read, and those come from RAM.
    std::vector<ContentEntry> contents;
    contents.reserve(m_entries.size());
    for (const auto& e : m_entries) {
        ContentEntry ce;
        ce.name        = e.name;
        ce.size        = e.size;
        ce.is_nca      = e.is_nca;
        ce.is_cnmt_nca = e.is_cnmt_nca;
        ce.is_tik      = e.is_tik;
        ce.is_cert     = e.is_cert;
        ce.is_ncz      = e.is_ncz;

        const std::vector<uint8_t>* bytes = nullptr;
        for (const auto& s : m_small)
            if (s.first == e.name) { bytes = &s.second; break; }

        if (bytes) {
            auto shared = std::make_shared<std::vector<uint8_t>>(*bytes);
            ce.read = [shared](uint64_t off, void* dst, size_t n) -> size_t {
                if (off >= shared->size()) return 0;
                const size_t avail = (size_t)(shared->size() - off);
                const size_t take  = std::min(n, avail);
                std::memcpy(dst, shared->data() + off, take);
                return take;
            };
        } else {
            // A large NCA: install() must never read this. If it somehow tries,
            // return 0 so it fails loudly rather than writing garbage.
            ce.read = [](uint64_t, void*, size_t) -> size_t { return 0; };
        }
        contents.push_back(std::move(ce));
    }

#ifdef PLATFORM_SWITCH
    // install() opens its own handle to the same storage.
    if (m_have_cs) { ncmContentStorageClose(&m_cs); m_have_cs = false; }
#endif

    // contents_preregistered: every large NCA is already in NCM — this call has
    // only metadata and tickets left. It also stops install() from resetting
    // m_progress and erasing the log of the transfer we just completed.
    return install(std::move(contents), m_storage, m_keys, m_progress, true);
}

void StreamInstaller::abort() {
    // Teardown runs on two threads: the MTP worker (recv_install's explicit
    // m_install->abort()) and, ultimately, whichever thread destroys the
    // installer (~StreamInstaller calls abort()). The real cross-thread safety
    // comes from ~MtpServer joining the worker before members are destroyed
    // (see mtp_server.hpp) — that is what stops the two aborts overlapping. The
    // guard and ordering below make abort() correct and idempotent regardless.
    //
    // Thread teardown always runs and is NOT guarded: ncz_join() must close the
    // worker's Thread handle on every call or a cancelled NSZ leaks a kernel
    // handle. ncz_join() is idempotent (early-returns when nothing is running),
    // so a second call — e.g. from the destructor after an explicit abort — is a
    // safe no-op.
    ncz_join(false);

    // The ncm teardown must run exactly once: a second DeletePlaceHolder/Close on
    // already-freed ncm state faults. exchange() is a single atomic check-and-set
    // — if it returns true, another call already claimed the teardown.
    if (m_aborted.exchange(true)) { m_entry_open = false; return; }

#ifdef PLATFORM_SWITCH
    // m_ph_open is the authority: on the NSZ path the worker — not this thread —
    // creates the placeholder, and it may or may not have got that far.
    if (m_ph_open) {
        ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
        m_ph_open = false;
    }
    if (m_have_cs) {
        ncmContentStorageClose(&m_cs);
        m_have_cs = false;
    }
#endif
    m_entry_open = false;
}

// ─── NSZ decompression worker (slice 4b) ─────────────────────────────────────

bool StreamInstaller::ncz_begin() {
    m_ncz_error.clear();
    m_ncz_win = std::make_unique<NczWindow>();

#ifdef PLATFORM_SWITCH
    // Same priority/core as OverlapBuffer's drain worker. Stack is 0x10000
    // rather than that worker's 0x8000: zstd keeps its context and buffers on
    // the heap, but this frame also carries the section table, the recryptor and
    // several std::function trampolines, and an overflow here would corrupt a
    // neighbouring thread rather than fault cleanly.
    if (R_FAILED(threadCreate(&m_ncz_thread, &StreamInstaller::ncz_thread_entry, this,
                              nullptr, 0x10000, 0x2C, -2)))
        return fail("NSZ: could not create the decompression thread");
    if (R_FAILED(threadStart(&m_ncz_thread))) {
        threadClose(&m_ncz_thread);
        return fail("NSZ: could not start the decompression thread");
    }
#else
    m_ncz_thread = std::thread([this] { ncz_worker(); });
#endif
    m_ncz_running = true;
    return true;
}

#ifdef PLATFORM_SWITCH
void StreamInstaller::ncz_thread_entry(void* self) {
    static_cast<StreamInstaller*>(self)->ncz_worker();
}
#endif

void StreamInstaller::ncz_join(bool graceful) {
    if (!m_ncz_running) return;

    if (m_ncz_win) {
        // finish(): all bytes are in, let the worker read to the end.
        // abort():  give up; unblocks a worker parked in read().
        if (graceful) m_ncz_win->finish();
        else          m_ncz_win->abort("transfer aborted");
    }

#ifdef PLATFORM_SWITCH
    threadWaitForExit(&m_ncz_thread);
    threadClose(&m_ncz_thread);
#else
    if (m_ncz_thread.joinable()) m_ncz_thread.join();
#endif

    m_ncz_running = false;
    // Only now is it safe to drop the window: the worker held a raw pointer to it.
    m_ncz_win.reset();
}

void StreamInstaller::ncz_worker() {
    // m_cur is stable for this worker's whole life: the MTP thread only advances
    // it in feed_data() after end_entry(), which joins first.
    const StreamEntry& e = m_entries[m_cur];
    const uint64_t compressed_size = e.size;   // the entry's size ON THE WIRE

    ReadFn read = [this](uint64_t off, void* buf, size_t len) -> size_t {
        return m_ncz_win->read(off, buf, len);
    };

    // Blocks until the MTP thread has pushed the header region. This is the call
    // that forces the retained prefix: it reads 0x4000, then seeks back to 0.
    const uint64_t dec_size = NczDecompressor::get_decompressed_size(read, compressed_size, m_keys);
    if (dec_size == 0) {
        m_ncz_error = "NSZ: could not read the NCZ header of " + e.name +
                      " (truncated, not an NCZ, or wrong keys)";
        m_ncz_win->abort(m_ncz_error);
        return;
    }

    char lb[128];
    std::snprintf(lb, sizeof(lb), "      decompressing to %.1f MB",
                  (double)dec_size / (1024.0 * 1024.0));
    m_progress.push_log(lb);   // push_log takes its own lock; safe from here

#ifdef PLATFORM_SWITCH
    // Sized to the DECOMPRESSED NCA — the reconstructed NCA is byte-identical to
    // the original, so its SHA-256 must match content_id. Sizing this to the
    // compressed length would fail on the first write past the end.
    if (R_FAILED(ncmContentStorageGeneratePlaceHolderId(&m_cs, &m_ph))) {
        m_ncz_error = "GeneratePlaceHolderId failed";
        m_ncz_win->abort(m_ncz_error);
        return;
    }
    ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
    if (R_FAILED(ncmContentStorageCreatePlaceHolder(&m_cs, &m_cid, &m_ph, (s64)dec_size))) {
        m_ncz_error = "CreatePlaceHolder failed for " + e.name;
        m_ncz_win->abort(m_ncz_error);
        return;
    }
    m_ph_open = true;   // read by abort()/end_entry() only after the join
#endif

    // write_cb's offset is the ABSOLUTE NCA offset (ncz.cpp delivers the 0x4000
    // verbatim header at 0, then body chunks at their true offsets), so it maps
    // straight onto WritePlaceHolder with no cursor to keep.
    WriteCallback write_cb = [this](uint64_t nca_off, const uint8_t* data, size_t size) -> bool {
        // The UI cancel reaches the decompressor here. Returning false makes
        // decompress() unwind with an error rather than run to completion on a
        // transfer nobody wants.
        if (m_progress.cancel.load()) return false;
#ifdef PLATFORM_SWITCH
        return R_SUCCEEDED(ncmContentStorageWritePlaceHolder(
            &m_cs, &m_ph, nca_off, (void*)data, size));
#else
        (void)nca_off; (void)data; (void)size;
        return true;
#endif
    };

    const std::string err = NczDecompressor::decompress(read, compressed_size, m_keys, write_cb);
    if (!err.empty()) {
        m_ncz_error = err;
        // Unblock a producer parked in push() so the MTP thread notices.
        m_ncz_win->abort(err);
    }
}

} // namespace Install
