// source/core/album_mount.hpp
//
// Mounts the console's Album (screenshots + gameplay videos) as a browseable
// stdio device "album:/", so the file-manager transports can list and pull album
// media the same way they browse the SD card.
//
// Mechanism (confirmed from switchbrew/libnx source, not guessed):
//   fsOpenImageDirectoryFileSystem(&fs, FsImageDirectoryId_Sd)  // the SD album
//   fsdevMountDevice("album", fs)                               // -> "album:/"
// fsdev takes ownership of the FsFileSystem and closes it on unmount / at exit.

#pragma once

namespace Core {

// Mount the SD album as "album:/". Safe to call once at startup. Returns true on
// success. On failure the device simply won't exist and the Album surface, though
// enabled in the catalog, will resolve to an empty/error listing rather than
// crashing — the transports tolerate a missing mount.
bool mount_album();

// Unmount "album:/" if it was mounted. Called during teardown.
void unmount_album();

} // namespace Core
