// source/core/album_mount.cpp

#include "core/album_mount.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <SDL2/SDL.h>
#include <cstdio>

namespace {
bool g_album_mounted = false;
}
#endif

namespace Core {

bool mount_album() {
#ifdef PLATFORM_SWITCH
    if (g_album_mounted) return true;

    // Open the SD-card image directory (the album). FsImageDirectoryId_Sd is the
    // user-facing album on the SD card; _Nand is the (rarely used) internal one.
    FsFileSystem fs;
    Result rc = fsOpenImageDirectoryFileSystem(&fs, FsImageDirectoryId_Sd);
    if (R_FAILED(rc)) {
        SDL_Log("album: fsOpenImageDirectoryFileSystem failed rc=0x%08X", rc);
        return false;
    }

    // Mount it as "album:". fsdev takes ownership of `fs` — it closes it on
    // unmount and even if the mount call itself fails, so we must NOT fsFsClose
    // it ourselves. Device name must be <=31 chars, no trailing colon.
    if (fsdevMountDevice("album", fs) == -1) {
        SDL_Log("album: fsdevMountDevice failed");
        return false;
    }

    g_album_mounted = true;
    return true;
#else
    return false;
#endif
}

void unmount_album() {
#ifdef PLATFORM_SWITCH
    if (!g_album_mounted) return;
    fsdevUnmountDevice("album");
    g_album_mounted = false;
#endif
}

} // namespace Core
