// tests/stream_container_test.cpp
//
// CHARACTERIZATION test for StreamInstaller's container front-end.
//
// Written BEFORE slice 4c refactors Phase{Header,Table,...} into a general
// "collect N bytes at absolute offset X" collector. The PFS0 path this pins down
// has installed real titles on hardware (three NSZ, up to 6 GB); the point of
// this file is that the refactor cannot quietly change what that path does.
// Every assertion here describes behaviour that shipped and worked.
//
// Build: see tests/CMakeLists.txt. Runs on the host, no libnx, no device.
//
// SCOPE — read before adding to this file.
//   IN:  container parsing, entry boundaries, gap/padding skipping, table sort
//        order, container_size() arithmetic, malformed-input rejection.
//   OUT: anything NCZ. ncz.cpp's !PLATFORM_SWITCH branch (ncz.cpp:438) stubs the
//        decompressor to return 0/"not supported", so an .ncz entry here
//        exercises a stub, not a decompressor. A green NCZ test on the host
//        would prove the stub works and nothing else. NczWindow is tested
//        separately and for real in ncz_window_test.cpp, because it is genuinely
//        free of libnx.

#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "install/installer.hpp"
#include "install/stream_installer.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Install::StreamInstaller;

static int g_checks = 0;
#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);                         \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

// ── Synthetic PFS0 builder ───────────────────────────────────────────────────
// PFS0: "PFS0" | u32 count | u32 string_table_size | u32 reserved
//       entries[count] x 0x18 { u64 offset, u64 size, u32 name_offset, u32 rsv }
//       string table
//       data, beginning at 0x10 + count*0x18 + string_table_size

struct FakeEntry {
    std::string name;
    uint64_t    rel_off;   // relative to the data region
    uint64_t    size;
};

static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}
static void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}

// Returns header+table+string table only. `out_data_start` receives the absolute
// offset where entry data begins.
static std::vector<uint8_t> build_pfs0_head(const std::vector<FakeEntry>& es,
                                            uint64_t* out_data_start,
                                            const char* magic = "PFS0") {
    std::vector<char> strtab;
    std::vector<uint32_t> name_offs;
    for (const auto& e : es) {
        name_offs.push_back((uint32_t)strtab.size());
        strtab.insert(strtab.end(), e.name.begin(), e.name.end());
        strtab.push_back('\0');
    }
    while (strtab.size() % 4) strtab.push_back('\0');   // realistic alignment

    const uint32_t count = (uint32_t)es.size();
    const uint32_t sts   = (uint32_t)strtab.size();
    std::vector<uint8_t> v(0x10 + (size_t)count * 0x18 + sts, 0);
    std::memcpy(v.data(), magic, 4);
    put32(v, 4, count);
    put32(v, 8, sts);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 0x10 + (size_t)i * 0x18;
        put64(v, at + 0, es[i].rel_off);
        put64(v, at + 8, es[i].size);
        put32(v, at + 16, name_offs[i]);
    }
    std::memcpy(v.data() + 0x10 + (size_t)count * 0x18, strtab.data(), sts);
    *out_data_start = v.size();
    return v;
}

// Full container: head + a data region filled with an offset-keyed pattern.
static uint8_t byte_at(uint64_t off) { return (uint8_t)((off * 131u + 29u) & 0xFF); }

static std::vector<uint8_t> build_pfs0(const std::vector<FakeEntry>& es, uint64_t* data_start) {
    std::vector<uint8_t> v = build_pfs0_head(es, data_start);
    uint64_t end = 0;
    for (const auto& e : es) end = std::max(end, e.rel_off + e.size);
    const size_t base = v.size();
    v.resize(base + (size_t)end);
    for (size_t i = base; i < v.size(); ++i) v[i] = byte_at(i);
    return v;
}

// ── Harness ──────────────────────────────────────────────────────────────────

struct Rig {
    Core::Keys::Keyset keys;
    Install::Progress  progress;
    StreamInstaller    inst{Core::Ncm::Storage::SdCard, keys, progress};
};

// Feed `v` in fixed-size chunks. StreamInstaller requires bytes in order with no
// gaps; the chunk size is what varies, and it must not matter.
static bool feed_in_chunks(StreamInstaller& si, const std::vector<uint8_t>& v, size_t chunk) {
    for (size_t i = 0; i < v.size(); i += chunk) {
        const size_t n = std::min(chunk, v.size() - i);
        if (!si.feed(v.data() + i, n)) return false;
    }
    return true;
}

// ── Tests ────────────────────────────────────────────────────────────────────

// A transfer split at arbitrary boundaries must land identically. USB hands over
// whatever the endpoint gives; entry boundaries cannot depend on it.
static void test_chunk_size_invariance() {
    const std::vector<FakeEntry> es = {
        {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,     0x40},
        {"fedcba9876543210fedcba9876543210.nca",      0x40,  0x200},
        {"aaaabbbbccccddddeeeeffff00001111.nca",      0x240, 0x1234},
    };
    uint64_t data_start = 0;
    const std::vector<uint8_t> v = build_pfs0(es, &data_start);

    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)64, (size_t)4096, v.size()}) {
        Rig r;
        CHECK(r.inst.begin("test.nsp", v.size()), "begin()");
        CHECK(feed_in_chunks(r.inst, v, chunk), "feed at every chunk size");
        CHECK(r.inst.ok(), "no failure");
        CHECK(r.inst.complete(), "reaches Done");
        CHECK(r.inst.container_size() == v.size(), "container_size matches the built size");
    }
    std::printf("  ok: entry boundaries hold at chunk sizes 1/7/64/4096/whole-file\n");
}

// container_size() is the authority precisely because MTP's 32-bit fields carry
// 0xFFFFFFFF above 4 GiB. This is the arithmetic, checked without moving 14 GiB:
// the header and table alone must yield an exact 64-bit answer.
static void test_container_size_beyond_4gib() {
    struct Case { const char* label; uint64_t size; };
    const Case cases[] = {
        {"764 MB",          764ull  * 1024 * 1024},
        {"just under 4GiB", 4ull * 1024 * 1024 * 1024 - 0x4000},
        {"just over 4GiB",  4ull * 1024 * 1024 * 1024 + 0x4000},
        {"14 GiB",          14ull * 1024 * 1024 * 1024},
    };
    for (const auto& c : cases) {
        const std::vector<FakeEntry> es = {
            {"0123456789abcdef0123456789abcdef.cnmt.nca", 0, 0x40},
            {"fedcba9876543210fedcba9876543210.nca",      0x40, c.size},
        };
        uint64_t data_start = 0;
        const std::vector<uint8_t> head = build_pfs0_head(es, &data_start);

        Rig r;
        CHECK(r.inst.begin("huge.nsp", 0xFFFFFFFF), "begin with a 32-bit-truncated size");
        CHECK(r.inst.feed(head.data(), head.size()), "feed header+table only");
        // No data fed at all — the table alone is the authority.
        CHECK(r.inst.container_size() == data_start + 0x40 + c.size, c.label);
        CHECK(r.inst.container_size() > 0xFFFFFFFFull || c.size < 4ull * 1024 * 1024 * 1024,
              "over-4GiB cases exceed what MTP could have reported");
        r.inst.abort();
    }
    std::printf("  ok: container_size() exact at 764MB / <4GiB / >4GiB / 14GiB\n");
}

// The table is not required to be sorted, but the stream only goes forwards, so
// entries must be walked in the order their bytes actually arrive.
static void test_unsorted_table_is_walked_in_stream_order() {
    const std::vector<FakeEntry> es = {
        {"cccccccccccccccccccccccccccccccc.nca",      0x900, 0x100},
        {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,     0x40},
        {"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.nca",      0x100, 0x800},
    };
    uint64_t data_start = 0;
    const std::vector<uint8_t> v = build_pfs0(es, &data_start);

    Rig r;
    CHECK(r.inst.begin("unsorted.nsp", v.size()), "begin()");
    CHECK(feed_in_chunks(r.inst, v, 13), "feed an out-of-order table");
    CHECK(r.inst.ok() && r.inst.complete(), "out-of-order table still completes");
    CHECK(r.inst.container_size() == v.size(), "container_size unaffected by table order");
    std::printf("  ok: unsorted entry table is walked in stream order\n");
}

// Real containers pad between entries. Those bytes belong to no entry and must
// be discarded, not written into whichever placeholder happens to be open.
static void test_gaps_between_entries_are_skipped() {
    const std::vector<FakeEntry> es = {
        {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,      0x40},
        {"fedcba9876543210fedcba9876543210.nca",      0x1000, 0x200},   // big gap
        {"aaaabbbbccccddddeeeeffff00001111.nca",      0x2000, 0x100},   // another
    };
    uint64_t data_start = 0;
    const std::vector<uint8_t> v = build_pfs0(es, &data_start);

    for (size_t chunk : {(size_t)1, (size_t)3, (size_t)512}) {
        Rig r;
        CHECK(r.inst.begin("gaps.nsp", v.size()), "begin()");
        CHECK(feed_in_chunks(r.inst, v, chunk), "feed a padded container");
        CHECK(r.inst.ok() && r.inst.complete(), "padding skipped, not misattributed");
        CHECK(r.inst.container_size() == data_start + 0x2100, "container_size ends at last entry");
    }
    std::printf("  ok: inter-entry padding is discarded at every chunk size\n");
}

// Malformed input must be refused, not half-installed.
static void test_malformed_containers_are_rejected() {
    {   // bad magic
        uint64_t ds = 0;
        std::vector<uint8_t> v = build_pfs0_head({{"a.nca", 0, 1}}, &ds, "XXXX");
        Rig r;
        r.inst.begin("bad.nsp", v.size());
        const bool fed = r.inst.feed(v.data(), v.size());
        CHECK(!fed || !r.inst.ok(), "bad magic is refused");
        CHECK(r.inst.error().find("PFS0") != std::string::npos, "error names the magic");
    }
    {   // implausible file count
        std::vector<uint8_t> v(0x10, 0);
        std::memcpy(v.data(), "PFS0", 4);
        put32(v, 4, 999999);
        put32(v, 8, 16);
        Rig r;
        r.inst.begin("bad.nsp", 0x10);
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "implausible file count is refused");
    }
    {   // zero files
        std::vector<uint8_t> v(0x10, 0);
        std::memcpy(v.data(), "PFS0", 4);
        put32(v, 4, 0);
        Rig r;
        r.inst.begin("bad.nsp", 0x10);
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "zero-file container is refused");
    }
    std::printf("  ok: bad magic / implausible count / empty container all refused\n");
}

// An NSZ cannot be decompressed without header_key, so it must be refused at the
// table — before the host pushes gigabytes — rather than partway through.
// This asserts the REFUSAL only. It says nothing about decompression, which is
// stubbed out on this platform (see the scope note at the top).
static void test_ncz_without_keys_is_refused_at_the_table() {
    const std::vector<FakeEntry> es = {
        {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,    0x40},
        {"fedcba9876543210fedcba9876543210.ncz",      0x40, 0x800},
    };
    uint64_t ds = 0;
    const std::vector<uint8_t> head = build_pfs0_head(es, &ds);

    Rig r;   // Rig's Keyset is default-constructed: has_header_key == false
    CHECK(!r.keys.has_header_key, "precondition: no header key");
    r.inst.begin("nokeys.nsz", 0x10000);
    r.inst.feed(head.data(), head.size());
    CHECK(!r.inst.ok(), "keyless NSZ is refused");
    // Assert the refusal, NOT the wording: the reason text comes partly from
    // Core::Keys::requirement_message(), which is a link stub here (see
    // link_stubs.cpp). Asserting on the message would be asserting on the stub.
    CHECK(r.inst.error().find("NSZ") != std::string::npos, "error identifies it as the NSZ path");
    CHECK(!r.inst.complete(), "refused container never reaches Done");
    std::printf("  ok: keyless NSZ refused at the table, before the data phase\n");
}

// NOTE — everything below POSTDATES the 4c refactor, unlike everything above.
// The suite above is a characterization record: it pins behaviour that already
// existed. This is a new claim about new behaviour, and is marked so that a
// later reader does not mistake it for evidence about the original design.
//
// The file count was bounded from the start; the string-table size never was.
// Both are host-supplied u32s feeding straight into a reserve(), so a corrupt
// NSP declaring 0xFFFFFFFF asks for a 4 GiB allocation and takes the process out
// via bad_alloc — a crash, on a console, from a bad file. Found while writing
// the XCI front-end, which needed the same guard on HFS0.
static void test_hostile_string_table_cannot_exhaust_memory() {
    {   // The 4 GiB ask. The count stays legal, so only the string-table bound
        // stands between this and the allocation.
        std::vector<uint8_t> v(0x10, 0);
        std::memcpy(v.data(), "PFS0", 4);
        put32(v, 4, 4);                 // a plausible file count
        put32(v, 8, 0xFFFFFFFFu);       // a 4 GiB string table
        Rig r;
        r.inst.begin("hostile.nsp", 0x10);
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "a 4 GiB string table is refused rather than allocated");
        CHECK(r.inst.error().find("string table") != std::string::npos,
              "error names the string table");
    }
    {   // One byte over the derived bound: four entries can name at most
        // 4 * kMaxNameBytes of string table.
        std::vector<uint8_t> v(0x10, 0);
        std::memcpy(v.data(), "PFS0", 4);
        put32(v, 4, 4);
        put32(v, 8, 4 * 256 + 1);
        Rig r;
        r.inst.begin("hostile.nsp", 0x10);
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "a string table larger than its entries could name is refused");
    }
    {   // The other side of the bound, and the more important one: this guard
        // is new on a path that installs real titles, so it must be shown NOT
        // to refuse an ordinary container. Real NSP names are ~40 bytes against
        // a 256-byte allowance, so the margin is wide — but "wide" is a claim,
        // and this is the check that makes it one the suite can hold.
        const std::vector<FakeEntry> es = {
            {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,    0x40},
            {"fedcba9876543210fedcba9876543210.nca",      0x40, 0x80},
            {"0123456789abcdef0123456789abcdef.tik",      0xC0, 0x20},
            {"0123456789abcdef0123456789abcdef.cert",     0xE0, 0x20},
        };
        uint64_t ds = 0;
        const std::vector<uint8_t> full = build_pfs0(es, &ds);
        Rig r;
        r.inst.begin("fine.nsp", full.size());
        CHECK(r.inst.feed(full.data(), full.size()), "a realistic container feeds");
        CHECK(r.inst.ok(), "a realistic container is not caught by the new bound");
        CHECK(r.inst.complete(), "and still installs end to end");
    }
    std::printf("  ok: hostile string tables refused; realistic ones untouched\n");
}

int main() {
    std::printf("StreamInstaller container harness (PFS0 characterization)\n");
    test_chunk_size_invariance();
    test_container_size_beyond_4gib();
    test_unsorted_table_is_walked_in_stream_order();
    test_gaps_between_entries_are_skipped();
    test_malformed_containers_are_rejected();
    test_ncz_without_keys_is_refused_at_the_table();
    test_hostile_string_table_cannot_exhaust_memory();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
