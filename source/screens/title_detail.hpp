#pragma once
// source/screens/title_detail.hpp
// Milestone 4 Phase B — detail view for a single application.
//
// Shows the base application (icon, name, version, program id, size, storage)
// followed by its installed updates and DLC. In Phase C this screen gains the
// delete / dump-to-SD / move actions via the + context menu.

#include "screens/screen.hpp"
#include "core/ncm.hpp"
#include "core/dump.hpp"
#include "core/title_ops.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <cstdint>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

class TitleDetailScreen : public Screen {
public:
    TitleDetailScreen(Core::Ncm::TitleGroup group, std::string name);
    ~TitleDetailScreen() override;

    void on_enter() override;
    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;

private:
    Core::Ncm::TitleGroup m_group;
    std::string           m_name;
    std::string           m_version;
    SDL_Texture*          m_icon = nullptr;
    int                   m_scroll = 0;

    // ── Delete flow ─────────────────────────────────────────────────────────
    // + opens a small action menu; choosing Delete arms the hold-to-confirm
    // overlay. The user must hold A for HOLD_SECONDS to actually delete — real
    // friction for an irreversible action.
    enum class Mode { Browsing, ActionMenu, ConfirmDelete, Deleting, Result,
                      Dumping, DumpResult, Moving, MoveResult };
    Mode   m_mode = Mode::Browsing;
    int    m_action_cursor = 0;      // action menu selection
    float  m_hold_progress = 0.f;   // 0..1 while holding A on the confirm screen
    uint32_t m_hold_start = 0;      // SDL_GetTicks() when the A-hold began (0 = not holding)
    bool   m_result_ok = false;
    std::string m_result_msg;
    bool   m_deleted = false;       // set when the title was removed (tells list to refresh)

    // ── Dump flow ───────────────────────────────────────────────────────────
    Core::Dump::Progress m_dump;         // live progress (worker updates, UI polls)
#ifdef PLATFORM_SWITCH
    Thread               m_dump_thread{};
    Thread               m_move_thread{};
#endif
    bool                 m_dump_thread_active = false;
    std::string          m_dump_out_path;

    // ── Move flow ───────────────────────────────────────────────────────────
    Core::TitleOps::MoveProgress m_move;
    bool                 m_move_thread_active = false;

    static constexpr float HOLD_SECONDS = 1.5f;

    void load_icon();
    void do_delete();
    void start_dump();
    void poll_dump();
    static void dump_thread_fn(void* arg);
    void start_move();
    void poll_move();
    static void move_thread_fn(void* arg);

public:
    // True if this title was deleted, so the parent list knows to rebuild.
    bool was_deleted() const { return m_deleted; }
};
