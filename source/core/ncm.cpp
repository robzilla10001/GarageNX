// source/core/ncm.cpp

#include "core/ncm.hpp"
#include <SDL2/SDL.h>
#include <cstring>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Core::Ncm {

// Signals that the installed-title set has changed since the list was last
// built (set after a delete/move), so a cached TitleList rebuilds on re-entry.
static bool s_titles_dirty = false;
void mark_titles_dirty()  { s_titles_dirty = true; }
bool titles_dirty()       { return s_titles_dirty; }
void clear_titles_dirty() { s_titles_dirty = false; }

#ifdef PLATFORM_SWITCH

static TitleType map_meta_type(u8 meta_type) {
    switch (meta_type) {
        case NcmContentMetaType_Application:  return TitleType::Application;
        case NcmContentMetaType_Patch:        return TitleType::Patch;
        case NcmContentMetaType_AddOnContent: return TitleType::AddOnContent;
        default:                              return TitleType::Other;
    }
}

// Enumerate one storage's meta database. Appends found titles to `out`.
static void list_storage(NcmStorageId storage_id, Storage tag,
                         std::vector<Title>& out) {
    NcmContentMetaDatabase db;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage_id))) {
        // Storage may be empty/unavailable (e.g. no SD titles) — not an error.
        return;
    }

    NcmContentStorage cs;
    bool have_cs = R_SUCCEEDED(ncmOpenContentStorage(&cs, storage_id));

    // List all content-meta keys. We ask for a generous window; most consoles
    // have well under a few thousand entries.
    constexpr s32 WINDOW = 256;
    std::vector<NcmContentMetaKey> keys(WINDOW);

    s32 total = 0, written = 0;
    // meta_type 0 = list ALL types (applications, patches, add-ons).
    Result rc = ncmContentMetaDatabaseList(&db, &total, &written,
                                           keys.data(), WINDOW,
                                           (NcmContentMetaType)0 /*all*/,
                                           0 /*application_id filter: 0 = any*/,
                                           0 /*min*/, UINT64_MAX /*max*/,
                                           NcmContentInstallType_Full);
    if (R_SUCCEEDED(rc)) {
        s32 count = (written < WINDOW) ? written : WINDOW;
        for (s32 i = 0; i < count; ++i) {
            const NcmContentMetaKey& k = keys[i];

            Title t;
            t.meta_id    = k.id;
            t.program_id = k.id;   // for applications these match; patches/DLC differ
            t.version    = k.version;
            t.type       = map_meta_type(k.type);
            t.storage    = tag;

            // Total content size (best-effort).
            if (have_cs) {
                u64 sz = 0;
                if (R_SUCCEEDED(ncmContentMetaDatabaseGetSize(&db, &sz, &k)))
                    t.size_bytes = sz;
            }

            out.push_back(std::move(t));
        }
    }

    if (have_cs) ncmContentStorageClose(&cs);
    ncmContentMetaDatabaseClose(&db);
}

#endif // PLATFORM_SWITCH

std::vector<Title> list_all(bool* ok) {
    std::vector<Title> out;
#ifdef PLATFORM_SWITCH
    list_storage(NcmStorageId_BuiltInUser, Storage::BuiltIn, out);
    list_storage(NcmStorageId_SdCard,      Storage::SdCard,  out);
    if (ok) *ok = true;
#else
    // PC stub: a couple of fake titles for UI development.
    Title a; a.program_id = 0x0100000000010000ULL; a.type = TitleType::Application;
    a.name = "Test Game"; a.name_resolved = true; a.size_bytes = 4ULL<<30;
    out.push_back(a);
    if (ok) *ok = true;
#endif
    return out;
}

// ─── Application grouping ───────────────────────────────────────────────────────
// Title ID relationships (worked example, BotW):
//   Base:   01007EF00011E000   (4th-to-last nibble even, low 12 bits = 0)
//   Update: 01007EF00011E800   (base | 0x800)
//   DLC:    01007EF00011F001..  (base + 0x1000, low 12 bits = DLC index)
// So to recover the base application id:
//   update → clear the 0x800 bit
//   DLC    → clear the low 12 bits and subtract the 0x1000 add-on flag


std::vector<TitleGroup> group_by_application(const std::vector<Title>& all) {
    // Only user games (exclude system titles below the game id range).
    static constexpr uint64_t USER_GAME_ID_BASE = 0x0100000000100000ULL;

    // First pass: create a group for every base application.
    std::vector<TitleGroup> groups;
    // Map base id → index into groups, so updates/DLC can attach quickly.
    std::vector<std::pair<uint64_t, size_t>> index;  // (base_id, group_idx)

    auto find_group = [&](uint64_t base_id) -> TitleGroup* {
        for (auto& pr : index)
            if (pr.first == base_id) return &groups[pr.second];
        return nullptr;
    };

    for (const auto& t : all) {
        if (t.type != TitleType::Application) continue;
        if (t.program_id < USER_GAME_ID_BASE) continue;  // system app
        TitleGroup g;
        g.app = t;
        index.push_back({ t.program_id, groups.size() });
        groups.push_back(std::move(g));
    }

    // Second pass: attach updates and DLC to their base application. If the base
    // app isn't installed (orphan update/DLC), we skip it here — orphans get
    // their own handling later if needed.
    for (const auto& t : all) {
        if (t.type == TitleType::Application) continue;
        uint64_t base = base_application_id(t.program_id, t.type);
        TitleGroup* g = find_group(base);
        if (!g) continue;  // orphan; base not installed
        if (t.type == TitleType::Patch)        g->updates.push_back(t);
        else if (t.type == TitleType::AddOnContent) g->dlc.push_back(t);
    }

    return groups;
}

Core::Nca::ControlData resolve_control(const Title& title,
                                       const Core::Keys::Keyset& keys,
                                       bool want_icon) {
#ifndef PLATFORM_SWITCH
    (void)title; (void)keys; (void)want_icon;
    return Core::Nca::ControlData{};
#else
    Core::Nca::ControlData result;

    NcmStorageId storage_id = (title.storage == Storage::SdCard)
        ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    NcmContentMetaDatabase db;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage_id))) return result;

    NcmContentStorage cs;
    if (R_FAILED(ncmOpenContentStorage(&cs, storage_id))) {
        ncmContentMetaDatabaseClose(&db);
        return result;
    }

    // Rebuild the meta key for this title to query its Control content id.
    NcmContentMetaKey key;
    std::memset(&key, 0, sizeof(key));
    key.id      = title.meta_id;
    key.version = title.version;
    key.type    = (title.type == TitleType::Application) ? NcmContentMetaType_Application
                : (title.type == TitleType::Patch)       ? NcmContentMetaType_Patch
                : (title.type == TitleType::AddOnContent)? NcmContentMetaType_AddOnContent
                                                         : (NcmContentMetaType)0;
    key.install_type = NcmContentInstallType_Full;

    // Find the Control content id for this meta.
    NcmContentId control_id;
    Result rc = ncmContentMetaDatabaseGetContentIdByType(
        &db, &control_id, &key, NcmContentType_Control);

    if (R_FAILED(rc)) {
        // Applications carry Control; patches/DLC may not. Bail cleanly.
        ncmContentStorageClose(&cs);
        ncmContentMetaDatabaseClose(&db);
        return result;
    }

    // Determine the NCA's size so the decryptor knows its bounds.
    s64 nca_size = 0;
    ncmContentStorageGetSizeFromContentId(&cs, &nca_size, &control_id);
    if (nca_size <= 0) nca_size = 32 * 1024 * 1024; // fallback cap

    // ReadFn over the Control NCA via ncm.
    Core::Nca::ReadFn reader =
        [&cs, &control_id](uint64_t offset, void* dst, size_t size) -> size_t {
            Result r = ncmContentStorageReadContentIdFile(
                &cs, dst, size, &control_id, (s64)offset);
            return R_SUCCEEDED(r) ? size : 0;
        };

    result = Core::Nca::read_control(reader, (uint64_t)nca_size, keys, want_icon);

    ncmContentStorageClose(&cs);
    ncmContentMetaDatabaseClose(&db);
    return result;
#endif
}

} // namespace Core::Ncm
