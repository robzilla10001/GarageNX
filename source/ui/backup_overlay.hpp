#pragma once
// source/ui/backup_overlay.hpp
//
// The "backing up saves" progress frame, shared by the AUTOMATIC sweep (run once
// at app open from main.cpp) and the MANUAL one (the main-menu action) - the
// same picture drawn at different times, so one implementation.
//
// WHY IT DRAWS ITS OWN FRAME: both sweeps are synchronous and block the main
// loop, so the sweep's progress callback painting and presenting a complete
// frame is the only way to show progress without a worker thread. The enumerate
// phase is called out explicitly - it is the slow part (priming the ncm name
// cache), and a bar that only appears once copying starts looks like a hang.

#include "core/save_backup.hpp"

namespace UI {

/// Paint and present one complete frame describing the sweep's current state.
/// Safe to call from a progress callback while the main loop is blocked.
void draw_backup_overlay(const Core::SaveBackup::AutoProgress& pr);

} // namespace UI
