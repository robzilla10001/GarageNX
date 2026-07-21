// tests/text_cache_test.cpp
//
// Tests the pure core of the rendered-text cache (source/ui/text_cache.hpp):
// key equality/hashing and the eviction policy. The SDL texture handling in
// renderer.cpp is glue around this and is hardware-verified; the logic that can
// be subtly wrong — what counts as the same cached string, and when an entry is
// evicted — lives here and is tested without SDL.

#include "ui/text_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

using namespace Renderer;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static TextKey key(const std::string& t, int size = 1, int weight = 0,
                   int family = 0, uint32_t color = 0xFFFFFFFF) {
    return TextKey{t, size, weight, family, color};
}

// ── Equal content ⇒ equal key ⇒ same cache slot ─────────────────────────────
static void test_identical_keys_are_equal() {
    CHECK(key("Games") == key("Games"), "same text/attrs compare equal");
    TextKeyHash h;
    CHECK(h(key("Games")) == h(key("Games")), "same key hashes equal");
    std::printf("  ok: identical keys are equal and hash equally\n");
}

// ── Any differing attribute ⇒ different key (so it gets its own texture) ─────
static void test_differing_attributes_differ() {
    CHECK(!(key("Games") == key("games")), "text case differs");
    CHECK(!(key("A", 1) == key("A", 2)), "size differs");
    CHECK(!(key("A", 1, 0) == key("A", 1, 1)), "weight differs");
    CHECK(!(key("A", 1, 0, 0) == key("A", 1, 0, 1)), "family differs");
    CHECK(!(key("A", 1, 0, 0, 0xFFFFFFFF) == key("A", 1, 0, 0, 0xFF0000FF)),
          "colour differs");
    std::printf("  ok: any differing attribute yields a distinct key\n");
}

// ── The key works as an unordered_map key (hash + eq together) ──────────────
static void test_usable_as_map_key() {
    std::unordered_map<TextKey, int, TextKeyHash> m;
    m[key("one")] = 1;
    m[key("two")] = 2;
    m[key("one")] = 11;                 // overwrites, not a second entry
    CHECK(m.size() == 2, "same key overwrites rather than duplicating");
    CHECK(m[key("one")] == 11, "value updated for existing key");
    CHECK(m[key("two")] == 2, "other key untouched");
    std::printf("  ok: TextKey works as an unordered_map key\n");
}

// ── pack_color round-trips the byte order used in the key ────────────────────
static void test_pack_color_distinct() {
    CHECK(pack_color(255, 0, 0, 255) != pack_color(0, 0, 255, 255),
          "red vs blue pack differently");
    CHECK(pack_color(1, 2, 3, 4) == pack_color(1, 2, 3, 4),
          "same bytes pack identically");
    std::printf("  ok: pack_color distinguishes colours\n");
}

// ── Eviction: staleness by frame age ─────────────────────────────────────────
static void test_eviction_staleness() {
    EvictionPolicy p;                   // max_idle_frames = 240
    CHECK(!p.is_stale(/*last*/1000, /*now*/1000), "just-used is not stale");
    CHECK(!p.is_stale(1000, 1000 + 239), "under the idle window is not stale");
    CHECK(p.is_stale(1000, 1000 + 240), "at the idle window is stale");
    CHECK(p.is_stale(1000, 1000 + 10000), "long-idle is stale");
    std::printf("  ok: staleness triggers exactly at max_idle_frames\n");
}

// ── Eviction: hard capacity cap ──────────────────────────────────────────────
static void test_eviction_capacity() {
    EvictionPolicy p;                   // max_entries = 512
    CHECK(!p.over_capacity(512), "at cap is not over");
    CHECK(p.over_capacity(513), "one past cap is over");
    CHECK(!p.over_capacity(0), "empty is not over");
    std::printf("  ok: capacity cap triggers past max_entries\n");
}

int main() {
    std::printf("Text cache core (file-browser render-perf fix)\n");
    test_identical_keys_are_equal();
    test_differing_attributes_differ();
    test_usable_as_map_key();
    test_pack_color_distinct();
    test_eviction_staleness();
    test_eviction_capacity();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
