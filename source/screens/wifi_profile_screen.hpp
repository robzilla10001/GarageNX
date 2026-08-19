#pragma once
// source/screens/wifi_profile_screen.hpp
//
// Picker for deleting ONE saved Wi-Fi network profile: enumerate -> select ->
// confirm (names the specific network) -> delete -> refresh.
//
// Pushed from the Tools screen's "Delete Wi-Fi Profiles" row rather than using
// the Tools batch scan/dry-run/hold-confirm pattern (see ToolsScreen::Op::push
// for why) — there's no way to say which SPECIFIC network "should" be deleted;
// that's an inherently per-item choice, not a bulk cleanup category. The
// original implementation here WAS a "delete everything" batch op; it was
// corrected to this shape after real-world use showed that's not what's
// wanted, matching how "delete users" was scoped from the start for the same
// reason.
//
// Scoped to NetworkProfileType::User only (not System/SsidList, not
// Temporary) — matches what official Nintendo software itself does per
// Switchbrew's own note on the underlying enumerate command.

#include "screens/screen.hpp"
#include "ui/widgets.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WifiProfileScreen : public Screen {
public:
    WifiProfileScreen();

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

private:
    struct Candidate {
        std::array<uint8_t, 0x10> uuid{};
        std::string               name;   // network name, shown in the confirm text
    };

    std::vector<Candidate> m_profiles;
    Widgets::List           m_list;
    int                      m_pending = -1;   // index awaiting held confirmation

    void reload();
    void select(int idx);
};
