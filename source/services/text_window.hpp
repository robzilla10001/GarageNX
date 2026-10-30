#pragma once
// source/services/text_window.hpp
//
// The pure half of the paged text editor: how a byte window of a file becomes
// editable lines and back again. No SDL, no libnx, no I/O - host-tested, because
// its whole job is getting boundary cases byte-exact (empty file, no trailing
// newline, CRLF, blank lines). A wrong answer here silently corrupts a user's
// config file on save.
//
// The editor never holds a whole file. It holds one WINDOW: a run of complete
// lines. The window is always line-aligned (starts at a line start, ends after a
// '\n' or at EOF), so editing it can never split a line or a UTF-8 sequence that
// straddles the window boundary.
//
// Invariant, pinned by tests: serialize(parse(x, eof)) == x for every x.

#include <cstddef>
#include <string>
#include <vector>

namespace Services::TextWindow {

struct Doc {
    // Lines WITHOUT their '\n'. A '\r' from CRLF stays on the line (it is data the
    // editor must preserve, not something to normalise away).
    std::vector<std::string> lines;
    // True when the window reaches EOF and the file's last line has no '\n'.
    // Only the LAST line can lack its terminator; every other line always has one.
    bool eof_no_nl = false;
};

/// Split `bytes` into lines. `reaches_eof` says whether the window's end is the
/// file's end; a non-EOF window is expected to end with '\n' (callers cut it so).
inline Doc parse(const std::string& bytes, bool reaches_eof) {
    Doc d;
    size_t start = 0;
    while (start < bytes.size()) {
        const size_t nl = bytes.find('\n', start);
        if (nl == std::string::npos) {
            d.lines.emplace_back(bytes, start);   // final line, no terminator
            d.eof_no_nl = true;
            return d;
        }
        d.lines.emplace_back(bytes, start, nl - start);
        start = nl + 1;
    }
    // Empty input, or input that ended exactly on a '\n'. An empty EOF window still
    // needs one (empty, unterminated) line so there is something to put a cursor on
    // and so serialize() reproduces the empty file.
    if (d.lines.empty()) {
        d.lines.emplace_back();
        d.eof_no_nl = reaches_eof;
    }
    return d;
}

/// Number of bytes serialize(d) would produce, without building the string. Lets
/// the editor enforce its size cap on every keystroke-level edit cheaply.
inline size_t byte_size(const Doc& d) {
    size_t n = 0;
    for (const auto& l : d.lines) n += l.size() + 1;
    if (d.eof_no_nl && n > 0) n -= 1;
    return n;
}

/// Exact inverse of parse().
inline std::string serialize(const Doc& d) {
    std::string out;
    out.reserve(byte_size(d));
    for (size_t i = 0; i < d.lines.size(); ++i) {
        out += d.lines[i];
        const bool last = (i + 1 == d.lines.size());
        if (!(last && d.eof_no_nl)) out += '\n';
    }
    return out;
}

/// Length of the longest prefix of p[0..n) that ends on a '\n' (that '\n'
/// included); 0 when there is none. Used to cut a freshly read chunk back to a
/// whole number of lines.
inline size_t complete_prefix_len(const char* p, size_t n) {
    while (n > 0) {
        if (p[n - 1] == '\n') return n;
        --n;
    }
    return 0;
}

/// Offset of the first byte after the first '\n' in p[0..n); n when there is none.
/// Used when a read starts mid-file (window moved backwards): the bytes before the
/// first newline are the tail of a line that began earlier and must be skipped.
inline size_t first_line_start(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] == '\n') return i + 1;
    return n;
}

/// True if the buffer contains a NUL byte - the cheap, reliable "this is not a text
/// file" signal. The editor refuses such files rather than risk rewriting binary.
inline bool has_nul(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] == '\0') return true;
    return false;
}

} // namespace Services::TextWindow
