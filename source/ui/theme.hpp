#pragma once
// source/ui/theme.hpp
// Color token system for GarageNX.
// All rendering code uses these tokens — never hardcoded colors.
// Swapping dark ↔ light is a single call to Theme::set().

#include <SDL2/SDL.h>

namespace Theme {

// ─── Token identifiers ────────────────────────────────────────────────────────
// Every distinct color role in the UI has a named token.
// Add new tokens here when a new role is needed; never add raw colors elsewhere.

enum class Token {
    BgBase,         // main background
    BgSurface,      // cards, panels, list rows
    BgElevated,     // focused / selected elements
    FgPrimary,      // main text
    FgSecondary,    // metadata, labels, dimmed text
    FgDisabled,     // unavailable options
    Accent,         // selection highlight, progress bars, focus ring
    AccentWarn,     // warnings, destructive confirmations
    AccentDanger,   // delete / wipe — stronger than warn
    AccentOk,       // success states, complete indicators
    Border,         // subtle separators
    StatusBarBg,    // persistent bottom bar background
    TitleBarBg,     // persistent top bar background

    _Count          // sentinel — keep last
};

enum class Variant { Dark, Light };

// ─── Public API ───────────────────────────────────────────────────────────────

/// Apply a theme variant. Must be called before the first frame.
/// Safe to call at any time — takes effect on the next frame.
void set(Variant v);

/// Retrieve the current variant.
Variant current();

/// Look up a color token. Returns an SDL_Color ready for use with SDL_SetRenderDrawColor.
SDL_Color get(Token token);

/// Convenience: set the renderer draw color from a token.
/// Equivalent to: SDL_SetRenderDrawColor(renderer, get(token)...)
void apply(SDL_Renderer* renderer, Token token);

/// Toggle between dark and light. Convenience for the settings toggle.
void toggle();

} // namespace Theme
