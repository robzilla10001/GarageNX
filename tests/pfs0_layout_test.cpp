// tests/pfs0_layout_test.cpp
//
// Pins the on-disk PFS0 layout. Two properties matter most:
//   1. pfs0_total_size() agrees EXACTLY with pfs0_build().total_size — the first is
//      what a directory listing advertises, the second is what we actually stream.
//      A mismatch truncates or overruns every download.
//   2. The header bytes match the format an installer expects, field for field.

#include "services/pfs0_layout.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failed; std::printf("  FAIL: %s\n", what.c_str()); }
}

using namespace Services;

static uint32_t rd32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v; std::memcpy(&v, b.data() + off, 4); return v;
}
static uint64_t rd64(const std::vector<uint8_t>& b, size_t off) {
    uint64_t v; std::memcpy(&v, b.data() + off, 8); return v;
}

int main() {
    std::printf("pfs0_layout_test\n");

    // ── A typical multi-NCA title ─────────────────────────────────────────────
    {
        std::vector<Pfs0File> files = {
            {"aaaa.nca",      0x1000},
            {"bbbb.cnmt.nca", 0x800},
            {"cccc.nca",      0x2000000},
        };
        const Pfs0Layout L = pfs0_build(files);

        check(std::memcmp(L.header.data(), "PFS0", 4) == 0, "magic is PFS0");
        check(rd32(L.header, 4) == 3, "file count");
        check(L.header_size % 16 == 0, "header is 16-byte aligned");
        check(L.header.size() == L.header_size, "header buffer matches header_size");

        // The string-table field must cover the padding, so data starts exactly at
        // header_size — this is the field installers use to find the data region.
        const size_t entries_end = 0x10 + files.size() * 0x18;
        check(rd32(L.header, 8) == (uint32_t)(L.header_size - entries_end),
              "string table field includes padding");

        // Entry offsets are relative to the end of the header, contiguous, in order.
        check(rd64(L.header, 0x10 + 0 * 0x18 + 0) == 0,        "file 0 at relative 0");
        check(rd64(L.header, 0x10 + 1 * 0x18 + 0) == 0x1000,   "file 1 follows file 0");
        check(rd64(L.header, 0x10 + 2 * 0x18 + 0) == 0x1800,   "file 2 follows file 1");
        check(rd64(L.header, 0x10 + 0 * 0x18 + 8) == 0x1000,   "file 0 size");
        check(rd64(L.header, 0x10 + 2 * 0x18 + 8) == 0x2000000,"file 2 size");

        // Absolute offsets are what a streamer seeks to.
        check(L.data_offsets[0] == L.header_size,          "abs offset 0");
        check(L.data_offsets[1] == L.header_size + 0x1000, "abs offset 1");

        // Names round-trip out of the string table.
        const size_t strtab = entries_end;
        check(std::string((const char*)L.header.data() + strtab + rd32(L.header, 0x10 + 0 * 0x18 + 0x10))
                  == "aaaa.nca", "name 0");
        check(std::string((const char*)L.header.data() + strtab + rd32(L.header, 0x10 + 1 * 0x18 + 0x10))
                  == "bbbb.cnmt.nca", "name 1");

        // THE property: advertised size == streamed size.
        check(pfs0_total_size(files) == L.total_size,
              "pfs0_total_size agrees with pfs0_build");
        check(L.total_size == L.header_size + 0x1000 + 0x800 + 0x2000000,
              "total = header + all data");
    }

    // ── Alignment edge cases: name lengths that land exactly on a boundary ────
    for (int namelen = 1; namelen <= 40; ++namelen) {
        std::vector<Pfs0File> files = { { std::string((size_t)namelen, 'n'), 7 } };
        const Pfs0Layout L = pfs0_build(files);
        check(L.header_size % 16 == 0,
              "header stays aligned for name length " + std::to_string(namelen));
        check(pfs0_total_size(files) == L.total_size,
              "sizes agree at name length " + std::to_string(namelen));
        check(L.data_offsets[0] == L.header_size,
              "data starts at header end, name length " + std::to_string(namelen));
    }

    // ── Degenerate inputs must not produce a malformed header ─────────────────
    {
        const Pfs0Layout L = pfs0_build({});
        check(rd32(L.header, 4) == 0, "empty: zero files");
        check(L.header_size % 16 == 0, "empty: aligned");
        check(L.total_size == L.header_size, "empty: total is header only");
        check(pfs0_total_size({}) == L.total_size, "empty: sizes agree");
    }
    {
        // A multi-GB file must not overflow 32-bit arithmetic anywhere.
        const uint64_t huge = 0x2'0000'0000ull;   // 8 GiB
        std::vector<Pfs0File> files = { {"big.nca", huge}, {"small.nca", 1} };
        const Pfs0Layout L = pfs0_build(files);
        check(rd64(L.header, 0x10 + 0 * 0x18 + 8) == huge, "8 GiB size survives");
        check(rd64(L.header, 0x10 + 1 * 0x18 + 0) == huge, "offset past 4 GiB survives");
        check(L.total_size == L.header_size + huge + 1, "total survives >4 GiB");
        check(pfs0_total_size(files) == L.total_size, "sizes agree past 4 GiB");
    }

    std::printf("%s (%d checks", g_failed ? "FAILURES" : "ALL PASS", g_checks);
    if (g_failed) std::printf(", %d failed", g_failed);
    std::printf(")\n");
    return g_failed ? 1 : 0;
}
