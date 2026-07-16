#pragma once
// source/ui/keyboard.hpp
// Text input via the Switch system software keyboard (swkbd).
// On PC, falls back to a simple stdin read for development.
//
// These calls are BLOCKING — they hand control to the OS keyboard overlay and
// return when the user confirms or cancels. Call them in response to a button
// press, not inside the render loop's draw phase.

#include <string>

namespace Keyboard {

struct Options {
    std::string header;          // prompt/title shown above the keyboard
    std::string initial_text;    // pre-filled text (e.g. current filename for rename)
    std::string ok_text = "OK";  // confirm button label
    int         max_length = 255;
    bool        allow_empty = false;
};

/// Show the keyboard for a line of text. Returns true if the user confirmed;
/// `out` receives the entered string. Returns false if cancelled.
bool get_text(const Options& opts, std::string& out);

/// Convenience for a numeric-only entry (port numbers, etc.).
bool get_number(const std::string& header, int initial, int& out);

} // namespace Keyboard
