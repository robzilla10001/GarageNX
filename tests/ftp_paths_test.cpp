// tests/ftp_paths_test.cpp
//
// Tests the pure FTP install-path classifier (source/services/ftp_paths.hpp).
// This decides whether a STOR writes a file or drives an install, and where the
// virtual install folders appear in a listing — logic that must be exactly right
// for parity, and that is fully host-testable because it touches no sockets.

#include "services/ftp_paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Services;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static void test_root_detection() {
    CHECK(ftp_is_root("/"), "single slash is root");
    CHECK(ftp_is_root(""), "empty is root");
    CHECK(ftp_is_root("///"), "multiple slashes is root");
    CHECK(!ftp_is_root("/SD Card"), "a storage folder is not root");
    CHECK(!ftp_is_root("/SD Install"), "an install folder is not root");
    std::printf("  ok: root detection\n");
}

static void test_root_classifies_as_root() {
    std::string rel;
    CHECK(ftp_classify("/", rel) == FtpTarget::Root, "root path classifies as Root");
    CHECK(rel.empty(), "root has no relative path");
    std::printf("  ok: the root is a chooser (Root), holding no files\n");
}

static void test_sd_card_maps_to_filesystem() {
    std::string rel;
    CHECK(ftp_classify("/SD Card", rel) == FtpTarget::Filesystem, "SD Card is Filesystem");
    CHECK(rel.empty(), "SD Card root maps to the sd root");
    CHECK(ftp_classify("/SD Card/games/x.nsp", rel) == FtpTarget::Filesystem,
          "path under SD Card is Filesystem");
    CHECK(rel == "games/x.nsp", "relative path is stripped of the SD Card prefix");
    std::printf("  ok: /SD Card/... maps to the real filesystem (relative path)\n");
}

static void test_bare_path_is_invalid() {
    std::string rel;
    // A path NOT under any storage root must be Invalid — this is what stops the
    // SD contents from leaking into the root listing.
    CHECK(ftp_classify("/games/x.nsp", rel) == FtpTarget::Invalid,
          "bare path (no storage root) is Invalid");
    CHECK(ftp_classify("/switch", rel) == FtpTarget::Invalid, "bare folder is Invalid");
    std::printf("  ok: bare paths outside a storage root are Invalid\n");
}

static void test_classify_install_targets() {
    std::string leaf;

    CHECK(ftp_classify("/SD Install/Game.nsz", leaf) == FtpTarget::SdInstall,
          "SD Install routes to SdInstall");
    CHECK(leaf == "Game.nsz", "SD Install leaf is the filename");

    CHECK(ftp_classify("/NAND Install/Title.nsp", leaf) == FtpTarget::NandInstall,
          "NAND Install routes to NandInstall");
    CHECK(leaf == "Title.nsp", "NAND Install leaf is the filename");
    std::printf("  ok: install folders route to the right target with the leaf name\n");
}

static void test_storage_dir_detection() {
    CHECK(ftp_is_storage_dir("/SD Card"), "SD Card is a storage folder");
    CHECK(ftp_is_storage_dir("/SD Install"), "SD Install is a storage folder");
    CHECK(ftp_is_storage_dir("/NAND Install"), "NAND Install is a storage folder");
    CHECK(ftp_is_storage_dir("/SD Card/"), "trailing slash still the folder");
    CHECK(!ftp_is_storage_dir("/SD Card/games"), "a path inside is not the folder");
    CHECK(!ftp_is_storage_dir("/"), "root is not a storage folder");

    CHECK(ftp_is_install_dir("/SD Install"), "SD Install is an install folder");
    CHECK(!ftp_is_install_dir("/SD Card"), "SD Card is NOT an install folder");
    CHECK(!ftp_is_install_dir("/SD Install/x.nsp"), "a file inside is not the folder");
    std::printf("  ok: storage- and install-dir detection\n");
}

static void test_name_collision_safety() {
    std::string rel;
    CHECK(ftp_classify("/SD Installer/x.nsp", rel) == FtpTarget::Invalid,
          "similar-but-different name is not an install root");
    CHECK(ftp_classify("/SD Cardigan/x", rel) == FtpTarget::Invalid,
          "SD Cardigan is not SD Card");
    std::printf("  ok: similar folder names do not collide with storage roots\n");
}

int main() {
    std::printf("FTP storage-root path model\n");
    test_root_detection();
    test_root_classifies_as_root();
    test_sd_card_maps_to_filesystem();
    test_bare_path_is_invalid();
    test_classify_install_targets();
    test_storage_dir_detection();
    test_name_collision_safety();
    std::printf("ALL PASS (%d checks)\n", g_checks);
    return 0;
}
