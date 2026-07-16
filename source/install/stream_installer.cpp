// source/install/stream_installer.cpp

#include "install/stream_installer.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

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

} // namespace

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
    m_phase    = Phase::Header;
    m_pos      = 0;
    m_cur      = 0;
    m_entry_open = false;
    m_hdr.clear();
    m_table.clear();
    m_entries.clear();
    m_small.clear();

    m_progress.reset();
    m_progress.running = true;
    m_progress.bytes_total = total_size;
    m_progress.push_log("Stream install: " + filename);
    m_progress.push_log(std::string("Target: ") +
        (m_storage == Core::Ncm::Storage::SdCard ? "SD card" : "internal (NAND)"));

#ifdef PLATFORM_SWITCH
    const NcmStorageId sid = (m_storage == Core::Ncm::Storage::SdCard)
                           ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;
    if (R_FAILED(ncmOpenContentStorage(&m_cs, sid)))
        return fail("Could not open destination content storage");
    m_have_cs = true;
#endif
    return true;
}

// ─── PFS0 header ─────────────────────────────────────────────────────────────

bool StreamInstaller::parse_header() {
    if (std::memcmp(m_hdr.data(), "PFS0", 4) != 0)
        return fail("Not an NSP (bad PFS0 magic)");

    const uint32_t count = rd32(m_hdr.data() + 4);
    const uint32_t sts   = rd32(m_hdr.data() + 8);
    if (count == 0 || count > 4096) return fail("PFS0: implausible file count");

    m_table_size = (size_t)count * kPfs0EntrySize + sts;
    m_table.clear();
    m_table.reserve(m_table_size);
    m_phase = Phase::Table;
    return true;
}

bool StreamInstaller::parse_table() {
    const uint32_t count = rd32(m_hdr.data() + 4);
    const uint32_t sts   = rd32(m_hdr.data() + 8);
    const size_t   names_at = (size_t)count * kPfs0EntrySize;

    // Data begins right after the header, entry table and string table.
    const uint64_t data_start = kPfs0HeaderSize + m_table_size;

    m_entries.clear();
    m_entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = m_table.data() + (size_t)i * kPfs0EntrySize;
        StreamEntry se;
        se.abs_off = data_start + rd64(e);
        se.size    = rd64(e + 8);
        const uint32_t noff = rd32(e + 16);
        if (noff >= sts) return fail("PFS0: name offset out of range");

        // The string table is host-supplied: scan for the terminator without
        // running past the table (strnlen is not available on this toolchain).
        const char*  np     = (const char*)m_table.data() + names_at + noff;
        const size_t maxlen = sts - noff;
        size_t nlen = 0;
        while (nlen < maxlen && np[nlen] != '\0') nlen++;
        se.name.assign(np, nlen);
        if (se.name.empty()) return fail("PFS0: empty filename");

        se.is_ncz      = ends_with(se.name, ".ncz");
        se.is_nca      = ends_with(se.name, ".nca") || se.is_ncz;
        se.is_cnmt_nca = ends_with(se.name, ".cnmt.nca") || ends_with(se.name, ".cnmt.ncz");
        se.is_tik      = ends_with(se.name, ".tik");
        se.is_cert     = ends_with(se.name, ".cert");
        m_entries.push_back(std::move(se));
    }

    // The stream can only go forwards, so the entries must be walked in the
    // order their bytes actually arrive — the table is not required to be sorted.
    std::sort(m_entries.begin(), m_entries.end(),
              [](const StreamEntry& a, const StreamEntry& b) { return a.abs_off < b.abs_off; });

    // Slice 4a limitation: reject up front rather than half-installing.
    for (const auto& e : m_entries)
        if (e.is_ncz)
            return fail("NSZ/XCZ stream install is not supported yet — copy the file to the SD card and install it from the file browser");

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
    (void)e;   // only consulted on the Switch path below
    m_entry_skip = false;
    m_tee.clear();

#ifdef PLATFORM_SWITCH
    if (e.is_nca) {
        if (!content_id_from_name(e.name, m_cid))
            return fail("Bad NCA filename: " + e.name);

        bool has = false;
        if (R_SUCCEEDED(ncmContentStorageHas(&m_cs, &has, &m_cid)) && has) {
            m_entry_skip = true;   // already installed; still tee it if small
            m_progress.push_log("[skip] " + e.name + " already installed");
        } else {
            if (R_FAILED(ncmContentStorageGeneratePlaceHolderId(&m_cs, &m_ph)))
                return fail("GeneratePlaceHolderId failed");
            ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
            if (R_FAILED(ncmContentStorageCreatePlaceHolder(&m_cs, &m_cid, &m_ph, (s64)e.size)))
                return fail("CreatePlaceHolder failed for " + e.name);
            m_ph_off = 0;
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

#ifdef PLATFORM_SWITCH
    if (e.is_nca && !m_entry_skip) {
        if (R_FAILED(ncmContentStorageWritePlaceHolder(&m_cs, &m_ph, m_ph_off,
                                                       (void*)data, len)))
            return fail("WritePlaceHolder failed for " + e.name);
        m_ph_off += len;
    }
#endif
    // The small entries are what install() will read back from RAM.
    if (e.is_cnmt_nca || e.is_tik || e.is_cert)
        m_tee.insert(m_tee.end(), data, data + len);

    m_progress.bytes_done.fetch_add(len);
    return true;
}

bool StreamInstaller::end_entry() {
    StreamEntry& e = m_entries[m_cur];

#ifdef PLATFORM_SWITCH
    if (e.is_nca && !m_entry_skip) {
        if (R_FAILED(ncmContentStorageRegister(&m_cs, &m_cid, &m_ph))) {
            ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
            return fail("Register failed for " + e.name);
        }
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
            case Phase::Header: {
                const size_t take = std::min(kPfs0HeaderSize - m_hdr.size(), len);
                m_hdr.insert(m_hdr.end(), data, data + take);
                data += take; len -= take; m_pos += take;
                if (m_hdr.size() == kPfs0HeaderSize && !parse_header()) return false;
                break;
            }
            case Phase::Table: {
                const size_t take = std::min(m_table_size - m_table.size(), len);
                m_table.insert(m_table.end(), data, data + take);
                data += take; len -= take; m_pos += take;
                if (m_table.size() == m_table_size && !parse_table()) return false;
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
    return m_phase != Phase::Failed;
}

// ─── Finish: hand off to the validated installer ─────────────────────────────

bool StreamInstaller::finish() {
    if (m_phase == Phase::Failed) return false;
    if (m_phase != Phase::Done && m_phase != Phase::Data)
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

    return install(std::move(contents), m_storage, m_keys, m_progress);
}

void StreamInstaller::abort() {
#ifdef PLATFORM_SWITCH
    if (m_entry_open && !m_entry_skip && m_cur < m_entries.size() && m_entries[m_cur].is_nca)
        ncmContentStorageDeletePlaceHolder(&m_cs, &m_ph);
    if (m_have_cs) { ncmContentStorageClose(&m_cs); m_have_cs = false; }
#endif
    m_entry_open = false;
}

} // namespace Install
