// tests/write_guard_test.cpp
//
// The write guard is what stands between a PC client and the console's NAND, so
// its decision table gets explicit coverage — including the negative cases, which
// are the ones that actually matter for safety.

#include "services/write_guard.hpp"

#include <cstdio>
#include <string>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failed; std::printf("  FAIL: %s\n", what.c_str()); }
}

using namespace Services;

static const char* name(WritePolicy p) {
    switch (p) {
        case WritePolicy::Allow:        return "Allow";
        case WritePolicy::NeedsConfirm: return "NeedsConfirm";
        case WritePolicy::Deny:         return "Deny";
    }
    return "?";
}

static void expect(const std::string& path, const Config::MTP& cfg,
                   WritePolicy want, const std::string& why) {
    const WritePolicy got = classify_write(path, cfg);
    check(got == want,
          why + " (path='" + path + "' want=" + name(want) + " got=" + name(got) + ")");
}

int main() {
    std::printf("write_guard_test\n");

    // Defaults: SD read-write, album on, NAND user on, NAND system OFF.
    Config::MTP def;

    // ── Freely writable surfaces: no prompt ───────────────────────────────────
    expect("sdmc:/game.nsp",           def, WritePolicy::Allow,
           "SD card is read-write");
    expect("sdmc:/switch/sub/dir",     def, WritePolicy::Allow,
           "nested SD path is read-write");
    expect("album:/2024/pic.jpg",      def, WritePolicy::Allow,
           "album is read-write");

    // ── Save data: readable freely, but WRITES need confirmation ──────────────
    // Restoring a save silently overwrites game progress and is unrecoverable for
    // the player, so it is gated exactly like NAND even though it lives on SD.
    expect("save:/0100000000010000/x.dat", def, WritePolicy::NeedsConfirm,
           "save writes require on-device confirmation");
    check(classify_write("save:/anything", def) != WritePolicy::Allow,
          "save data is NEVER freely writable");

    // ── NAND: protected, requires on-device confirmation ──────────────────────
    expect("bis_user:/save/x.bin",     def, WritePolicy::NeedsConfirm,
           "NAND user requires confirmation");
    expect("bis_user:/",               def, WritePolicy::NeedsConfirm,
           "NAND user root requires confirmation");

    // NAND system is DISABLED by default -> not merely protected, unreachable.
    expect("bis_system:/Contents/x",   def, WritePolicy::Deny,
           "NAND system disabled by default is denied outright");

    // ...and when explicitly enabled it becomes confirm-required, never free.
    Config::MTP sys_on = def;
    sys_on.nand_system = true;
    expect("bis_system:/Contents/x",   sys_on, WritePolicy::NeedsConfirm,
           "NAND system enabled still requires confirmation");
    check(classify_write("bis_system:/Contents/x", sys_on) != WritePolicy::Allow,
          "NAND system is NEVER freely writable");

    // ── Disabling a surface removes write access too ──────────────────────────
    Config::MTP user_off = def;
    user_off.nand_user = false;
    expect("bis_user:/save/x.bin",     user_off, WritePolicy::Deny,
           "disabled NAND user cannot be written even by guessing the path");

    Config::MTP album_off = def;
    album_off.album = false;
    expect("album:/2024/pic.jpg",      album_off, WritePolicy::Deny,
           "disabled album cannot be written");

    // ── Unknown / malformed paths: default-deny ───────────────────────────────
    expect("",                         def, WritePolicy::Deny, "empty path denied");
    expect("nonsense:/x",              def, WritePolicy::Deny, "unknown mount denied");
    expect("/SD Card/game.nsp",        def, WritePolicy::Deny,
           "a POSIX display path is not a VFS path — denied");
    expect("sdmc",                     def, WritePolicy::Deny,
           "prefix without colon does not match a surface");

    // ── The safety invariant, stated directly ─────────────────────────────────
    // No configuration makes a NAND surface freely writable without confirmation.
    {
        Config::MTP all_on;
        all_on.nand_user = true;
        all_on.nand_system = true;
        for (const char* p : {"bis_user:/a", "bis_system:/b"}) {
            check(classify_write(p, all_on) == WritePolicy::NeedsConfirm,
                  std::string("NAND always needs confirmation, never Allow: ") + p);
        }
    }

    std::printf("%s (%d checks", g_failed ? "FAILURES" : "ALL PASS", g_checks);
    if (g_failed) std::printf(", %d failed", g_failed);
    std::printf(")\n");
    return g_failed ? 1 : 0;
}
