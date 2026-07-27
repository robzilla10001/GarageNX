// source/services/http_paths.hpp
//
// Pure URL-path model for the HTTP server's install routing — no sockets, no
// libnx, so it is unit-tested on the host. It mirrors the FTP model (ftp_paths.hpp)
// so the two transports classify install targets identically:
//
//   PUT /install/sd/Game.nsz    → install Game.nsz to the SD card
//   PUT /install/nand/Game.nsz  → install Game.nsz to NAND
//   PUT /<anything else>        → write a plain file (the original behaviour)
//
// Why a distinct prefix ("/install/...") rather than FTP's folder names ("SD
// Install"): a URL path with spaces has to be percent-encoded, and a client that
// forgets is a silent mis-route. A slash-delimited ASCII prefix is unambiguous
// over HTTP and reads naturally in a URL. The TARGET meanings are identical to
// FTP's; only the spelling suits the transport.

#pragma once

#include <string>

namespace Services {

// The URL prefix that marks an install upload, and the per-target sub-segments.
inline constexpr const char* kHttpInstallPrefix = "install";
inline constexpr const char* kHttpInstallSd     = "sd";
inline constexpr const char* kHttpInstallNand   = "nand";

enum class HttpTarget {
    Filesystem,   // a normal path — plain file I/O (download / upload)
    SdInstall,    // PUT under /install/sd   — install to SD card
    NandInstall,  // PUT under /install/nand — install to NAND
    Invalid,      // an /install/... path that names no valid target
};

// Percent-decode a URL path component in place-ish (returns a new string). Only
// the two sequences that matter for filenames are handled generally: "%XX" hex
// escapes and '+' is left as-is (paths, unlike query strings, do not use '+' for
// space). An invalid escape is left literal rather than dropped, so a filename is
// never silently corrupted.
inline std::string http_percent_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexval(s[i + 1]);
            const int lo = hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Split a URL path into its first component and the remainder, skipping leading
// slashes. "/install/sd/Game.nsz" → first="install", rest="sd/Game.nsz".
inline void http_split_first(const std::string& path,
                             std::string& first, std::string& rest) {
    size_t i = 0;
    while (i < path.size() && path[i] == '/') ++i;
    const size_t start = i;
    while (i < path.size() && path[i] != '/') ++i;
    first = path.substr(start, i - start);
    while (i < path.size() && path[i] == '/') ++i;
    rest = (i < path.size()) ? path.substr(i) : std::string();
}

// Classify a request path. For an install target, `leaf` receives the
// percent-decoded filename (the last path component); a nested path under an
// install target keeps only its leaf, since installs take a single file, not a
// tree. For Filesystem/Invalid, `leaf` is cleared.
//
// A query string (?...) is stripped before classification — install uploads carry
// no query, and leaving one on would corrupt the leaf filename.
inline HttpTarget http_classify(const std::string& raw_path, std::string& leaf) {
    leaf.clear();

    // Drop any query string.
    std::string path = raw_path;
    const size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);

    std::string first, rest;
    http_split_first(path, first, rest);
    if (first != kHttpInstallPrefix) return HttpTarget::Filesystem;

    // Under /install — the next segment is the target.
    std::string target, after;
    http_split_first(rest, target, after);

    HttpTarget which;
    if (target == kHttpInstallSd)        which = HttpTarget::SdInstall;
    else if (target == kHttpInstallNand) which = HttpTarget::NandInstall;
    else return HttpTarget::Invalid;     // /install/<unknown>

    // `after` is the filename (possibly with further slashes; take the last
    // component so "/install/sd/a/b/Game.nsz" still installs "Game.nsz").
    std::string tail = after;
    size_t slash = tail.find_last_of('/');
    if (slash != std::string::npos) tail = tail.substr(slash + 1);
    leaf = http_percent_decode(tail);

    if (leaf.empty()) return HttpTarget::Invalid;   // /install/sd with no filename
    return which;
}

// True when a PUT to this path should be treated as an install rather than a
// plain file write.
inline bool http_is_install_path(const std::string& raw_path) {
    std::string leaf;
    const HttpTarget t = http_classify(raw_path, leaf);
    return t == HttpTarget::SdInstall || t == HttpTarget::NandInstall;
}

// Return the path portion of a request target, with any query string removed.
// "/api/list?path=/foo" → "/api/list".
inline std::string http_path_only(const std::string& raw_path) {
    const size_t q = raw_path.find('?');
    return (q == std::string::npos) ? raw_path : raw_path.substr(0, q);
}

// Extract a query parameter's value, percent-decoded. Returns "" when absent.
// Handles '+' as space, which IS the convention inside a query string (unlike a
// path), so a browser-encoded directory name round-trips correctly.
inline std::string http_query_param(const std::string& raw_path,
                                    const std::string& key) {
    const size_t q = raw_path.find('?');
    if (q == std::string::npos) return std::string();

    const std::string qs = raw_path.substr(q + 1);
    size_t i = 0;
    while (i < qs.size()) {
        size_t amp = qs.find('&', i);
        if (amp == std::string::npos) amp = qs.size();
        const std::string pair = qs.substr(i, amp - i);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            std::string v = pair.substr(eq + 1);
            for (char& c : v) if (c == '+') c = ' ';   // query-string convention
            return http_percent_decode(v);
        }
        i = amp + 1;
    }
    return std::string();
}

// True when a path targets the web API rather than a file.
inline bool http_is_api_path(const std::string& raw_path) {
    const std::string p = http_path_only(raw_path);
    return p.compare(0, 5, "/api/") == 0;
}

// True when a path should serve the embedded web UI page.
inline bool http_is_ui_path(const std::string& raw_path) {
    const std::string p = http_path_only(raw_path);
    return p == "/" || p == "/index.html";
}

} // namespace Services
