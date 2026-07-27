#pragma once
// source/services/http_server.hpp
// Clean-room HTTP server (HTTP/1.1 subset) built directly on BSD sockets — no
// external httpd/libmicrohttpd dependency. Runs on the NetworkService worker thread.
// Supports GET (file download, directory listing as JSON) and PUT (upload) with
// path traversal clamping to the root directory.

#include "services/service_manager.hpp"
#include "services/http_paths.hpp"
#include "install/stream_installer.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace Services {

class HttpServer : public NetworkService {
public:
    // root is a VFS prefix; "sdmc:/" exposes the SD card as the HTTP root "/".
    HttpServer(uint16_t port, bool allow_upload, std::string root = "sdmc:/");

    // CRITICAL, and learned the hard way on both MTP and FTP: stop() (which joins
    // the worker thread) MUST run before any member is destroyed. Otherwise C++
    // destroys members first — including m_install, which the worker may be inside
    // during an install — and only then runs the base ~NetworkService that joins
    // the worker. That is a cross-thread use-after-free; cancelling a transfer
    // crashed FTP and MTP with a Data Abort @ 0x0 until each got this one-liner.
    // Applied here PROACTIVELY the moment HTTP gained the same install member,
    // rather than waiting to re-learn it on device.
    ~HttpServer() override { stop(); }

    const char* name() const override { return "HTTP"; }

    uint16_t port() const { return m_port; }
    bool allow_upload() const { return m_allow_upload; }
    int client_count() const { return m_clients.load(); }
    int request_count() const { return m_requests.load(); }
    uint64_t bytes_sent() const { return m_bytes_sent.load(); }
    uint64_t bytes_recv() const { return m_bytes_recv.load(); }

    // Install progress surface, mirroring FTP/MTP so a future HTTP progress screen
    // (or the B2 web UI's progress channel) reads it the same way.
    const Install::Progress& install_progress() const { return m_install_progress; }
    bool     installing()        const { return m_installing.load(); }
    uint64_t current_wire_size() const { return m_wire_size.load(); }
    uint64_t current_wire_recv() const { return m_wire_recv.load(); }

protected:
    void run() override;

private:
    uint16_t m_port;
    bool m_allow_upload;
    std::string m_prefix; // root with trailing '/' stripped, e.g. "sdmc:"
    std::atomic<int> m_clients{0};
    std::atomic<uint64_t> m_requests{0};
    std::atomic<uint64_t> m_bytes_sent{0};
    std::atomic<uint64_t> m_bytes_recv{0};

    // Install state — same shape as FtpServer, so the shared StreamDriver and its
    // teardown discipline are reused, not re-implemented.
    std::atomic<uint64_t> m_wire_size{0};
    std::atomic<uint64_t> m_wire_recv{0};
    Install::Progress                         m_install_progress;
    std::unique_ptr<Install::StreamInstaller> m_install;
    std::atomic<bool>                         m_installing{false};

    // Map a client path to a VFS path, resolving "." / ".." without escaping the
    // root. Goes through the shared catalog, so every surface the other transports
    // expose is reachable here too.
    std::string resolve_vfs(const std::string& path) const;

    // Surface-aware JSON listing for the web UI: root chooser, synthesized Save
    // Data levels, Installed Titles, or a real directory.
    void list_path_json(int fd, const std::string& posix);

    // Install a PUT body via the shared StreamDriver. `cfd` is the client socket
    // positioned at the first body byte after `prebuffered` (bytes already read
    // with the headers). `content_length` is the exact wire size (0 if absent).
    // Returns true on a completed install.
    bool http_install(int cfd, HttpTarget target, const std::string& leaf,
                      const std::string& prebuffered, uint64_t content_length);
    void save_install_log(const std::string& filename, bool ok);
};

} // namespace Services
