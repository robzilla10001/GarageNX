// source/install/nsp_reader.cpp
// source/install/nsp_reader.cpp
// PFS0 (NSP) container parser.

#include "install/nsp_reader.hpp"
#include "core/fs.hpp"
#include <cstring>
#include <algorithm>

namespace Install {

// ── PFS0 on-disk structures (little-endian) ──────────────────────────────────

struct Pfs0Header {
    uint8_t  magic[4];           // "PFS0"
    uint32_t file_count;
    uint32_t string_table_size;
    uint32_t reserved;
};

struct Pfs0FileEntry {
    uint64_t data_offset;        // relative to start of data region
    uint64_t data_size;
    uint32_t name_offset;        // into string table
    uint32_t reserved;
};

static_assert(sizeof(Pfs0Header)    == 0x10, "Pfs0Header size");
static_assert(sizeof(Pfs0FileEntry) == 0x18, "Pfs0FileEntry size");

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool ends_with(const std::string& s, const char* suffix) {
    size_t sl = std::strlen(suffix);
    if (s.size() < sl) return false;
    return s.compare(s.size() - sl, sl, suffix) == 0;
}

// ── NspReader ────────────────────────────────────────────────────────────────

// ── Split-archive support ────────────────────────────────────────────────────
//
// A large NSP cannot be one file on FAT32 (4 GiB limit), so GarageNX's dumper —
// like DBI and Tinfoil — writes a DIRECTORY named <name>.nsp containing numbered
// parts 00, 01, 02 ... The parts concatenated ARE the PFS0; there is no per-part
// header. So the reader presents them as one contiguous stream and nothing above
// it needs to know.
//
// Adding split OUTPUT without split INPUT produced dumps that completed and could
// not be installed — a worse outcome than not splitting, because the failure moved
// from the dump to a later, quieter step.
bool NspReader::open_source(const std::string& path) {
    if (Fs::is_directory(path)) {
        // Parts are strictly ordered; stop at the first gap rather than skipping,
        // because a missing middle part means a truncated archive, and silently
        // reading past the hole would produce garbage that looks like data.
        for (int i = 0; ; ++i) {
            char pn[8];
            std::snprintf(pn, sizeof(pn), "%02d", i);
            FILE* f = std::fopen((path + "/" + pn).c_str(), "rb");
            if (!f) break;
            std::fseek(f, 0, SEEK_END);
            const long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz <= 0) { std::fclose(f); break; }
            m_part_start.push_back(m_total);
            m_parts.push_back(f);
            m_total += (uint64_t)sz;
        }
        if (m_parts.empty()) { m_error = "No parts found in: " + path; return false; }
        return true;
    }

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { m_error = "Cannot open: " + path; return false; }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    m_part_start.push_back(0);
    m_parts.push_back(f);
    m_total = (sz > 0) ? (uint64_t)sz : 0;
    return true;
}

// Absolute read across the concatenated parts. One call may span a boundary.
size_t NspReader::pread_at(void* buf, size_t len, uint64_t off) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t   done = 0;
    while (done < len && off + done < m_total) {
        // Find the part holding this offset.
        size_t pi = 0;
        while (pi + 1 < m_parts.size() && m_part_start[pi + 1] <= off + done) ++pi;

        const uint64_t part_off = (off + done) - m_part_start[pi];
        const uint64_t part_end = (pi + 1 < m_parts.size()) ? m_part_start[pi + 1] : m_total;
        const size_t   avail    = (size_t)((part_end - m_part_start[pi]) - part_off);
        const size_t   want     = std::min(len - done, avail);
        if (want == 0) break;

        // NSP offsets routinely exceed 2 GB, so the seek offset must be 64-bit.
        // On this target (devkitA64 / aarch64, LP64) `long` IS 64-bit, so plain
        // fseek is correct — fseeko is not declared here, and reaching for it was
        // a mistake: I guarded against a truncation that cannot occur on this
        // platform and broke the build doing it.
        //
        // Asserted rather than assumed, so a future port to an ILP32 target fails
        // HERE with a clear message instead of silently truncating a seek past
        // 2 GB and reading the wrong bytes.
        static_assert(sizeof(long) >= 8,
                      "fseek's long must be 64-bit: NSP offsets exceed 2 GB");
        if (std::fseek(m_parts[pi], (long)part_off, SEEK_SET) != 0) break;
        const size_t got = std::fread(p + done, 1, want, m_parts[pi]);
        done += got;
        if (got != want) break;
    }
    return done;
}

NspReader::NspReader(const std::string& path) {
    if (!open_source(path)) return;
    parse();
}

NspReader::~NspReader() {
    for (FILE* f : m_parts) if (f) std::fclose(f);
    m_parts.clear();
}

void NspReader::parse() {
    // ── Read PFS0 header ────────────────────────────────────────────────────
    Pfs0Header hdr;
    uint64_t cur = 0;
    if (pread_at(&hdr, sizeof(hdr), cur) != sizeof(hdr)) {
        m_error = "Failed to read PFS0 header"; return;
    }
    if (std::memcmp(hdr.magic, "PFS0", 4) != 0) {
        m_error = "Not a PFS0 file (bad magic)"; return;
    }
    if (hdr.file_count == 0 || hdr.file_count > 1024) {
        m_error = "Implausible file_count"; return;
    }
    if (hdr.string_table_size > 64 * 1024) {
        m_error = "String table too large"; return;
    }

    // ── Read file entry table ────────────────────────────────────────────────
    std::vector<Pfs0FileEntry> raw(hdr.file_count);
    size_t table_bytes = hdr.file_count * sizeof(Pfs0FileEntry);
    cur = sizeof(hdr);
    if (pread_at(raw.data(), table_bytes, cur) != table_bytes) {
        m_error = "Failed to read file entry table"; return;
    }

    // ── Read string table ────────────────────────────────────────────────────
    std::vector<char> strtab(hdr.string_table_size + 1, '\0');
    if (hdr.string_table_size > 0) {
        cur = sizeof(hdr) + table_bytes;
        if (pread_at(strtab.data(), hdr.string_table_size, cur) != hdr.string_table_size) {
            m_error = "Failed to read string table"; return;
        }
    }

    // ── Compute absolute data region offset ──────────────────────────────────
    // data starts immediately after header + entry table + string table.
    uint64_t data_region_off = sizeof(Pfs0Header)
                             + (uint64_t)hdr.file_count * sizeof(Pfs0FileEntry)
                             + hdr.string_table_size;

    // ── Build entry list ─────────────────────────────────────────────────────
    m_entries.reserve(hdr.file_count);
    for (uint32_t i = 0; i < hdr.file_count; ++i) {
        const Pfs0FileEntry& fe = raw[i];
        if (fe.name_offset >= hdr.string_table_size) {
            m_error = "name_offset out of range for entry " + std::to_string(i);
            return;
        }

        PfsEntry e;
        e.name       = &strtab[fe.name_offset];
        e.offset     = data_region_off + fe.data_offset;
        e.size       = fe.data_size;
        e.is_nca      = ends_with(e.name, ".nca") || ends_with(e.name, ".ncz");
        e.is_cnmt_nca = ends_with(e.name, ".cnmt.nca") || ends_with(e.name, ".cnmt.ncz");
        e.is_tik      = ends_with(e.name, ".tik");
        e.is_cert     = ends_with(e.name, ".cert");
        e.is_ncz      = ends_with(e.name, ".ncz");

        m_entries.push_back(std::move(e));
    }

    m_valid = true;
}

size_t NspReader::read(size_t idx, uint64_t offset_in_entry, void* buf, size_t len) {
    if (!m_valid || m_parts.empty() || idx >= m_entries.size()) return 0;
    const PfsEntry& e = m_entries[idx];
    if (offset_in_entry >= e.size) return 0;
    len = (size_t)std::min<uint64_t>(len, e.size - offset_in_entry);
    if (len == 0) return 0;

    const uint64_t abs_off = e.offset + offset_in_entry;
    return pread_at(buf, len, abs_off);
}

bool NspReader::read_all(size_t idx, std::vector<uint8_t>& out) {
    if (!m_valid || idx >= m_entries.size()) return false;
    const PfsEntry& e = m_entries[idx];
    if (e.size > 1024 * 1024) return false;   // refuse to load > 1 MB inline
    out.resize((size_t)e.size);
    size_t got = read(idx, 0, out.data(), out.size());
    return got == out.size();
}

} // namespace Install
