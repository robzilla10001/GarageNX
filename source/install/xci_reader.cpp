// source/install/xci_reader.cpp
// source/install/xci_reader.cpp
// XCI (game card image) parser. Locates the Secure HFS0 partition and exposes
// its NCAs as PfsEntry records, identical in shape to what NspReader produces.

#include "install/xci_reader.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <algorithm>

namespace Install {

// ── On-disk structures ────────────────────────────────────────────────────────

// XCI file header starts at offset 0x000. We only need a few fields.
struct XciHeader {
    uint8_t  magic[4];          // "HEAD" at 0x100 (after RSA sig at 0x000)
    // Full header is 0x200 bytes. We care about:
    //   0x104: u32 header_size (usually 0x200)
    //   0x108: u64 package_id
    //   0x110: u64 valid_data_end_page (for size)
    //   0x120: u64 root_partition_offset  (absolute, usually 0x200 for HFS0 root)
    //   0x128: u64 root_partition_size
    // We just need to find the root HFS0 at 0x120.
    uint8_t pad0[0xFC];          // 0x104 - 0x004
    uint8_t reserved1[0x1C];     // through 0x120
    uint64_t root_hfs0_offset;   // 0x120
    uint64_t root_hfs0_size;     // 0x128
    // rest not needed
};
// Rather than a struct (the layout doesn't pack cleanly), we read fields by
// absolute offset.

struct Hfs0Header {
    uint8_t  magic[4];           // "HFS0"
    uint32_t file_count;
    uint32_t string_table_size;
    uint32_t reserved;
};

// HFS0 entries are 0x40 bytes each (unlike PFS0's 0x18; the extra space is
// SHA-256 + hash_region_size + reserved).
struct Hfs0FileEntry {
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t name_offset;
    uint32_t hash_region_size;
    uint64_t reserved;
    uint8_t  sha256[0x20];
};

static_assert(sizeof(Hfs0Header)    == 0x10, "Hfs0Header size");
static_assert(sizeof(Hfs0FileEntry) == 0x40, "Hfs0FileEntry size");

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool ends_with(const std::string& s, const char* suffix) {
    size_t sl = std::strlen(suffix);
    if (s.size() < sl) return false;
    return s.compare(s.size() - sl, sl, suffix) == 0;
}

static bool fread_at(FILE* fp, uint64_t offset, void* buf, size_t len) {
    if (std::fseek(fp, (long)offset, SEEK_SET) != 0) return false;
    return std::fread(buf, 1, len, fp) == len;
}

// ── XciReader ────────────────────────────────────────────────────────────────

XciReader::XciReader(const std::string& path) {
    m_fp = std::fopen(path.c_str(), "rb");
    if (!m_fp) { m_error = "Cannot open: " + path; return; }
    parse();
}

XciReader::~XciReader() {
    if (m_fp) { std::fclose(m_fp); m_fp = nullptr; }
}

bool XciReader::parse_hfs0(uint64_t abs_off, std::vector<PfsEntry>& out,
                            const char* /*label*/) {
    Hfs0Header hdr;
    if (!fread_at(m_fp, abs_off, &hdr, sizeof(hdr))) return false;
    if (std::memcmp(hdr.magic, "HFS0", 4) != 0) return false;
    if (hdr.file_count > 1024) return false;

    std::vector<Hfs0FileEntry> raw(hdr.file_count);
    uint64_t entries_off = abs_off + sizeof(Hfs0Header);
    if (hdr.file_count > 0) {
        if (!fread_at(m_fp, entries_off, raw.data(),
                      hdr.file_count * sizeof(Hfs0FileEntry))) return false;
    }

    std::vector<char> strtab(hdr.string_table_size + 1, '\0');
    if (hdr.string_table_size > 0) {
        uint64_t strtab_off = entries_off + hdr.file_count * sizeof(Hfs0FileEntry);
        if (!fread_at(m_fp, strtab_off, strtab.data(), hdr.string_table_size))
            return false;
    }

    uint64_t data_region_off = abs_off
                             + sizeof(Hfs0Header)
                             + hdr.file_count * sizeof(Hfs0FileEntry)
                             + hdr.string_table_size;

    for (uint32_t i = 0; i < hdr.file_count; ++i) {
        if (raw[i].name_offset >= hdr.string_table_size) return false;
        PfsEntry e;
        e.name       = &strtab[raw[i].name_offset];
        e.offset     = data_region_off + raw[i].data_offset;
        e.size       = raw[i].data_size;
        e.is_nca      = ends_with(e.name, ".nca") || ends_with(e.name, ".ncz");
        e.is_cnmt_nca = ends_with(e.name, ".cnmt.nca") || ends_with(e.name, ".cnmt.ncz");
        e.is_tik      = ends_with(e.name, ".tik");
        e.is_cert     = ends_with(e.name, ".cert");
        e.is_ncz      = ends_with(e.name, ".ncz");
        out.push_back(std::move(e));
    }
    return true;
}

void XciReader::parse() {
    // XCI header layout (from switchbrew / hactool):
    //   0x000  RSA-2048 signature
    //   0x100  "HEAD" magic (4 bytes)
    //   0x104  u32 secure_area_start_page
    //   0x110  u64 package_id
    //   0x120  u8[0x10] gamecard_iv        ← NOT the HFS0 offset
    //   0x130  u64 hfs0_partition_offset   ← byte offset to root HFS0
    //   0x138  u64 hfs0_header_size
    //
    // The root HFS0 offset is a BYTE offset, not a media-unit count.
    // Typical value: 0x200 (immediately after the 0x200-byte XCI header).

    uint8_t magic[4];
    if (!fread_at(m_fp, 0x100, magic, 4)) { m_error = "Cannot read XCI magic"; return; }
    if (std::memcmp(magic, "HEAD", 4) != 0) { m_error = "Not an XCI file (bad magic)"; return; }

    uint64_t root_off = 0;
    if (!fread_at(m_fp, 0x130, &root_off, 8)) { m_error = "Cannot read root HFS0 offset"; return; }

    SDL_Log("XciReader: root HFS0 at byte offset 0x%llX", (unsigned long long)root_off);

    // Parse the root HFS0 — its entries are the partition names ("update",
    // "normal", "secure", "logo"). We need to locate "secure".
    std::vector<PfsEntry> root_parts;
    if (!parse_hfs0(root_off, root_parts, "root")) {
        m_error = "Failed to parse root HFS0"; return;
    }

    // Find the "secure" partition.
    const PfsEntry* secure = nullptr;
    for (const auto& p : root_parts) {
        if (p.name == "secure") { secure = &p; break; }
    }
    if (!secure) { m_error = "No 'secure' partition found in XCI"; return; }

    // Parse the Secure HFS0 — its entries are the actual NCAs.
    if (!parse_hfs0(secure->offset, m_entries, "secure")) {
        m_error = "Failed to parse Secure HFS0"; return;
    }

    m_valid = true;
}

size_t XciReader::read(size_t idx, uint64_t offset_in_entry, void* buf, size_t len) {
    if (!m_valid || !m_fp || idx >= m_entries.size()) return 0;
    const PfsEntry& e = m_entries[idx];
    if (offset_in_entry >= e.size) return 0;
    len = (size_t)std::min<uint64_t>(len, e.size - offset_in_entry);
    if (len == 0) return 0;
    uint64_t abs_off = e.offset + offset_in_entry;
    if (std::fseek(m_fp, (long)abs_off, SEEK_SET) != 0) return 0;
    return std::fread(buf, 1, len, m_fp);
}

bool XciReader::read_all(size_t idx, std::vector<uint8_t>& out) {
    if (!m_valid || idx >= m_entries.size()) return false;
    const PfsEntry& e = m_entries[idx];
    if (e.size > 1024 * 1024) return false;
    out.resize((size_t)e.size);
    size_t got = read(idx, 0, out.data(), out.size());
    return got == out.size();
}

} // namespace Install
