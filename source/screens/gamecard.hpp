#pragma once
// source/screens/gamecard.hpp
//
// The inserted game card's titles, with Dump to NSP.
//
// Distinct from "Browse Game Card", which is a file browser over the mounted
// secure partition. This is the TITLE view: what is on the card as installable
// content, and what can be done with it.
//
// Dump reuses Core::Dump::dump_title_to_nsp() unchanged — the same builder that
// dumps an installed title. A cartridge title became an ordinary Title the moment
// Ncm::Storage gained a GameCard value and the storage-id mapping stopped being a
// two-way ternary; nothing about dumping is card-specific.
//
// INSTALL is not here yet. Copying cartridge content into SD/NAND is a separate
// operation from dumping and is scoped in the roadmap — deliberately not bolted
// on, because the destination/record story is exactly where an install goes wrong
// quietly.

#include "screens/screen.hpp"
#include "core/dump.hpp"
#include "install/stream_installer.hpp"
#include "core/ncm.hpp"
#include "ui/widgets.hpp"

#include <memory>
#include <string>
#include <vector>

class GamecardScreen : public Screen {
public:
    GamecardScreen() = default;

    /// CRITICAL: the dump runs on a worker that writes into m_progress and
    /// m_dump_out_path. If this screen is destroyed while that thread lives —
    /// which happens on app exit, where main.cpp clears the whole screen stack —
    /// the worker writes into freed memory. That is the identical cross-thread
    /// use-after-free that crashed MTP and FTP on transfer cancel until each got
    /// a one-line join in its destructor. Applied here BEFORE it can happen.
    ~GamecardScreen() override;

    std::unique_ptr<Screen> update(bool& pop) override;
    void draw() override;
    void on_modal_result(int result) override;

private:
    int  m_phase = 0;                       // 0 = let a frame land, 1 = gather, 2 = ready
    std::vector<Core::Ncm::TitleGroup> m_groups;
    Widgets::List m_list;

    int            m_pending_dump = -1;     // group awaiting its confirmation
    Core::Dump::Progress m_progress;         // worker updates, UI polls
    bool           m_dumping = false;
    std::string    m_status;
    std::string    m_dump_out_path;
    int            m_dump_idx = -1;          // group the worker is dumping
#ifdef PLATFORM_SWITCH
    Thread         m_dump_thread{};
    bool           m_dump_thread_active = false;
#endif

    // Install runs through the SAME machinery as every other transport: an
    // NspStream over the cartridge title feeds Install::drive(), which feeds
    // StreamInstaller. No cartridge-specific install path exists, and none should:
    // once NspStream could read gamecard content, a card became just another byte
    // source, which is what the driver was extracted for.
    Install::Progress    m_inst_progress;
    bool                 m_installing = false;
    Core::Ncm::Storage   m_inst_dest = Core::Ncm::Storage::SdCard;
    int                  m_inst_idx  = -1;
    int                  m_pending_install = -1;   // group awaiting confirmation
    Core::Ncm::Storage   m_pending_dest = Core::Ncm::Storage::SdCard;
#ifdef PLATFORM_SWITCH
    Thread               m_inst_thread{};
    bool                 m_inst_thread_active = false;
#endif

    void build_rows();
    void start_dump(int idx);
    void poll_dump();
    static void dump_thread_fn(void* arg);
    void start_install(int idx, Core::Ncm::Storage dest);
    void poll_install();
    static void install_thread_fn(void* arg);
};
