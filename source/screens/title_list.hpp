#pragma once
// source/screens/title_list.hpp
// Milestone 4 Phase B — the installed-titles browser.
//
// Shows one row per user application (icon + name + version + size + storage).
// Names and icons are resolved up front by decrypting each app's Control NCA,
// behind a progress bar. Selecting an app opens TitleDetail, which shows that
// app plus its updates and DLC.
//
// Requires keys (sdmc:/switch/prod.keys). If keys are missing, this screen shows
// the blocking "keys required" message instead of a list.

#include "screens/screen.hpp"
#include "core/ncm.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <memory>

class TitleListScreen : public Screen {
public:
    TitleListScreen();
    ~TitleListScreen() override;

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    // One resolved application row.
    struct Row {
        Core::Ncm::TitleGroup group;      // app + its updates/DLC
        std::string           name;       // resolved display name (or the ID)
        std::string           version;    // display version from NACP
        SDL_Texture*          icon = nullptr;  // decoded icon (may be null)
    };

    enum class Phase { CheckKeys, Loading, Ready, NoKeys };
    Phase m_phase = Phase::CheckKeys;

    std::string m_keys_msg;

    std::vector<Row> m_rows;
    int  m_cursor = 0;
    int  m_scroll = 0;

    // Loading progress
    int  m_load_total = 0;
    int  m_load_done  = 0;
    std::vector<Core::Ncm::TitleGroup> m_pending;  // groups awaiting resolution

    void begin_load();
    void resolve_next();       // resolve one group per call (keeps UI responsive)
    SDL_Texture* decode_icon(const std::vector<uint8_t>& jpeg);
    void free_icons();

    int visible_rows() const;
};
