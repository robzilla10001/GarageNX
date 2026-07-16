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

} // namespace Renderer
