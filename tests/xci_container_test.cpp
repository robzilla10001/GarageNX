// tests/xci_container_test.cpp
//
// Tests for StreamInstaller's XCI/XCZ front-end (slice 4c).
//
// Unlike stream_container_test.cpp, this is NOT a characterization suite: there
// is no prior behaviour to pin. It is written alongside the front-end it covers,
// and every assertion here is a claim about what the code SHOULD do, not a
// record of what it already did. Nothing in this file has been on hardware.
//
// Build: see tests/CMakeLists.txt. Runs on the host, no libnx, no device.
//
// SCOPE — read before adding to this file.
//   IN:  the XCI header, both HFS0 layers, locating `secure`, the collector's
//        forward-skip over the signature and the update/normal partitions,
//        container_size()'s must-be-zero rule, malformed-input rejection,
//        and the table bounds that keep a hostile image from exhausting memory.
//   OUT: anything NCZ, for the same reason the PFS0 suite gives — ncz.cpp's
//        !PLATFORM_SWITCH branch stubs the decompressor, so an .ncz entry here
//        exercises a stub. The XCZ test below asserts a REFUSAL (no keys) and
//        nothing about decompression.
//   OUT: finish(). It calls Install::install(), which is a link stub that
//        aborts — meta registration is hardware work. See link_stubs.cpp.

#include "core/keys.hpp"
#include "core/ncm.hpp"
#include "install/installer.hpp"
#include "install/stream_installer.hpp"

#include <algorithm>
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
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

// ── Builders ─────────────────────────────────────────────────────────────────
//
// XCI:  0x000 RSA-2048 signature
//       0x100 "HEAD"
//       0x130 u64 root HFS0 offset          <- 0x120 is the gamecard IV, not this
//       ...   root HFS0 { update, normal, secure, logo }
//       ...   trailing padding to the gamecard's capacity
//
// HFS0: "HFS0" | u32 count | u32 string_table_size | u32 reserved
//       entries[count] x 0x40 { u64 off, u64 size, u32 name_off,
//                               u32 hash_region_size, u64 rsv, u8[0x20] sha256 }
//       string table
//       data, beginning at 0x10 + count*0x40 + string_table_size

struct FakeEntry {
    std::string name;
    uint64_t    rel_off;   // relative to the partition's data region
    uint64_t    size;
};

static void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}
static void put64(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[at + i] = (uint8_t)(x >> (8 * i));
}
static uint8_t byte_at(uint64_t off) { return (uint8_t)((off * 131u + 29u) & 0xFF); }

static std::vector<uint8_t> build_hfs0_head(const std::vector<FakeEntry>& es,
                                            const char* magic = "HFS0") {
    std::vector<char>     strtab;
    std::vector<uint32_t> name_offs;
    for (const auto& e : es) {
        name_offs.push_back((uint32_t)strtab.size());
        strtab.insert(strtab.end(), e.name.begin(), e.name.end());
        strtab.push_back('\0');
    }
    while (strtab.size() % 4) strtab.push_back('\0');

    const uint32_t count = (uint32_t)es.size();
    const uint32_t sts   = (uint32_t)strtab.size();
    std::vector<uint8_t> v(0x10 + (size_t)count * 0x40 + sts, 0);
    std::memcpy(v.data(), magic, 4);
    put32(v, 4, count);
    put32(v, 8, sts);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 0x10 + (size_t)i * 0x40;
        put64(v, at + 0, es[i].rel_off);
        put64(v, at + 8, es[i].size);
        put32(v, at + 16, name_offs[i]);
    }
    if (sts) std::memcpy(v.data() + 0x10 + (size_t)count * 0x40, strtab.data(), sts);
    return v;
}

// A complete HFS0: head plus a data region carrying an offset-keyed pattern.
static std::vector<uint8_t> build_hfs0(const std::vector<FakeEntry>& es,
                                       const char* magic = "HFS0") {
    std::vector<uint8_t> v = build_hfs0_head(es, magic);
    uint64_t end = 0;
    for (const auto& e : es) end = std::max(end, e.rel_off + e.size);
    const size_t base = v.size();
    v.resize(base + (size_t)end);
    for (size_t i = base; i < v.size(); ++i) v[i] = byte_at(i);
    return v;
}

struct XciOpts {
    const char* head_magic   = "HEAD";
    const char* root_magic   = "HFS0";
    const char* secure_magic = "HFS0";
    bool        with_secure  = true;
    uint64_t    root_off     = 0x200;
    // A partition BEFORE secure. Non-zero by default and deliberately not a
    // round multiple of any chunk size the tests use: the collector must skip
    // it whatever the byte boundaries happen to be.
    uint64_t    update_size  = 0x333;
    uint64_t    normal_size  = 0x111;
    // Gamecard padding after the last partition. This is the whole reason
    // container_size() cannot be derived from the entries.
    uint64_t    trailing_pad = 0x4000;
    // Write a different value into 0x130 than where the root HFS0 actually is.
    // 0 means "tell the truth".
    uint64_t    lie_root_off = 0;
};

// Builds a synthetic gamecard image. `out_secure_abs` receives the absolute
// offset of the secure HFS0's own header, which the corruption tests patch.
static std::vector<uint8_t> build_xci(const std::vector<FakeEntry>& ncas,
                                      const XciOpts& o = {},
                                      uint64_t* out_secure_abs = nullptr) {
    const std::vector<uint8_t> secure = build_hfs0(ncas, o.secure_magic);

    std::vector<FakeEntry> parts;
    uint64_t at = 0;
    parts.push_back({"update", at, o.update_size}); at += o.update_size;
    parts.push_back({"normal", at, o.normal_size}); at += o.normal_size;
    const uint64_t secure_rel = at;
    if (o.with_secure) { parts.push_back({"secure", at, secure.size()}); at += secure.size(); }
    parts.push_back({"logo", at, 0x40}); at += 0x40;   // a partition AFTER secure

    const std::vector<uint8_t> root_head = build_hfs0_head(parts, o.root_magic);

    std::vector<uint8_t> v(o.root_off, 0);
    for (size_t i = 0; i < v.size(); ++i) v[i] = byte_at(i);   // RSA sig + header noise
    std::memcpy(v.data() + 0x100, o.head_magic, 4);
    put64(v, 0x130, o.lie_root_off ? o.lie_root_off : o.root_off);

    const uint64_t root_data_abs = o.root_off + root_head.size();
    v.insert(v.end(), root_head.begin(), root_head.end());
    // update + normal: bytes the collector must discard on the way to secure.
    for (uint64_t i = 0; i < o.update_size + o.normal_size; ++i) v.push_back(byte_at(v.size()));
    if (o.with_secure) v.insert(v.end(), secure.begin(), secure.end());
    for (int i = 0; i < 0x40; ++i) v.push_back(byte_at(v.size()));            // logo
    for (uint64_t i = 0; i < o.trailing_pad; ++i) v.push_back(byte_at(v.size()));

    if (out_secure_abs) *out_secure_abs = root_data_abs + secure_rel;
    return v;
}

// ── Harness ──────────────────────────────────────────────────────────────────

struct Rig {
    Core::Keys::Keyset keys;
    Install::Progress  progress;
    StreamInstaller    inst{Core::Ncm::Storage::SdCard, keys, progress};
};

static bool feed_in_chunks(StreamInstaller& si, const std::vector<uint8_t>& v, size_t chunk) {
    for (size_t i = 0; i < v.size(); i += chunk) {
        const size_t n = std::min(chunk, v.size() - i);
        if (!si.feed(v.data() + i, n)) return false;
    }
    return true;
}

static const std::vector<FakeEntry> kNcas = {
    {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,     0x40},
    {"fedcba9876543210fedcba9876543210.nca",      0x40,  0x800},
    {"aaaabbbbccccddddeeeeffff00001111.nca",      0x840, 0x200},
};

// ── Tests ────────────────────────────────────────────────────────────────────

// The core claim: a gamecard image parses, and the NCAs found are the ones in
// `secure`. Passing at chunk size 1 is the strongest single statement here —
// every collection boundary, and the skip across the signature and the
// update/normal partitions, is then crossed one byte at a time.
static void test_xci_parses_at_every_chunk_size() {
    uint64_t secure_abs = 0;
    const std::vector<uint8_t> v = build_xci(kNcas, {}, &secure_abs);

    for (size_t chunk : {(size_t)1, (size_t)7, (size_t)512, (size_t)4096}) {
        Rig r;
        CHECK(r.inst.begin("game.xci", v.size()), "begin() accepts an .xci");
        CHECK(feed_in_chunks(r.inst, v, chunk), "feed a synthetic XCI");
        CHECK(r.inst.ok(), "XCI parsed without error");
        CHECK(r.inst.complete(), "every secure entry consumed");
        CHECK(r.progress.ncas_total.load() == 3, "found exactly the secure partition's NCAs");
    }
    std::printf("  ok: synthetic XCI parses identically at 1/7/512/4096-byte chunks\n");
}

// THE rule of slice 4c. A PFS0's last entry ends at the file's end, so its table
// yields an exact size. A gamecard image does not: it continues past `secure`
// into padding that belongs to the transfer but to no entry. Reporting a size
// derived from the entries would come up short — which does not fail an install,
// it leaves unread bytes in the endpoint and desyncs the session.
static void test_container_size_is_zero_for_xci() {
    uint64_t secure_abs = 0;
    const std::vector<uint8_t> v = build_xci(kNcas, {}, &secure_abs);

    Rig r;
    r.inst.begin("game.xci", v.size());

    // Zero before the tables arrive...
    CHECK(r.inst.container_size() == 0, "zero before parsing");

    // ...zero once they have been parsed and entries exist...
    const size_t head_only = (size_t)secure_abs + 0x400;
    CHECK(head_only < v.size(), "test setup: head_only is inside the image");
    CHECK(r.inst.feed(v.data(), head_only), "feed through the secure table");
    CHECK(r.inst.ok(), "still healthy mid-transfer");
    CHECK(r.progress.ncas_total.load() == 3, "entries are known by now");
    CHECK(r.inst.container_size() == 0, "STILL zero once entries are known");

    // ...and zero at the end. A caller must never be able to sample a moment
    // where this looks like an authority.
    CHECK(r.inst.feed(v.data() + head_only, v.size() - head_only), "feed the rest");
    CHECK(r.inst.complete(), "reached Done");
    CHECK(r.inst.container_size() == 0, "still zero at Done");
    std::printf("  ok: container_size() is 0 for XCI at every point in the transfer\n");
}

// An untrimmed image is padded to the gamecard's capacity, so the bytes after
// `secure` can outweigh the content. They must be swallowed, not misattributed.
static void test_trailing_gamecard_padding_is_ignored() {
    XciOpts o;
    o.trailing_pad = 0x20000;
    uint64_t secure_abs = 0;
    const std::vector<uint8_t> v = build_xci(kNcas, o, &secure_abs);

    Rig r;
    CHECK(r.inst.begin("game.xci", v.size()), "begin()");
    CHECK(feed_in_chunks(r.inst, v, 1024), "feed an untrimmed image");
    CHECK(r.inst.ok() && r.inst.complete(), "padding past secure is swallowed");
    CHECK(r.inst.container_size() == 0, "padding did not tempt a size out of us");
    std::printf("  ok: trailing gamecard padding is discarded\n");
}

// The collector's forward-skip was dead code until this slice: PFS0's two
// collections are contiguous, so nothing ever exercised `m_pos < m_want_off`.
// Here it carries the whole format. If the skip miscounted by even one byte the
// secure HFS0's magic would not land where expected, so these are really
// assertions about the skip rather than about the magic.
static void test_forward_skips_land_exactly() {
    for (uint64_t upd : {(uint64_t)0, (uint64_t)1, (uint64_t)0x333, (uint64_t)0x10001}) {
        XciOpts o;
        o.update_size = upd;
        o.normal_size = 0x111;
        uint64_t secure_abs = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &secure_abs);

        Rig r;
        CHECK(r.inst.begin("game.xci", v.size()), "begin()");
        CHECK(feed_in_chunks(r.inst, v, 13), "feed with an awkward chunk size");
        CHECK(r.inst.ok() && r.inst.complete(), "skip landed on the secure header exactly");
    }
    // A root HFS0 that does not sit at the usual 0x200 must work too — the
    // offset at 0x130 is the authority, not the convention.
    {
        XciOpts o;
        o.root_off = 0x1000;
        uint64_t secure_abs = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &secure_abs);
        Rig r;
        CHECK(r.inst.begin("game.xci", v.size()), "begin()");
        CHECK(feed_in_chunks(r.inst, v, 64), "feed an image with a displaced root");
        CHECK(r.inst.ok() && r.inst.complete(), "root HFS0 found at a non-standard offset");
    }
    std::printf("  ok: forward skips land exactly, at four skip distances and a moved root\n");
}

// The format is chosen from the filename, and the host controls its case.
static void test_format_is_chosen_from_the_name() {
    uint64_t secure_abs = 0;
    const std::vector<uint8_t> v = build_xci(kNcas, {}, &secure_abs);
    {
        Rig r;
        r.inst.begin("GAME.XCI", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(r.inst.ok() && r.inst.complete(), "an uppercase .XCI routes to the XCI front-end");
    }
    {   // The same bytes offered as an .nsp must be refused, not misparsed.
        Rig r;
        r.inst.begin("game.nsp", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "XCI bytes under an .nsp name are refused");
        CHECK(r.inst.error().find("PFS0") != std::string::npos, "refused by the PFS0 front-end");
    }
    std::printf("  ok: .XCI/.xcz route by name, case-insensitively\n");
}

// An XCZ is an XCI whose secure partition holds .ncz files. It needs no XCI-side
// support at all — classification is by name, and the key precondition in
// finalize_entries() is shared. This asserts the REFUSAL only; decompression is
// stubbed on this platform (see the scope note above).
static void test_xcz_without_keys_is_refused_at_the_table() {
    const std::vector<FakeEntry> ncas = {
        {"0123456789abcdef0123456789abcdef.cnmt.nca", 0,    0x40},
        {"fedcba9876543210fedcba9876543210.ncz",      0x40, 0x800},
    };
    uint64_t secure_abs = 0;
    const std::vector<uint8_t> v = build_xci(ncas, {}, &secure_abs);

    Rig r;
    CHECK(!r.keys.has_header_key, "precondition: no header key");
    r.inst.begin("game.xcz", v.size());
    r.inst.feed(v.data(), v.size());
    CHECK(!r.inst.ok(), "keyless XCZ is refused");
    CHECK(!r.inst.complete(), "refused container never reaches Done");
    // As in the PFS0 suite: assert that the refusal happened, not the reason
    // text, which comes partly from a link stub.
    CHECK(r.inst.error().find("prod.keys") != std::string::npos, "error names the missing keys");
    // The noun IS asserted, though, and deliberately. Both formats reach this
    // refusal by the same route, so it is easy to write one that says NSZ to
    // everyone — and this string is the entire explanation a user gets. Someone
    // installing an XCZ being told about NSZ reads as a bug in us.
    CHECK(r.inst.error().find("XCZ") != std::string::npos, "refusal names XCZ, not NSZ");
    CHECK(r.inst.error().find("NSZ") == std::string::npos, "and does not mention NSZ at all");
    std::printf("  ok: keyless XCZ refused at the secure table, and told it was an XCZ\n");
}

// Malformed images must be refused, not half-installed.
static void test_malformed_images_are_rejected() {
    {   // not a gamecard image at all
        XciOpts o; o.head_magic = "XXXX";
        uint64_t sa = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &sa);
        Rig r;
        r.inst.begin("bad.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "bad HEAD magic is refused");
        CHECK(r.inst.error().find("XCI") != std::string::npos, "error says XCI");
    }
    {   // root HFS0 is not an HFS0
        XciOpts o; o.root_magic = "XXXX";
        uint64_t sa = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &sa);
        Rig r;
        r.inst.begin("bad.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "bad root HFS0 magic is refused");
        CHECK(r.inst.error().find("root") != std::string::npos, "error names the root partition");
    }
    {   // secure HFS0 is not an HFS0
        XciOpts o; o.secure_magic = "XXXX";
        uint64_t sa = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &sa);
        Rig r;
        r.inst.begin("bad.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "bad secure HFS0 magic is refused");
        CHECK(r.inst.error().find("secure") != std::string::npos, "error names the secure partition");
    }
    {   // a gamecard image with no secure partition has nothing to install
        XciOpts o; o.with_secure = false;
        uint64_t sa = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &sa);
        Rig r;
        r.inst.begin("nosecure.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "missing secure partition is refused");
        CHECK(r.inst.error().find("secure") != std::string::npos, "error names what is missing");
    }
    std::printf("  ok: bad HEAD / bad root / bad secure / no secure all refused\n");
}

// The root offset at 0x130 is host-supplied and entirely unvalidated by the
// format. Both directions of nonsense must be caught at the header, not
// discovered gigabytes later.
static void test_wild_root_offsets_are_refused() {
    {   // Past the end. want() would accept this (it is forward), and the
        // collector would then skip silently to the end of a multi-GB transfer
        // before anything complained.
        XciOpts o; o.lie_root_off = 0x7FFFFFFFFFFFull;
        uint64_t sa = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &sa);
        Rig r;
        r.inst.begin("wild.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "a root offset past the end of the image is refused");
        CHECK(r.inst.error().find("past the end") != std::string::npos, "error explains why");
    }
    {   // Backwards. The header collection ends at 0x138, so 0x50 is already
        // gone. This is want()'s rewind guard, which nothing else exercises.
        XciOpts o; o.lie_root_off = 0x50;
        uint64_t sa = 0;
        const std::vector<uint8_t> v = build_xci(kNcas, o, &sa);
        Rig r;
        r.inst.begin("wild.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "a backwards root offset is refused");
        CHECK(r.inst.error().find("rewind") != std::string::npos, "refused by the rewind guard");
    }
    std::printf("  ok: root offsets past the end and behind us are both refused\n");
}

// count and string-table size are host-supplied and feed straight into an
// allocation. Unbounded, a hostile or corrupt image reserves gigabytes and takes
// the process out via bad_alloc — a crash, on a console, from a bad file.
static void test_hostile_table_sizes_cannot_exhaust_memory() {
    {   // implausible file count
        uint64_t secure_abs = 0;
        std::vector<uint8_t> v = build_xci(kNcas, {}, &secure_abs);
        put32(v, (size_t)secure_abs + 4, 999999);
        Rig r;
        r.inst.begin("hostile.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "implausible secure file count is refused");
        CHECK(r.inst.error().find("file count") != std::string::npos, "error names the count");
    }
    {   // a string table claiming 4 GiB. The count stays legal, so only the
        // table-size bound stands between this and a 4 GiB reserve().
        uint64_t secure_abs = 0;
        std::vector<uint8_t> v = build_xci(kNcas, {}, &secure_abs);
        put32(v, (size_t)secure_abs + 8, 0xFFFFFFFFu);
        Rig r;
        r.inst.begin("hostile.xci", v.size());
        r.inst.feed(v.data(), v.size());
        CHECK(!r.inst.ok(), "a 4 GiB string table is refused rather than allocated");
        CHECK(r.inst.error().find("string table") != std::string::npos, "error names the string table");
    }
    std::printf("  ok: hostile counts and table sizes are refused, not allocated\n");
}

int main() {
    std::printf("StreamInstaller XCI/XCZ front-end (slice 4c)\n");
    test_xci_parses_at_every_chunk_size();
    test_container_size_is_zero_for_xci();
    test_trailing_gamecard_padding_is_ignored();
    test_forward_skips_land_exactly();
    test_format_is_chosen_from_the_name();
    test_xcz_without_keys_is_refused_at_the_table();
    test_malformed_images_are_rejected();
    test_wild_root_offsets_are_refused();
    test_hostile_table_sizes_cannot_exhaust_memory();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
