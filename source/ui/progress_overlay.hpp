#pragma once
// source/ui/progress_overlay.hpp
//
// A one-shot "working..." frame for blocking operations that report progress from
// a callback (same technique as backup_overlay: the main loop is blocked, so the
// callback paints and presents a whole frame itself).

#include <string>

namespace UI {

/// Paint and present one complete frame: heading, optional detail line, and a bar
/// when `fraction` >= 0 (pass a negative value for "indeterminate", no bar).
void draw_progress_overlay(const std::string& heading, const std::string& detail,
                           float fraction);

} // namespace UI
