// source/core/nand_mount.cpp

#include "core/nand_mount.hpp"
#include "services/storage_catalog.hpp"
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
    using Services::StorageCatalog;
    using Id = Services::StorageSurface::Id;
    const auto& cfg = Config::get().mtp;

    // Only mount a partition if its catalog surface is enabled. NAND system is
    // OFF by default, so bis_system: is not even mounted unless the user opts in.
    if (!g_user_mounted && StorageCatalog::enabled(Id::NandUser, cfg))
        g_user_mounted = mount_bis(FsBisPartitionId_User, "bis_user");

    if (!g_system_mounted && StorageCatalog::enabled(Id::NandSystem, cfg))
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
