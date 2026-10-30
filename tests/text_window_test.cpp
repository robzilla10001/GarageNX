// tests/text_window_test.cpp
//
// Byte-exactness of the editor's window model. The editor rewrites a user's file
// from this representation, so the property that matters is the round trip:
// serialize(parse(x)) == x for EVERY input. A failure here is a corrupted config
// file, not a cosmetic bug - which is why the awkward inputs (empty file, no final
// newline, bare newlines, CRLF) are enumerated rather than sampled.

#include "services/text_window.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Services::TextWindow;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static void roundtrip(const std::string& x, const char* what) {
    Doc d = parse(x, true);
    CHECK(serialize(d) == x, what);
    CHECK(byte_size(d) == x.size(), what);
}

int main() {
    roundtrip("", "empty file");
    roundtrip("\n", "single newline");
    roundtrip("\n\n\n", "only newlines");
    roundtrip("a", "no trailing newline");
    roundtrip("a\n", "one terminated line");
    roundtrip("a\nb", "last line unterminated");
    roundtrip("a\nb\n", "two terminated lines");
    roundtrip("a\n\nb\n", "blank line between");
    roundtrip("a\r\nb\r\n", "CRLF");
    roundtrip("a\r\nb", "CRLF, last line unterminated");
    roundtrip("[sec]\nkey=val\n; comment\n", "ini shaped");
    roundtrip(std::string("h\xC3\xA9llo\n\xE2\x82\xAC\n"), "UTF-8 multibyte");

    // Shape of the parse, not just the round trip.
    {
        Doc d = parse("a\nb\n", true);
        CHECK(d.lines.size() == 2 && d.lines[0] == "a" && d.lines[1] == "b", "two lines");
        CHECK(!d.eof_no_nl, "terminated file has no eof_no_nl");
    }
    {
        Doc d = parse("a\nb", true);
        CHECK(d.lines.size() == 2 && d.eof_no_nl, "unterminated final line flagged");
    }
    {
        Doc d = parse("", true);
        CHECK(d.lines.size() == 1 && d.lines[0].empty() && d.eof_no_nl,
              "empty file is one empty unterminated line");
    }
    {
        Doc d = parse("a\r\n", true);
        CHECK(d.lines.size() == 1 && d.lines[0] == "a\r", "CR is kept on the line");
    }

    // Edits: the operations the editor performs, then serialize.
    {
        Doc d = parse("a\nb\n", true);
        d.lines.insert(d.lines.begin() + 1, "x");
        CHECK(serialize(d) == "a\nx\nb\n", "insert in the middle");
    }
    {
        // Appending after an unterminated last line: the old last line must gain a
        // newline and the new last line takes over the "no terminator" status.
        Doc d = parse("a\nb", true);
        d.lines.push_back("c");
        CHECK(serialize(d) == "a\nb\nc", "append after unterminated last line");
    }
    {
        Doc d = parse("a\nb\n", true);
        d.lines.erase(d.lines.begin());
        CHECK(serialize(d) == "b\n", "delete first line");
        d.lines.erase(d.lines.begin());
        CHECK(d.lines.empty() && serialize(d).empty(), "deleting every line leaves nothing");
    }
    {
        Doc d = parse("a\nb\n", true);
        d.lines[0] = "changed";
        CHECK(serialize(d) == "changed\nb\n", "replace a line");
    }

    // Non-EOF window: always newline-terminated, so every line gets its '\n'.
    {
        Doc d = parse("a\nb\n", false);
        CHECK(!d.eof_no_nl && serialize(d) == "a\nb\n", "mid-file window round trip");
    }

    // Chunk cutting.
    {
        const char buf[] = "aa\nbb\ncc";
        CHECK(complete_prefix_len(buf, 8) == 6, "cut back to last newline");
        CHECK(complete_prefix_len(buf, 5) == 3, "cut shorter chunk");
        CHECK(complete_prefix_len("abc", 3) == 0, "no newline in chunk");
        CHECK(complete_prefix_len(buf, 0) == 0, "empty chunk");
        CHECK(first_line_start(buf, 8) == 3, "skip partial first line");
        CHECK(first_line_start("abc", 3) == 3, "no newline: skip everything");
        CHECK(first_line_start("\nabc", 4) == 1, "starts on a newline");
    }

    // Binary sniff.
    {
        const char text[] = "plain text\n";
        const char bin[]  = {'a', 0, 'b'};
        CHECK(!has_nul(text, sizeof(text) - 1), "text has no NUL");
        CHECK(has_nul(bin, 3), "NUL detected");
    }

    std::printf("text_window_test: %d checks passed\n", g_checks);
    return 0;
}
