// source/services/ftp_server.cpp

#include "services/ftp_server.hpp"
#include "services/ftp_paths.hpp"
#include "services/storage_paths.hpp"
#include "services/storage_catalog.hpp"
#include "services/write_guard.hpp"
#include "services/title_surface.hpp"
#include "install/stream_driver.hpp"
#include "core/keys.hpp"
#include "config/config.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace Services {

// ─── Path helpers ─────────────────────────────────────────────────────────────

// Resolve `arg` (absolute or relative) against `cwd`, collapsing "." / ".." and
// clamping at root ("/"). Returns a clean POSIX path beginning with '/'.
static std::string resolve_posix(const std::string& cwd, const std::string& arg) {
    std::string start = (!arg.empty() && arg[0] == '/') ? arg : (cwd + "/" + arg);
    std::vector<std::string> parts;
    std::string cur;
    std::stringstream ss(start);
    while (std::getline(ss, cur, '/')) {
        if (cur.empty() || cur == ".") continue;
        if (cur == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(cur);
    }
    std::string out = "/";
    for (size_t i = 0; i < parts.size(); ++i) { out += parts[i]; if (i + 1 < parts.size()) out += "/"; }
    return out;
}

std::string FtpServer::to_vfs(const std::string& cwd, const std::string& arg) const {
    const std::string posix = resolve_posix(cwd, arg);
    // Resolve against the shared catalog: any ENABLED Filesystem surface (SD Card,
    // Album, and later NAND/gamecard) maps to its concrete VFS path. Root, the
    // install folders, title-query surfaces, and bare/disabled paths return "" so
    // filesystem commands reject them — this is what keeps non-SD storages from
    // leaking into the root listing and keeps disabled storages unreachable.
    const auto r = Services::sp_resolve(posix, Config::get().mtp);
    if (r.kind == Services::PathKind::Filesystem ||
        r.kind == Services::PathKind::StorageRoot) {
        return r.vfs;   // already "<root>/rest" or "<root>/"
    }
    return "";
}

// ─── Small socket helpers ─────────────────────────────────────────────────────

static bool send_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n <= 0) { if (errno == EINTR) continue; return false; }
        off += (size_t)n;
    }
    return true;
}
static void reply(int fd, const std::string& line) { send_all(fd, line.data(), line.size()); }
static void replyf(int fd, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) send_all(fd, buf, (size_t)std::min<int>(n, (int)sizeof(buf) - 1));
}

// ─── Per-client control state ─────────────────────────────────────────────────

namespace {
struct Client {
    int         ctrl_fd = -1;
    uint32_t    peer_ip = 0;      // network-order IPv4 of the peer, for distinct-client counting
    std::string inbuf;
    bool        logged_in = false;
    bool        user_ok   = false;
    std::string cwd  = "/";
    char        type = 'I';
    int         pasv_fd = -1;     // passive data listener, armed by PASV/EPSV
    std::string rnfr;             // pending RNFR (VFS path)
};
}

// Count distinct connected peers rather than open control sockets.
//
// A single FTP user routinely holds more than one control connection: most
// clients (FileZilla, and the file managers that browse over FTP) open a second
// channel the first time you enter a directory so browsing and transfers don't
// block each other. Counting sockets therefore reports "2 clients" for one
// person. Counting unique peer addresses reports people, which is what the
// status screen is actually claiming to show.
static int distinct_peers(const std::vector<Client>& clients) {
    std::vector<uint32_t> seen;
    for (const auto& c : clients) {
        if (std::find(seen.begin(), seen.end(), c.peer_ip) == seen.end())
            seen.push_back(c.peer_ip);
    }
    return (int)seen.size();
}

// Open a non-blocking-safe passive listener on an ephemeral port; return fd or -1.
static int open_pasv_listener(uint16_t& out_port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = 0;
    if (::bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0 || ::listen(fd, 1) < 0) { ::close(fd); return -1; }
    socklen_t len = sizeof(a);
    if (::getsockname(fd, (struct sockaddr*)&a, &len) < 0) { ::close(fd); return -1; }
    out_port = ntohs(a.sin_port);
    return fd;
}

// Accept the data connection for a PASV transfer, with a bounded wait so a
// client that never connects can't wedge the server.
static int accept_data(Client& c) {
    if (c.pasv_fd < 0) return -1;
    fd_set r; FD_ZERO(&r); FD_SET(c.pasv_fd, &r);
    struct timeval tv{ 10, 0 };
    int s = ::select(c.pasv_fd + 1, &r, nullptr, nullptr, &tv);
    int dfd = (s > 0) ? ::accept(c.pasv_fd, nullptr, nullptr) : -1;
    ::close(c.pasv_fd); c.pasv_fd = -1;
    return dfd;
}

// ─── Directory listing (ls -l style) ──────────────────────────────────────────

// Emit one synthetic directory entry (the virtual install folders at the root).
static void list_virtual_dir(int data_fd, const char* name, bool names_only) {
    if (names_only) { replyf(data_fd, "%s\r\n", name); return; }
    replyf(data_fd, "drwxr-xr-x 1 switch switch %10llu %s %s\r\n",
           0ull, "Jan 01  2000", name);
}

// Emit one synthetic FILE entry (a virtual NSP under Installed Titles). Listed as
// a regular read-only file so clients offer to download it; the bytes are
// generated on demand rather than read from disk.
static void list_virtual_file(int data_fd, const std::string& name,
                              uint64_t size, bool names_only) {
    if (names_only) { replyf(data_fd, "%s\r\n", name.c_str()); return; }
    replyf(data_fd, "-r--r--r-- 1 switch switch %10llu %s %s\r\n",
           (unsigned long long)size, "Jan 01  2000", name.c_str());
}

static void list_dir(int data_fd, const std::string& vfs_dir, bool names_only) {
    DIR* d = ::opendir(vfs_dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = vfs_dir;
        if (!full.empty() && full.back() != '/') full += '/';
        full += name;

        struct stat st{};
        bool ok = (::stat(full.c_str(), &st) == 0);
        bool is_dir = ok && S_ISDIR(st.st_mode);

        if (names_only) {
            replyf(data_fd, "%s\r\n", name.c_str());
            continue;
        }

        char date[32] = "Jan 01  2000";
        if (ok) { struct tm* tmv = ::localtime(&st.st_mtime); if (tmv) std::strftime(date, sizeof(date), "%b %d %H:%M", tmv); }
        replyf(data_fd, "%crw%s 1 switch switch %10llu %s %s\r\n",
               is_dir ? 'd' : '-', is_dir ? "xr-xr-x" : "-r--r--",
               (unsigned long long)(ok ? (uint64_t)st.st_size : 0), date, name.c_str());
    }
    ::closedir(d);
}

// ─── Command dispatch ─────────────────────────────────────────────────────────
// Returns false to close the control connection.

bool FtpServer_handle(FtpServer& srv, Client& c, const std::string& line,
                      bool anon, const std::string& user, const std::string& pass,
                      std::atomic<uint64_t>& bytes_sent, std::atomic<uint64_t>& bytes_recv,
                      const std::function<std::string(const std::string&, const std::string&)>& to_vfs,
                      const std::function<bool()>& should_stop,
                      const std::function<bool(int, FtpTarget, const std::string&)>& do_install);

// ─── Server loop ──────────────────────────────────────────────────────────────

void FtpServer::run() {
    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { set_error("socket() failed"); return; }
    int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(m_port);
    if (::bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        set_error("bind() failed — port in use?"); ::close(listen_fd); return;
    }
    if (::listen(listen_fd, 4) < 0) {
        set_error("listen() failed"); ::close(listen_fd); return;
    }

    std::vector<Client> clients;

    auto to_vfs_fn = [this](const std::string& cwd, const std::string& arg) {
        return to_vfs(cwd, arg);
    };
    auto stop_fn = [this]() { return should_stop(); };
    // ftp_install is a private member; the free-function handler reaches it through
    // this bound callback, matching how to_vfs/should_stop are passed in.
    std::function<bool(int, FtpTarget, const std::string&)> install_fn =
        [this](int dfd, FtpTarget tgt, const std::string& leaf) {
            return ftp_install(dfd, tgt, leaf);
        };

    while (!should_stop()) {
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        for (auto& c : clients) { FD_SET(c.ctrl_fd, &rfds); if (c.ctrl_fd > maxfd) maxfd = c.ctrl_fd; }

        struct timeval tv{ 0, 200000 };   // 200 ms → checks should_stop() ~5×/s
        int n = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        // New control connection.
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int cfd = ::accept(listen_fd, (struct sockaddr*)&peer, &plen);
            if (cfd >= 0) {
                if (clients.size() >= 8) {
                    reply(cfd, "421 Too many connections\r\n"); ::close(cfd);
                } else {
                    Client c; c.ctrl_fd = cfd; c.peer_ip = peer.sin_addr.s_addr;
                    reply(cfd, "220 GarageNX FTP ready\r\n");
                    clients.push_back(std::move(c));
                    m_clients.store(distinct_peers(clients));
                }
            }
        }

        // Existing clients.
        for (size_t i = 0; i < clients.size();) {
            Client& c = clients[i];
            bool keep = true;
            if (FD_ISSET(c.ctrl_fd, &rfds)) {
                char buf[1024];
                ssize_t r = ::recv(c.ctrl_fd, buf, sizeof(buf), 0);
                if (r <= 0) {
                    keep = false;
                } else {
                    c.inbuf.append(buf, (size_t)r);
                    size_t pos;
                    while ((pos = c.inbuf.find('\n')) != std::string::npos) {
                        std::string line = c.inbuf.substr(0, pos);
                        c.inbuf.erase(0, pos + 1);
                        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                        keep = FtpServer_handle(*this, c, line, m_anon, m_user, m_pass,
                                                m_bytes_sent, m_bytes_recv, to_vfs_fn, stop_fn,
                                                install_fn);
                        if (!keep) break;
                    }
                }
            }
            if (!keep) {
                if (c.pasv_fd >= 0) ::close(c.pasv_fd);
                ::close(c.ctrl_fd);
                clients.erase(clients.begin() + i);
                m_clients.store(distinct_peers(clients));
            } else {
                ++i;
            }
        }
    }

    for (auto& c : clients) { if (c.pasv_fd >= 0) ::close(c.pasv_fd); ::close(c.ctrl_fd); }
    ::close(listen_fd);
    m_clients.store(0);
}

// ─── Command handler ──────────────────────────────────────────────────────────

bool FtpServer_handle(FtpServer& srv, Client& c, const std::string& line,
                      bool anon, const std::string& user, const std::string& pass,
                      std::atomic<uint64_t>& bytes_sent, std::atomic<uint64_t>& bytes_recv,
                      const std::function<std::string(const std::string&, const std::string&)>& to_vfs,
                      const std::function<bool()>& should_stop,
                      const std::function<bool(int, FtpTarget, const std::string&)>& do_install) {
    // Split "CMD arg".
    std::string cmd, arg;
    {
        size_t sp = line.find(' ');
        cmd = (sp == std::string::npos) ? line : line.substr(0, sp);
        if (sp != std::string::npos) arg = line.substr(sp + 1);
        for (auto& ch : cmd) ch = (char)toupper((unsigned char)ch);
    }
    const int fd = c.ctrl_fd;

    auto require_login = [&]() -> bool {
        if (!c.logged_in) { reply(fd, "530 Please login with USER and PASS\r\n"); return false; }
        return true;
    };

    if (cmd == "USER") {
        c.user_ok = (anon && (arg == "anonymous" || arg == "ftp")) || (arg == user);
        c.logged_in = false;
        if (anon && (arg == "anonymous" || arg == "ftp")) { c.logged_in = true; reply(fd, "230 Anonymous access granted\r\n"); }
        else reply(fd, "331 Password required\r\n");
        return true;
    }
    if (cmd == "PASS") {
        if (c.logged_in) { reply(fd, "230 Already logged in\r\n"); return true; }
        if (c.user_ok && arg == pass) { c.logged_in = true; reply(fd, "230 Login successful\r\n"); }
        else reply(fd, "530 Login incorrect\r\n");
        return true;
    }
    if (cmd == "QUIT") { reply(fd, "221 Goodbye\r\n"); return false; }
    if (cmd == "NOOP") { reply(fd, "200 OK\r\n"); return true; }
    if (cmd == "SYST") { reply(fd, "215 UNIX Type: L8\r\n"); return true; }
    if (cmd == "FEAT") { reply(fd, "211-Features:\r\n PASV\r\n EPSV\r\n SIZE\r\n TYPE\r\n211 End\r\n"); return true; }
    if (cmd == "OPTS") { reply(fd, "200 OK\r\n"); return true; }
    if (cmd == "TYPE") {
        c.type = (!arg.empty() && (arg[0] == 'A' || arg[0] == 'a')) ? 'A' : 'I';
        replyf(fd, "200 Type set to %c\r\n", c.type);
        return true;
    }

    if (!require_login()) return true;

    if (cmd == "PWD" || cmd == "XPWD") {
        replyf(fd, "257 \"%s\" is current directory\r\n", c.cwd.c_str());
        return true;
    }
    if (cmd == "CWD" || cmd == "XCWD") {
        const std::string posix = resolve_posix(c.cwd, arg);
        // Root, and any storage folder that is virtual (the install folders, whose
        // roots don't stat) are accepted directly. A Filesystem storage folder
        // (SD Card, Album, ...) falls through to the stat check below via to_vfs.
        const auto rp = Services::sp_resolve(posix, Config::get().mtp);
        // Accept CWD into the root chooser, or into an install folder ONLY at its
        // root (rel empty). We must NOT accept a path UNDER an install folder:
        // clients (e.g. Nemo) probe whether an upload target already exists by
        // trying to CWD into "/SD Install/<file>". Answering 250 for that made the
        // client believe the file exists and prompt to overwrite. An install
        // folder has no real subdirectories, so only its root is a valid CWD.
        if (rp.kind == Services::PathKind::Root ||
            (rp.kind == Services::PathKind::Install && rp.rel.empty()) ||
            (rp.kind == Services::PathKind::TitleQuery && rp.rel.empty())) {
            c.cwd = posix;
            replyf(fd, "250 CWD to %s\r\n", c.cwd.c_str());
            return true;
        }
        // A Filesystem storage root or a deeper path must exist on disk.
        std::string vfs = to_vfs(c.cwd, arg);
        struct stat st{};
        if (!vfs.empty() && ::stat(vfs.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            c.cwd = posix;
            replyf(fd, "250 CWD to %s\r\n", c.cwd.c_str());
        } else reply(fd, "550 No such directory\r\n");
        return true;
    }
    if (cmd == "CDUP") {
        c.cwd = resolve_posix(c.cwd, "..");
        reply(fd, "250 CDUP OK\r\n");
        return true;
    }
    if (cmd == "PASV") {
        if (c.pasv_fd >= 0) { ::close(c.pasv_fd); c.pasv_fd = -1; }
        uint16_t dport = 0;
        c.pasv_fd = open_pasv_listener(dport);
        if (c.pasv_fd < 0) { reply(fd, "425 Cannot open passive port\r\n"); return true; }
        // Advertise the local address the client reached us on.
        struct sockaddr_in local{}; socklen_t ll = sizeof(local);
        ::getsockname(fd, (struct sockaddr*)&local, &ll);
        uint32_t ip = ntohl(local.sin_addr.s_addr);
        replyf(fd, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
               (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
               (dport >> 8) & 0xFF, dport & 0xFF);
        return true;
    }
    if (cmd == "EPSV") {
        if (c.pasv_fd >= 0) { ::close(c.pasv_fd); c.pasv_fd = -1; }
        uint16_t dport = 0;
        c.pasv_fd = open_pasv_listener(dport);
        if (c.pasv_fd < 0) { reply(fd, "425 Cannot open passive port\r\n"); return true; }
        replyf(fd, "229 Entering Extended Passive Mode (|||%u|)\r\n", dport);
        return true;
    }
    if (cmd == "LIST" || cmd == "NLST") {
        // Ignore any "-l" style flags in arg; list the CWD (or arg dir).
        std::string target = arg;
        if (!target.empty() && target[0] == '-') target.clear();
        const std::string posix = resolve_posix(c.cwd, target);
        std::string vfs = to_vfs(c.cwd, target);
        reply(fd, "150 Opening data connection\r\n");
        int dfd = accept_data(c);
        if (dfd < 0) { reply(fd, "425 Cannot open data connection\r\n"); return true; }
        const bool names_only = (cmd == "NLST");
        const auto rp_list = Services::sp_resolve(posix, Config::get().mtp);
        if (ftp_is_root(posix)) {
            // The root is a pure chooser: the enabled storage folders from the
            // shared catalog. Filesystem surfaces are listed ONLY if their mount is
            // actually available — a surface can be enabled in config but not yet
            // mounted (NAND is Wave 2, Saves is Wave 3), and listing an unmounted
            // one produces a folder that errors on entry ("not a directory"). We
            // probe the mount root; as each wave adds its mount, the surface
            // auto-appears here with no listing change. Install surfaces have no
            // mount and always list; the TitleQuery surface is skipped until Wave 3.
            for (const auto& s : Services::StorageCatalog::enabled_surfaces(Config::get().mtp)) {
                // TitleQuery surfaces (Installed Titles) are synthesized — they have
                // no mount to probe, so they always list.
                if (s.kind == Services::StorageKind::Filesystem) {
                    struct stat mst{};
                    const std::string root = std::string(s.vfs_root) + "/";
                    if (::stat(root.c_str(), &mst) != 0 || !S_ISDIR(mst.st_mode))
                        continue;   // mount not available yet — don't list it
                }
                list_virtual_dir(dfd, s.display, names_only);
            }
        } else if (rp_list.kind == Services::PathKind::TitleQuery && rp_list.rel.empty()) {
            // Synthesized: one virtual .nsp per installed title. No filesystem is
            // touched; the listing comes from ncm. Only the surface ROOT lists —
            // a path deeper than that names a virtual file, not a directory, and
            // must not re-list everything.
            for (const auto& e : Services::installed_titles_list())
                list_virtual_file(dfd, e.name, e.size, names_only);
        } else if (ftp_is_install_dir(posix)) {
            // Install folders are write-only drop targets; they list as empty.
        } else if (!vfs.empty()) {
            // A path under a Filesystem surface (SD Card, Album, ...) — list it.
            list_dir(dfd, vfs, names_only);
        }
        // else: an invalid path (bare/disabled, not under a storage root) — nothing.
        ::close(dfd);
        reply(fd, "226 Directory send OK\r\n");
        return true;
    }
    if (cmd == "SIZE") {
        std::string vfs = to_vfs(c.cwd, arg);
        // Empty vfs = not a browseable filesystem location (an install folder, the
        // root chooser, or a disabled/invalid path). Clean 550, never stat("").
        if (vfs.empty()) { reply(fd, "550 Not a regular file\r\n"); return true; }
        struct stat st{};
        if (::stat(vfs.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            replyf(fd, "213 %llu\r\n", (unsigned long long)st.st_size);
        else reply(fd, "550 Not a regular file\r\n");
        return true;
    }
    if (cmd == "RETR") {
        std::string vfs = to_vfs(c.cwd, arg);
        if (vfs.empty()) { reply(fd, "550 Cannot open file\r\n"); return true; }
        FILE* f = ::fopen(vfs.c_str(), "rb");
        if (!f) { reply(fd, "550 Cannot open file\r\n"); return true; }
        reply(fd, "150 Opening data connection\r\n");
        int dfd = accept_data(c);
        if (dfd < 0) { ::fclose(f); reply(fd, "425 Cannot open data connection\r\n"); return true; }
        std::vector<char> buf(64 * 1024);
        bool ok = true;
        size_t rd;
        while ((rd = ::fread(buf.data(), 1, buf.size(), f)) > 0) {
            if (should_stop()) { ok = false; break; }
            if (!send_all(dfd, buf.data(), rd)) { ok = false; break; }
            bytes_sent.fetch_add(rd);
        }
        ::close(dfd); ::fclose(f);
        reply(fd, ok ? "226 Transfer complete\r\n" : "426 Transfer aborted\r\n");
        return true;
    }
    if (cmd == "STOR") {
        // Classify the target: a STOR into a virtual install folder drives an
        // install; anywhere else writes a plain file (the original behaviour).
        // resolve_posix gives the path before the "sdmc:" prefix, which is what
        // the classifier expects.
        std::string leaf;
        const std::string posix = resolve_posix(c.cwd, arg);
        const FtpTarget tgt = ftp_classify(posix, leaf);

        if (tgt == FtpTarget::SdInstall || tgt == FtpTarget::NandInstall) {
            // Honour the same config gating as the listing: a disabled target is
            // not installable even if a client guesses the path.
            const auto& m = Config::get().mtp;
            const bool enabled = (tgt == FtpTarget::SdInstall) ? m.sd_install
                                                               : m.nand_install;
            if (!enabled) { reply(fd, "550 Install target not enabled\r\n"); return true; }
            if (leaf.empty()) { reply(fd, "550 Specify a filename to install\r\n"); return true; }
            reply(fd, "150 Opening data connection\r\n");
            int dfd = accept_data(c);
            if (dfd < 0) { reply(fd, "425 Cannot open data connection\r\n"); return true; }
            const bool ok = do_install(dfd, tgt, leaf);
            ::close(dfd);
            reply(fd, ok ? "226 Install complete\r\n"
                         : "550 Install failed (see GarageNX log)\r\n");
            return true;
        }

        // Only paths under "SD Card" can receive a plain file. Root, the install
        // folders themselves, and bare paths have no filesystem location.
        std::string vfs = to_vfs(c.cwd, arg);
        if (vfs.empty()) { reply(fd, "550 Cannot write here — choose a storage folder\r\n"); return true; }
        if (Services::guard_write("FTP", "write file", vfs, Config::get().mtp)
                != Services::WriteDecision::Allow) {
            reply(fd, "550 Permission denied\r\n"); return true;
        }
        FILE* f = ::fopen(vfs.c_str(), "wb");
        if (!f) { reply(fd, "550 Cannot create file\r\n"); return true; }
        reply(fd, "150 Opening data connection\r\n");
        int dfd = accept_data(c);
        if (dfd < 0) { ::fclose(f); reply(fd, "425 Cannot open data connection\r\n"); return true; }
        std::vector<char> buf(64 * 1024);
        bool ok = true;
        ssize_t r;
        while ((r = ::recv(dfd, buf.data(), buf.size(), 0)) > 0) {
            if (should_stop()) { ok = false; break; }
            if (::fwrite(buf.data(), 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
            bytes_recv.fetch_add((uint64_t)r);
        }
        if (r < 0) ok = false;
        ::close(dfd); ::fclose(f);
        reply(fd, ok ? "226 Transfer complete\r\n" : "426 Transfer aborted\r\n");
        return true;
    }
    if (cmd == "DELE") {
        std::string vfs = to_vfs(c.cwd, arg);
        if (vfs.empty()) { reply(fd, "550 Not a writable location\r\n"); return true; }
        if (Services::guard_write("FTP", "delete", vfs, Config::get().mtp)
                != Services::WriteDecision::Allow) {
            reply(fd, "550 Permission denied\r\n"); return true;
        }
        reply(fd, (::remove(vfs.c_str()) == 0) ? "250 File deleted\r\n" : "550 Delete failed\r\n");
        return true;
    }
    if (cmd == "MKD" || cmd == "XMKD") {
        std::string vfs = to_vfs(c.cwd, arg);
        if (vfs.empty()) { reply(fd, "550 Not a writable location\r\n"); return true; }
        if (Services::guard_write("FTP", "create folder", vfs, Config::get().mtp)
                != Services::WriteDecision::Allow) {
            reply(fd, "550 Permission denied\r\n"); return true;
        }
        reply(fd, (::mkdir(vfs.c_str(), 0777) == 0) ? "257 Directory created\r\n" : "550 MKD failed\r\n");
        return true;
    }
    if (cmd == "RMD" || cmd == "XRMD") {
        std::string vfs = to_vfs(c.cwd, arg);
        if (vfs.empty()) { reply(fd, "550 Not a writable location\r\n"); return true; }
        if (Services::guard_write("FTP", "remove folder", vfs, Config::get().mtp)
                != Services::WriteDecision::Allow) {
            reply(fd, "550 Permission denied\r\n"); return true;
        }
        reply(fd, (::rmdir(vfs.c_str()) == 0) ? "250 Directory removed\r\n" : "550 RMD failed\r\n");
        return true;
    }
    if (cmd == "RNFR") {
        std::string vfs = to_vfs(c.cwd, arg);
        // Empty vfs must not reach stat(): stat("") can spuriously succeed.
        if (vfs.empty()) { reply(fd, "550 No such file\r\n"); return true; }
        struct stat st{};
        if (::stat(vfs.c_str(), &st) == 0) { c.rnfr = vfs; reply(fd, "350 Ready for RNTO\r\n"); }
        else reply(fd, "550 No such file\r\n");
        return true;
    }
    if (cmd == "RNTO") {
        if (c.rnfr.empty()) { reply(fd, "503 RNFR required first\r\n"); return true; }
        std::string vfs = to_vfs(c.cwd, arg);
        if (vfs.empty()) { reply(fd, "550 Not a writable location\r\n"); c.rnfr.clear(); return true; }
        if (Services::guard_move("FTP", "rename", c.rnfr, vfs, Config::get().mtp)
                != Services::WriteDecision::Allow) {
            reply(fd, "550 Permission denied\r\n"); c.rnfr.clear(); return true;
        }
        reply(fd, (::rename(c.rnfr.c_str(), vfs.c_str()) == 0) ? "250 Rename OK\r\n" : "550 Rename failed\r\n");
        c.rnfr.clear();
        return true;
    }

    reply(fd, "502 Command not implemented\r\n");
    return true;
}

// ─── Install over FTP (STOR into a virtual install folder) ────────────────────

bool FtpServer::ftp_install(int data_fd, FtpTarget target, const std::string& leaf) {
#ifdef PLATFORM_SWITCH
    const auto dest = (target == FtpTarget::NandInstall)
                          ? Core::Ncm::Storage::BuiltIn
                          : Core::Ncm::Storage::SdCard;

    if (!Core::Keys::available()) Core::Keys::load();
    if (!Core::Keys::available()) {
        m_install_progress.reset();
        m_install_progress.message = Core::Keys::requirement_message();
        m_install_progress.push_log("ERROR: " + Core::Keys::requirement_message());
        save_install_log(leaf, false);
        return false;
    }

    m_install = std::make_unique<Install::StreamInstaller>(
        dest, Core::Keys::get(), m_install_progress);
    m_installing.store(true);

    if (!m_install->begin(leaf, 0)) {          // FTP declares no size; recover from table
        save_install_log(leaf, false);
        m_install.reset(); m_installing.store(false);
        return false;
    }

    // FTP delivers raw file bytes with no framing and no size declaration, so the
    // FirstChunk is empty and the driver recovers the size from the container's
    // own PFS0/HFS0 table. The socket recv IS the byte source; a socket close (0)
    // ends the stream. This is the same StreamDriver the MTP path uses — same
    // install semantics, same teardown safety.
    std::vector<uint8_t> scratch(256 * 1024);

    Install::StreamSource src;
    src.buffer      = scratch.data();
    src.buffer_size = scratch.size();
    src.read = [this, data_fd](uint8_t* buf, size_t n) -> ssize_t {
        ssize_t r = ::recv(data_fd, buf, n, 0);
        if (r > 0) m_bytes_recv.fetch_add((uint64_t)r);
        return r;   // 0 = peer closed = end of file; <0 = error
    };
    src.stop  = [this] { return should_stop(); };
    // A socket transport needs no drain: closing the data connection discards any
    // unread bytes. (MTP must drain its shared USB pipe; FTP does not.)
    src.drain = [](uint64_t) {};

    m_wire_size.store(0);
    m_wire_recv.store(0);
    Install::WireSink wire;
    wire.set_size = [this](uint64_t sz) { m_wire_size.store(sz); };
    wire.add_recv = [this](uint64_t n)  { m_wire_recv.fetch_add(n); };

    Install::FirstChunk first{ nullptr, 0 };
    const Install::DriveResult dr =
        Install::drive(*m_install, src, first, /*declared*/0, /*exact*/false, wire);

    const bool ok = (dr == Install::DriveResult::Ok);
    if (!ok && dr == Install::DriveResult::Cancelled)
        m_install_progress.push_log("Install cancelled");
    save_install_log(leaf, ok);
    m_install.reset();
    m_installing.store(false);
    return ok;
#else
    (void)data_fd; (void)target; (void)leaf;
    return false;
#endif
}

void FtpServer::save_install_log(const std::string& filename, bool ok) {
#ifdef PLATFORM_SWITCH
    const std::string dir = "sdmc:/switch/GarageNX/logs";
    ::mkdir("sdmc:/switch", 0777);
    ::mkdir("sdmc:/switch/GarageNX", 0777);
    ::mkdir(dir.c_str(), 0777);
    FILE* f = ::fopen((dir + "/ftp_install.log").c_str(), "a");
    if (!f) return;
    std::fprintf(f, "GarageNX FTP install — %s : %s\n",
                 filename.c_str(), ok ? "OK" : "FAILED");
    ::fclose(f);
#else
    (void)filename; (void)ok;
#endif
}

// ─── Construction ─────────────────────────────────────────────────────────────

FtpServer::FtpServer(uint16_t port, bool allow_anonymous,
                     std::string user, std::string pass, std::string root)
    : m_port(port), m_anon(allow_anonymous),
      m_user(std::move(user)), m_pass(std::move(pass)) {
    m_prefix = std::move(root);
    while (m_prefix.size() > 1 && m_prefix.back() == '/') m_prefix.pop_back();  // "sdmc:/" -> "sdmc:"
}

} // namespace Services
