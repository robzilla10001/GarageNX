#pragma once
// source/core/ncm.hpp
// Enumerate installed titles via the Content Manager (ncm) service, and provide
// a ReadFn over a title's Control NCA so core/nca can decrypt name + icon.
//
// This is the read-only half of Milestone 4 (Phase A). Destructive operations
// (delete/dump/move) build on the same handles in Phase C.

#include "core/nca.hpp"
#include <string>
#include <vector>
#include <cstdint>

#ifdef PLATFORM_SWITCH
#include <switch.h>          // NcmStorageId, for to_ncm_storage_id() below
#endif

namespace Core::Ncm {

enum class Storage { BuiltIn, SdCard, GameCard };   // NAND / SD / inserted cartridge

#ifdef PLATFORM_SWITCH
/// The ONE mapping from our Storage to NCM's storage id.
///
/// This was a two-way ternary repeated in six files
/// (`storage == SdCard ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser`). Adding
/// a third value to the enum would have left all six silently mapping GameCard to
/// BuiltInUser — a wrong answer with no error, in the code that decides which
/// content database to open. Derived once instead, so a new storage is one edit
/// and every caller follows.
inline NcmStorageId to_ncm_storage_id(Storage s) {
    switch (s) {
        case Storage::SdCard:   return NcmStorageId_SdCard;
        case Storage::GameCard: return NcmStorageId_GameCard;
        case Storage::BuiltIn:
        default:                return NcmStorageId_BuiltInUser;
    }
}
#endif

enum class TitleType { Application, Patch, AddOnContent, Other };

// One enumerated title (base app, update, or DLC).
struct Title {
    uint64_t  program_id = 0;      // application/program id
    uint64_t  meta_id = 0;         // content-meta id (may differ for patches/DLC)
    uint32_t  version = 0;         // title version
    TitleType type = TitleType::Other;
    Storage   storage = Storage::BuiltIn;
    uint64_t  size_bytes = 0;      // total content size

    // Resolved lazily from the Control NCA (Phase B fills these in the UI):
    std::string name;              // display name, empty until resolved
    bool        name_resolved = false;
};

// The base application ID that an update or DLC belongs to. Updates share the
// app's program id with bit 0x800 set; DLC live just above the app id (low bits
// 0x0001..0x0FFF above the app's 0x…000 base). This maps any title back to the
// application it extends.
// Pure: the base application id for a patch/DLC. Inline in the header so host
// tests (and anything else) can use it without linking the libnx-only ncm.cpp.
inline uint64_t base_application_id(uint64_t program_id, TitleType type) {
    switch (type) {
        case TitleType::Patch:        return program_id & ~0x800ULL;
        case TitleType::AddOnContent: return (program_id & ~0xFFFULL) - 0x1000ULL;
        case TitleType::Application:
        default:                      return program_id;
    }
}

// An application together with its updates and DLC, for the TitleDetail screen.
struct TitleGroup {
    Title              app;        // the base application
    std::vector<Title> updates;    // patches for this app
    std::vector<Title> dlc;        // add-on content for this app
};

// List all installed titles across both storages. Applications, patches, and
// add-on content are all included, tagged by `type`.
std::vector<Title> list_all(bool* ok = nullptr);

/// Titles on the INSERTED GAME CARD, if any. Empty when no card is present.
///
/// Deliberately SEPARATE from list_all() rather than a third storage folded into
/// it. list_all() feeds the "Installed Titles" surface on every transport, and a
/// cartridge title is not installed — folding it in would put a game you can only
/// play with the card in the slot into a list whose whole meaning is "this console
/// holds these". Dump and install want cartridge titles; the browse surfaces do
/// not, and conflating them would be a lie told in four places at once.
std::vector<Title> list_gamecard(bool* ok = nullptr);

// Group enumerated titles: returns one TitleGroup per user application, with its
// updates and DLC attached. System titles (below the user-game id range) are
// excluded. `all` is typically the result of list_all().
std::vector<TitleGroup> group_by_application(const std::vector<Title>& all);

// Set/cleared to signal that the installed-title set changed (e.g. after a
// delete), so a cached TitleList knows to rebuild on next entry.
// The title database changed (install/uninstall). Bumps a GENERATION counter.
//
// This used to be a single boolean that the first observer cleared — which broke
// as soon as there were two observers: the on-device title list and the transports'
// Installed Titles cache. Whichever ran first consumed the flag and the other never
// saw the change (symptom: uninstall a title and the on-device list still shows it
// until you back out and re-enter). A counter lets every observer notice
// independently by remembering the last generation it acted on.
void mark_titles_dirty();

/// Current generation. An observer stores this when it rebuilds and rebuilds again
/// whenever the value differs.
uint64_t titles_generation();
bool titles_dirty();
void clear_titles_dirty();

// Resolve a title's display name + author + version by locating its Control
// content, reading it through ncm, and decrypting via core/nca. Returns an
// invalid ControlData (ok=false) on any failure (missing keys, no control, etc).
Core::Nca::ControlData resolve_control(const Title& title,
                                       const Core::Keys::Keyset& keys,
                                       bool want_icon);

} // namespace Core::Ncm
