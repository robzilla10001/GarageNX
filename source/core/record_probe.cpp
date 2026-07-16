// source/core/record_probe.cpp

#include "core/record_probe.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <vector>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::RecordProbe {

#ifdef PLATFORM_SWITCH

#pragma pack(push, 1)
struct CSR {                      // ContentStorageRecord (tinleaf layout)
    NcmContentMetaKey key;        // 0x10
    u64               storage_id; // 0x8
};
#pragma pack(pop)

static NcmContentMetaKey meta_key(const Core::Ncm::Title& t) {
    NcmContentMetaKey k; std::memset(&k, 0, sizeof(k));
    k.id = t.meta_id; k.version = t.version;
    k.type = (t.type == Core::Ncm::TitleType::Patch)        ? NcmContentMetaType_Patch
           : (t.type == Core::Ncm::TitleType::AddOnContent) ? NcmContentMetaType_AddOnContent
           : (t.type == Core::Ncm::TitleType::Application)  ? NcmContentMetaType_Application
                                                            : (NcmContentMetaType)0;
    k.install_type = NcmContentInstallType_Full;
    return k;
}

// ── Service-handle acquisition variants ─────────────────────────────────────────
enum class Handle { AppManagerIface, RawNsAm2, RawNsAm };

static bool open_handle(Handle h, Service* out) {
    switch (h) {
        case Handle::AppManagerIface:
            return R_SUCCEEDED(nsGetApplicationManagerInterface(out));
        case Handle::RawNsAm2:
            return R_SUCCEEDED(smGetService(out, "ns:am2"));
        case Handle::RawNsAm:
            return R_SUCCEEDED(smGetService(out, "ns:am"));
    }
    return false;
}

// ── List (to prove the handle works) ────────────────────────────────────────────
static ::Result do_list(Service* srv, u32 list_cmd, u64 app_id,
                        CSR* out, size_t count, u32* out_count) {
    const struct { u64 offset; u64 app_id; } in = { 0, app_id };
    return serviceDispatchInOut(srv, list_cmd, in, *out_count,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, count * sizeof(CSR) } });
}

// ── Delete variants ─────────────────────────────────────────────────────────────
// A: bare u64
static ::Result del_bare_u64(Service* srv, u32 cmd, u64 app_id) {
    return serviceDispatchIn(srv, cmd, app_id);
}
// B: u64 wrapped in a struct
static ::Result del_struct(Service* srv, u32 cmd, u64 app_id) {
    struct { u64 app_id; } in = { app_id };
    return serviceDispatchIn(srv, cmd, in);
}

// ── Push variants ───────────────────────────────────────────────────────────────
// A: in = {u8 lme; u8 pad[7]; u64 app_id}, buffer type-5
static ::Result push_pad_type5(Service* srv, u32 cmd, u8 lme, u64 app_id,
                               const CSR* recs, size_t n) {
    struct { u8 lme; u8 pad[7]; u64 app_id; } in = { lme, {0}, app_id };
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}
// B: in = {u64 app_id; u8 lme} (order swapped), buffer type-5
static ::Result push_app_first(Service* srv, u32 cmd, u8 lme, u64 app_id,
                               const CSR* recs, size_t n) {
    struct { u64 app_id; u8 lme; u8 pad[7]; } in = { app_id, lme, {0} };
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}
// C: in = {u8 lme; u8 pad[7]; u64 app_id}, buffer type-0x21 (AutoSelect)
static ::Result push_autoselect(Service* srv, u32 cmd, u8 lme, u64 app_id,
                                const CSR* recs, size_t n) {
    struct { u8 lme; u8 pad[7]; u64 app_id; } in = { lme, {0}, app_id };
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}

// D: in = {u64 app_id; u8 lme; pad[7]}, buffer type-5 (app_id first, no separate lme struct)
static ::Result push_appid_lme_t5(Service* srv, u32 cmd, u8 lme, u64 app_id,
                                  const CSR* recs, size_t n) {
    struct { u64 app_id; u8 lme; u8 pad[7]; } in = { app_id, lme, {0} };
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}
// E: in = {u8 lme; pad[7]; u64 app_id}, buffer type HipcPointer
static ::Result push_pad_pointer(Service* srv, u32 cmd, u8 lme, u64 app_id,
                                 const CSR* recs, size_t n) {
    struct { u8 lme; u8 pad[7]; u64 app_id; } in = { lme, {0}, app_id };
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}
// F: in = just {u64 app_id} (lme sent as... no, official is (u8 lme, u64 app_id));
//    try in = {u8 lme; u64 app_id} with NATURAL alignment (compiler pads to 16)
static ::Result push_natural(Service* srv, u32 cmd, u8 lme, u64 app_id,
                             const CSR* recs, size_t n) {
    struct In { u8 lme; u64 app_id; } in; in.lme = lme; in.app_id = app_id;
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}
// G: lme passed as u32, in = {u32 lme; u32 pad; u64 app_id}, buffer type-5
static ::Result push_lme_u32(Service* srv, u32 cmd, u8 lme, u64 app_id,
                             const CSR* recs, size_t n) {
    struct { u32 lme; u32 pad; u64 app_id; } in = { (u32)lme, 0, app_id };
    return serviceDispatchIn(srv, cmd, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { recs, n * sizeof(CSR) } });
}

// ── Variant table ───────────────────────────────────────────────────────────────
// DELETE is confirmed: cmd 27 on the app-manager iface returns 0x0.
// So all push probes below FIX delete=27, push=26, and vary ONLY the push
// input/buffer layout to find the one that returns 0x0.
struct VDef {
    const char* name;
    Handle handle;
    u32  list_cmd;
    u32  del_cmd;
    u32  push_cmd;
    int  del_variant;
    int  push_variant;  // 0=pad_type5 1=app_first 2=autoselect 3=appid_lme_t5
                        // 4=pad_pointer 5=natural 6=lme_u32
};

static const VDef VARIANTS[] = {
    // Confirm baseline: del=27 works, push=26 with the layout that already ran.
    { "1: del27 push26 pad+t5",     Handle::AppManagerIface, 17, 27, 26, 0, 0 },
    // Push layout permutations at cmd 26:
    { "2: del27 push26 appid+lme",  Handle::AppManagerIface, 17, 27, 26, 0, 3 },
    { "3: del27 push26 pointer",    Handle::AppManagerIface, 17, 27, 26, 0, 4 },
    { "4: del27 push26 natural",    Handle::AppManagerIface, 17, 27, 26, 0, 5 },
    { "5: del27 push26 lme_u32",    Handle::AppManagerIface, 17, 27, 26, 0, 6 },
    { "6: del27 push26 autosel",    Handle::AppManagerIface, 17, 27, 26, 0, 2 },
    // Maybe push is a different cmd than 26 — try 25 and 28 with pad+t5:
    { "7: del27 push25 pad+t5",     Handle::AppManagerIface, 17, 27, 25, 0, 0 },
    { "8: del27 push28 pad+t5",     Handle::AppManagerIface, 17, 27, 28, 0, 0 },
    // And push24 (some tables), plus lme_u32 at 26 on raw am2:
    { "9: del27 push24 pad+t5",     Handle::AppManagerIface, 17, 27, 24, 0, 0 },
    { "10: am2 del27 push26 pad+t5",Handle::RawNsAm2,        17, 27, 26, 0, 0 },
};

static constexpr int N_VARIANTS = (int)(sizeof(VARIANTS)/sizeof(VARIANTS[0]));

#endif // PLATFORM_SWITCH

int variant_count() {
#ifdef PLATFORM_SWITCH
    return N_VARIANTS;
#else
    return 0;
#endif
}

std::vector<std::string> variant_names() {
    std::vector<std::string> names;
#ifdef PLATFORM_SWITCH
    for (int i = 0; i < N_VARIANTS; ++i) names.push_back(VARIANTS[i].name);
#endif
    return names;
}

bool run_variant(int index, uint64_t base_app_id,
                 const Core::Ncm::Title& removed_title,
                 std::string& out_detail) {
#ifndef PLATFORM_SWITCH
    (void)index; (void)base_app_id; (void)removed_title;
    out_detail = "no-switch"; return false;
#else
    if (index < 0 || index >= N_VARIANTS) { out_detail = "bad index"; return false; }
    const VDef& v = VARIANTS[index];

    Service srv;
    if (!open_handle(v.handle, &srv)) { out_detail = "open handle failed"; return false; }

    // List the CURRENT record (proves handle + list cmd, and gives us the real
    // record contents to push back).
    constexpr size_t MAXR = 64;
    std::vector<CSR> recs(MAXR);
    u32 count = 0;
    ::Result lrc = do_list(&srv, v.list_cmd, base_app_id, recs.data(), MAXR, &count);

    // Build the filtered "kept" set (drop the removed meta).
    NcmContentMetaKey rk = meta_key(removed_title);
    std::vector<CSR> kept;
    int dropped = 0;
    if (R_SUCCEEDED(lrc)) {
        for (u32 i = 0; i < count; ++i) {
            if (recs[i].key.id == rk.id && recs[i].key.type == rk.type) { dropped++; continue; }
            kept.push_back(recs[i]);
        }
    }

    // ── SAFE PUSH-ONLY PROBE ────────────────────────────────────────────────────
    // We do NOT delete first. We test whether PushApplicationRecord accepts our
    // input layout by pushing the CURRENT (unfiltered) record back unchanged —
    // this is a no-op that leaves the record intact but tells us if push=0x0.
    // Only once we KNOW the working push layout do we do the real delete+push.
    //
    // If the list failed (no record present, e.g. already-removed title), we
    // can't probe push here — report the list error so the user reinstalls.
    ::Result prc = 0xFFFFFFFF;
    if (R_SUCCEEDED(lrc) && count > 0) {
        // Push the FULL current record back (unchanged) to validate the layout
        // safely — no records are dropped, so this can't orphan the title.
        switch (v.push_variant) {
            case 0: prc = push_pad_type5   (&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
            case 1: prc = push_app_first   (&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
            case 2: prc = push_autoselect  (&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
            case 3: prc = push_appid_lme_t5(&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
            case 4: prc = push_pad_pointer (&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
            case 5: prc = push_natural     (&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
            case 6: prc = push_lme_u32     (&srv, v.push_cmd, 3, base_app_id, recs.data(), count); break;
        }
    }

    serviceClose(&srv);

    char buf[128];
    // Dump the first record's raw bytes so we can see the real storage_id
    // position/size, plus the push result.
    char rawhex[40] = "none";
    if (R_SUCCEEDED(lrc) && count > 0) {
        const uint8_t* p = (const uint8_t*)&recs[0];
        int q = 0;
        for (int i = 0x10; i < 0x18 && q < 36; ++i)  // bytes 0x10..0x18 = storage_id area
            q += snprintf(rawhex + q, sizeof(rawhex) - q, "%02X", p[i]);
    }
    snprintf(buf, sizeof(buf), "L=%08X n=%u sid[10:18]=%s push=%08X",
             lrc, count, rawhex, prc);
    out_detail = buf;

    return R_SUCCEEDED(prc);
#endif
}

} // namespace Core::RecordProbe
