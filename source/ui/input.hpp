#pragma once
// source/ui/input.hpp
// Controller state abstraction.
// Wraps SDL joystick events into named actions with button-repeat support.
// All UI code reads from this — never from SDL directly.

#include <SDL2/SDL.h>
#include <cstdint>

namespace Input {

// ─── Logical button map ───────────────────────────────────────────────────────
// Maps to Switch Pro Controller / Joy-Con layout.
// On PC these map to keyboard keys for development convenience.

enum class Button : uint32_t {
    A       = (1 << 0),   // Confirm / Open
    B       = (1 << 1),   // Back / Cancel
    X       = (1 << 2),   // Select (file browser)
    Y       = (1 << 3),   // Mark All / Deselect All
    L       = (1 << 4),   // Page previous
    R       = (1 << 5),   // Page next
    ZL      = (1 << 6),
    ZR      = (1 << 7),
    Plus    = (1 << 8),   // Context menu
    Minus   = (1 << 9),   // Split view toggle
    DUp     = (1 << 10),  // Navigate up
    DDown   = (1 << 11),  // Navigate down
    DLeft   = (1 << 12),  // Navigate left
    DRight  = (1 << 13),  // Navigate right
    LStickUp    = (1 << 14),
    LStickDown  = (1 << 15),
    LStickLeft  = (1 << 16),
    LStickRight = (1 << 17),
    LStickClick = (1 << 18), // R3 equivalent on left stick (unused by default)
    RStickClick = (1 << 19), // R3 — hex/text view toggle in file viewer
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Initialize the input system. Opens joystick 0 (Joy-Con pair / Pro Controller).
/// Call after SDL_Init(SDL_INIT_JOYSTICK).
void init();

/// Release joystick handle.
void shutdown();

/// Process all pending SDL events. Call once at the start of each frame.
/// Returns false if the app should exit (SDL_QUIT received).
bool poll();

// ─── State queries ────────────────────────────────────────────────────────────

/// True on the frame the button was first pressed.
bool pressed(Button b);

/// True on the frame the button was released.
bool released(Button b);

/// True while the button is held (every frame, including the first).
bool held(Button b);

/// True on the first press AND again after repeat_delay_ms, then every
/// repeat_interval_ms while held. Respects the global button_repeat setting.
/// Use for D-pad navigation in lists.
bool repeat(Button b);

/// Raw bitmask of all currently held buttons.
uint32_t held_mask();

// ─── Settings ─────────────────────────────────────────────────────────────────

/// Enable or disable button repeat globally (from config).
void set_repeat_enabled(bool enabled);

/// Delay before repeat kicks in, in ms. Default: 400.
void set_repeat_delay(int ms);

/// Interval between repeat events, in ms. Default: 80.
void set_repeat_interval(int ms);

} // namespace Input
