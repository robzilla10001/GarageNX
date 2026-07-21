// tests/storage_paths_test.cpp
//
// Tests the shared catalog-driven path resolver (storage_paths.hpp). This is the
// choke point every transport uses to turn "/Album/2024/x.jpg" into
// "album:/2024/x.jpg", gated by the catalog. Pure logic → host-tested. Guards the
// invariants that keep disabled storages unreachable and each surface kind routed
// correctly.

#include "services/storage_paths.hpp"

#include <cstdio>
#include <cstdlib>
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

static void test_root() {
    Config::MTP c;
    auto r = sp_resolve("/", c);
    CHECK(r.kind == PathKind::Root, "/ is Root");
    CHECK(sp_resolve("", c).kind == PathKind::Root, "empty is Root");
    CHECK(sp_resolve("///", c).kind == PathKind::Root, "//// is Root");
    std::printf("  ok: root chooser\n");
}

static void test_sd_filesystem() {
    Config::MTP c;
    auto root = sp_resolve("/SD Card", c);
    CHECK(root.kind == PathKind::StorageRoot, "/SD Card is a StorageRoot");
    CHECK(root.vfs == "sdmc:/", "SD Card root maps to sdmc:/");
    CHECK(root.id == Id::SdCard, "id is SdCard");

    auto f = sp_resolve("/SD Card/switch/app.nro", c);
    CHECK(f.kind == PathKind::Filesystem, "path under SD Card is Filesystem");
    CHECK(f.vfs == "sdmc:/switch/app.nro", "maps to sdmc:/switch/app.nro");
    CHECK(f.rel == "switch/app.nro", "rel stripped of storage name");
    std::printf("  ok: SD Card maps to sdmc:\n");
}

static void test_album_filesystem() {
    Config::MTP c;                 // album enabled by default
    auto root = sp_resolve("/Album", c);
    CHECK(root.kind == PathKind::StorageRoot, "/Album is a StorageRoot");
    CHECK(root.vfs == "album:/", "Album root maps to album:/");

    auto f = sp_resolve("/Album/2024/01/pic.jpg", c);
    CHECK(f.kind == PathKind::Filesystem, "path under Album is Filesystem");
    CHECK(f.vfs == "album:/2024/01/pic.jpg", "maps to album:/2024/01/pic.jpg");
    CHECK(f.id == Id::Album, "id is Album");
    std::printf("  ok: Album maps to album:\n");
}

static void test_install_and_titlequery_kinds() {
    Config::MTP c;
    c.sd_install = true;
    auto ins = sp_resolve("/SD Install/Game.nsz", c);
    CHECK(ins.kind == PathKind::Install, "SD Install path is Install kind");
    CHECK(ins.id == Id::SdInstall, "id is SdInstall");
    CHECK(ins.rel == "Game.nsz", "rel is the leaf");

    // Installed Titles enabled by default.
    auto tq = sp_resolve("/Installed Titles/somegame", c);
    CHECK(tq.kind == PathKind::TitleQuery, "Installed Titles path is TitleQuery kind");
    CHECK(tq.id == Id::InstalledTitles, "id is InstalledTitles");
    std::printf("  ok: install and title-query kinds routed correctly\n");
}

static void test_disabled_is_unreachable() {
    Config::MTP c;                 // nand_system=false, gamecard=false by default
    // A path into a disabled surface must be Invalid — a client cannot reach it.
    auto ns = sp_resolve("/NAND (System)/x", c);
    CHECK(ns.kind == PathKind::Invalid, "disabled NAND System is unreachable");
    auto gc = sp_resolve("/Game Card/y", c);
    CHECK(gc.kind == PathKind::Invalid, "disabled Game Card is unreachable");

    // Enable NAND system → now reachable (as a StorageRoot / Filesystem).
    c.nand_system = true;
    auto ns2 = sp_resolve("/NAND (System)", c);
    CHECK(ns2.kind == PathKind::StorageRoot, "enabled NAND System is reachable");
    CHECK(ns2.vfs == "bis_system:/", "NAND System maps to bis_system:/");
    std::printf("  ok: disabled surfaces are unreachable; enabling exposes them\n");
}

static void test_bare_and_unknown_invalid() {
    Config::MTP c;
    // A name that isn't any storage.
    CHECK(sp_resolve("/games/x", c).kind == PathKind::Invalid, "unknown root is Invalid");
    // Collision safety: "SD Cardigan" is not "SD Card".
    CHECK(sp_resolve("/SD Cardigan/x", c).kind == PathKind::Invalid,
          "similar name does not match SD Card");
    std::printf("  ok: unknown/typo roots are Invalid (no collision)\n");
}

int main() {
    std::printf("StoragePaths (shared catalog-driven path resolver)\n");
    test_root();
    test_sd_filesystem();
    test_album_filesystem();
    test_install_and_titlequery_kinds();
    test_disabled_is_unreachable();
    test_bare_and_unknown_invalid();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
