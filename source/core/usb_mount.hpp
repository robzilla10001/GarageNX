#pragma once
// source/core/usb_mount.hpp
//
// USB mass-storage devices (external drives in the dock), via libusbhsfs.
//
// WHY A LIBRARY: doing this on libnx alone means implementing USB-host
// enumeration, SCSI Bulk-Only Transport AND a filesystem driver. libusbhsfs is
// what DBI, Goldleaf and Awoo all use; writing it again would be months of work
// for a worse result.
//
// libusbhsfs mounts each volume under its OWN devoptab name ("ums0:", "ums1:",
// ...), which is unlike every other surface in this project: SD, NAND and the
// game card each have one fixed mount point. So the USB surface is a CHOOSER of
// currently-attached volumes rather than a single root, and the catalog's
// vfs_root for it is only meaningful once a volume is selected.
//
// HOT-PLUG is the norm here, more so than the game card: drives appear, disappear,
// and can carry several volumes each. The library exposes a status-change event;
// this module polls it rather than blocking, because the caller is the main loop.

#include <cstdint>
#include <string>
#include <vector>

namespace Core::UsbMount {

struct Volume {
    std::string mount;        // devoptab prefix, e.g. "ums0:" — use as a path root
    std::string label;        // human name: product, or manufacturer + product
    std::string fs_name;      // "FAT32", "exFAT", "NTFS", "ext" — for display
    uint64_t    capacity = 0; // raw logical-unit capacity, may be shared
    bool        write_protect = false;
};

/// Initialise the USB host interface. Safe to call when no drive is attached.
/// Returns false if unavailable — notably when the deprecated fsp-usb sysmodule
/// is running, which libusbhsfs refuses to coexist with. That is a real and
/// common configuration, so the failure is reported rather than swallowed.
bool init();

/// Shut down and unmount everything. For app exit.
void exit();

/// Re-read attached volumes if the library signalled a change. Cheap when nothing
/// has changed (one event poll), so it is safe on the 1 Hz tick alongside the
/// game-card refresh.
void refresh();

/// Volumes mounted right now. Empty when no drive is attached.
const std::vector<Volume>& volumes();

/// True if the interface initialised. False means USB browse is unavailable, not
/// that no drive is attached.
bool available();

} // namespace Core::UsbMount
