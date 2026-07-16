// source/install/nsp_reader.cpp
// source/install/nsp_reader.cpp
// PFS0 (NSP) container parser.

#include "install/nsp_reader.hpp"
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

NspReader::NspReader(const std::string& path) {
    m_fp = std::fopen(path.c_str(), "rb");
    if (!m_fp) {
        m_error = "Cannot open: " + path;
        return;
    }
    parse();
}

NspReader::~NspReader() {
    if (m_fp) { std::fclose(m_fp); m_fp = nullptr; }
}

void NspReader::parse() {
    // ── Read PFS0 header ────────────────────────────────────────────────────
    Pfs0Header hdr;
    if (std::fread(&hdr, 1, sizeof(hdr), m_fp) != sizeof(hdr)) {
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
    if (std::fread(raw.data(), 1, table_bytes, m_fp) != table_bytes) {
        m_error = "Failed to read file entry table"; return;
    }

    // ── Read string table ────────────────────────────────────────────────────
    std::vector<char> strtab(hdr.string_table_size + 1, '\0');
    if (hdr.string_table_size > 0) {
        if (std::fread(strtab.data(), 1, hdr.string_table_size, m_fp) != hdr.string_table_size) {
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
    if (!m_valid || !m_fp || idx >= m_entries.size()) return 0;
    const PfsEntry& e = m_entries[idx];
    if (offset_in_entry >= e.size) return 0;
    len = (size_t)std::min<uint64_t>(len, e.size - offset_in_entry);
    if (len == 0) return 0;

    uint64_t abs_off = e.offset + offset_in_entry;
    if (std::fseek(m_fp, (long)abs_off, SEEK_SET) != 0) return 0;
    return std::fread(buf, 1, len, m_fp);
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
