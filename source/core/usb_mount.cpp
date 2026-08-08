// source/core/usb_mount.cpp

#include "core/usb_mount.hpp"
#include "config/config.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <usbhsfs.h>
#include <SDL2/SDL.h>
#endif

namespace Core::UsbMount {

namespace {
std::vector<Volume> g_volumes;
bool g_available = false;

#ifdef PLATFORM_SWITCH
const char* fs_name_for(u8 t) {
    // Mirrors UsbHsFsDeviceFileSystemType. Named rather than numbered because the
    // value is shown to a user.
    switch (t) {
        case UsbHsFsDeviceFileSystemType_FAT12:  return "FAT12";
        case UsbHsFsDeviceFileSystemType_FAT16:  return "FAT16";
        case UsbHsFsDeviceFileSystemType_FAT32:  return "FAT32";
        case UsbHsFsDeviceFileSystemType_exFAT:  return "exFAT";
        case UsbHsFsDeviceFileSystemType_NTFS:   return "NTFS";
        case UsbHsFsDeviceFileSystemType_EXT2:   return "ext2";
        case UsbHsFsDeviceFileSystemType_EXT3:   return "ext3";
        case UsbHsFsDeviceFileSystemType_EXT4:   return "ext4";
        default:                                 return "Unknown";
    }
}

void rebuild() {
    g_volumes.clear();
    const u32 n = usbHsFsGetMountedDeviceCount();
    if (n == 0) return;

    std::vector<UsbHsFsDevice> devs(n);
    const u32 got = usbHsFsListMountedDevices(devs.data(), n);
    for (u32 i = 0; i < got; ++i) {
        const UsbHsFsDevice& d = devs[i];
        Volume v;
        v.mount = d.name;                 // already "umsN:" including the colon
        v.fs_name = fs_name_for(d.fs_type);
        v.capacity = d.capacity;
        v.write_protect = d.write_protect;

        // Prefer the product name; fall back through manufacturer to the mount
        // name, so a device with empty SCSI strings still lists as something
        // selectable rather than a blank row.
        if (d.product_name[0])        v.label = d.product_name;
        else if (d.manufacturer[0])   v.label = d.manufacturer;
        else                          v.label = v.mount;
        g_volumes.push_back(std::move(v));
    }
}
#endif
} // namespace

bool init() {
#ifdef PLATFORM_SWITCH
    if (g_available) return true;
    // event_idx 0: this app uses no usb:hs interface-available events of its own.
    const Result rc = usbHsFsInitialize(0);
    if (R_FAILED(rc)) {
        // The common real-world cause is the deprecated fsp-usb sysmodule being
        // installed — libusbhsfs refuses to coexist with it. Worth logging by rc
        // so that is diagnosable rather than "USB just does not work".
        SDL_Log("usb: usbHsFsInitialize rc=0x%08X (fsp-usb running?)", rc);
        g_available = false;
        return false;
    }
    g_available = true;
    rebuild();          // a drive may already be attached at startup
    return true;
#else
    return false;
#endif
}

void exit() {
#ifdef PLATFORM_SWITCH
    if (!g_available) return;
    usbHsFsExit();
    g_available = false;
    g_volumes.clear();
#endif
}

void refresh() {
#ifdef PLATFORM_SWITCH
    if (!g_available) return;

    UEvent* ev = usbHsFsGetStatusChangeUserEvent();
    if (!ev) return;

    // Zero timeout: poll, never block. This runs on the main loop, and blocking
    // here would stall the UI for as long as no drive changed — the mirror-bug
    // pattern this project has hit twice.
    //
    // libnx has NO ueventWait(). Events are waited on through the generic waiter
    // API — waiterForUEvent() + waitSingle/waitMulti — which is what libusbhsfs's
    // own example_event does. I originally wrote ueventWait() and DECLARED IT IN
    // MY OWN STUB, so the guard cheerfully validated a function that does not
    // exist. See the warning in tools/stubs/switch.h.
    // waitMulti with ONE waiter, not waitSingle. Both are libnx API, but
    // libusbhsfs's own example uses waitMulti and I could verify that by reading
    // it; waitSingle I merely believed in, and believing is what produced the
    // ueventWait() call this replaced. Prefer the form you can see working.
    int idx = 0;
    Waiter w = waiterForUEvent(ev);
    if (R_SUCCEEDED(waitMulti(&idx, 0, w))) rebuild();
#endif
}

const std::vector<Volume>& volumes() { return g_volumes; }
bool available() { return g_available; }

} // namespace Core::UsbMount
