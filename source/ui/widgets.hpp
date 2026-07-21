#pragma once
// source/ui/widgets.hpp
// Reusable UI widget primitives.
// All widgets draw into the current SDL renderer frame — call inside begin_frame/end_frame.

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include "ui/font.hpp"
#include "ui/theme.hpp"
#include "core/qr.hpp"

namespace Widgets {

// ─── Delayed round-robin navigation ───────────────────────────────────────────
// Shared cursor-stepping helper used by List and by screens that manage their
// own cursor (TitleList, TitleDetail action row). When the cursor is already at
// an edge and the user keeps navigating in that direction, the cursor wraps to
// the opposite end — but only after `delay_ms` of sustained boundary contact, so
// a long hold or an accidental extra press that reaches the edge doesn't wrap
// unintentionally. `step()` returns the new cursor index (0 .. count-1).
//
// delay_ms was 450 and is 300 as of rev 4 — a third off, on the judgement that
// the original was slower than it needed to be. All three call sites take the
// default; there is deliberately no per-site override, so this constant is the
// whole knob.
//
// Note the timer is wall-clock from first edge contact, and so is independent of
// the input layer's repeat state machine. But the wrap only fires on a call
// where `down`/`up` is true, so it does still need a press to LAND after the
// delay has elapsed. While the input layer coalesces sub-frame taps (see §16
// Open Items), wrapping will feel unreliable at the edge no matter what this
// value is — that is a separate defect and lowering this will not mask it.
struct WrapNav {
    int      edge_dir   = 0;   // +1 = armed at bottom, -1 = armed at top, 0 = none
    uint32_t edge_since = 0;   // SDL_GetTicks() when the edge was first reached

    void reset() { edge_dir = 0; }
    int  step(int cursor, int count, bool down, bool up, uint32_t delay_ms = 300);
};

// ─── List widget ──────────────────────────────────────────────────────────────
// A scrollable, selectable vertical list of text rows.
// Used by MainMenu, TitleList, FileBrowser listing column, etc.

struct ListItem {
    std::string label;          // primary text (left-aligned)
    std::string meta;           // secondary text (right-aligned, FgSecondary)
    bool        is_selected = false;   // multi-select checkbox state
    bool        is_focused  = false;   // cursor is on this row (set by List::draw)
    bool        is_disabled = false;
};

struct ListStyle {
    int row_height      = 40;
    int indent_x        = 16;
    bool show_checkbox  = false;   // when true, draw a checkbox for is_selected
    bool show_dividers  = true;
};

/// Stateful list widget.
class List {
public:
    List() = default;

    /// Replace all items. Resets scroll; preserves cursor if possible.
    void set_items(std::vector<ListItem> items);

    /// Update items without resetting scroll/cursor.
    void update_items(std::vector<ListItem> items);

    /// Process input for this list. Call once per frame before draw().
    /// Returns true if the user pressed A on an item.
    bool handle_input();

    /// Draw the list into the given region.
    void draw(int x, int y, int w, int h, const ListStyle& style = {});

    /// Current cursor index. -1 if the list is empty.
    int  cursor() const { return m_cursor; }

    /// Move cursor to a specific index. Adjusts scroll offset if needed.
    void set_cursor(int idx);

    /// Number of items.
    int  count() const { return static_cast<int>(m_items.size()); }

    /// Access item at index (const).
    const ListItem& item(int idx) const { return m_items[idx]; }

    /// Toggle selection on the item at the current cursor.
    void toggle_selection();

    /// Select all / deselect all.
    void select_all();
    void deselect_all();

    /// Indices of all currently selected items.
    std::vector<int> selected_indices() const;

private:
    std::vector<ListItem> m_items;
    int m_cursor       = 0;
    int m_scroll_offset = 0;   // index of first visible row
    WrapNav m_wrap;            // delayed round-robin navigation state

    void clamp_cursor();
    void ensure_visible(int idx, int visible_rows);
};

// ─── Toggle widget ────────────────────────────────────────────────────────────
// A boolean on/off pill — used in Settings.

/// Draw a toggle at (x, y). Returns the new value if the user pressed A while focused.
bool draw_toggle(int x, int y, bool value, bool focused);

// ─── Progress bar ─────────────────────────────────────────────────────────────

/// Draw a horizontal progress bar. fraction in [0.0, 1.0].
void draw_progress(int x, int y, int w, int h, float fraction);

// ─── Battery indicator ────────────────────────────────────────────────────────

enum class ChargeState { Discharging, Charging, Full };

/// Draw the battery icon + percentage used in the status bar.
void draw_battery(int x, int y, int w, int h, float fraction, ChargeState state);

// ─── Button legend ────────────────────────────────────────────────────────────
// Renders the hint row at the bottom of the content area.

struct ButtonHint {
    std::string button;   // e.g. "A", "B", "+"
    std::string label;    // e.g. "Confirm", "Back"
};

/// Draw a horizontal row of button hints, right-aligned within the given region.
void draw_button_legend(int x, int y, int w, const std::vector<ButtonHint>& hints);

// ─── Section header ───────────────────────────────────────────────────────────

/// Draw a full-width section label (bold, FgSecondary, with bottom divider).
void draw_section_header(int x, int y, int w, const std::string& text);

// ─── Text rendering helpers ───────────────────────────────────────────────────

/// Draw a single line of text. Returns the rendered width in pixels.
int draw_text(int x, int y,
              const std::string& text,
              Font::Size size   = Font::Size::Body,
              Font::Weight weight = Font::Weight::Regular,
              Theme::Token color  = Theme::Token::FgPrimary,
              int max_width     = 0);   // 0 = no clipping

// ─── QR code ──────────────────────────────────────────────────────────────────

/// Draw a QR code inside a box_px square at (x, y), including the mandatory
/// 4-module quiet zone. Always rendered dark-on-white regardless of theme —
/// scanners need the contrast and the light margin, so this deliberately does
/// not use Theme tokens. The module size is floored to a whole pixel and the
/// result is centred in the box, keeping every module crisp.
/// No-op if the code is invalid (see Core::Qr::encode).
void draw_qr(int x, int y, int box_px, const Core::Qr::Code& code);

} // namespace Widgets
