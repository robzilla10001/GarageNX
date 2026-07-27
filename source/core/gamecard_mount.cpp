// source/core/gamecard_mount.cpp

#include "core/gamecard_mount.hpp"
#include "config/config.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <SDL2/SDL.h>

namespace {

bool g_mounted = false;

// Log once per distinct failure so a card that cannot be read does not spam the
// log at the refresh rate.
uint32_t g_last_fail_rc = 0;

// ── THE UNVERIFIED CALLS (5.4) ───────────────────────────────────────────────
// There is no libnx header in the build sandbox, so the exact names/signatures
// below are taken from switchbrew rather than checked. They are all confined to
// this one function, each logs its Result, and the fallbacks to try are recorded
// here so a build failure is a two-minute fix rather than an investigation:
//
//   fsOpenDeviceOperator(&devop)
//       — older libnx: takes no argument and returns a global; newer takes FsDeviceOperator*.
//   fsDeviceOperatorIsGameCardInserted(&devop, &out)
//       — `out` may be `bool*` or `u8*`/`u32*` depending on version.
//   fsDeviceOperatorGetGameCardHandle(&devop, &handle)
//       — may be named ...GetGameCardHandle or ...GetGameCardHandleForDebug.
//   fsOpenGameCardFileSystem(&fs, &handle, FsGameCardPartition_Secure)
//       — partition enum may be FsGameCardPartition_Secure or FsGameCardPartiton_Secure
//         (libnx carried that typo for a long time). Try both if it will not compile.
//
// Nothing outside this file depends on which spelling is correct.
bool try_mount() {
    FsDeviceOperator devop;
    Result rc = fsOpenDeviceOperator(&devop);
    if (R_FAILED(rc)) {
        if (rc != g_last_fail_rc) {
            g_last_fail_rc = rc;
            SDL_Log("gamecard: fsOpenDeviceOperator rc=0x%08X", rc);
        }
        return false;
    }

    bool inserted = false;
    rc = fsDeviceOperatorIsGameCardInserted(&devop, &inserted);
    if (R_FAILED(rc) || !inserted) {
        fsDeviceOperatorClose(&devop);
        return false;
    }

    FsGameCardHandle handle{};
    rc = fsDeviceOperatorGetGameCardHandle(&devop, &handle);
    fsDeviceOperatorClose(&devop);
    if (R_FAILED(rc)) {
        if (rc != g_last_fail_rc) {
            g_last_fail_rc = rc;
            SDL_Log("gamecard: GetGameCardHandle rc=0x%08X", rc);
        }
        return false;
    }

    // SECURE partition: the installable content. The other partitions (update,
    // normal, logo) are not what a user means by "browse the game card", and the
    // secure partition is the one the installer will need.
    FsFileSystem fs;
    rc = fsOpenGameCardFileSystem(&fs, &handle, FsGameCardPartition_Secure);
    if (R_FAILED(rc)) {
        if (rc != g_last_fail_rc) {
            g_last_fail_rc = rc;
            SDL_Log("gamecard: fsOpenGameCardFileSystem rc=0x%08X", rc);
        }
        return false;
    }

    // fsdev takes ownership and closes the FsFileSystem on unmount — do NOT
    // fsFsClose it here. Same contract as the NAND and album mounts.
    if (fsdevMountDevice("gamecard", fs) == -1) {
        SDL_Log("gamecard: fsdevMountDevice failed");
        return false;
    }
    g_last_fail_rc = 0;
    return true;
}

// Is a card physically present? Cheap enough to ask every refresh.
bool card_present() {
    FsDeviceOperator devop;
    if (R_FAILED(fsOpenDeviceOperator(&devop))) return false;
    bool inserted = false;
    const Result rc = fsDeviceOperatorIsGameCardInserted(&devop, &inserted);
    fsDeviceOperatorClose(&devop);
    return R_SUCCEEDED(rc) && inserted;
}

} // namespace
#endif

namespace Core {

void gamecard_refresh() {
#ifdef PLATFORM_SWITCH
    // Gated on the catalog, like NAND: if no transport exposes the surface there
    // is nothing to mount for. Checked FIRST so the common (disabled) case costs
    // nothing but a bool read.
    if (!Config::any_transport_exposes(&Config::Surfaces::gamecard)) {
        if (g_mounted) gamecard_unmount();
        return;
    }

    const bool present = card_present();

    // Act only on a CHANGE. A card can be inserted or ejected at any moment, so
    // unlike NAND this cannot be a one-shot at startup — but re-mounting an
    // already-mounted card every frame would be both wasteful and a good way to
    // upset the filesystem.
    if (present && !g_mounted) {
        g_mounted = try_mount();
    } else if (!present && g_mounted) {
        // Ejected. Unmount so the surface disappears rather than lingering as a
        // folder whose every read fails.
        gamecard_unmount();
    }
#endif
}

bool gamecard_mounted() {
#ifdef PLATFORM_SWITCH
    return g_mounted;
#else
    return false;
#endif
}

void gamecard_unmount() {
#ifdef PLATFORM_SWITCH
    if (!g_mounted) return;
    fsdevUnmountDevice("gamecard");
    g_mounted = false;
#endif
}

} // namespace Core
