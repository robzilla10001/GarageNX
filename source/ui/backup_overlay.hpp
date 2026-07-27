#pragma once
// source/ui/backup_overlay.hpp
//
// The "backing up saves" progress frame, shared by the AUTOMATIC sweep (run once
// at app open from main.cpp) and the MANUAL one (the main-menu action).
//
// Shared rather than copied because the two callers are the same picture drawn at
// different times. A second copy would be the one that keeps the old wording, or
// misses the enumerate phase, the first time this is touched — the pattern this
// codebase has already paid for with the save label, the mount probe, and
// format_eta().
//
// WHY IT DRAWS ITS OWN FRAME: both sweeps are synchronous and block the main
// loop, so nothing else will draw until they return. The sweep reports progress
// through a callback and the callback paints and presents a complete frame; that
// is the only way a blocking operation can show progress without a worker thread.
// It is also why the enumerate phase is called out explicitly — that is the slow
// part (priming the ncm name cache), and a bar that only appears once copying
// starts would leave the longest stretch looking like a hang.

#include "core/save_backup.hpp"

namespace UI {

/// Paint and present one complete frame describing the sweep's current state.
/// Safe to call from a progress callback while the main loop is blocked.
void draw_backup_overlay(const Core::SaveBackup::AutoProgress& pr);

} // namespace UI
