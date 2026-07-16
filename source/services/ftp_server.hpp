#pragma once
// source/services/ftp_server.hpp
// Clean-room FTP server (RFC 959 subset) built directly on BSD sockets — no
// external ftpd/ftpsrv dependency. Runs on the NetworkService worker thread.
// Passive mode only (PASV/EPSV); single-threaded select() loop over the listener
// and client control sockets, with blocking data transfers that still poll
// should_stop() so shutdown and large transfers stay responsive.

#include "services/service_manager.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace Services {

class FtpServer : public NetworkService {
public:
    // root is a VFS prefix; "sdmc:/" exposes the SD card as the FTP root "/".
    FtpServer(uint16_t port, bool allow_anonymous,
              std::string user, std::string pass,
              std::string root = "sdmc:/");

    const char* name() const override { return "FTP"; }

    uint16_t port()          const { return m_port; }
    int      client_count()  const { return m_clients.load(); }
    uint64_t bytes_sent()    const { return m_bytes_sent.load(); }
    uint64_t bytes_recv()    const { return m_bytes_recv.load(); }

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

    // Map a client CWD + argument to a VFS path, resolving "." / ".." without
    // escaping the root. Returns "" on an invalid path.
    std::string to_vfs(const std::string& cwd, const std::string& arg) const;
};

} // namespace Services
