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
    Config::Surfaces c;
    auto r = sp_resolve("/", c);
    CHECK(r.kind == PathKind::Root, "/ is Root");
    CHECK(sp_resolve("", c).kind == PathKind::Root, "empty is Root");
    CHECK(sp_resolve("///", c).kind == PathKind::Root, "//// is Root");
    std::printf("  ok: root chooser\n");
}

static void test_sd_filesystem() {
    Config::Surfaces c;
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
    Config::Surfaces c;                 // album enabled by default
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
    Config::Surfaces c;
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
    Config::Surfaces c;                 // nand_system=false by default
    // A path into a disabled surface must be Invalid — a client cannot reach it.
    auto ns = sp_resolve("/NAND (System)/x", c);
    CHECK(ns.kind == PathKind::Invalid, "disabled NAND System is unreachable");

    // Gamecard now defaults ON (it is physically read-only and a mount exists), so
    // disable it EXPLICITLY here. That is the stronger test anyway: it exercises
    // the gating mechanism rather than the happenstance of a default.
    c.gamecard = false;
    auto gc = sp_resolve("/Game Card/y", c);
    CHECK(gc.kind == PathKind::Invalid, "disabled Game Card is unreachable");
    c.gamecard = true;
    auto gc_on = sp_resolve("/Game Card/y", c);
    CHECK(gc_on.kind != PathKind::Invalid, "enabled Game Card IS reachable");

    // Enable NAND system → now reachable (as a StorageRoot / Filesystem).
    c.nand_system = true;
    auto ns2 = sp_resolve("/NAND (System)", c);
    CHECK(ns2.kind == PathKind::StorageRoot, "enabled NAND System is reachable");
    CHECK(ns2.vfs == "bis_system:/", "NAND System maps to bis_system:/");
    std::printf("  ok: disabled surfaces are unreachable; enabling exposes them\n");
}

static void test_bare_and_unknown_invalid() {
    Config::Surfaces c;
    // A name that isn't any storage.
    CHECK(sp_resolve("/games/x", c).kind == PathKind::Invalid, "unknown root is Invalid");
    // Collision safety: "SD Cardigan" is not "SD Card".
    CHECK(sp_resolve("/SD Cardigan/x", c).kind == PathKind::Invalid,
          "similar name does not match SD Card");
    std::printf("  ok: unknown/typo roots are Invalid (no collision)\n");
}

static void test_save_split();

int main() {
    std::printf("StoragePaths (shared catalog-driven path resolver)\n");
    test_root();
    test_sd_filesystem();
    test_album_filesystem();
    test_install_and_titlequery_kinds();
    test_disabled_is_unreachable();
    test_bare_and_unknown_invalid();
    test_save_split();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}

// ─── Save Data three-level decomposition ─────────────────────────────────────
static void test_save_split() {
    using L = Services::SavePath::Level;
    struct { const char* rel; L lvl; const char* user; const char* title; const char* rest; } cases[] = {
        { "",                          L::Users,  "",      "",           "" },
        { "Alice",                     L::Titles, "Alice", "",           "" },
        { "Alice/",                    L::Titles, "Alice", "",           "" },
        { "Alice/Zelda [0100]",        L::Files,  "Alice", "Zelda [0100]", "" },
        { "Alice/Zelda [0100]/save.dat", L::Files, "Alice","Zelda [0100]", "save.dat" },
        { "Alice/Zelda [0100]/a/b.bin",  L::Files, "Alice","Zelda [0100]", "a/b.bin" },
        // A user name containing spaces must survive intact.
        { "Big Bird/Game [1]/x",       L::Files,  "Big Bird", "Game [1]", "x" },
    };
    for (auto& c : cases) {
        auto s = Services::sp_split_save(c.rel);
        CHECK(s.level == c.lvl,            (std::string("level for '") + c.rel + "'").c_str());
        CHECK(s.user  == c.user,           (std::string("user for '")  + c.rel + "'").c_str());
        CHECK(s.title == c.title,          (std::string("title for '") + c.rel + "'").c_str());
        CHECK(s.rest  == c.rest,           (std::string("rest for '")  + c.rel + "'").c_str());
    }
    std::printf("  ok: save-data path splits into user/title/rest\n");
}
