#pragma once
// source/ui/text_wrap.hpp
//
// The word-wrapping ALGORITHM, separated from font rendering so it can be
// host-tested. Font::wrap() is a thin adapter that supplies a real text measurer.
//
// Worth separating because the tricky cases here cannot be checked by looking at
// a console screen: whether a break landed inside a multi-byte glyph, whether a
// space-free 200-character path breaks at the right column, whether a blank line
// survived. Those are exactly the things a unit test settles in a second and a
// hardware round settles badly.
//
// The measurement function is injected, so a test can use an exact synthetic
// metric ("every character is 10px") and assert precise column positions instead
// of guessing at proportional font widths.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace TextWrap {

/// Byte length of the UTF-8 sequence starting at s[i]. Breaking a line inside a
/// multi-byte glyph renders as mojibake, so hard breaks advance whole code
/// points. Malformed input degrades to one byte rather than running off the end.
inline size_t utf8_len(const std::string& s, size_t i) {
    if (i >= s.size()) return 1;
    const unsigned char c = (unsigned char)s[i];
    size_t n = (c < 0x80)        ? 1
             : ((c >> 5) == 0x06) ? 2
             : ((c >> 4) == 0x0E) ? 3
             : ((c >> 3) == 0x1E) ? 4
             : 1;
    return (i + n <= s.size()) ? n : 1;
}

/// Break `text` into lines each fitting `max_width`, per `measure`.
///
///   - '\n' always starts a new line, and an empty line is PRESERVED (blank
///     lines are deliberate spacing, not noise to be collapsed).
///   - Normal text wraps at spaces; the space stays with the word before it.
///   - A single token wider than max_width is broken mid-token. This is not an
///     edge case here: confirmation modals show file paths, which contain no
///     spaces and routinely exceed the modal width.
inline std::vector<std::string> wrap(
    const std::string& text, int max_width,
    const std::function<int(const std::string&)>& measure)
{
    std::vector<std::string> out;
    if (max_width <= 0 || !measure) { out.push_back(text); return out; }

    size_t start = 0;
    for (;;) {
        const size_t nl = text.find('\n', start);
        const std::string para = text.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);

        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            const size_t sp = para.find(' ', i);
            const std::string word = para.substr(
                i, sp == std::string::npos ? std::string::npos : sp - i + 1);
            i += word.size();

            const std::string candidate = line + word;
            if (line.empty() || measure(candidate) <= max_width) {
                line = candidate;
            } else {
                out.push_back(line);
                line = word;
            }

            // The token alone may still overflow; break it by code points until
            // the remainder fits.
            while (measure(line) > max_width) {
                std::string head;
                size_t k = 0;
                while (k < line.size()) {
                    const size_t n = utf8_len(line, k);
                    const std::string test = head + line.substr(k, n);
                    if (!head.empty() && measure(test) > max_width) break;
                    head = test;
                    k += n;
                }
                if (k == 0 || k >= line.size()) break;   // cannot split further
                out.push_back(head);
                line = line.substr(k);
            }
        }
        out.push_back(line);

        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

} // namespace TextWrap
