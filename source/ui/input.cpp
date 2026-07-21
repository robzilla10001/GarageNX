// source/ui/input.cpp

#include "ui/input.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <map>

namespace Input {

// ─── State ────────────────────────────────────────────────────────────────────

static SDL_Joystick* s_joystick = nullptr;

static uint32_t s_held_prev  = 0;
static uint32_t s_held_curr  = 0;
static uint32_t s_pressed    = 0;
static uint32_t s_released   = 0;

// Distinct button-down events per button this frame. pressed()/s_pressed is a
// bitmask and therefore collapses multiple sub-frame taps of one button into a
// single bit; this array preserves the count so rapid navigation does not drop
// steps. Indexed by button bit position (0..31). Reset each poll().
static uint8_t s_press_count[32] = {0};

static inline int button_bit_index(uint32_t mask) {
    // Exactly one bit is set in a Button value; return its index (0..31).
    int i = 0;
    while (i < 32 && !((mask >> i) & 1u)) ++i;
    return i;
}

// Button repeat tracking
static bool s_repeat_enabled   = true;
static int  s_repeat_delay_ms  = 400;
static int  s_repeat_interval_ms = 80;

struct RepeatState {
    bool     active       = false;
    uint32_t first_press  = 0;   // SDL_GetTicks() when button was first held
    uint32_t last_repeat  = 0;   // SDL_GetTicks() of last repeat fire
    bool     fired_this_frame = false;
};
static std::map<uint32_t, RepeatState> s_repeat_map;

// Analog stick dead zone (SDL joystick axis range: -32768 to 32767)
static constexpr int AXIS_DEAD_ZONE = 10000;

// ─── Platform-specific button mapping ─────────────────────────────────────────
// SDL joystick button indices for the Switch Pro Controller via libnx SDL2.
// Joy-Con in handheld mode uses the same mapping.

#ifdef PLATFORM_SWITCH
// libnx SDL2 maps HID pad buttons to SDL joystick buttons 0-based.
// These indices match the devkitPro SDL2 port behavior.
static constexpr int JOY_A     = 0;
static constexpr int JOY_B     = 1;
static constexpr int JOY_X     = 2;
static constexpr int JOY_Y     = 3;
static constexpr int JOY_LSTK  = 4;
static constexpr int JOY_RSTK  = 5;
static constexpr int JOY_L     = 6;
static constexpr int JOY_R     = 7;
static constexpr int JOY_ZL    = 8;
static constexpr int JOY_ZR    = 9;
static constexpr int JOY_PLUS  = 10;
static constexpr int JOY_MINUS = 11;
static constexpr int JOY_DLEFT = 12;
static constexpr int JOY_DUP   = 13;
static constexpr int JOY_DRIGHT= 14;
static constexpr int JOY_DDOWN = 15;

// Axis indices
static constexpr int AXIS_LX   = 0;
static constexpr int AXIS_LY   = 1;

// Reverse of the poll mapping: SDL joystick button index -> logical Button mask.
// Used to turn SDL_JOYBUTTONDOWN/UP events into pressed/released edges.
static uint32_t joy_button_to_mask(int idx) {
    switch (idx) {
        case JOY_A:      return static_cast<uint32_t>(Button::A);
        case JOY_B:      return static_cast<uint32_t>(Button::B);
        case JOY_X:      return static_cast<uint32_t>(Button::X);
        case JOY_Y:      return static_cast<uint32_t>(Button::Y);
        case JOY_L:      return static_cast<uint32_t>(Button::L);
        case JOY_R:      return static_cast<uint32_t>(Button::R);
        case JOY_ZL:     return static_cast<uint32_t>(Button::ZL);
        case JOY_ZR:     return static_cast<uint32_t>(Button::ZR);
        case JOY_PLUS:   return static_cast<uint32_t>(Button::Plus);
        case JOY_MINUS:  return static_cast<uint32_t>(Button::Minus);
        case JOY_DUP:    return static_cast<uint32_t>(Button::DUp);
        case JOY_DDOWN:  return static_cast<uint32_t>(Button::DDown);
        case JOY_DLEFT:  return static_cast<uint32_t>(Button::DLeft);
        case JOY_DRIGHT: return static_cast<uint32_t>(Button::DRight);
        case JOY_LSTK:   return static_cast<uint32_t>(Button::LStickClick);
        case JOY_RSTK:   return static_cast<uint32_t>(Button::RStickClick);
        default:         return 0;
    }
}
#endif

// ─── PC keyboard mapping (development) ────────────────────────────────────────
// When running on PC without a joystick, arrow keys + Enter/Esc + letters.
// These are only checked when no joystick is present.

static const uint8_t* s_keyboard = nullptr;

static uint32_t keyboard_to_mask() {
    if (!s_keyboard) return 0;
    uint32_t mask = 0;
    if (s_keyboard[SDL_SCANCODE_RETURN] || s_keyboard[SDL_SCANCODE_Z])
        mask |= static_cast<uint32_t>(Button::A);
    if (s_keyboard[SDL_SCANCODE_ESCAPE] || s_keyboard[SDL_SCANCODE_X])
        mask |= static_cast<uint32_t>(Button::B);
    if (s_keyboard[SDL_SCANCODE_A])
        mask |= static_cast<uint32_t>(Button::X);
    if (s_keyboard[SDL_SCANCODE_S])
        mask |= static_cast<uint32_t>(Button::Y);
    if (s_keyboard[SDL_SCANCODE_Q])
        mask |= static_cast<uint32_t>(Button::L);
    if (s_keyboard[SDL_SCANCODE_E])
        mask |= static_cast<uint32_t>(Button::R);
    if (s_keyboard[SDL_SCANCODE_RETURN])
        mask |= static_cast<uint32_t>(Button::Plus);
    if (s_keyboard[SDL_SCANCODE_BACKSPACE])
        mask |= static_cast<uint32_t>(Button::Minus);
    if (s_keyboard[SDL_SCANCODE_UP])
        mask |= static_cast<uint32_t>(Button::DUp);
    if (s_keyboard[SDL_SCANCODE_DOWN])
        mask |= static_cast<uint32_t>(Button::DDown);
    if (s_keyboard[SDL_SCANCODE_LEFT])
        mask |= static_cast<uint32_t>(Button::DLeft);
    if (s_keyboard[SDL_SCANCODE_RIGHT])
        mask |= static_cast<uint32_t>(Button::DRight);
    return mask;
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void init() {
    if (SDL_NumJoysticks() > 0) {
        s_joystick = SDL_JoystickOpen(0);
        if (!s_joystick) {
            SDL_Log("Input::init — SDL_JoystickOpen failed: %s", SDL_GetError());
        } else {
            SDL_Log("Input::init — Joystick: %s", SDL_JoystickName(s_joystick));
        }
        // Ensure discrete button events are queued so no press is dropped when a
        // frame stalls (see poll()).
        SDL_JoystickEventState(SDL_ENABLE);
    } else {
        SDL_Log("Input::init — No joystick found, using keyboard fallback");
    }
}

void shutdown() {
    if (s_joystick) {
        SDL_JoystickClose(s_joystick);
        s_joystick = nullptr;
    }
}

bool poll() {
    SDL_Event event;
    bool keep_running = true;

    // Discrete button edges from the SDL event queue. Polling the joystick state
    // once per frame (below) drops any press that begins and ends between two
    // poll() calls — which happens whenever a frame stalls (e.g. reading a file
    // preview on cursor move). The event queue records every transition, so we
    // OR these in and each rapid tap registers as exactly one press.
    uint32_t event_pressed  = 0;
    uint32_t event_released = 0;

    // Reset per-frame press counts before draining the queue.
    std::memset(s_press_count, 0, sizeof(s_press_count));

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                keep_running = false;
                break;
#ifdef PLATFORM_SWITCH
            case SDL_JOYBUTTONDOWN: {
                const uint32_t m = joy_button_to_mask(event.jbutton.button);
                event_pressed |= m;
                // Count each down separately — this is the whole point: two taps
                // in one (stalled) frame are two presses, not one. Cap at 255.
                if (m) {
                    const int idx = button_bit_index(m);
                    if (s_press_count[idx] < 255) s_press_count[idx]++;
                }
                break;
            }
            case SDL_JOYBUTTONUP:
                event_released |= joy_button_to_mask(event.jbutton.button);
                break;
#endif
            default:
                break;
        }
    }

    s_held_prev = s_held_curr;
    s_held_curr = 0;

    if (s_joystick) {
        SDL_JoystickUpdate();

#ifdef PLATFORM_SWITCH
        auto jb = [](int idx) -> bool {
            return SDL_JoystickGetButton(s_joystick, idx) != 0;
        };
        auto mask = [](Button b) -> uint32_t {
            return static_cast<uint32_t>(b);
        };

        if (jb(JOY_A))      s_held_curr |= mask(Button::A);
        if (jb(JOY_B))      s_held_curr |= mask(Button::B);
        if (jb(JOY_X))      s_held_curr |= mask(Button::X);
        if (jb(JOY_Y))      s_held_curr |= mask(Button::Y);
        if (jb(JOY_L))      s_held_curr |= mask(Button::L);
        if (jb(JOY_R))      s_held_curr |= mask(Button::R);
        if (jb(JOY_ZL))     s_held_curr |= mask(Button::ZL);
        if (jb(JOY_ZR))     s_held_curr |= mask(Button::ZR);
        if (jb(JOY_PLUS))   s_held_curr |= mask(Button::Plus);
        if (jb(JOY_MINUS))  s_held_curr |= mask(Button::Minus);
        if (jb(JOY_DUP))    s_held_curr |= mask(Button::DUp);
        if (jb(JOY_DDOWN))  s_held_curr |= mask(Button::DDown);
        if (jb(JOY_DLEFT))  s_held_curr |= mask(Button::DLeft);
        if (jb(JOY_DRIGHT)) s_held_curr |= mask(Button::DRight);
        if (jb(JOY_LSTK))   s_held_curr |= mask(Button::LStickClick);
        if (jb(JOY_RSTK))   s_held_curr |= mask(Button::RStickClick);

        // Analog left stick
        Sint16 lx = SDL_JoystickGetAxis(s_joystick, AXIS_LX);
        Sint16 ly = SDL_JoystickGetAxis(s_joystick, AXIS_LY);
        if (lx < -AXIS_DEAD_ZONE) s_held_curr |= mask(Button::LStickLeft);
        if (lx >  AXIS_DEAD_ZONE) s_held_curr |= mask(Button::LStickRight);
        if (ly < -AXIS_DEAD_ZONE) s_held_curr |= mask(Button::LStickUp);
        if (ly >  AXIS_DEAD_ZONE) s_held_curr |= mask(Button::LStickDown);
#endif
    } else {
        // PC keyboard fallback
        s_keyboard  = SDL_GetKeyboardState(nullptr);
        s_held_curr = keyboard_to_mask();
    }

    // Merge analog stick directions into D-pad for navigation convenience.
    // Screens only need to check DUp/DDown/DLeft/DRight.
    if (s_held_curr & static_cast<uint32_t>(Button::LStickUp))
        s_held_curr |= static_cast<uint32_t>(Button::DUp);
    if (s_held_curr & static_cast<uint32_t>(Button::LStickDown))
        s_held_curr |= static_cast<uint32_t>(Button::DDown);
    if (s_held_curr & static_cast<uint32_t>(Button::LStickLeft))
        s_held_curr |= static_cast<uint32_t>(Button::DLeft);
    if (s_held_curr & static_cast<uint32_t>(Button::LStickRight))
        s_held_curr |= static_cast<uint32_t>(Button::DRight);

    // Combine the polled edge diff (covers held-across-frames and the analog
    // stick, which emits no button events) with the event-derived edges (cover
    // sub-frame taps the poll would miss). OR-ing is idempotent for a normal
    // held press, and recovers a press+release that fit inside one frame.
    s_pressed  = (s_held_curr & ~s_held_prev) | event_pressed;
    s_released = (s_held_prev & ~s_held_curr) | event_released;

    // A poll-derived edge (button newly held this frame, or an analog-stick
    // direction — the stick emits no button events) must count as at least one
    // press even when the event queue saw nothing. Only bump when the event
    // queue didn't already count this button, so a normal single tap that both
    // the queue and the poll observe stays a count of 1, not 2.
    const uint32_t poll_edge = s_held_curr & ~s_held_prev;
    for (int i = 0; i < 32; ++i) {
        if ((poll_edge >> i) & 1u) {
            if (s_press_count[i] == 0) s_press_count[i] = 1;
        }
    }

    // Update repeat state
    uint32_t now = SDL_GetTicks();
    for (auto& [bit, rs] : s_repeat_map) {
        rs.fired_this_frame = false;
        if (s_held_curr & bit) {
            if (!rs.active) {
                rs.active      = true;
                rs.first_press = now;
                rs.last_repeat = now;
            }
        } else {
            rs.active = false;
        }
    }

    return keep_running;
}

// ─── State queries ────────────────────────────────────────────────────────────

bool pressed(Button b)  { return (s_pressed  & static_cast<uint32_t>(b)) != 0; }

int press_count(Button b) {
    return (int)s_press_count[button_bit_index(static_cast<uint32_t>(b))];
}
bool released(Button b) { return (s_released & static_cast<uint32_t>(b)) != 0; }
bool held(Button b)     { return (s_held_curr & static_cast<uint32_t>(b)) != 0; }

bool repeat(Button b) {
    uint32_t bit = static_cast<uint32_t>(b);

    // Always fire on the initial press
    if (s_pressed & bit) return true;
    if (!(s_held_curr & bit)) return false;
    if (!s_repeat_enabled) return false;

    auto& rs = s_repeat_map[bit];
    if (!rs.active) return false;

    uint32_t now = SDL_GetTicks();
    uint32_t held_duration = now - rs.first_press;

    if (held_duration >= static_cast<uint32_t>(s_repeat_delay_ms)) {
        uint32_t since_last = now - rs.last_repeat;
        if (since_last >= static_cast<uint32_t>(s_repeat_interval_ms)) {
            rs.last_repeat = now;
            rs.fired_this_frame = true;
            return true;
        }
    }

    return false;
}

uint32_t held_mask() { return s_held_curr; }

// ─── Settings ─────────────────────────────────────────────────────────────────

void set_repeat_enabled(bool enabled)  { s_repeat_enabled    = enabled; }
void set_repeat_delay(int ms)          { s_repeat_delay_ms   = ms;      }
void set_repeat_interval(int ms)       { s_repeat_interval_ms = ms;     }

} // namespace Input
