#pragma once
// source/ui/renderer.hpp
// SDL2 context owner. One renderer for the entire app lifetime.
// Handles docked/handheld scale detection and the frame lifecycle.

#include <SDL2/SDL.h>
#include <string>

namespace Renderer {

// ─── Constants ────────────────────────────────────────────────────────────────
static constexpr int BASE_WIDTH  = 1280;
static constexpr int BASE_HEIGHT = 720;

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Initialize SDL2, create the window and renderer.
/// asset_root is passed through to Font::init().
/// Returns false on failure.
bool init(const std::string& asset_root);

/// Destroy renderer, window, and quit SDL. Call at app exit.
void shutdown();

// ─── Frame lifecycle ──────────────────────────────────────────────────────────

/// Begin a frame: clear the screen to BgBase.
void begin_frame();

/// End a frame: present the renderer.
void end_frame();

// ─── Accessors ────────────────────────────────────────────────────────────────

SDL_Renderer* get();
SDL_Window*   window();

/// True when running docked (1080p output). The renderer is always addressed
/// in 1280x720 logical coordinates — SDL handles the scale automatically.
bool is_docked();

/// Draw a filled rectangle using the current draw color.
void fill_rect(int x, int y, int w, int h);

/// Draw a rectangle outline using the current draw color.
void draw_rect(int x, int y, int w, int h);

/// Draw a horizontal line using the current draw color.
void hline(int x, int y, int w);

/// Blit an SDL_Surface to the renderer at (x, y).
/// Caller retains ownership of the surface.
void blit(SDL_Surface* surface, int x, int y);

/// Blit an SDL_Surface centered within a bounding box.
void blit_centered(SDL_Surface* surface, int x, int y, int w, int h);

/// Blit an SDL_Surface right-aligned within a bounding box.
void blit_right(SDL_Surface* surface, int x, int y, int w, int h);

// ─── Cached text ──────────────────────────────────────────────────────────────
// draw_text rasterises + uploads a string once and reuses the texture across
// frames, keyed by (text, size, weight, family, colour). Use this instead of
// Font::render + blit for anything drawn every frame (list rows, labels): the
// old path rebuilt and destroyed a texture per row per frame, which is what made
// busy screens stutter. Returns the drawn size via out_w/out_h (may be null).
// Call advance_frame() once per frame so the cache can age out unused entries.
struct TextParams {
    int size;     // Font::Size
    int weight;   // Font::Weight
    int family;   // Font::Family
};
void draw_text(const std::string& text, int size, int weight, int family,
               SDL_Color color, int x, int y, int* out_w = nullptr, int* out_h = nullptr,
               int clip_w = 0);

/// Measure what draw_text would produce, without drawing. Populates a cache entry
/// as a side effect (the texture will be reused on the subsequent draw).
void measure_text(const std::string& text, int size, int weight, int family,
                  SDL_Color color, int* out_w, int* out_h);

/// Advance the cache's frame clock and evict stale/over-cap entries. Called once
/// per frame by begin_frame(); exposed for tests.
void text_cache_advance_frame();

/// Live entry count — for diagnostics/tests.
size_t text_cache_size();

/// Destroy all cached textures. Called by shutdown() before the renderer dies.
void text_cache_clear();

} // namespace Renderer
