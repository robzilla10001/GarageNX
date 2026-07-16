// tools/ns_probe/ns_probe.cpp
// Standalone Switch homebrew tool for rapid NS/NCM install state probing.
// Does NOT install anything. Reads existing NCM state for title 010028600EBDA000
// and runs the same NS queries GarageNX installer does.
//
// Build: cmake .. -DPLATFORM=Switch && make -j$(nproc) ns_probe
// Output: build/ns_probe.nro
// Usage: copy to switch/ on SD, run from hbmenu, read install_probe.txt

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <vector>
#include <sys/stat.h>

// ── Title under test ─────────────────────────────────────────────────────────
static constexpr u64 TARGET_APP_ID  = 0x010028600EBDA000ULL;
static constexpr u64 TARGET_PATCH_ID= 0x010028600EBDA800ULL;
static const NcmStorageId STORAGE   = NcmStorageId_SdCard;

// ── Logging ──────────────────────────────────────────────────────────────────
static FILE* g_log = nullptr;

static void L(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

static void Lhex(const char* label, const uint8_t* p, size_t n) {
    if (!g_log) return;
    fprintf(g_log, "%s: ", label);
    for (size_t i = 0; i < n; ++i) fprintf(g_log, "%02x", p[i]);
    fputc('\n', g_log); fflush(g_log);
}

// ── Helper: content_id to hex string ─────────────────────────────────────────
static void cid_hex(const NcmContentId& cid, char out[33]) {
    for (int i = 0; i < 16; ++i) snprintf(out + i*2, 3, "%02x", cid.c[i]);
    out[32] = 0;
}

// ── PackagedContentInfo layout (0x38 bytes, packed) ──────────────────────────
#pragma pack(push, 1)
struct PackagedContentInfo {
    uint8_t hash[0x20];
    uint8_t content_id[0x10];
    uint8_t size_le[6];
    uint8_t content_type;
    uint8_t id_offset;
};
#pragma pack(pop)
static_assert(sizeof(PackagedContentInfo) == 0x38, "PackagedContentInfo");

// ── CNMT header layout (0x20 bytes, packed) ───────────────────────────────────
#pragma pack(push, 1)
struct CnmtHeader {
    uint64_t title_id;
    uint32_t version;
    uint8_t  meta_type;
    uint8_t  field_D;
    uint16_t extended_header_size;
    uint16_t content_count;
    uint16_t content_meta_count;
    uint8_t  attributes;
    uint8_t  storage_id;
    uint8_t  content_install_type;
    uint8_t  padding;
    uint8_t  reserved[8];
};
#pragma pack(pop)
static_assert(sizeof(CnmtHeader) == 0x20, "CnmtHeader");

// ── Probe a single meta key ───────────────────────────────────────────────────
static void probe_key(NcmContentMetaDatabase& db, NcmContentStorage& cs,
                      const NcmContentMetaKey& key) {
    char header[80];
    snprintf(header, sizeof(header), "=== key %016llX v%u type=0x%02X ===",
             (unsigned long long)key.id, key.version, (unsigned)key.type);
    L("%s", header);

    // ── 1. GetSize + Get raw blob ─────────────────────────────────────────────
    u64 blob_size = 0;
    Result sz_rc = ncmContentMetaDatabaseGetSize(&db, &blob_size, &key);
    L("GetSize rc=0x%08X size=%llu", sz_rc, (unsigned long long)blob_size);
    if (R_FAILED(sz_rc) || blob_size == 0) return;

    std::vector<uint8_t> blob(blob_size);
    u64 got = 0;
    Result get_rc = ncmContentMetaDatabaseGet(&db, &key, &got, blob.data(), blob.size());
    L("Get rc=0x%08X got=%llu", get_rc, (unsigned long long)got);
    if (R_FAILED(get_rc) || got < sizeof(CnmtHeader)) return;

    // Dump first 64 bytes
    size_t dump = got < 64 ? got : 64;
    char hex[129] = {};
    for (size_t i = 0; i < dump; ++i) snprintf(hex + i*2, 3, "%02x", blob[i]);
    L("blob[0..%zu]: %s", dump-1, hex);

    // ── 2. Parse content records ──────────────────────────────────────────────
    const CnmtHeader* hdr = reinterpret_cast<const CnmtHeader*>(blob.data());
    uint16_t ext_sz    = hdr->extended_header_size;
    uint16_t cnt_count = hdr->content_count;
    size_t   rec_off   = sizeof(CnmtHeader) + ext_sz;
    L("ext_hdr_size=%u content_count=%u records_at=0x%zx", ext_sz, cnt_count, rec_off);

    for (uint16_t i = 0; i < cnt_count; ++i) {
        size_t r = rec_off + i * sizeof(PackagedContentInfo);
        if (r + sizeof(PackagedContentInfo) > got) { L("  [%d] truncated", i); break; }
        const PackagedContentInfo* rec =
            reinterpret_cast<const PackagedContentInfo*>(blob.data() + r);
        NcmContentId cid{};
        memcpy(cid.c, rec->content_id, 0x10);
        char cidhex[33]; cid_hex(cid, cidhex);
        bool has = false;
        ncmContentStorageHas(&cs, &has, &cid);
        L("  [%d] type=0x%02X id=%s has=%d", i, rec->content_type, cidhex, (int)has);
    }

    // ── 3. GetContentIdByType for each interesting type ───────────────────────
    const struct { NcmContentType type; const char* name; } types[] = {
        { NcmContentType_Meta,           "Meta"    },
        { NcmContentType_Program,        "Program" },
        { NcmContentType_Control,        "Control" },
        { NcmContentType_Data,           "Data"    },
        { NcmContentType_LegalInformation,"Legal"  },
    };
    for (auto& t : types) {
        NcmContentId cid{};
        Result rc = ncmContentMetaDatabaseGetContentIdByType(&db, &cid, &key, t.type);
        char cidhex[33]; cid_hex(cid, cidhex);
        L("GetCidByType(%s) rc=0x%08X id=%s", t.name, rc, cidhex);
        if (R_SUCCEEDED(rc)) {
            // Verify it's actually in storage.
            bool has = false;
            ncmContentStorageHas(&cs, &has, &cid);
            // Get the path.
            char path[512] = {};
            Result pr = ncmContentStorageGetPath(&cs, path, sizeof(path), &cid);
            L("  has=%d path_rc=0x%08X path=%s", (int)has, pr, path);
        }
    }

    // ── 4. List content infos via NCM ─────────────────────────────────────────
    // ncmContentMetaDatabaseListContentInfo gives the NcmContentInfo[] that NCM
    // uses internally — distinct from the PackagedContentInfo in the blob.
    NcmContentInfo infos[32];
    s32 info_written = 0;
    Result list_rc = ncmContentMetaDatabaseListContentInfo(
        &db, &info_written, infos, 32, &key, 0);
    L("ListContentInfo rc=0x%08X written=%d", list_rc, info_written);
    for (s32 i = 0; i < info_written && i < 32; ++i) {
        char cidhex[33]; cid_hex(infos[i].content_id, cidhex);
        L("  info[%d] type=0x%02X id=%s", i, (unsigned)infos[i].content_type, cidhex);
    }
}

// ── NS probing ────────────────────────────────────────────────────────────────
static void probe_ns(const NcmContentMetaKey& app_key) {
    L("=== NS application record probe ===");

    // List all application records — find ours.
    {
        // Total count via list with big offset.
        NsApplicationRecord recs[256];
        s32 count = 0;
        Result rc = nsListApplicationRecord(recs, 256, 0, &count);
        L("nsListApplicationRecord rc=0x%08X count=%d", rc, count);
        bool found = false;
        for (s32 i = 0; i < count; ++i) {
            if (recs[i].application_id == app_key.id) {
                found = true;
                L("FOUND at index %d: app_id=%016llX attr=0x%02X",
                  i, (unsigned long long)recs[i].application_id, recs[i].attributes);
                break;
            }
        }
        if (!found) L("NOT FOUND in %d NS application records", count);
    }

    // PushApplicationRecord (cmd 16, confirmed from ITotalJustice XCI installer).
    // Delete first (cmd 27), then push with HipcMapAlias buffer.
    {
        #pragma pack(push,1)
        struct CSR { NcmContentMetaKey key; u8 storage_id; u8 _pad[7]; };
        #pragma pack(pop)
        CSR csr{}; csr.key = app_key; csr.storage_id = (u8)STORAGE;

        Service ns_srv{};
        if (R_SUCCEEDED(nsGetApplicationManagerInterface(&ns_srv))) {
            // Delete existing record first (cmd 27).
            Result del_rc = serviceDispatchIn(&ns_srv, 27, app_key.id);
            L("DeleteApplicationRecord(%016llX) cmd27 rc=0x%08X",
              (unsigned long long)app_key.id, del_rc);

            const struct { u8 lme; u8 _pad[7]; u64 tid; } push_in = { 3, {0}, app_key.id };
            Result push_rc = serviceDispatchIn(&ns_srv, 16, push_in,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                .buffers = { { &csr, sizeof(csr) } });
            L("PushApplicationRecord(%016llX) cmd16 rc=0x%08X",
              (unsigned long long)app_key.id, push_rc);
            serviceClose(&ns_srv);
        } else {
            L("nsGetApplicationManagerInterface failed");
        }
    }

    // nsTouchApplication.
    {
        Result rc = nsTouchApplication(app_key.id);
        L("nsTouchApplication(%016llX) rc=0x%08X (0410=NotFound expected)",
          (unsigned long long)app_key.id, rc);
    }

    // Trigger NS scan via update event.
    {
        Event evt{};
        Result erc = nsGetApplicationRecordUpdateSystemEvent(&evt);
        L("nsGetApplicationRecordUpdateSystemEvent rc=0x%08X", erc);
        if (R_SUCCEEDED(erc)) {
            Result wrc = eventWait(&evt, 2000000000ULL); // 2s
            L("eventWait rc=0x%08X", wrc);
            eventClose(&evt);
        }
    }

    // Re-check after scan.
    {
        NsApplicationRecord recs[256];
        s32 count = 0;
        nsListApplicationRecord(recs, 256, 0, &count);
        bool found = false;
        for (s32 i = 0; i < count; ++i)
            if (recs[i].application_id == app_key.id) { found = true; break; }
        L("After scan: found=%d (total=%d)", (int)found, count);
    }
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int, char**) {
    consoleInit(nullptr);
    printf("ns_probe: running, check sdmc:/switch/install_probe.txt\n");
    consoleUpdate(nullptr);

    // Ensure the output directory exists.
    mkdir("sdmc:/switch", 0777);
    g_log = fopen("sdmc:/switch/install_probe.txt", "wb");
    L("=== ns_probe start ===");

    // Initialise services.
    nsInitialize();
    ncmInitialize();
    L("services initialized");

    NcmContentMetaDatabase db{};
    NcmContentStorage cs{};
    Result db_rc = ncmOpenContentMetaDatabase(&db, STORAGE);
    Result cs_rc = ncmOpenContentStorage(&cs, STORAGE);
    L("OpenMetaDB rc=0x%08X  OpenStorage rc=0x%08X", db_rc, cs_rc);

    if (R_SUCCEEDED(db_rc) && R_SUCCEEDED(cs_rc)) {
        // Probe base Application key.
        NcmContentMetaKey app_key{};
        app_key.id           = TARGET_APP_ID;
        app_key.version      = 0;
        app_key.type         = NcmContentMetaType_Application;
        app_key.install_type = NcmContentInstallType_Full;
        probe_key(db, cs, app_key);

        // Probe patch key.
        NcmContentMetaKey patch_key{};
        patch_key.id           = TARGET_PATCH_ID;
        patch_key.version      = 65536;
        patch_key.type         = NcmContentMetaType_Patch;
        patch_key.install_type = NcmContentInstallType_Full;
        probe_key(db, cs, patch_key);

        if (R_SUCCEEDED(db_rc)) ncmContentMetaDatabaseClose(&db);
        if (R_SUCCEEDED(cs_rc)) ncmContentStorageClose(&cs);

        // Force NCM service restart to rebuild internal index from disk.
        L("--- ncmExit + ncmInitialize (force index rebuild) ---");
        ncmExit();
        Result reinit_rc = ncmInitialize();
        L("ncmInitialize after restart: rc=0x%08X", reinit_rc);

        // Re-open and probe again to see if index rebuilt correctly.
        NcmContentMetaDatabase db2{};
        NcmContentStorage cs2{};
        Result db2_rc = ncmOpenContentMetaDatabase(&db2, STORAGE);
        Result cs2_rc = ncmOpenContentStorage(&cs2, STORAGE);
        L("Re-open after restart: db_rc=0x%08X cs_rc=0x%08X", db2_rc, cs2_rc);
        L("--- POST-RESTART probe ---");
        if (R_SUCCEEDED(db2_rc) && R_SUCCEEDED(cs2_rc)) {
            probe_key(db2, cs2, app_key);
            probe_key(db2, cs2, patch_key);
        }
        if (R_SUCCEEDED(db2_rc)) ncmContentMetaDatabaseClose(&db2);
        if (R_SUCCEEDED(cs2_rc)) ncmContentStorageClose(&cs2);

        // NS probing on the Application key.
        probe_ns(app_key);
    }

    nsExit();
    ncmExit();

    L("=== ns_probe end ===");
    if (g_log) fclose(g_log);

    printf("Done. Press + to exit.\n");
    consoleUpdate(nullptr);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    }
    consoleExit(nullptr);
    return 0;
}
