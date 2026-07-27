// source/core/nand_mount.cpp

#include "core/nand_mount.hpp"
#include "config/config.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <SDL2/SDL.h>

namespace {
bool g_user_mounted   = false;
bool g_system_mounted = false;

// Mount one BIS partition as `device`. Read-only by policy: we open the partition
// and never issue writes. fsdev takes ownership of the FsFileSystem and closes it
// on unmount, so we must NOT fsFsClose it ourselves (same contract as album).
bool mount_bis(FsBisPartitionId part, const char* device) {
    FsFileSystem fs;
    Result rc = fsOpenBisFileSystem(&fs, part, "");
    if (R_FAILED(rc)) {
        SDL_Log("nand: fsOpenBisFileSystem(%s) failed rc=0x%08X", device, rc);
        return false;
    }
    if (fsdevMountDevice(device, fs) == -1) {
        SDL_Log("nand: fsdevMountDevice(%s) failed", device);
        return false;
    }
    return true;
}
} // namespace
#endif

namespace Core {

void mount_nand() {
#ifdef PLATFORM_SWITCH
    // Mount if ANY transport exposes the partition. A mount is process-wide but
    // the toggles are per-transport, so asking one transport's block would leave
    // the device unmounted for a user who enabled NAND on a different one — the
    // surface would then appear and fail to open, which is the exact symptom this
    // module's gating was meant to prevent.
    //
    // CALL ORDER MATTERS: this reads config, so it must run AFTER Config::load().
    // It used to run before, which meant it gated on compile-time defaults and
    // bis_system: could never mount however config.json was set. See main.cpp.
    if (!g_user_mounted &&
        Config::any_transport_exposes(&Config::Surfaces::nand_user))
        g_user_mounted = mount_bis(FsBisPartitionId_User, "bis_user");

    if (!g_system_mounted &&
        Config::any_transport_exposes(&Config::Surfaces::nand_system))
        g_system_mounted = mount_bis(FsBisPartitionId_System, "bis_system");
#endif
}

void unmount_nand() {
#ifdef PLATFORM_SWITCH
    if (g_user_mounted)   { fsdevUnmountDevice("bis_user");   g_user_mounted = false; }
    if (g_system_mounted) { fsdevUnmountDevice("bis_system"); g_system_mounted = false; }
#endif
}

} // namespace Core
