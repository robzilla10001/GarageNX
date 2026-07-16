// source/services/http_server.cpp

#include "services/http_server.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace Services {

// ─── Path helpers ─────────────────────────────────────────────────────────────

// Resolve `path` against the server root, collapsing "." / ".." and clamping at root ("/").
// Returns a clean POSIX path beginning with '/'.
static std::string resolve_posix(const std::string& root, const std::string& path) {
    std::string start = (!path.empty() && path[0] == '/') ? path : (root + "/" + path);
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

std::string HttpServer::resolve_vfs(const std::string& path) const {
    const std::string posix = resolve_posix(m_prefix, path);
    return m_prefix + posix; // m_prefix="sdmc:" + "/foo" -> "sdmc:/foo"; "/" -> "sdmc:/"
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

// ─── JSON escaping helper ─────────────────────────────────────────────────────

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// ─── Directory listing (JSON style) ───────────────────────────────────────────

static void list_dir_json(int data_fd, const std::string& posix_path, const std::string& vfs_dir) {
    DIR* d = ::opendir(vfs_dir.c_str());
    if (!d) return;
    

    std::string json = "{\"path\":\"" + json_escape(posix_path) + "\",\"entries\":[";
    bool first = true;
    
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

        if (!first) json += ",";
        first = false;
        
        json += "{\"name\":\"" + json_escape(name) + "\",";
        json += std::string("\"type\":\"") + (is_dir ? "dir" : "file") + "\",";
        json += "\"size\":" + std::to_string(ok ? (uint64_t)st.st_size : 0) + "}";
    }
    ::closedir(d);
    
    json += "]}";
    send_all(data_fd, json.data(), json.size());
}

// ─── Server loop ──────────────────────────────────────────────────────────────

void HttpServer::run() {
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

    set_status(Status::Running);

    while (!should_stop()) {
        fd_set rfds; FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        struct timeval tv{ 0, 200000 };   // 200 ms → checks should_stop() ~5×/s
        int n = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) continue;

        // New connection.
        if (FD_ISSET(listen_fd, &rfds)) {
            int cfd = ::accept(listen_fd, nullptr, nullptr);
            if (cfd >= 0) {
                struct timeval tv;
                tv.tv_sec = 10;
                tv.tv_usec = 0;
                ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

                m_clients.fetch_add(1);
                
                // Read full header block (loop until CRLFCRLF or cap)
                std::string header_block;
                char buf[1024];
                size_t total_read = 0;
                size_t header_cap = 8 * 1024; // 8KB cap
                bool headers_done = false;
                
                while (!headers_done && total_read < header_cap) {
                    ssize_t r = ::recv(cfd, buf, sizeof(buf), 0);
                    if (r <= 0) break;
                    
                    header_block.append(buf, r);
                    total_read += r;
                    
                    // Check for CRLFCRLF
                    if (header_block.size() >= 4) {
                        const std::string::size_type pos = header_block.find("\r\n\r\n");
                        if (pos != std::string::npos) {
                            headers_done = true;
                        }
                    }
                }
                
                if (!header_block.empty()) {
                    // Parse request line
                    std::string method, path;
                    size_t first_space = header_block.find(' ');
                    if (first_space != std::string::npos) {
                        method = header_block.substr(0, first_space);
                        size_t second_space = header_block.find(' ', first_space + 1);
                        if (second_space != std::string::npos) {
                            path = header_block.substr(first_space + 1, second_space - first_space - 1);
                        }
                    }
                    m_requests.fetch_add(1);
                    // Clamp path traversal
                    std::string vfs_path = resolve_vfs(path);
                    

                    std::string posix_path = resolve_posix(m_prefix, path);
                    // Process request
                    if (method == "GET") {
                        struct stat st{};
                        if (::stat(vfs_path.c_str(), &st) == 0) {
                            if (S_ISDIR(st.st_mode)) {
                                // Directory listing as JSON
                                std::string response = "HTTP/1.1 200 OK\r\n";
                                response += "Content-Type: application/json\r\n";
                                response += "Connection: close\r\n";
                                response += "\r\n";
                                send_all(cfd, response.data(), response.size());
                                list_dir_json(cfd, posix_path, vfs_path);
                            } else if (S_ISREG(st.st_mode)) {
                                // File download
                                FILE* f = ::fopen(vfs_path.c_str(), "rb");
                                if (f) {
                                    std::string response = "HTTP/1.1 200 OK\r\n";
                                    response += "Content-Type: application/octet-stream\r\n";
                                    response += "Content-Length: " + std::to_string(st.st_size) + "\r\n";
                                    response += "Connection: close\r\n";
                                    response += "\r\n";
                                    send_all(cfd, response.data(), response.size());
                                    
                                    std::vector<char> file_buf(64 * 1024);
                                    size_t rd;
                                    while ((rd = ::fread(file_buf.data(), 1, file_buf.size(), f)) > 0) {
                                        if (should_stop()) break;
                                        if (!send_all(cfd, file_buf.data(), rd)) break;
                                        m_bytes_sent.fetch_add(rd);
                                    }
                                    ::fclose(f);
                                } else {
                                    reply(cfd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
                                }
                            } else {
                                reply(cfd, "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
                            }
                        } else {
                            reply(cfd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
                        }
                    } else if (method == "PUT") {
                        if (m_allow_upload) {
                            // Create directory if needed
                            std::string dir_path = vfs_path;
                            size_t last_slash = dir_path.rfind('/');
                            if (last_slash != std::string::npos && last_slash > 0) {
                                dir_path = dir_path.substr(0, last_slash);
                                ::mkdir(dir_path.c_str(), 0755);
                            }
                            
                            // Open file for writing
                            FILE* f = ::fopen(vfs_path.c_str(), "wb");
                            if (f) {
                                // Find Content-Length header
                                std::string::size_type cl_pos = header_block.find("Content-Length:");
                                uint64_t content_length = 0;
                                if (cl_pos != std::string::npos) {
                                    std::string::size_type cl_end = header_block.find('\n', cl_pos);
                                    if (cl_end != std::string::npos) {
                                        std::string cl_str = header_block.substr(cl_pos + 15, cl_end - cl_pos - 15);
                                        // Trim whitespace
                                        while (!cl_str.empty() && (cl_str.back() == ' ' || cl_str.back() == '\r')) {
                                            cl_str.pop_back();
                                        }
                                        try {
                                            content_length = std::stoull(cl_str);
                                        } catch (...) {
                                            content_length = 0;
                                        }
                                    }
                                }
                                
                                // Skip headers
                                std::string::size_type header_end = header_block.find("\r\n\r\n");
                                if (header_end != std::string::npos) {
                                    size_t body_start = header_end + 4;
                                    if (body_start < header_block.length()) {
                                        std::string body = header_block.substr(body_start);
                                        size_t written = fwrite(body.data(), 1, body.size(), f);
                                        m_bytes_recv.fetch_add(written);
                                        if (content_length >= body.size()) {
                                            content_length -= body.size();
                                        } else {
                                            content_length = 0;
                                        }
                                    }
                                    
                                    // Read remaining body
                                    std::vector<char> body_buf(64 * 1024);
                                    while (content_length > 0) {
                                        if (should_stop()) break;
                                        size_t to_read = std::min(content_length, (uint64_t)body_buf.size());
                                        ssize_t rd = ::recv(cfd, body_buf.data(), to_read, 0);
                                        if (rd <= 0) break;
                                        size_t written = fwrite(body_buf.data(), 1, rd, f);
                                        m_bytes_recv.fetch_add(written);
                                        content_length -= rd;
                                    }
                                }
                                
                                ::fclose(f);
                                reply(cfd, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                            } else {
                                reply(cfd, "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n");
                            }
                        } else {
                            reply(cfd, "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n");
                        }
                    } else {
                        // Method not allowed
                        reply(cfd, "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, PUT\r\nConnection: close\r\n\r\n");
                    }
                }
                ::close(cfd);
                m_clients.fetch_sub(1);
            }
        }
    }

    ::close(listen_fd);
    set_status(Status::Stopped);
}

// ─── Construction ─────────────────────────────────────────────────────────────

HttpServer::HttpServer(uint16_t port, bool allow_upload, std::string root)
    : m_port(port), m_allow_upload(allow_upload) {
    m_prefix = std::move(root);
    while (m_prefix.size() > 1 && m_prefix.back() == '/') m_prefix.pop_back();  // "sdmc:/" -> "sdmc:"
}

}