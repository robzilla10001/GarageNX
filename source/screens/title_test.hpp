#pragma once
// source/screens/title_test.hpp
// TEMPORARY validation screen for Milestone 4 Phase A. Enumerates installed
// titles via NCM and, for the first Application, attempts to decrypt its Control
// NCA to extract the display name — proving the keys → NCM → NCA pipeline works
// on real hardware before we build the full TitleList/TitleDetail UI.
//
// This screen is disposable; it will be replaced by TitleList in Phase B.

#include "screens/screen.hpp"
#include "core/ncm.hpp"
#include <string>
#include <vector>

class TitleTestScreen : public Screen {
public:
    TitleTestScreen();

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    bool                     m_keys_ok = false;
    std::string              m_keys_msg;
    std::vector<Core::Ncm::Title> m_titles;
    int                      m_app_count = 0;
    int                      m_patch_count = 0;
    int                      m_dlc_count = 0;

    // Result of the decrypt test on the first application.
    std::string              m_test_status;
    std::string              m_test_name;
    std::string              m_test_version;
    bool                     m_test_ran = false;

    int m_scroll = 0;

    void run();
};
