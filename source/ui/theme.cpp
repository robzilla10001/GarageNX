// source/ui/theme.cpp

#include "ui/theme.hpp"
#include <array>
#include <stdexcept>

namespace Theme {

// ─── Color tables ─────────────────────────────────────────────────────────────
// One entry per Token, ordered to match the Token enum.
// Adjust values here only — never touch rendering code for color changes.

static constexpr int TOKEN_COUNT = static_cast<int>(Token::_Count);

using ColorTable = std::array<SDL_Color, TOKEN_COUNT>;

// Helper macro: RGB with full alpha
#define C(r, g, b) SDL_Color{ (r), (g), (b), 255 }

static constexpr ColorTable DARK_THEME = {
    //                      R    G    B
    C(0x1C, 0x1F, 0x26),  // BgBase
    C(0x25, 0x28, 0x30),  // BgSurface
    C(0x2E, 0x32, 0x40),  // BgElevated
    C(0xE8, 0xEA, 0xF0),  // FgPrimary
    C(0x8B, 0x90, 0xA0),  // FgSecondary
    C(0x4A, 0x4F, 0x60),  // FgDisabled
    C(0x4A, 0x90, 0xD9),  // Accent
    C(0xE8, 0xA0, 0x20),  // AccentWarn
    C(0xD9, 0x4A, 0x4A),  // AccentDanger
    C(0x4A, 0xB8, 0x70),  // AccentOk
    C(0x33, 0x37, 0x4A),  // Border
    C(0x14, 0x16, 0x1C),  // StatusBarBg
    C(0x14, 0x16, 0x1C),  // TitleBarBg
};

static constexpr ColorTable LIGHT_THEME = {
    //                      R    G    B
    C(0xF2, 0xF4, 0xF7),  // BgBase
    C(0xFF, 0xFF, 0xFF),  // BgSurface
    C(0xE8, 0xEC, 0xF2),  // BgElevated
    C(0x1C, 0x1F, 0x26),  // FgPrimary
    C(0x5A, 0x60, 0x70),  // FgSecondary
    C(0xB0, 0xB8, 0xC8),  // FgDisabled
    C(0x29, 0x70, 0xB8),  // Accent
    C(0xC0, 0x70, 0x10),  // AccentWarn
    C(0xB8, 0x30, 0x30),  // AccentDanger
    C(0x2A, 0x90, 0x50),  // AccentOk
    C(0xD0, 0xD8, 0xE8),  // Border
    C(0xE0, 0xE4, 0xEC),  // StatusBarBg
    C(0xE0, 0xE4, 0xEC),  // TitleBarBg
};

#undef C

// ─── State ────────────────────────────────────────────────────────────────────

static Variant s_current = Variant::Dark;
static const ColorTable* s_table = &DARK_THEME;

// ─── Implementation ───────────────────────────────────────────────────────────

void set(Variant v) {
    s_current = v;
    s_table   = (v == Variant::Dark) ? &DARK_THEME : &LIGHT_THEME;
}

Variant current() {
    return s_current;
}

SDL_Color get(Token token) {
    return (*s_table)[static_cast<int>(token)];
}

void apply(SDL_Renderer* renderer, Token token) {
    SDL_Color c = get(token);
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
}

void toggle() {
    set(s_current == Variant::Dark ? Variant::Light : Variant::Dark);
}

} // namespace Theme
