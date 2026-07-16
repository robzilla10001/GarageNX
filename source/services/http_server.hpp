#pragma once
// source/services/http_server.hpp
// Clean-room HTTP server (HTTP/1.1 subset) built directly on BSD sockets — no
// external httpd/libmicrohttpd dependency. Runs on the NetworkService worker thread.
// Supports GET (file download, directory listing as JSON) and PUT (upload) with
// path traversal clamping to the root directory.

#include "services/service_manager.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace Services {

class HttpServer : public NetworkService {
public:
    // root is a VFS prefix; "sdmc:/" exposes the SD card as the HTTP root "/".
    HttpServer(uint16_t port, bool allow_upload, std::string root = "sdmc:/");

    const char* name() const override { return "HTTP"; }

    uint16_t port() const { return m_port; }
    bool allow_upload() const { return m_allow_upload; }
    int client_count() const { return m_clients.load(); }
    int request_count() const { return m_requests.load(); }
    uint64_t bytes_sent() const { return m_bytes_sent.load(); }
    uint64_t bytes_recv() const { return m_bytes_recv.load(); }

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

    // Map a client path to a VFS path, resolving "." / ".." without escaping the root.
    std::string resolve_vfs(const std::string& path) const;
};

} // namespace Services
