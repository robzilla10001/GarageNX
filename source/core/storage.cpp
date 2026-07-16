// source/core/storage.cpp

#include "core/storage.hpp"
#include <SDL2/SDL.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#else
#include <sys/statvfs.h>
#endif

namespace Core::Storage {

#ifdef PLATFORM_SWITCH

static SpaceInfo query_fs(FsFileSystem* fs) {
    SpaceInfo info;
    s64 total = 0, free = 0;
    if (R_SUCCEEDED(fsFsGetTotalSpace(fs, "/", &total)) &&
        R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free))) {
        info.total_bytes = (uint64_t)total;
        info.free_bytes  = (uint64_t)free;
        info.valid = true;
    }
    return info;
}

SpaceInfo sd_card() {
    // libnx exposes the mounted SD filesystem via fsdev; get its handle.
    FsFileSystem* sdmc = fsdevGetDeviceFileSystem("sdmc");
    if (!sdmc) return SpaceInfo{};
    return query_fs(sdmc);
}

SpaceInfo nand_user() {
    // Open the BIS User partition read-only for a capacity query, then close.
    FsFileSystem nand;
    SpaceInfo info;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&nand, FsBisPartitionId_User, ""))) {
        info = query_fs(&nand);
        fsFsClose(&nand);
    }
    return info;
}

#else  // PC stub

static SpaceInfo query_path(const char* path) {
    SpaceInfo info;
    struct statvfs st;
    if (statvfs(path, &st) == 0) {
        info.total_bytes = (uint64_t)st.f_blocks * st.f_frsize;
        info.free_bytes  = (uint64_t)st.f_bavail * st.f_frsize;
        info.valid = true;
    }
    return info;
}

SpaceInfo sd_card()   { return query_path("."); }
SpaceInfo nand_user() {
    // Fabricate a 32GB NAND that's mostly full, for UI layout testing.
    SpaceInfo info;
    info.total_bytes = 32ULL * 1024 * 1024 * 1024;
    info.free_bytes  = 3ULL  * 1024 * 1024 * 1024;
    info.valid = true;
    return info;
}

#endif

} // namespace Core::Storage


namespace Core::Thermal {

#ifdef PLATFORM_SWITCH

// Temperature reads changed across firmware versions:
//   - tsGetTemperatureMilliC: removed on HOS 14.0.0+ (returns error on 18.x)
//   - tsGetTemperature (s32 Celsius): works across versions
//   - tsOpenSession + tsSessionGetTemperature (float): 10.0.0+, most precise
// Also note the location naming in libnx: Internal = PCB, External = SoC.
// We try the precise session API first, then fall back to the whole-degree call.

static bool read_temp(u32 device_code, TsLocation fallback_loc, float& out) {
    // Preferred: session API (float precision), available 10.0.0+.
    TsSession session;
    if (R_SUCCEEDED(tsOpenSession(&session, device_code))) {
        float t = 0.f;
        Result rc = tsSessionGetTemperature(&session, &t);
        tsSessionClose(&session);
        if (R_SUCCEEDED(rc)) { out = t; return true; }
    }

    // Fallback: whole-degree Celsius call (works on 14.0.0+ too).
    s32 whole = 0;
    if (R_SUCCEEDED(tsGetTemperature(fallback_loc, &whole))) {
        out = (float)whole;
        return true;
    }
    return false;
}

Temp soc() {
    Temp t;
    // SoC = External location / TMP451 external. Device code 0x41000002.
    if (read_temp(TsDeviceCode_LocationExternal, TsLocation_External, t.celsius))
        t.valid = true;
    return t;
}

Temp pcb() {
    Temp t;
    // PCB = Internal location / TMP451 internal. Device code 0x41000001.
    if (read_temp(TsDeviceCode_LocationInternal, TsLocation_Internal, t.celsius))
        t.valid = true;
    return t;
}

#else

Temp soc() { return Temp{42.5f, true}; }   // plausible idle temp for the stub
Temp pcb() { return Temp{38.0f, true}; }

#endif

} // namespace Core::Thermal
