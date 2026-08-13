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

static const char* sd_manufacturer_name(uint8_t mid) {
    switch (mid) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba";
        case 0x03: return "SanDisk";
        case 0x12: return "Micron";
        case 0x1b: return "Samsung";
        case 0x1d: return "ADATA";
        case 0x22: return "Kowin";           // Patriot (Viper) and others; Hekate "Kowin(22)"
        case 0x27: return "Phison";
        case 0x28: return "Lexar";
        case 0x31: return "Silicon Power";
        case 0x41: return "Kingston";
        case 0x74: return "Transcend";
        case 0x76: return "Patriot";
        case 0x82: return "Sony";
        case 0x9c: return "Angelbird / Hoodman";
        default:   return nullptr;
    }
}

SdCid sd_cid() {
    SdCid c;
    FsDeviceOperator devop;
    if (R_FAILED(fsOpenDeviceOperator(&devop))) return c;
    uint8_t cid[16] = {};
    Result rc = fsDeviceOperatorGetSdCardCid(&devop, cid, sizeof(cid), sizeof(cid));
    fsDeviceOperatorClose(&devop);
    if (R_FAILED(rc)) return c;

    // FS returns the 16-byte CID with the register in reverse byte order AND a
    // trailing pad byte, so the spec's MSB-first field k lives at cid[14 - k]
    // (NOT cid[15 - k]). Verified against a Patriot/Kowin card vs Hekate.
    // Spec layout (MSB-first): MID[0] OID[1..2] PNM[3..7] PRV[8] PSN[9..12]
    // (reserved|MDT)[13..14] CRC[15-not-returned].
    uint8_t r[15];
    for (int i = 0; i < 15; ++i) r[i] = cid[14 - i];

    const uint8_t  mid = r[0];
    char pnm[6] = { char(r[3]), char(r[4]), char(r[5]), char(r[6]), char(r[7]), 0 };
    const uint8_t  prv = r[8];
    const uint32_t psn = (uint32_t(r[9]) << 24) | (uint32_t(r[10]) << 16) |
                         (uint32_t(r[11]) << 8) | uint32_t(r[12]);
    const uint16_t mdt = (uint16_t(r[13] & 0x0F) << 8) | r[14];  // year<<4 | month
    const int year  = 2000 + (mdt >> 4);
    const int month = mdt & 0x0F;

    for (int i = 4; i >= 0; --i) { if (pnm[i] == ' ' || pnm[i] == 0) pnm[i] = 0; else break; }

    char buf[32];
    const char* mname = sd_manufacturer_name(mid);
    // Manufacturer: "Name (HH)" with the id in HEX, matching Hekate ("Kowin(22)").
    std::snprintf(buf, sizeof(buf), "%s (%02X)", mname ? mname : "Unknown", mid);

    c.valid        = true;
    c.manufacturer = buf;
    std::snprintf(buf, sizeof(buf), "%02X%02X", r[1], r[2]); c.oem_id = buf;   // hex, Hekate-style
    c.product_name = pnm;
    std::snprintf(buf, sizeof(buf), "%d.%d", prv >> 4, prv & 0x0F); c.revision = buf;
    std::snprintf(buf, sizeof(buf), "%08X", psn);                   c.serial   = buf;
    std::snprintf(buf, sizeof(buf), "%02d/%04d", month, year);      c.mfg_date = buf;
    return c;
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

SpaceInfo nand_system() {
    // Identical to nand_user() but for the System partition — same open/query/
    // close shape, deliberately copied rather than reinvented. Open-and-close per
    // call is what nand_user() has always done and it is a cheap query; do NOT
    // hold the handle open, since this is polled from the status bar.
    FsFileSystem nand;
    SpaceInfo info;
    if (R_SUCCEEDED(fsOpenBisFileSystem(&nand, FsBisPartitionId_System, ""))) {
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
SdCid sd_cid() {
    // PC stub — a plausible card so the section renders in dev.
    return SdCid{ true, "SanDisk (3)", "5344", "SL16G", "8.0", "1A2B3C4D", "07/2021" };
}
SpaceInfo nand_user() {
    // Fabricate a 32GB NAND that's mostly full, for UI layout testing.
    SpaceInfo info;
    info.total_bytes = 32ULL * 1024 * 1024 * 1024;
    info.free_bytes  = 3ULL  * 1024 * 1024 * 1024;
    info.valid = true;
    return info;
}
SpaceInfo nand_system() {
    // Fabricate a small, nearly-full system partition — the realistic shape, and
    // the one that would expose a layout bug at the tight end.
    SpaceInfo info;
    info.total_bytes = 5ULL * 1024 * 1024 * 1024;
    info.free_bytes  = 512ULL * 1024 * 1024;
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
