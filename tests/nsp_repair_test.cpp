// tests/nsp_repair_test.cpp
//
// Host tests for the PURE half of the NSP ticket repair (tools/nsp_repair).
// Builds synthetic PFS0 containers in memory - outer container, nested CNMT
// NCA, cnmt record - and checks the classification: Complete vs MissingTik vs
// Malformed, title-id extraction, and the filename predicates.
//
// NOTE on the synthetic layout: real PFS0 places file data IMMEDIATELY after
// the string table (no alignment padding - see services/pfs0_layout.hpp and
// dump.cpp's header construction). The helpers below follow that exactly; an
// earlier draft padded the table to 16 bytes, which silently misaligned every
// nested offset and failed the title-id check.

#include "tools/nsp_repair.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

using namespace Tools::NspRepair;

namespace {

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x));         v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)(x >> 16));   v.push_back((uint8_t)(x >> 24));
}

void put_u64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((uint8_t)(x >> (8 * i)));
}

void put_str(std::vector<uint8_t>& v, const std::string& s) {
    v.insert(v.end(), s.begin(), s.end());
    v.push_back(0);
}

// Build a PFS0 container from {name, size} entries; file data is zero-filled.
// Layout: "PFS0" | u32 count | u32 strtab_size | u32 rsvd | entries[0x18] |
// string table | file data (NO padding - matches the real container format).
std::vector<uint8_t> make_pfs0(
        const std::vector<std::pair<std::string, uint64_t>>& files) {
    std::vector<uint8_t> out;

    uint32_t strtab_size = 0;
    for (const auto& f : files) strtab_size += (uint32_t)f.first.size() + 1;

    out.insert(out.end(), {'P', 'F', 'S', '0'});
    put_u32(out, (uint32_t)files.size());
    put_u32(out, strtab_size);
    put_u32(out, 0);

    uint64_t data_off = 0;
    uint32_t name_off = 0;
    for (const auto& f : files) {
        put_u64(out, data_off);
        put_u64(out, f.second);
        put_u32(out, name_off);
        put_u32(out, 0);
        data_off += f.second;
        name_off += (uint32_t)f.first.size() + 1;
    }
    for (const auto& f : files) put_str(out, f.first);
    for (const auto& f : files) out.resize(out.size() + (size_t)f.second, 0);
    return out;
}

// Absolute offset of file i's data within a make_pfs0 container.
uint64_t data_offset(size_t count, uint32_t strtab_size, size_t index,
                     const std::vector<uint64_t>& sizes) {
    uint64_t off = 0x10 + 0x18 * count + strtab_size;
    for (size_t i = 0; i < index; ++i) off += sizes[i];
    return off;
}

// Wrap a cnmt record (title id + zero padding) in an inner PFS0 and place it
// as the .cnmt.nca entry of an outer container - the shape a real dumped NSP
// has around its CNMT metadata NCA.
std::vector<uint8_t> make_dump_nsp(uint64_t title_id,
                                   bool with_tik,
                                   bool with_cert) {
    constexpr uint64_t kCnmtSize = 0x60;  // record header + one content entry

    // Inner PFS0: one file named "cnmt". Its data starts right after the
    // string table ("cnmt\0" = 5 bytes).
    std::vector<uint8_t> inner = make_pfs0({{"cnmt", kCnmtSize}});
    const uint64_t inner_data_off = 0x10 + 0x18 + 5;
    for (uint64_t i = 0; i < kCnmtSize; ++i) inner[inner_data_off + i] = 0;
    std::memcpy(inner.data() + inner_data_off, &title_id, 8);

    std::vector<std::pair<std::string, uint64_t>> files;
    files.push_back({"00000000000000000000000000000000.cnmt.nca", inner.size()});
    if (with_tik)
        files.push_back({"0123456789abcdef0123456789abcdef.tik", 0x2C0});
    if (with_cert)
        files.push_back({"0123456789abcdef0123456789abcdef.cert", 0x100});

    std::vector<uint8_t> outer = make_pfs0(files);

    // Overwrite the .cnmt.nca entry's data region with the inner container.
    uint32_t strtab_size = 0;
    for (const auto& f : files) strtab_size += (uint32_t)f.first.size() + 1;
    const uint64_t first_data = data_offset(files.size(), strtab_size, 0, {});
    std::memcpy(outer.data() + first_data, inner.data(), inner.size());
    return outer;
}

} // namespace

static void test_names() {
    CHECK(is_tik_name("0123456789abcdef0123456789abcdef.tik"), ".tik recognized");
    CHECK(is_cert_name("0123456789abcdef0123456789abcdef.cert"), ".cert recognized");
    CHECK(!is_tik_name("0123456789abcdef0123456789abcdef.cert"), ".cert is not .tik");
    CHECK(!is_tik_name("short.tik"), "short name is not .tik");
    CHECK(!is_tik_name("00000000000000000000000000000000.cnmt.nca"), ".cnmt.nca is not .tik");
    std::printf("  ok: filename predicates\n");
}

static void test_malformed() {
    CHECK(inspect(std::vector<uint8_t>{}).state == State::Malformed, "empty is Malformed");
    CHECK(inspect(std::vector<uint8_t>(8, 0)).state == State::Malformed, "8 bytes is Malformed");

    std::vector<uint8_t> not_pfs0(0x40, 0);
    std::memcpy(not_pfs0.data(), "XFS0", 4);
    CHECK(inspect(not_pfs0).state == State::Malformed, "wrong magic is Malformed");

    std::vector<uint8_t> trunc(0x14, 0);
    std::memcpy(trunc.data(), "PFS0", 4);
    trunc[4] = 0xFF; trunc[5] = 0xFF;  // absurd entry count
    CHECK(inspect(trunc).state == State::Malformed, "absurd count is Malformed");

    std::vector<uint8_t> no_cnmt =
        make_pfs0({{"00000000000000000000000000000000.nca", 0x100}});
    Inspection i = inspect(no_cnmt);
    CHECK(i.state == State::Malformed, "container without .cnmt.nca is Malformed");
    CHECK(i.detail.find("cnmt") != std::string::npos, "detail names the cnmt problem");
    std::printf("  ok: malformed containers rejected\n");
}

static void test_missing_tik() {
    const uint64_t tid = 0x0100000000010000ull;
    std::vector<uint8_t> nsp = make_dump_nsp(tid, /*with_tik=*/false, /*with_cert=*/false);
    Inspection i = inspect(nsp);
    CHECK(i.state == State::MissingTik, "no-tik dump is MissingTik");
    CHECK(i.title_id == tid, "title id extracted from cnmt record");
    CHECK(!i.has_cert, "no cert reported");
    CHECK(i.data_end > 0, "data end computed");
    std::printf("  ok: missing-ticket dump classified with title id\n");
}

static void test_complete() {
    const uint64_t tid = 0x0100000000010000ull;
    std::vector<uint8_t> nsp = make_dump_nsp(tid, /*with_tik=*/true, /*with_cert=*/true);
    Inspection i = inspect(nsp);
    CHECK(i.state == State::Complete, "tik+cert dump is Complete");
    CHECK(i.title_id == tid, "title id still extracted");
    CHECK(i.has_cert, "cert presence reported");
    std::printf("  ok: complete dump classified\n");
}

int main() {
    std::printf("NspRepair (dumped-NSP ticket repair inspector)\n");
    test_names();
    test_malformed();
    test_missing_tik();
    test_complete();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
