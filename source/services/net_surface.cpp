// source/services/net_surface.cpp
//
// The non-pure half of the shared Network surface: the config-backed connection
// listing, and the single-slot connect/mount POLICY. The header holds everything
// pure and host-tested; this file holds what reads the live config and what talks
// to the console.
//
// ── API PINNING (§5.4) ───────────────────────────────────────────────────────
// Every library call below was checked against the upstream headers/symbols, not
// written from memory:
//   * libnfs sync API — sahlberg/libnfs include/nfsc/libnfs.h:
//       nfs_init_context(), nfs_mount(nfs, server, export), nfs_umount(nfs),
//       nfs_set_autoreconnect(nfs, n), nfs_get_error(nfs), nfs_destroy_context(nfs).
//   * libsmb2 sync API — sahlberg/libsmb2 include/smb2/libsmb2.h + lib/libsmb2.syms:
//       smb2_init_context(), smb2_set_user/_domain/_password(), smb2_set_timeout(),
//       smb2_connect_share(smb2, server, share, user), smb2_disconnect_share(),
//       smb2_get_error(), smb2_destroy_context().
// The structure is checked by the syntax guard; TYPES are not (the guard stops
// before type-checking), so these still need a real compile on device. Nothing
// here is invented — where a call is optional or a constant is uncertain, it is
// omitted rather than guessed (the ueventWait() lesson from core/usb_mount.cpp).
//
// ── THE ONE REMAINING SEAM: the "net:" devoptab ──────────────────────────────
// connect + mount is implemented below and can be verified in isolation on device
// (it logs success/failure). What is NOT here is the devoptab that exposes the
// mounted context as "net:/..." so FileBrowserScreen and the transports can use
// ordinary file I/O. That wrapper (open/close/read/seek/fstat/stat/opendir/
// dirnext/closedir dispatching to nfs_*/smb2_*) is the piece best written where it
// compiles against the real headers, since a wrong devoptab field would pass the
// guard and fault on hardware. Until it lands, net_resolve() connects (proving
// reachability + credentials), logs, releases, and returns "" — so no caller opens
// a browser on a surface that cannot answer, and there is no leaked connection.

#include "services/net_surface.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#include <SDL2/SDL.h>
#endif
// The SMB/NFS client headers live in the devkitPro portlibs (switch-libnfs /
// switch-libsmb2). They are pulled in ONLY when the client is explicitly enabled
// (CMake: -DGARAGENX_NET_CLIENT=ON, which defines GNX_NET_CLIENT and links
// -lnfs -lsmb2). Gating the INCLUDES — not just the link flags — is what lets the
// tree compile before the portlibs are installed. (A previous cut commented the
// link flags but left these includes active, which broke the build with
// "nfsc/libnfs.h: No such file or directory".)
#if defined(PLATFORM_SWITCH) && defined(GNX_NET_CLIENT)
// libnfs.h references `struct timeval` (in struct nfsdirent) but only includes
// <sys/time.h> for a few platforms — NOT Switch, where it pulls <time.h>, which
// does not define timeval on newlib. Include it ourselves first so the type is
// complete before libnfs.h uses it.
#include <sys/time.h>
#include <nfsc/libnfs.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <sys/iosupport.h>   // devoptab_t, DIR_ITER, AddDevice/RemoveDevice
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <new>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#endif

namespace Services {

// Human-readable reason for the last net_resolve() that returned "" (see header).
// File scope so it is set/read regardless of PLATFORM_SWITCH / GNX_NET_CLIENT.
static std::string g_last_error;

std::string net_last_error() { return g_last_error; }

std::vector<std::string> net_connection_names() {
    std::vector<std::string> out;
    for (const auto& s : Config::get().network.shares)
        out.push_back(s.name);
    return out;
}

namespace {

const Config::NetShare* find_live(const std::string& name) {
    return net_find(Config::get().network.shares, name);
}

#if defined(PLATFORM_SWITCH) && defined(GNX_NET_CLIENT)
// Single-slot mounted context. At most one of these is non-null at a time; the
// single-slot rule releases the previous connection before opening another.
struct nfs_context*  g_nfs  = nullptr;
struct smb2_context* g_smb2 = nullptr;
std::string          g_mounted;   // connection name currently connected, "" if none

// Diagnostic trace to a file the user can pull off the SD card:
// sdmc:/switch/GarageNX/logs/net.log. Same pattern as save.log / nsp_stream.log.
// Defined early so the connect/reconnect helpers below can use it (SDL_Log is not
// visible without nxlink).
static void net_log(const char* fmt, ...) {
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir("sdmc:/switch/GarageNX", 0777);
    ::mkdir("sdmc:/switch/GarageNX/logs", 0777);
    FILE* f = ::fopen("sdmc:/switch/GarageNX/logs/net.log", "a");
    if (!f) return;
    ::fprintf(f, "[%lld] ", static_cast<long long>(::time(nullptr)));   // wall-clock s, for timing
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// Connect + mount an NFS export. The export path is the connection's `share`
// field (e.g. "/volume1/pub"). Auto-reconnect is disabled so an unreachable or
// dropped server fails fast instead of retrying forever and hanging the UI.
bool connect_nfs(const Config::NetShare& s) {
    g_nfs = nfs_init_context();
    if (!g_nfs) return false;
    nfs_set_autoreconnect(g_nfs, 0);
    if (nfs_mount(g_nfs, s.host.c_str(), s.share.c_str()) != 0) {
        g_last_error = std::string("NFS mount failed: ") + nfs_get_error(g_nfs);
        SDL_Log("net: nfs_mount %s:%s failed: %s",
                s.host.c_str(), s.share.c_str(), nfs_get_error(g_nfs));
        nfs_destroy_context(g_nfs);
        g_nfs = nullptr;
        return false;
    }
    return true;
}

// Connect + mount an SMB share. Credentials come from the in-memory session store
// (net_credentials()); a guest connection has an empty username and no password.
bool connect_smb(const Config::NetShare& s) {
    g_smb2 = smb2_init_context();
    if (!g_smb2) return false;

    if (!s.username.empty()) smb2_set_user(g_smb2, s.username.c_str());
    if (!s.domain.empty())   smb2_set_domain(g_smb2, s.domain.c_str());

    std::string pw;
    if (net_credentials().get(s.name, pw) && !pw.empty())
        smb2_set_password(g_smb2, pw.c_str());

    smb2_set_timeout(g_smb2, 10);   // seconds — fast-fail the CONNECT on a dead host

    const char* user = s.username.empty() ? nullptr : s.username.c_str();
    // An SMB share name is bare ("media") — never a path. Trim any leading/trailing
    // slashes a user might type ("/media", "media/"), which a server otherwise
    // rejects at Tree Connect as STATUS_BAD_NETWORK_NAME. (NFS export paths keep
    // their leading '/', so this trimming is SMB-only.)
    std::string share = s.share;
    while (!share.empty() && (share.front() == '/' || share.front() == '\\'))
        share.erase(share.begin());
    while (!share.empty() && (share.back() == '/' || share.back() == '\\'))
        share.pop_back();

    if (smb2_connect_share(g_smb2, s.host.c_str(), share.c_str(), user) != 0) {
        g_last_error = std::string("SMB connect failed: ") + smb2_get_error(g_smb2);
        SDL_Log("net: smb2_connect_share %s/%s failed: %s",
                s.host.c_str(), share.c_str(), smb2_get_error(g_smb2));
        smb2_destroy_context(g_smb2);
        g_smb2 = nullptr;
        return false;
    }
    // Connected: relax the timeout for the session. 10 s is too aggressive for large
    // transfers, but with reconnect-and-retry handling recovery we don't need the
    // very long 300 s either — 60 s detects a genuinely stalled read in about a
    // minute, then the read path reconnects and retries rather than freezing.
    smb2_set_timeout(g_smb2, 60);
    return true;
}

// Rebuild g_smb2 for the currently-mounted connection after the socket dies or the
// server stops responding mid-transfer. Open file handles must be reopened
// afterward (they belonged to the dead context). Credentials come from the session
// store, same as the first connect.
//
// The server is often briefly unresponsive right when it drops us, so a single
// immediate reconnect fails — we retry a few times with a pause to let the server
// and the network recover.
static bool smb_reconnect() {
    const Config::NetShare* s = find_live(g_mounted);
    if (!s) return false;
    for (int i = 0; i < 5; ++i) {
        if (g_smb2) { smb2_destroy_context(g_smb2); g_smb2 = nullptr; }
        if (i > 0) svcSleepThread(2'000'000'000ULL);   // 2 s between attempts
        if (connect_smb(*s)) { net_log("  smb_reconnect OK (attempt %d)", i); return true; }
        net_log("  smb_reconnect attempt %d failed: %s", i,
                g_smb2 ? smb2_get_error(g_smb2) : "no ctx");
    }
    return false;
}

// ── devoptab: expose the mounted share as "net:" ──────────────────────────────
//
// FileBrowserScreen and the dump/install paths use ordinary POSIX file I/O over a
// devoptab root (like "sdmc:", "ums0:"), so a single "net:" device backed by the
// active g_nfs/g_smb2 context lets a network share reuse the entire file manager.
// Only ONE context is live at a time (single-slot), so each callback dispatches on
// whichever of g_nfs/g_smb2 is set. Read path only for now (browse + install-from);
// writes are intentionally absent (NULL in the table) until they're needed.
//
// PINNED against the real headers (§5.4): devoptab_t layout from devkitPro newlib
// sys/iosupport.h; libnfs/libsmb2 calls from sahlberg's libnfs.h / libsmb2.h. Two
// things to CONFIRM on the first device compile, since the guard can't type-check:
//   • the file-op fd parameter is `void *fd` in devkitPro's newlib (64-bit); if
//     your installed iosupport.h uses `int fd`, adjust the five callback sigs.
//   • RemoveDevice() is called with "net:" below — match it to how AddDevice
//     registered the name if your newlib is picky.
// The table is filled field-by-field on a zero-initialised static (not a designated
// initialiser), so it is robust to newlib adding devoptab fields and needs no
// specific C++ standard.

static constexpr uint32_t kReadAhead = 1u << 20;   // 1 MiB default network read

struct NetFile {
    struct nfsfh*  nfs  = nullptr;
    struct smb2fh* smb  = nullptr;
    uint64_t       pos  = 0;   // logical position; reads are pread-based (explicit offset)
    uint64_t       size = 0;   // captured at open, for SEEK_END
    std::string    rel;        // share-relative path, for reopening after a reconnect
    int            flags = 0;  // open flags, for the same reason
    // Read-ahead buffer: collapses many small reads into one network request, which
    // is what makes small-read phases (e.g. NSZ block headers) usable over the
    // network instead of one round-trip per KB.
    std::vector<uint8_t> rbuf;
    uint64_t             rbuf_off = 0;   // file offset of rbuf[0]
    size_t               rbuf_len = 0;   // valid bytes in rbuf (0 = empty)
    uint32_t             ra_size  = kReadAhead;  // per-file read-ahead size (SMB: negotiated max)
};


struct NetDirState {
    struct nfsdir*  nfs = nullptr;
    struct smb2dir* smb = nullptr;
};

// "net:/Movies/x" -> "/Movies/x" (path relative to the mounted export/share).
static const char* dev_relpath(const char* path) {
    const char* colon = std::strchr(path, ':');
    return colon ? colon + 1 : path;
}

// libsmb2 wants a share-relative path with NO leading slash ("" = the share root),
// and converts internal '/' to '\' itself. A leading slash makes it a literal
// "\..." name the server rejects — which shows up as an empty (blank) listing.
static std::string smb_relpath(const char* path) {
    const char* rel = dev_relpath(path);   // -> "/Movies/x", or "/" at the root
    while (*rel == '/') ++rel;              // -> "Movies/x", or "" at the root
    return std::string(rel);
}

static void fill_stat(struct stat* st, bool is_dir, uint64_t size, uint64_t mtime) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode  = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    st->st_size  = static_cast<off_t>(size);
    st->st_nlink = 1;
    st->st_mtime = static_cast<time_t>(mtime);
}

static int net_dev_open(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
    (void)mode;
    NetFile* f = new (fileStruct) NetFile();
    if (g_nfs) {
        if (nfs_open(g_nfs, dev_relpath(path), flags, &f->nfs) != 0 || !f->nfs) { r->_errno = EIO; return -1; }
        struct nfs_stat_64 s{};
        if (nfs_fstat64(g_nfs, f->nfs, &s) == 0) f->size = s.nfs_size;
        net_log("open NFS '%s' size=%llu", path, (unsigned long long)f->size);
        return 0;
    }
    if (g_smb2) {
        f->rel   = smb_relpath(path);
        f->flags = flags;
        f->smb   = smb2_open(g_smb2, f->rel.c_str(), flags);
        if (!f->smb) { r->_errno = EIO; return -1; }
        struct smb2_stat_64 s{};
        if (smb2_fstat(g_smb2, f->smb, &s) == 0) f->size = s.smb2_size;
        // Read in the server's negotiated max per request (SMB3 NAS commonly allow
        // ~8 MiB) instead of 1 MiB: with reads serialized one at a time, fewer,
        // larger requests hide per-read round-trip latency, which is what caps
        // throughput on this path. Cap the buffer at 8 MiB.
        uint32_t m = smb2_get_max_read_size(g_smb2);
        if (m < kReadAhead)      m = kReadAhead;
        if (m > (8u << 20))      m = 8u << 20;
        f->ra_size = m;
        net_log("open SMB '%s' size=%llu max_read=%u ra=%u", path,
                (unsigned long long)f->size, smb2_get_max_read_size(g_smb2), f->ra_size);
        return 0;
    }
    r->_errno = ENODEV; return -1;
}

static int net_dev_close(struct _reent* r, void* fd) {
    NetFile* f = static_cast<NetFile*>(fd);
    int rc = 0;
    if (f->nfs) rc = nfs_close(g_nfs, f->nfs);
    if (f->smb) rc = smb2_close(g_smb2, f->smb);
    f->~NetFile();
    if (rc != 0) { r->_errno = EIO; return -1; }
    return 0;
}

// One network read (no buffering) at an explicit offset, with SMB reconnect-and-
// retry so a mid-transfer connection drop doesn't kill the install. Returns bytes
// read (>=0) or -1.
static int raw_pread(NetFile* f, uint8_t* dst, uint32_t count, uint64_t offset, struct _reent* r) {
    if (f->nfs) {
        // libnfs order is (nfs, fh, offset, count, buf) — NOT (buf, count, offset).
        int n = nfs_pread(g_nfs, f->nfs, offset, count, dst);
        if (n < 0) { net_log("read NFS ERR pos=%llu len=%u: %s",
                             (unsigned long long)offset, count, nfs_get_error(g_nfs)); r->_errno = EIO; return -1; }
        return n;
    }
    if (f->smb) {
        for (int attempt = 0; ; ++attempt) {
            int n = smb2_pread(g_smb2, f->smb, dst, count, offset);
            if (n >= 0) return n;
            // Connection likely dropped mid-transfer. Reconnect, reopen at the same
            // offset (pread is offset-explicit), and retry a bounded number of times.
            net_log("read SMB ERR pos=%llu len=%u attempt=%d: %s",
                    (unsigned long long)offset, count, attempt, smb2_get_error(g_smb2));
            if (attempt >= 4) { r->_errno = EIO; return -1; }
            if (!smb_reconnect()) {
                net_log("  reconnect FAILED: %s", g_smb2 ? smb2_get_error(g_smb2) : "no ctx");
                r->_errno = EIO; return -1;
            }
            f->smb = smb2_open(g_smb2, f->rel.c_str(), f->flags);
            if (!f->smb) {
                net_log("  reopen '%s' FAILED: %s", f->rel.c_str(), smb2_get_error(g_smb2));
                r->_errno = EIO; return -1;
            }
            net_log("  reconnected + reopened '%s', retrying at pos=%llu",
                    f->rel.c_str(), (unsigned long long)offset);
        }
    }
    r->_errno = EBADF; return -1;
}

static ssize_t net_dev_read(struct _reent* r, void* fd, char* ptr, size_t len) {
    NetFile* f = static_cast<NetFile*>(fd);
    if (len == 0) return 0;
    const uint64_t before = f->pos;
    size_t total = 0;
    while (len > 0) {
        // Serve from the read-ahead buffer whenever the position falls inside it.
        // This is what turns a phase of KB-sized reads (one network round-trip each
        // over SMB — the "hours after transfer" symptom) into one read per MiB.
        if (f->rbuf_len && f->pos >= f->rbuf_off && f->pos < f->rbuf_off + f->rbuf_len) {
            const size_t off_in = static_cast<size_t>(f->pos - f->rbuf_off);
            size_t chunk = f->rbuf_len - off_in;
            if (chunk > len) chunk = len;
            std::memcpy(reinterpret_cast<uint8_t*>(ptr) + total, f->rbuf.data() + off_in, chunk);
            f->pos += chunk; total += chunk; len -= chunk;
            continue;
        }
        // Miss: pull a full read-ahead chunk at the current position.
        if (f->rbuf.size() < f->ra_size) f->rbuf.resize(f->ra_size);
        int n = raw_pread(f, f->rbuf.data(), f->ra_size, f->pos, r);
        if (n < 0)  return total > 0 ? static_cast<ssize_t>(total) : -1;
        if (n == 0) { if (total == 0) net_log("read EOF/0 at pos=%llu", (unsigned long long)f->pos); break; }
        f->rbuf_off = f->pos;
        f->rbuf_len = static_cast<size_t>(n);
    }
    if ((before >> 28) != (f->pos >> 28)) net_log("read pos=%llu", (unsigned long long)f->pos);
    return static_cast<ssize_t>(total);
}

static off_t net_dev_seek(struct _reent* r, void* fd, off_t pos, int dir) {
    NetFile* f = static_cast<NetFile*>(fd);
    int64_t base;
    if      (dir == SEEK_SET) base = 0;
    else if (dir == SEEK_CUR) base = static_cast<int64_t>(f->pos);
    else if (dir == SEEK_END) base = static_cast<int64_t>(f->size);
    else { r->_errno = EINVAL; return -1; }
    const int64_t np = base + static_cast<int64_t>(pos);
    if (np < 0) { r->_errno = EINVAL; return -1; }
    f->pos = static_cast<uint64_t>(np);
    return static_cast<off_t>(f->pos);
}

static int net_dev_fstat(struct _reent* r, void* fd, struct stat* st) {
    NetFile* f = static_cast<NetFile*>(fd);
    if (f->nfs) {
        struct nfs_stat_64 s{};
        if (nfs_fstat64(g_nfs, f->nfs, &s) != 0) { r->_errno = EIO; return -1; }
        fill_stat(st, S_ISDIR(s.nfs_mode), s.nfs_size, s.nfs_mtime);
        return 0;
    }
    if (f->smb) {
        struct smb2_stat_64 s{};
        if (smb2_fstat(g_smb2, f->smb, &s) != 0) { r->_errno = EIO; return -1; }
        fill_stat(st, s.smb2_type == SMB2_TYPE_DIRECTORY, s.smb2_size, s.smb2_mtime);
        return 0;
    }
    r->_errno = EBADF; return -1;
}

static int net_dev_stat(struct _reent* r, const char* path, struct stat* st) {
    if (g_nfs) {
        struct nfs_stat_64 s{};
        int rc = nfs_stat64(g_nfs, dev_relpath(path), &s);
        net_log("stat NFS path='%s' rc=%d", path, rc);
        if (rc != 0) { r->_errno = ENOENT; return -1; }
        fill_stat(st, S_ISDIR(s.nfs_mode), s.nfs_size, s.nfs_mtime);
        return 0;
    }
    if (g_smb2) {
        std::string rel = smb_relpath(path);
        struct smb2_stat_64 s{};
        int rc = smb2_stat(g_smb2, rel.c_str(), &s);
        net_log("stat SMB path='%s' rel='%s' rc=%d type=%u", path, rel.c_str(), rc,
                rc == 0 ? (unsigned)s.smb2_type : 0u);
        if (rc != 0) { r->_errno = ENOENT; return -1; }
        fill_stat(st, s.smb2_type == SMB2_TYPE_DIRECTORY, s.smb2_size, s.smb2_mtime);
        return 0;
    }
    r->_errno = ENODEV; return -1;
}

static DIR_ITER* net_dev_diropen(struct _reent* r, DIR_ITER* dirState, const char* path) {
    NetDirState* d = new (dirState->dirStruct) NetDirState();
    if (g_nfs) {
        const char* rel = dev_relpath(path);
        int rc = nfs_opendir(g_nfs, rel, &d->nfs);
        net_log("diropen NFS path='%s' rel='%s' rc=%d dir=%p", path, rel, rc, (void*)d->nfs);
        if (rc != 0 || !d->nfs) { net_log("  nfs_opendir FAILED: %s", nfs_get_error(g_nfs)); r->_errno = ENOENT; return nullptr; }
        return dirState;
    }
    if (g_smb2) {
        std::string rel = smb_relpath(path);
        d->smb = smb2_opendir(g_smb2, rel.c_str());
        net_log("diropen SMB path='%s' rel='%s' dir=%p", path, rel.c_str(), (void*)d->smb);
        if (!d->smb) { net_log("  smb2_opendir FAILED: %s", smb2_get_error(g_smb2)); r->_errno = ENOENT; return nullptr; }
        return dirState;
    }
    net_log("diropen path='%s' but no context mounted", path);
    r->_errno = ENODEV; return nullptr;
}

static int net_dev_dirnext(struct _reent* r, DIR_ITER* dirState, char* filename, struct stat* filestat) {
    NetDirState* d = static_cast<NetDirState*>(dirState->dirStruct);
    if (d->nfs) {
        struct nfsdirent* e;
        while ((e = nfs_readdir(g_nfs, d->nfs)) != nullptr) {
            if (std::strcmp(e->name, ".") == 0 || std::strcmp(e->name, "..") == 0) continue;
            std::strncpy(filename, e->name, NAME_MAX); filename[NAME_MAX] = '\0';
            // NF3DIR == 2 in the NFSv3 ftype3 enum (libnfs-raw-nfs.h). Avoid pulling
            // that header in just for the constant.
            fill_stat(filestat, e->type == 2u, e->size, static_cast<uint64_t>(e->mtime.tv_sec));
            net_log("  dirent NFS '%s' type=%u", e->name, e->type);
            return 0;
        }
        net_log("dirnext NFS EOD");
        r->_errno = ENOENT; return -1;   // end of stream
    }
    if (d->smb) {
        struct smb2dirent* e;
        while ((e = smb2_readdir(g_smb2, d->smb)) != nullptr) {
            if (std::strcmp(e->name, ".") == 0 || std::strcmp(e->name, "..") == 0) continue;
            std::strncpy(filename, e->name, NAME_MAX); filename[NAME_MAX] = '\0';
            fill_stat(filestat, e->st.smb2_type == SMB2_TYPE_DIRECTORY, e->st.smb2_size, e->st.smb2_mtime);
            net_log("  dirent SMB '%s' type=%u", e->name, (unsigned)e->st.smb2_type);
            return 0;
        }
        net_log("dirnext SMB EOD");
        r->_errno = ENOENT; return -1;
    }
    r->_errno = ENODEV; return -1;
}

static int net_dev_dirclose(struct _reent* r, DIR_ITER* dirState) {
    (void)r;
    NetDirState* d = static_cast<NetDirState*>(dirState->dirStruct);
    if (d->nfs) nfs_closedir(g_nfs, d->nfs);
    if (d->smb) smb2_closedir(g_smb2, d->smb);
    d->~NetDirState();
    return 0;
}

static int net_dev_dirreset(struct _reent* r, DIR_ITER* dirState) {
    // A just-opened dir is already at the start, so reset is a no-op success. We
    // return 0 (not ENOSYS) because newlib's opendir()/rewinddir() can treat a
    // failing dirreset as a failed open — which libusbhsfs avoids by returning 0.
    (void)r; (void)dirState;
    return 0;
}

static devoptab_t g_net_devoptab;    // zero-initialised; filled once in register_net_device()
static bool       g_net_dev_added = false;

static void register_net_device() {
    if (g_net_dev_added) return;
    // Field-by-field on the zero-initialised static: every unset op (write_r,
    // unlink_r, mkdir_r, …) stays NULL, and this is immune to newlib reordering or
    // adding fields, unlike a positional/designated initialiser.
    g_net_devoptab.name         = "net";
    g_net_devoptab.structSize   = sizeof(NetFile);
    g_net_devoptab.open_r       = net_dev_open;
    g_net_devoptab.close_r      = net_dev_close;
    g_net_devoptab.read_r       = net_dev_read;
    g_net_devoptab.seek_r       = net_dev_seek;
    g_net_devoptab.fstat_r      = net_dev_fstat;
    g_net_devoptab.stat_r       = net_dev_stat;
    g_net_devoptab.dirStateSize = sizeof(NetDirState);
    g_net_devoptab.diropen_r    = net_dev_diropen;
    g_net_devoptab.dirreset_r   = net_dev_dirreset;
    g_net_devoptab.dirnext_r    = net_dev_dirnext;
    g_net_devoptab.dirclose_r   = net_dev_dirclose;
    int rc = AddDevice(&g_net_devoptab);
    net_log("register net: AddDevice rc=%d", rc);
    if (rc >= 0) g_net_dev_added = true;
    else SDL_Log("net: AddDevice(\"net\") failed");
}

static void unregister_net_device() {
    if (!g_net_dev_added) return;
    RemoveDevice("net:");
    g_net_dev_added = false;
}
#endif // PLATFORM_SWITCH && GNX_NET_CLIENT

} // namespace

std::string net_resolve(const NetPath& np) {
    g_last_error.clear();

    // The chooser level mounts nothing, and by the single-slot rule anything
    // shallower than a Files path releases whatever was mounted before.
    if (np.level != NetPath::Level::Files) {
        net_surface_release();
        return {};
    }

    const Config::NetShare* s = find_live(np.connection);
    if (!s) {                        // unknown connection — refuse, nothing mounted
        g_last_error = "Unknown connection.";
        net_surface_release();
        return {};
    }

    // An SMB connection that names a user needs a password; without one held for
    // this session we cannot connect. Refuse rather than attempt a bind that would
    // fail confusingly on the server side.
    if (net_needs_password(*s)) {
        std::string pw;
        if (!net_credentials().get(s->name, pw)) {
            g_last_error = "A password is required for this connection.";
            net_surface_release();
            return {};
        }
    }

#if defined(PLATFORM_SWITCH) && defined(GNX_NET_CLIENT)
    // Always (re)connect on an explicit selection: release whatever was mounted and
    // connect fresh. net_resolve is called only when the user picks a connection
    // from the chooser (browsing uses the devoptab's live context, never this), so
    // this never reconnects mid-browse — but it DOES ensure an edited connection
    // (changed host/share/path, same name) actually takes effect instead of reusing
    // the previous mount.
    net_surface_release();
    net_log("resolve '%s' -> connect host=%s share=%s path=%s rest=%s",
            s->name.c_str(), s->host.c_str(), s->share.c_str(),
            s->path.c_str(), np.rest.c_str());
    {
        const NetProtocol proto = net_protocol_parse(s->protocol);
        bool ok = false;
        if (proto == NetProtocol::Nfs)      ok = connect_nfs(*s);
        else if (proto == NetProtocol::Smb) ok = connect_smb(*s);
        else g_last_error = "Unknown protocol '" + s->protocol + "'.";
        if (!ok) { g_mounted.clear(); return {}; }   // g_last_error set by connect_*
        register_net_device();                        // expose the share as "net:"
        g_mounted = s->name;
    }

    // The share is mounted and "net:" is registered: hand back the concrete path so
    // FileBrowserScreen (and, later, the transports) can use ordinary file I/O.
    SDL_Log("net: '%s' (%s) mounted at net:/", s->name.c_str(), s->protocol.c_str());
    return std::string("net:/") + np.rest;
#elif defined(PLATFORM_SWITCH)
    // Client not compiled in (GARAGENX_NET_CLIENT is OFF). The chooser still runs;
    // selecting a connection reports failure rather than pretending to mount.
    g_last_error = "The SMB/NFS client is not built into this build "
                   "(rebuild with -DGARAGENX_NET_CLIENT=ON).";
    SDL_Log("net: SMB/NFS client not built (enable GARAGENX_NET_CLIENT); "
            "cannot open '%s'", s->name.c_str());
    return {};
#else
    g_last_error = "Network browsing is not available on this platform.";
    return {};
#endif
}

std::string net_resolve_synth(const std::string& synth) {
    return net_resolve(net_split(net_synth_rel(synth)));
}

void net_surface_release() {
#if defined(PLATFORM_SWITCH) && defined(GNX_NET_CLIENT)
    unregister_net_device();   // remove "net:" before the backing context goes away
    if (g_nfs) {
        nfs_umount(g_nfs);
        nfs_destroy_context(g_nfs);
        g_nfs = nullptr;
    }
    if (g_smb2) {
        smb2_disconnect_share(g_smb2);
        smb2_destroy_context(g_smb2);
        g_smb2 = nullptr;
    }
    g_mounted.clear();
#endif
    // Session credentials are intentionally NOT cleared here: releasing the mount
    // to browse a different connection must not force re-entry of a password the
    // user just typed. Wholesale clearing happens at app teardown via
    // net_credentials().clear_all().
}

} // namespace Services
