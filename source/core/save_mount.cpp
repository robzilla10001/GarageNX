// source/core/save_mount.cpp

#include "core/save_mount.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>

namespace {

// The single mount slot.
bool     g_mounted    = false;
uint64_t g_mnt_uid_lo = 0;
uint64_t g_mnt_uid_hi = 0;
uint64_t g_mnt_app    = 0;

AccountUid to_uid(uint64_t lo, uint64_t hi) {
    AccountUid u{};
    u.uid[0] = lo;
    u.uid[1] = hi;
    return u;
}

// Strip characters that cannot appear in a filename — a nickname becomes a
// directory name on the client's disk.
std::string sanitize(const std::string& raw) {
    std::string out;
    for (unsigned char c : raw) {
        const bool bad = c < 0x20 || c == 0x7F ||
                         c == '/' || c == '\\' || c == ':' || c == '*' ||
                         c == '?' || c == '"'  || c == '<' || c == '>' || c == '|';
        out.push_back(bad ? '_' : (char)c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "User";
    return out;
}

} // namespace
#endif

namespace Core {
namespace SaveMount {

std::vector<User> list_users(bool* ok) {
    std::vector<User> out;
    if (ok) *ok = false;
#ifdef PLATFORM_SWITCH
    AccountUid uids[8];
    s32 count = 0;
    if (R_FAILED(accountListAllUsers(uids, (s32)(sizeof(uids)/sizeof(uids[0])), &count)))
        return out;
    if (ok) *ok = true;

    for (s32 i = 0; i < count; ++i) {
        User u;
        u.uid_lo = uids[i].uid[0];
        u.uid_hi = uids[i].uid[1];

        // A profile we cannot read still gets an entry, named by id — otherwise a
        // user's saves would be silently unreachable.
        AccountProfile      prof;
        AccountProfileBase  base;
        if (R_SUCCEEDED(accountGetProfile(&prof, uids[i]))) {
            if (R_SUCCEEDED(accountProfileGet(&prof, nullptr, &base))) {
                char nick[0x21] = {0};
                std::memcpy(nick, base.nickname, sizeof(base.nickname) < 0x20
                                                 ? sizeof(base.nickname) : 0x20);
                u.name = sanitize(nick);
            }
            accountProfileClose(&prof);
        }
        if (u.name.empty()) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "User %016llX",
                          (unsigned long long)u.uid_hi);
            u.name = buf;
        }
        out.push_back(std::move(u));
    }
#endif
    return out;
}

std::vector<SaveEntry> list_saves(const User& u) {
    std::vector<SaveEntry> out;
#ifdef PLATFORM_SWITCH
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User)))
        return out;

    for (;;) {
        FsSaveDataInfo infos[16];
        s64 read_count = 0;
        if (R_FAILED(fsSaveDataInfoReaderRead(
                &reader, infos, sizeof(infos)/sizeof(infos[0]), &read_count)))
            break;
        if (read_count <= 0) break;

        for (s64 i = 0; i < read_count; ++i) {
            // Account saves only: system saves have no application and are not
            // something a user should be backing up through this surface.
            if (infos[i].save_data_type != FsSaveDataType_Account) continue;
            if (infos[i].uid.uid[0] != u.uid_lo || infos[i].uid.uid[1] != u.uid_hi)
                continue;

            SaveEntry e;
            e.application_id = infos[i].application_id;
            e.save_data_id   = infos[i].save_data_id;   // handle used for deletion
            e.size_bytes     = 0;   // not reported here; the listing shows a folder
            out.push_back(e);
        }
    }
    fsSaveDataInfoReaderClose(&reader);
#endif
    return out;
}

std::vector<OrphanCandidateSave> list_all_account_saves() {
    std::vector<OrphanCandidateSave> out;
#ifdef PLATFORM_SWITCH
    // Same reader loop as list_saves() above — same open call, same read call,
    // same FsSaveDataType_Account filter — just no per-user uid filter, and uid
    // is kept in the result instead of being discarded.
    FsSaveDataInfoReader reader;
    if (R_FAILED(fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User)))
        return out;

    for (;;) {
        FsSaveDataInfo infos[16];
        s64 read_count = 0;
        if (R_FAILED(fsSaveDataInfoReaderRead(
                &reader, infos, sizeof(infos)/sizeof(infos[0]), &read_count)))
            break;
        if (read_count <= 0) break;

        for (s64 i = 0; i < read_count; ++i) {
            if (infos[i].save_data_type != FsSaveDataType_Account) continue;
            OrphanCandidateSave e;
            e.uid_lo         = infos[i].uid.uid[0];
            e.uid_hi         = infos[i].uid.uid[1];
            e.application_id = infos[i].application_id;
            e.save_data_id   = infos[i].save_data_id;
            out.push_back(e);
        }
    }
    fsSaveDataInfoReaderClose(&reader);
#endif
    return out;
}

bool is_mounted(const User& u, uint64_t application_id) {
#ifdef PLATFORM_SWITCH
    return g_mounted && g_mnt_uid_lo == u.uid_lo && g_mnt_uid_hi == u.uid_hi &&
           g_mnt_app == application_id;
#else
    (void)u; (void)application_id;
    return false;
#endif
}

bool commit() {
#ifdef PLATFORM_SWITCH
    if (!g_mounted) return false;
    const Result rc = fsdevCommitDevice("save");
    if (R_FAILED(rc)) {
        FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/save.log", "a");
        if (f) {
            ::fprintf(f, "  fsdevCommitDevice(save) app=%016llX rc=0x%08X\n",
                      (unsigned long long)g_mnt_app, rc);
            ::fclose(f);
        }
        SDL_Log("save_mount: fsdevCommitDevice rc=0x%08X", rc);
        return false;
    }
    return true;
#else
    return false;
#endif
}

void release() {
#ifdef PLATFORM_SWITCH
    if (!g_mounted) return;
    fsdevUnmountDevice("save");
    g_mounted    = false;
    g_mnt_uid_lo = g_mnt_uid_hi = g_mnt_app = 0;
#endif
}

bool ensure_save_exists(const User& u, uint64_t application_id) {
#ifdef PLATFORM_SWITCH
    // If it already mounts, it already exists — cheapest possible check, and it
    // avoids asking the OS to create something that is there (some firmwares
    // return an error for that rather than succeeding).
    if (fsdevMountSaveData("save_probe", application_id, to_uid(u.uid_lo, u.uid_hi))
            == 0) {
        fsdevUnmountDevice("save_probe");
        return true;
    }

    // ── THE UNVERIFIED CALL (5.4) ────────────────────────────────────────────
    // Creating an account save. As with delete, there is no libnx header in the
    // sandbox to check the exact name/signature, so this is taken on knowledge.
    // The switchbrew-documented shape is fsCreateSaveDataFileSystem with an
    // FsSaveDataAttribute (application_id + uid + FsSaveDataType_Account) and an
    // FsSaveDataCreationInfo (size + journal_size + owner). If the Switch build
    // cannot find these, the things to check in order are: the struct field names
    // (application_id vs program_id; save_data_size vs size), and on older libnx
    // fsCreateSaveDataFileSystemBySystemSaveDataId is NOT the one — that is for
    // system saves. Sizes below are conservative; the OS enforces per-title.
    FsSaveDataAttribute attr = {};
    attr.application_id = application_id;
    attr.uid           = to_uid(u.uid_lo, u.uid_hi);
    attr.save_data_type = FsSaveDataType_Account;

    FsSaveDataCreationInfo ci = {};
    ci.save_data_size = 0x100000;    // 1 MiB; the OS rounds up to the title's need
    ci.journal_size   = 0x100000;
    ci.owner_id       = application_id;
    ci.flags          = 0;
    ci.save_data_space_id = FsSaveDataSpaceId_User;

    FsSaveDataMetaInfo meta = {};    // account saves need no meta

    const Result rc = fsCreateSaveDataFileSystem(&attr, &ci, &meta);
    if (R_FAILED(rc)) {
        FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/save.log", "a");
        if (f) {
            ::fprintf(f, "  ensure_save_exists app=%016llX rc=0x%08X\\n",
                      (unsigned long long)application_id, rc);
            ::fclose(f);
        }
        SDL_Log("save_mount: ensure_save_exists(%016llX) rc=0x%08X",
                (unsigned long long)application_id, rc);
        return false;
    }
    return true;
#else
    (void)u; (void)application_id;
    return false;
#endif
}

bool delete_save_record(uint64_t save_data_id) {
#ifdef PLATFORM_SWITCH
    // PRECONDITION: nothing may be mounted from this record. Deleting a mounted
    // filesystem is undefined; the caller releases first, and we release here too
    // as a belt-and-braces guard rather than trust every caller forever.
    release();

    // fsDeleteSaveDataFileSystemBySaveDataSpaceId — confirmed against a real
    // libnx fs.h (line 513: `Result fsDeleteSaveDataFileSystemBySaveDataSpace
    // Id(FsSaveDataSpaceId save_data_space_id, u64 saveID); ///< [2.0.0+]`,
    // exact match to the call below) and separately hardware-verified via
    // Save Manager's own working Delete button. Originally written "on
    // knowledge" without a header to check against — this replaces that
    // history rather than erasing it: it WAS unverified, it no longer is.
    const Result rc = fsDeleteSaveDataFileSystemBySaveDataSpaceId(
        FsSaveDataSpaceId_User, save_data_id);
    if (R_FAILED(rc)) {
        FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/save.log", "a");
        if (f) {
            ::fprintf(f, "  delete_save_record id=%016llX rc=0x%08X\n",
                      (unsigned long long)save_data_id, rc);
            ::fclose(f);
        }
        SDL_Log("save_mount: delete_save_record(%016llX) rc=0x%08X",
                (unsigned long long)save_data_id, rc);
        return false;
    }
    return true;
#else
    (void)save_data_id;
    return false;
#endif
}

bool ensure_mounted(const User& u, uint64_t application_id) {
#ifdef PLATFORM_SWITCH
    if (is_mounted(u, application_id)) return true;   // already the right one

    release();   // single slot: whatever else was mounted goes first

    const AccountUid uid = to_uid(u.uid_lo, u.uid_hi);
    Result rc = fsdevMountSaveData("save", application_id, uid);
    if (R_FAILED(rc)) {
        // Log the Result: it distinguishes "no such save for this user" from a
        // permissions/handle problem, which need different fixes.
        FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/save.log", "a");
        if (f) {
            ::fprintf(f, "  fsdevMountSaveData app=%016llX uid=%016llX%016llX rc=0x%08X\n",
                      (unsigned long long)application_id,
                      (unsigned long long)u.uid_hi, (unsigned long long)u.uid_lo, rc);
            ::fclose(f);
        }
        SDL_Log("save_mount: fsdevMountSaveData(%016llX) rc=0x%08X",
                (unsigned long long)application_id, rc);
        return false;
    }
    g_mounted    = true;
    g_mnt_uid_lo = u.uid_lo;
    g_mnt_uid_hi = u.uid_hi;
    g_mnt_app    = application_id;
    return true;
#else
    (void)u; (void)application_id;
    return false;
#endif
}

} // namespace SaveMount
} // namespace Core
