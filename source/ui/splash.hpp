#pragma once
// source/ui/splash.hpp
// One-shot startup splash, shown after init and before the first menu frame.
//
// Deliberately not a Screen: it has no state, takes no input beyond "skip", and
// never re-enters. Making it part of the screen stack would mean the whole
// stack had to reason about a screen that only ever exists once, at startup.

#include <string>

namespace Splash {

/// Draw the startup image full-screen for `hold_ms`, then cross-fade it out
/// over `fade_ms` to the menu's background colour, so the first menu frame
/// lands on a matching backdrop instead of cutting.
///
/// A button press during the hold skips ahead to the fade rather than cutting
/// abruptly. Silently does nothing if the image is missing — a decorative
/// asset must never block startup.
void show(const std::string& asset_root, int hold_ms, int fade_ms);

} // namespace Splash
