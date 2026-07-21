#pragma once
// source/services/ftp_server.hpp
// Clean-room FTP server (RFC 959 subset) built directly on BSD sockets — no
// external ftpd/ftpsrv dependency. Runs on the NetworkService worker thread.
// Passive mode only (PASV/EPSV); single-threaded select() loop over the listener
// and client control sockets, with blocking data transfers that still poll
// should_stop() so shutdown and large transfers stay responsive.

#include "services/service_manager.hpp"
#include "services/ftp_paths.hpp"
#include "install/stream_installer.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace Services {

class FtpServer : public NetworkService {
public:
    // root is a VFS prefix; "sdmc:/" exposes the SD card as the FTP root "/".
    FtpServer(uint16_t port, bool allow_anonymous,
              std::string user, std::string pass,
              std::string root = "sdmc:/");

    // CRITICAL: stop() (which joins the worker thread) MUST run before any member
    // is destroyed. Without this, C++ destroys members first — including
    // m_install, which the worker thread is actively using inside an install —
    // and only then runs the base ~NetworkService that joins the worker. That is
    // a cross-thread use-after-free: cancelling an FTP transfer crashed with a
    // Data Abort @ 0x0 (the 2168-0002 in the report is a stale register), the
    // identical failure the MTP path had before it got the same one-line fix.
    ~FtpServer() override { stop(); }

    const char* name() const override { return "FTP"; }

    uint16_t port()          const { return m_port; }
    int      client_count()  const { return m_clients.load(); }
    uint64_t bytes_sent()    const { return m_bytes_sent.load(); }
    uint64_t bytes_recv()    const { return m_bytes_recv.load(); }

    // Current install's wire (compressed) size and received bytes, for the
    // screen's average/ETA — same contract as MtpServer. 0 when no install is in
    // progress or the size isn't known yet (ETA shows "—").
    uint64_t current_wire_size() const { return m_wire_size.load(); }
    uint64_t current_wire_recv() const { return m_wire_recv.load(); }

    // Live install state, so a screen can surface an in-progress FTP install the
    // same way the MTP screen does. installing() is true only during a drive.
    const Install::Progress& install_progress() const { return m_install_progress; }
    bool     installing()     const { return m_installing.load(); }

protected:
    void run() override;

private:
    uint16_t    m_port;
    bool        m_anon;
    std::string m_user;
    std::string m_pass;
    std::string m_prefix;   // root with trailing '/' stripped, e.g. "sdmc:"

    std::atomic<int>      m_clients{0};
    std::atomic<uint64_t> m_bytes_sent{0};
    std::atomic<uint64_t> m_bytes_recv{0};
    std::atomic<uint64_t> m_wire_size{0};   // current install wire size (ETA denominator)
    std::atomic<uint64_t> m_wire_recv{0};   // current install wire bytes received

    // Install plumbing (mirrors MtpServer). Only touched on the FTP worker thread.
    Install::Progress                         m_install_progress;
    std::unique_ptr<Install::StreamInstaller> m_install;
    std::atomic<bool>                         m_installing{false};

    // Map a client CWD + argument to a VFS path, resolving "." / ".." without
    // escaping the root. Returns "" on an invalid path.
    std::string to_vfs(const std::string& cwd, const std::string& arg) const;

    // Drive a STOR into an install folder through the shared StreamDriver. `data_fd`
    // is the accepted data connection; `target` selects SD vs NAND; `leaf` is the
    // filename for logging. Returns true on a successful install. Runs on the FTP
    // worker thread; cancellation is via should_stop().
    bool ftp_install(int data_fd, FtpTarget target, const std::string& leaf);

    // Persist a per-install log, mirroring MtpServer::save_install_log.
    void save_install_log(const std::string& filename, bool ok);
};

} // namespace Services
