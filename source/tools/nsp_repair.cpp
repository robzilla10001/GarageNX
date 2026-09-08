// source/tools/nsp_repair.cpp
//
// Pure half of the NSP ticket repair - see nsp_repair.hpp for the contract.
// Deliberately libnx-free so the host test suite can exercise it (the tests'
// admission rule bars anything touching libnx from the host build).

#include "tools/nsp_repair.hpp"

#include <cstring>
#include <utility>

namespace Tools::NspRepair {
namespace {

constexpr uint32_t kPfs0Magic = 0x30534650;  // "PFS0" little-endian

struct Entry {
    uint64_t data_off = 0;
    uint64_t size     = 0;
    uint32_t name_off = 0;
    uint32_t reserved = 0;
};

void read_u32(const uint8_t* p, uint32_t& out) { std::memcpy(&out, p, 4); }
void read_u64(const uint8_t* p, uint64_t& out) { std::memcpy(&out, p, 8); }

std::string string_at(const uint8_t* table, size_t table_size, uint32_t off) {
    if (off >= table_size) return {};
    const uint8_t* begin = table + off;
    const size_t max_len = table_size - off;
    size_t len = 0;
    while (len < max_len && begin[len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(begin), len);
}

} // namespace

// ── File-oriented primitives (see nsp_repair.hpp for the contract) ──────────

Header read_header(const uint8_t* first, size_t size) {
    Header h;
    if (size < 0x10) return h;
    uint32_t magic = 0;
    read_u32(first, magic);
    if (magic != kPfs0Magic) return h;
    read_u32(first + 0x04, h.count);
    read_u32(first + 0x08, h.strtab_size);
    const uint64_t entries_bytes = 0x18ull * h.count;
    if (h.count > 0xFFFF || entries_bytes > 0xFFFFFFFFull) return h;
    h.header_size = 0x10ull + entries_bytes + h.strtab_size;
    h.ok = true;
    return h;
}

std::vector<EntryInfo> read_entries(const uint8_t* p, size_t size,
                                    uint32_t count, uint32_t strtab_size) {
    std::vector<EntryInfo> out;
    const uint64_t entries_bytes = 0x18ull * count;
    if (count > 0xFFFF || entries_bytes > 0xFFFFFFFFull ||
        entries_bytes + strtab_size > size) return out;
    const uint8_t* strtab = p + entries_bytes;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        EntryInfo e;
        read_u64(p + (size_t)i * 0x18 + 0x00, e.data_off);
        read_u64(p + (size_t)i * 0x18 + 0x08, e.size);
        uint32_t name_off = 0;
        read_u32(p + (size_t)i * 0x18 + 0x10, name_off);
        e.name = string_at(strtab, strtab_size, name_off);
        out.push_back(std::move(e));
    }
    return out;
}

uint64_t title_id_from_cnmt_nca(const uint8_t* nca, size_t size) {
    if (size < 0x10 + 0x18 || !nca) return 0;
    uint32_t magic = 0, count = 0, strtab = 0;
    read_u32(nca, magic);
    if (magic != kPfs0Magic) return 0;
    read_u32(nca + 0x04, count);
    read_u32(nca + 0x08, strtab);
    if (count < 1 || count > 0xFFFF) return 0;
    const uint64_t rec_off = 0x10ull + 0x18ull * count + strtab;
    if (rec_off + 0x24 > size) return 0;   // record header through content count
    uint64_t title_id = 0;
    std::memcpy(&title_id, nca + rec_off, 8);
    return title_id;
}

bool is_tik_name(const std::string& name) {
    return name.size() == 32 + 4 &&
           name.compare(name.size() - 4, 4, ".tik") == 0;
}

bool is_cert_name(const std::string& name) {
    return name.size() == 32 + 5 &&
           name.compare(name.size() - 5, 5, ".cert") == 0;
}

Inspection inspect(const uint8_t* data, size_t size) {
    Inspection ins;

    // Implemented ON TOP of the file-oriented primitives - one parsing path,
    // two front-ends (in-memory for tests, seek-based for real files).
    const Header h = read_header(data, size);
    if (!h.ok) {
        ins.detail = size < 0x10 ? "too small to be a PFS0 container"
                                 : "not a PFS0 container";
        return ins;
    }
    if (h.header_size > size) {
        ins.detail = "truncated or malformed PFS0 header";
        return ins;
    }

    const std::vector<EntryInfo> entries =
        read_entries(data + 0x10, size - 0x10, h.count, h.strtab_size);
    if (entries.size() != h.count) {
        ins.detail = "truncated or malformed PFS0 header";
        return ins;
    }

    bool have_tik = false;
    for (const auto& e : entries) {
        if (is_tik_name(e.name))  have_tik  = true;
        if (is_cert_name(e.name)) ins.has_cert = true;
        const uint64_t abs_end = h.header_size + e.data_off + e.size;
        if (abs_end > ins.data_end) ins.data_end = abs_end;
    }

    // Locate the .cnmt.nca entry and read its CNMT record's title id. The
    // .cnmt.nca is an NCA whose plaintext content section begins with a PFS0
    // holding one file - the raw cnmt record - so the record does NOT sit at
    // the entry's data offset; it is nested one PFS0 layer down. The inner
    // PFS0 is plaintext (the CNMT NCA's content section is not titlekey-
    // encrypted even for titlekey titles - it is a metadata NCA every console
    // can read), so this is a pure byte-walk.
    const EntryInfo* cnmt = nullptr;
    for (const auto& e : entries) {
        if (e.name.size() == 32 + 9 &&
            e.name.compare(e.name.size() - 9, 9, ".cnmt.nca") == 0) {
            cnmt = &e;
            break;
        }
    }
    if (!cnmt) {
        ins.detail = "no .cnmt.nca in container";
        return ins;
    }
    const uint64_t cnmt_abs = h.header_size + cnmt->data_off;
    if (cnmt_abs >= size || cnmt_abs + cnmt->size > size) {
        ins.detail = "cnmt.nca data not present in buffer";
        return ins;
    }

    ins.title_id = title_id_from_cnmt_nca(data + cnmt_abs,
                                          (size_t)cnmt->size);
    if (ins.title_id == 0) {
        ins.detail = "cnmt record unreadable";
        return ins;
    }

    if (have_tik) {
        ins.state  = State::Complete;
        ins.detail = "ticket present";
        return ins;
    }

    // No .tik entry. Whether one is REQUIRED depends on titlekey crypto,
    // which the outer container cannot prove (the application NCA headers
    // are encrypted inside their entries). The caller confirms via the ES
    // ticket list: a common ticket existing for this title id means the
    // title is titlekey-protected and the dump is repairable; no ticket
    // means the title is standard-crypto and needs nothing.
    ins.state  = State::MissingTik;
    ins.detail = "no .tik entry - repairable if a common ticket exists for this title";
    return ins;
}

Inspection inspect(const std::vector<uint8_t>& data) {
    return inspect(data.data(), data.size());
}

} // namespace Tools::NspRepair
