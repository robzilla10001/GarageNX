// tests/ncz_window_test.cpp
//
// Off-device harness for Install::NczWindow. Deliberately dependency-free —
// plain C++17 and asserts, no test framework — because the coding standard bars
// new third-party dependencies without an approved task, and because NczWindow
// uses only std::mutex/std::condition_variable and so needs no libnx stub to
// build. Compile and run it directly:
//
//   g++ -std=c++17 -I../source -fsanitize=thread  -O1 -g ncz_window_test.cpp ../source/install/ncz_window.cpp -o w_tsan && ./w_tsan
//   g++ -std=c++17 -I../source -fsanitize=address,undefined -O1 -g ncz_window_test.cpp ../source/install/ncz_window.cpp -o w_asan && ./w_asan
//
// Caps are deliberately tiny here (KB, not MB) so the ring wraps thousands of
// times and the prefix/window seam is crossed constantly. The production
// defaults would hide every one of those transitions behind sheer capacity.

#include "install/ncz_window.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using Install::NczWindow;

// Deterministic, offset-keyed content so any byte can be verified in isolation.
static uint8_t byte_at(uint64_t off) {
    return static_cast<uint8_t>((off * 131u + (off >> 13) * 7u + 29u) & 0xFF);
}

static std::vector<uint8_t> make_stream(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = byte_at(i);
    return v;
}

static int g_checks = 0;
#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);                         \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

// ── 1. The read pattern ncz.cpp actually issues ──────────────────────────────
// get_decompressed_size(): 0x4000 -> block header -> BACK to 0.
// decompress(): 0 -> 0x4000 -> sections -> magic at compressed_start -> data.
// Every one of those re-reads must be served from the retained prefix.
static void test_header_reread_pattern() {
    const size_t kPrefix = 64 * 1024, kWindow = 16 * 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(256 * 1024);

    std::thread producer([&] {
        w.push(data.data(), data.size());
        w.finish();
    });

    uint8_t buf[0x4000];

    // --- sizing pass ---
    CHECK(w.read(0x4000, buf, 16) == 16, "sizing: NczHeader at 0x4000");
    CHECK(buf[0] == byte_at(0x4000), "sizing: NczHeader content");
    CHECK(w.read(0x4010, buf, 0x18) == 0x18, "sizing: NczBlockHeader");
    CHECK(w.read(0, buf, 0xC00) == 0xC00, "sizing: BACKWARDS seek to 0");
    CHECK(buf[0] == byte_at(0), "sizing: offset 0 content");

    // --- decompress pass: the whole region again, from scratch ---
    CHECK(w.read(0, buf, 0x4000) == 0x4000, "decompress: re-read 0 for 0x4000");
    CHECK(buf[0x3FFF] == byte_at(0x3FFF), "decompress: header tail content");
    CHECK(w.read(0x4000, buf, 16) == 16, "decompress: re-read NczHeader");
    CHECK(w.read(0x4010, buf, 0x40 * 4) == 0x40 * 4, "decompress: section table");

    // --- the 4-byte magic sniff, then the re-read from the same offset ---
    const uint64_t compressed_start = 0x4118;
    uint32_t magic = 0;
    CHECK(w.read(compressed_start, &magic, 4) == 4, "magic sniff");
    CHECK(w.read(compressed_start, buf, 0x1000) == 0x1000, "re-read from compressed_start");
    CHECK(buf[0] == byte_at(compressed_start), "re-read content matches");

    CHECK(!w.failed(), "no failure across the header pattern");
    w.abort("test done");
    producer.join();
    std::printf("  ok: header re-read pattern (sizing + decompress + magic sniff)\n");
}

// ── 2. Reads that straddle the prefix/window seam ────────────────────────────
// safe_read() does not loop, so a request crossing the boundary must come back
// whole in ONE call. A ~1 MB block read crossing the 8 MB prefix does this for
// real; here the seam is at 64 KB.
static void test_seam_spanning_read() {
    const size_t kPrefix = 64 * 1024, kWindow = 32 * 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(512 * 1024);

    std::thread producer([&] {
        for (size_t i = 0; i < data.size(); i += 1337) {
            size_t n = std::min<size_t>(1337, data.size() - i);
            if (!w.push(data.data() + i, n))
                return;
        }
        w.finish();
    });

    // Straddle: starts inside the prefix, ends well above it.
    const uint64_t off = kPrefix - 4096;
    std::vector<uint8_t> buf(16 * 1024);
    const size_t got = w.read(off, buf.data(), buf.size());
    CHECK(got == buf.size(), "seam read returns the FULL length in one call");
    for (size_t i = 0; i < got; ++i)
        CHECK(buf[i] == byte_at(off + i), "seam read content is byte-exact");

    w.abort("test done");
    producer.join();
    std::printf("  ok: seam-spanning read served whole in one call\n");
}

// ── 3. Long forward run: back-pressure both ways, heavy ring wrap ────────────
// The window is far smaller than the stream, so the producer blocks on a slow
// reader and the reader blocks on a slow producer, thousands of times.
static void test_forward_run_byte_exact() {
    const size_t kPrefix = 8 * 1024, kWindow = 4 * 1024;
    const size_t kTotal = 4 * 1024 * 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(kTotal);

    std::thread producer([&] {
        std::mt19937 rng(1234);
        std::uniform_int_distribution<size_t> chunk(1, 3000);
        size_t i = 0;
        while (i < kTotal) {
            size_t n = std::min(chunk(rng), kTotal - i);
            if (!w.push(data.data() + i, n))
                return;
            i += n;
        }
        w.finish();
    });

    std::mt19937 rng(99);
    std::uniform_int_distribution<size_t> chunk(1, 3500);
    std::vector<uint8_t> buf(4096);
    uint64_t off = 0;
    while (off < kTotal) {
        size_t want = std::min<uint64_t>(chunk(rng), kTotal - off);
        size_t got = w.read(off, buf.data(), want);
        CHECK(got == want, "forward run: full-length read");
        for (size_t i = 0; i < got; ++i)
            CHECK(buf[i] == byte_at(off + i), "forward run: byte-exact");
        off += got;
    }
    CHECK(!w.failed(), "forward run: clean");
    producer.join();
    std::printf("  ok: 4 MB forward run byte-exact through a 4 KB window\n");
}

// ── 4. Design violations must fail loudly, not return wrong bytes ────────────
static void test_backwards_seek_outside_prefix_fails() {
    const size_t kPrefix = 8 * 1024, kWindow = 8 * 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(128 * 1024);
    std::thread producer([&] {
        w.push(data.data(), data.size());
        w.finish();
    });

    std::vector<uint8_t> buf(1024);
    CHECK(w.read(64 * 1024, buf.data(), 1024) == 1024, "advance the watermark");
    // Now seek back above the prefix but below the watermark.
    CHECK(w.read(32 * 1024, buf.data(), 1024) == 0, "backwards seek returns 0");
    CHECK(w.failed(), "backwards seek latches a failure");
    CHECK(w.error().find("watermark") != std::string::npos, "error names the watermark");
    producer.join();
    std::printf("  ok: backwards seek outside prefix fails loudly (%s)\n", w.error().c_str());
}

static void test_oversized_read_fails() {
    const size_t kPrefix = 4 * 1024, kWindow = 4 * 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(128 * 1024);
    std::thread producer([&] {
        w.push(data.data(), data.size());
        w.finish();
    });
    std::vector<uint8_t> buf(64 * 1024);
    // Would deadlock against the producer if it were allowed to wait.
    CHECK(w.read(8 * 1024, buf.data(), 64 * 1024) == 0, "oversized read returns 0");
    CHECK(w.failed(), "oversized read latches a failure");
    CHECK(w.error().find("window capacity") != std::string::npos, "error names capacity");
    producer.join();
    std::printf("  ok: read larger than window fails loudly instead of deadlocking\n");
}

// ── 5. End of stream returns short rather than blocking forever ──────────────
static void test_eof_short_read() {
    const size_t kPrefix = 4 * 1024, kWindow = 4 * 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(5000);
    w.push(data.data(), data.size());
    w.finish();

    std::vector<uint8_t> buf(4096);
    CHECK(w.read(4000, buf.data(), 4096) == 1000, "short read at EOF");
    for (size_t i = 0; i < 1000; ++i)
        CHECK(buf[i] == byte_at(4000 + i), "EOF short read content");
    CHECK(w.read(5000, buf.data(), 16) == 0, "read entirely past EOF returns 0");
    CHECK(!w.failed(), "EOF is not a failure");
    std::printf("  ok: EOF returns a short read, not a hang\n");
}

// ── 6. abort() unblocks both sides ───────────────────────────────────────────
static void test_abort_unblocks_reader() {
    NczWindow w(4 * 1024, 4 * 1024);
    std::atomic<bool> returned{false};
    std::thread reader([&] {
        std::vector<uint8_t> buf(64);
        w.read(1024, buf.data(), 64);  // nothing pushed: blocks
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!returned.load(), "reader is genuinely blocked before abort");
    w.abort("cancelled");
    reader.join();
    CHECK(returned.load(), "abort released the blocked reader");
    CHECK(w.error() == "cancelled", "first abort reason latches");
    std::printf("  ok: abort() releases a blocked reader\n");
}

static void test_abort_unblocks_producer() {
    const size_t kPrefix = 1024, kWindow = 1024;
    NczWindow w(kPrefix, kWindow);
    auto data = make_stream(64 * 1024);
    std::atomic<bool> returned{false};
    std::thread producer([&] {
        w.push(data.data(), data.size());  // no reader: fills, then blocks
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!returned.load(), "producer is genuinely blocked on a full window");
    w.abort("cancelled");
    producer.join();
    CHECK(returned.load(), "abort released the blocked producer");
    std::printf("  ok: abort() releases a blocked producer\n");
}

int main() {
    std::printf("NczWindow harness\n");
    test_header_reread_pattern();
    test_seam_spanning_read();
    test_forward_run_byte_exact();
    test_backwards_seek_outside_prefix_fails();
    test_oversized_read_fails();
    test_eof_short_read();
    test_abort_unblocks_reader();
    test_abort_unblocks_producer();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
