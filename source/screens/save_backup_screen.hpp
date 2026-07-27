#pragma once
// source/screens/save_backup_screen.hpp
//
// The main-menu "Back Up Saves" action: back up EVERY live save, now.
//
// A screen rather than an inline action because the sweep is synchronous and can
// take a while — the user needs something on screen before it starts, and a
// result afterwards. The screen paints a static page, then on its SECOND update
// runs the sweep, which blocks and draws its own progress frames through the
// shared overlay (see ui/backup_overlay.hpp). Waiting for the second update is
// the same trick main.cpp uses for the automatic sweep, and for the same reason:
// the first frame has to reach the display before anything blocks it.
//
// Distinct from the automatic sweep in ONE respect only — it backs up everything
// rather than only what is stale. Both share one implementation
// (Core::SaveBackup::sweep_impl) so they cannot drift apart.

#include "screens/screen.hpp"

#include <memory>
#include <string>

class SaveBackupScreen : public Screen {
public:
    SaveBackupScreen() = default;

    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;
    std::string hint_string() const override;

private:
    // 0 = just entered (let a frame reach the screen), 1 = run the sweep,
    // 2 = finished, waiting for the user to dismiss the result.
    int  m_phase = 0;
    bool m_finished = false;
    int  m_backed_up = 0;
};
