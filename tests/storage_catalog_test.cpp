// tests/storage_catalog_test.cpp
//
// Tests the shared StorageCatalog (source/services/storage_catalog.{hpp,cpp}) —
// the single definition of storage surfaces every transport consumes. Pure logic,
// no libnx, so it runs on the host. Guards the anti-drift invariants: the full
// surface set exists, config gating is correct per-surface, and the NAND safety
// policy (read-only + on-device confirm) is exactly as decided.

#include "services/storage_catalog.hpp"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace Services;
using Id = StorageSurface::Id;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// All ten surfaces the user enumerated must be present.
static void test_all_surfaces_present() {
    const auto& all = StorageCatalog::all();
    CHECK(all.size() == 9, "catalog defines all 9 surfaces");
    std::set<int> ids;
    for (const auto& s : all) ids.insert((int)s.id);
    CHECK(ids.count((int)Id::SdCard),          "SD Card present");
    CHECK(ids.count((int)Id::SdInstall),       "SD Install present");
    CHECK(ids.count((int)Id::NandInstall),     "NAND Install present");
    CHECK(ids.count((int)Id::NandUser),        "NAND User present");
    CHECK(ids.count((int)Id::NandSystem),      "NAND System present");
    CHECK(ids.count((int)Id::Saves),           "Save Data present");
    CHECK(ids.count((int)Id::Album),           "Album present");
    CHECK(ids.count((int)Id::Gamecard),        "Game Card present");
    CHECK(ids.count((int)Id::InstalledTitles), "Installed Titles present");
    std::printf("  ok: all storage surfaces present (9)\n");
}

// find() resolves every id and returns null for none.
static void test_find() {
    for (const auto& s : StorageCatalog::all()) {
        const StorageSurface* f = StorageCatalog::find(s.id);
        CHECK(f != nullptr, "find resolves a known id");
        CHECK(f->id == s.id, "find returns the right surface");
        CHECK(std::string(f->display).size() > 0, "surface has a display name");
    }
    std::printf("  ok: find() resolves every surface\n");
}

// Config gating: each surface maps to the right flag, and defaults match config.
static void test_gating_defaults() {
    Config::MTP c;   // defaults: nand_system=false, nand_install=false, gamecard=false
    CHECK(StorageCatalog::enabled(Id::SdCard, c),          "SD on by default");
    CHECK(StorageCatalog::enabled(Id::SdInstall, c),       "SD Install on by default");
    CHECK(!StorageCatalog::enabled(Id::NandInstall, c),    "NAND Install OFF by default");
    CHECK(StorageCatalog::enabled(Id::NandUser, c),        "NAND user on by default");
    CHECK(!StorageCatalog::enabled(Id::NandSystem, c),     "NAND system OFF by default");
    CHECK(StorageCatalog::enabled(Id::Saves, c),           "Saves on by default");
    CHECK(StorageCatalog::enabled(Id::Album, c),           "Album on by default");
    CHECK(!StorageCatalog::enabled(Id::Gamecard, c),       "Gamecard OFF by default");
    CHECK(StorageCatalog::enabled(Id::InstalledTitles, c), "Installed Titles on by default");
    std::printf("  ok: config gating matches defaults\n");
}

// Flipping a config flag flips only that surface.
static void test_gating_responds_to_config() {
    Config::MTP c;
    c.sd_card = false;
    c.nand_system = true;
    c.gamecard = true;
    CHECK(!StorageCatalog::enabled(Id::SdCard, c),     "SD off when flag off");
    CHECK(StorageCatalog::enabled(Id::NandSystem, c),  "NAND system on when flag on");
    CHECK(StorageCatalog::enabled(Id::Gamecard, c),    "Gamecard on when flag on");
    // Unrelated surfaces unaffected.
    CHECK(StorageCatalog::enabled(Id::Album, c),       "Album unaffected");
    std::printf("  ok: gating responds to config changes per-surface\n");
}

// enabled_surfaces returns only enabled ones, preserving catalog order.
static void test_enabled_surfaces() {
    Config::MTP c;                 // NAND install, system, gamecard off by default
    auto en = StorageCatalog::enabled_surfaces(c);
    for (const auto& s : en)
        CHECK(StorageCatalog::enabled(s.id, c), "enabled_surfaces contains only enabled");
    // The three default-off surfaces must be absent.
    for (const auto& s : en) {
        CHECK(s.id != Id::NandInstall, "NAND Install excluded by default");
        CHECK(s.id != Id::NandSystem,  "NAND System excluded by default");
        CHECK(s.id != Id::Gamecard,    "Gamecard excluded by default");
    }
    // Order preserved: each entry's index in `all` is strictly increasing.
    const auto& all = StorageCatalog::all();
    size_t last = 0; bool first = true;
    for (const auto& s : en) {
        size_t idx = 0;
        for (; idx < all.size(); ++idx) if (all[idx].id == s.id) break;
        if (!first) CHECK(idx > last, "enabled_surfaces preserves catalog order");
        last = idx; first = false;
    }
    std::printf("  ok: enabled_surfaces filters and preserves order\n");
}

// The safety policy is EXACTLY as decided: NAND read-only + on-device confirm;
// gamecard + installed titles read-only; SD/saves/album read-write no confirm.
static void test_safety_policy() {
    auto* nu = StorageCatalog::find(Id::NandUser);
    auto* ns = StorageCatalog::find(Id::NandSystem);
    CHECK(!StorageCatalog::writable(*nu), "NAND user is read-only by default");
    CHECK(!StorageCatalog::writable(*ns), "NAND system is read-only by default");
    CHECK(StorageCatalog::needs_confirmation(*nu), "NAND user needs on-device confirm");
    CHECK(StorageCatalog::needs_confirmation(*ns), "NAND system needs on-device confirm");

    auto* gc = StorageCatalog::find(Id::Gamecard);
    auto* it = StorageCatalog::find(Id::InstalledTitles);
    CHECK(!StorageCatalog::writable(*gc), "gamecard is read-only");
    CHECK(!StorageCatalog::writable(*it), "installed titles is read-only");
    CHECK(!StorageCatalog::needs_confirmation(*gc), "gamecard has no confirm (RO anyway)");

    auto* sd = StorageCatalog::find(Id::SdCard);
    auto* sv = StorageCatalog::find(Id::Saves);
    auto* al = StorageCatalog::find(Id::Album);
    CHECK(StorageCatalog::writable(*sd), "SD is read-write");
    CHECK(StorageCatalog::writable(*al), "album is read-write");
    CHECK(!StorageCatalog::needs_confirmation(*sd), "SD needs no confirm");
    // Save data is deliberately NOT freely writable: restoring a save silently
    // overwrites game progress and is unrecoverable for the player, so writes are
    // gated behind the same on-device confirmation as NAND. Reads stay free —
    // browsing/backing up saves is not affected by the write policy.
    CHECK(!StorageCatalog::writable(*sv), "saves are not freely writable");
    CHECK(StorageCatalog::needs_confirmation(*sv), "save writes need on-device confirm");
    std::printf("  ok: safety policy matches the decided model\n");
}

// Kinds are correct: installs are Install, installed-titles is TitleQuery,
// the rest are Filesystem.
static void test_kinds() {
    CHECK(StorageCatalog::find(Id::SdInstall)->kind == StorageKind::Install, "SD Install kind");
    CHECK(StorageCatalog::find(Id::NandInstall)->kind == StorageKind::Install, "NAND Install kind");
    CHECK(StorageCatalog::find(Id::InstalledTitles)->kind == StorageKind::TitleQuery, "Installed Titles kind");
    CHECK(StorageCatalog::find(Id::SdCard)->kind == StorageKind::Filesystem, "SD kind");
    CHECK(StorageCatalog::find(Id::Album)->kind == StorageKind::Filesystem, "Album kind");
    // Filesystem surfaces have a vfs_root; Install/TitleQuery don't.
    CHECK(std::string(StorageCatalog::find(Id::SdCard)->vfs_root) == "sdmc:", "SD root is sdmc:");
    CHECK(std::string(StorageCatalog::find(Id::SdInstall)->vfs_root).empty(), "Install has no vfs_root");
    CHECK(std::string(StorageCatalog::find(Id::InstalledTitles)->vfs_root).empty(), "TitleQuery has no vfs_root");
    std::printf("  ok: kinds and vfs roots are correct\n");
}

int main() {
    std::printf("StorageCatalog (shared storage surface definition)\n");
    test_all_surfaces_present();
    test_find();
    test_gating_defaults();
    test_gating_responds_to_config();
    test_enabled_surfaces();
    test_safety_policy();
    test_kinds();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
