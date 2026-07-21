// source/ui/text_cache.hpp
//
// The pure, SDL-free core of the rendered-text cache: the key that identifies a
// unique rasterized string, and the eviction policy. Kept separate from the SDL
// texture handling in renderer.cpp so this logic — the part that can be subtly
// wrong — is unit-testable on the host (see tests/text_cache_test.cpp).
//
// WHY THIS EXISTS: List::draw() and the file browser call Font::render() +
// SDL_CreateTextureFromSurface + SDL_DestroyTexture for every visible row every
// frame — rasterising and uploading text that never changed, then throwing the
// texture away. On a busy screen (two panes of file lists) that is 40+
// rasterise+upload+destroy cycles per frame, dropping the frame rate, which is
// what makes navigation stutter and drop inputs. Caching the texture by content
// collapses steady-state cost to one RenderCopy per row.

#pragma once

#include <cstdint>
#include <string>

namespace Renderer {

// Identifies a unique rasterised string. Two draws with equal keys can share one
// texture. Colour is part of the key because the same text in a different theme
// colour is a different bitmap; a theme change therefore produces new keys and
// the old ones age out naturally.
struct TextKey {
    std::string text;
    int         size   = 0;   // Font::Size cast to int
    int         weight = 0;   // Font::Weight cast to int
    int         family = 0;   // Font::Family cast to int
    uint32_t    color  = 0;   // packed RGBA

    bool operator==(const TextKey& o) const {
        return size == o.size && weight == o.weight && family == o.family &&
               color == o.color && text == o.text;
    }
};

struct TextKeyHash {
    size_t operator()(const TextKey& k) const {
        // FNV-1a over the attributes and the string. Cheap and adequate for a
        // few hundred live entries.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) {
            for (int i = 0; i < 8; ++i) { h ^= (v & 0xff); h *= 1099511628211ull; v >>= 8; }
        };
        mix((uint64_t)k.size);
        mix((uint64_t)k.weight);
        mix((uint64_t)k.family);
        mix((uint64_t)k.color);
        for (unsigned char c : k.text) { h ^= c; h *= 1099511628211ull; }
        return (size_t)h;
    }
};

// Pack an RGBA colour into the key's uint32.
inline uint32_t pack_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

// Eviction policy, expressed without any SDL dependency so it can be tested.
// The cache is bounded two ways: a hard entry cap (so a directory of thousands
// of unique names cannot grow the cache without limit) and an age cap (entries
// untouched for N frames are dropped, so scrolling away from a screen frees its
// text). touch() records the frame an entry was last used; should_evict() and
// over_capacity() decide what goes.
struct EvictionPolicy {
    uint64_t max_entries    = 512;   // hard cap on live textures
    uint64_t max_idle_frames = 240;  // ~4s at 60fps before an unused entry ages out

    // True if an entry last used at `last_used_frame` is stale at `now`.
    bool is_stale(uint64_t last_used_frame, uint64_t now) const {
        return (now - last_used_frame) >= max_idle_frames;
    }
    // True if the live count exceeds the hard cap and eviction must run.
    bool over_capacity(uint64_t live_count) const {
        return live_count > max_entries;
    }
};

} // namespace Renderer
