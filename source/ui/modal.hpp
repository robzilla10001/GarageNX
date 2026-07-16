#pragma once
// source/ui/modal.hpp
// Blocking confirmation dialogs. Rendered on top of whatever screen is active.
// Usage: push a modal, then check result() each frame until it's not Pending.

#include <string>
#include <vector>
#include <functional>

namespace Modal {

enum class Result { Pending, Confirmed, Cancelled };

enum class Kind {
    Info,       // one button: OK
    Confirm,    // two buttons: Confirm / Cancel
    Warning,    // two buttons, accent_warn styling: Proceed / Cancel
    Danger,     // two buttons, accent_danger styling, requires extra confirmation
};

struct Options {
    std::string title;
    std::string body;
    Kind        kind          = Kind::Confirm;
    std::string confirm_label = "Confirm";
    std::string cancel_label  = "Cancel";
};

// ─── Lifecycle ────────────────────────────────────────────────────────────────

/// Show a modal. Only one may be active at a time.
/// Replaces any existing pending modal.
void show(const Options& opts);

/// Process input and draw the modal (if active).
/// Call once per frame, after the background screen has been drawn.
/// Returns Pending until the user makes a choice.
Result update_and_draw();

/// True if a modal is currently displayed.
bool is_active();

/// Dismiss the modal without a result (e.g. B button = cancel).
void dismiss();

} // namespace Modal
