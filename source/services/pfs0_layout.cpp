// source/services/pfs0_layout.cpp

#include "services/pfs0_layout.hpp"

#include <cstring>

namespace Services {

namespace {

constexpr size_t kEntrySize  = 0x18;
constexpr size_t kHeaderBase = 0x10;

inline size_t align16(size_t v) { return (v + 0xF) & ~size_t(0xF); }

// Shared by both entry points so the size can never disagree with the header.
struct Dims {
    size_t strtab_len   = 0;
    size_t header_size  = 0;
    uint64_t data_total = 0;
};

Dims measure(const std::vector<Pfs0File>& files) {
    Dims d;
    for (const auto& f : files) {
        d.strtab_len += f.name.size() + 1;   // NUL terminator
        d.data_total += f.size;
    }
    const size_t unpadded = kHeaderBase + files.size() * kEntrySize + d.strtab_len;
    d.header_size = align16(unpadded);
    return d;
}

inline void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void put64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

} // namespace

uint64_t pfs0_total_size(const std::vector<Pfs0File>& files) {
    const Dims d = measure(files);
    return (uint64_t)d.header_size + d.data_total;
}

Pfs0Layout pfs0_build(const std::vector<Pfs0File>& files) {
    const Dims d = measure(files);

    Pfs0Layout out;
    out.header_size = d.header_size;
    out.total_size  = (uint64_t)d.header_size + d.data_total;
    out.header.assign(d.header_size, 0);

    const uint32_t file_count = (uint32_t)files.size();
    // The string-table field covers the padding too, so the data region starts
    // exactly at header_size.
    const uint32_t strtab_field =
        (uint32_t)(d.header_size - (kHeaderBase + files.size() * kEntrySize));

    std::memcpy(out.header.data(), "PFS0", 4);
    put32(out.header.data() + 4, file_count);
    put32(out.header.data() + 8, strtab_field);
    // 0x0C reserved, already zero.

    uint64_t rel_offset  = 0;          // relative to the end of the header
    uint32_t name_offset = 0;
    out.data_offsets.reserve(files.size());

    for (size_t i = 0; i < files.size(); ++i) {
        uint8_t* e = out.header.data() + kHeaderBase + i * kEntrySize;
        put64(e + 0x00, rel_offset);
        put64(e + 0x08, files[i].size);
        put32(e + 0x10, name_offset);
        // 0x14 reserved, already zero.

        out.data_offsets.push_back((uint64_t)d.header_size + rel_offset);
        rel_offset  += files[i].size;
        name_offset += (uint32_t)(files[i].name.size() + 1);
    }

    // String table sits directly after the entries; the remaining bytes up to
    // header_size stay zero, which is the padding.
    uint8_t* strtab = out.header.data() + kHeaderBase + files.size() * kEntrySize;
    size_t at = 0;
    for (const auto& f : files) {
        std::memcpy(strtab + at, f.name.data(), f.name.size());
        at += f.name.size();
        strtab[at++] = '\0';
    }

    return out;
}

} // namespace Services
