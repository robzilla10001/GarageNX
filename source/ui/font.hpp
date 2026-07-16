#pragma once
// source/ui/font.hpp
// Inter TTF loader and size cache.
// All text rendering goes through Font::get() — never load TTF_Font directly elsewhere.

#include <SDL2/SDL_ttf.h>
#include <string>

namespace Font {

// ─── Named sizes ──────────────────────────────────────────────────────────────
// Add sizes here as the UI requires them. Values are in points at 720p base.

enum class Size {
    Tiny    = 12,   // timestamps, footnotes
    Small   = 14,   // metadata, secondary labels
    Body    = 16,   // primary list text (default)
    Medium  = 18,   // section headers, emphasis
    Large   = 22,   // screen titles
    Title   = 26,   // main title bar
};

enum class Weight { Regular, Bold };

// Typeface family. Sans is Inter (proportional, the UI default).
// Mono is DejaVu Sans Mono, used where fixed-width alignment matters (hex view).
enum class Family { Sans, Mono };

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Initialize the font system. Call once after SDL_Init.
/// asset_root should be the directory containing assets/fonts/*.ttf.
/// Returns false and logs on failure.
bool init(const std::string& asset_root);

/// Release all loaded fonts. Call before SDL_Quit.
void shutdown();

// ─── Access ───────────────────────────────────────────────────────────────────

/// Retrieve a font at a given size, weight, and family.
/// Font is loaded on first access and cached — subsequent calls are O(1).
/// Returns nullptr on failure (log will have details).
TTF_Font* get(Size size, Weight weight = Weight::Regular,
              Family family = Family::Sans);

/// Convenience: render UTF-8 text to a surface using the specified font.
/// Caller owns the returned SDL_Surface (SDL_FreeSurface when done).
SDL_Surface* render(const std::string& text, Size size, Weight weight,
                    SDL_Color color, Family family = Family::Sans);

/// Advance width of a single glyph in the mono font at a given size, in pixels.
/// Used to lay out fixed-width columns (hex view). Cached per size.
int mono_advance(Size size);

} // namespace Font
