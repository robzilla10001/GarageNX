// tests/text_wrap_test.cpp
//
// The wrapping algorithm behind every modal body. Tested with a SYNTHETIC metric
// — "every ASCII character is 10px wide" — so assertions can be exact instead of
// guessing at proportional font widths. What is being tested is where the breaks
// land, which is font-independent.
//
// This exists because the failure it fixes was reported from hardware: modal text
// ran off the box and off the screen. The renderer only ever split on explicit
// '\n', which was fine while modals carried short labels and wrong the moment one
// carried a sentence. The confirmation modals that show FILE PATHS were exposed
// to the same bug and had simply not been hit yet — a path has no spaces, so it
// needs the mid-token break, which is the case most likely to be got wrong and
// the least likely to be noticed by looking at a screen.

#include "ui/text_wrap.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// 10px per byte, but only counting UTF-8 LEAD bytes, so a multi-byte glyph costs
// the same as one ASCII character — which is what a real font does.
static int measure(const std::string& s) {
    int glyphs = 0;
    for (size_t i = 0; i < s.size(); i += TextWrap::utf8_len(s, i)) ++glyphs;
    return glyphs * 10;
}

static std::vector<std::string> w(const std::string& t, int px) {
    return TextWrap::wrap(t, px, measure);
}

static void test_no_wrap_needed() {
    auto r = w("short", 200);
    CHECK(r.size() == 1, "one line");
    CHECK(r[0] == "short", "text unchanged");
    std::printf("  ok: short text is left alone\n");
}

static void test_wraps_at_spaces() {
    // 100px = 10 glyphs per line.
    auto r = w("aaa bbb ccc ddd", 100);
    CHECK(r.size() == 2, "two lines");
    CHECK(r[0] == "aaa bbb ", "first line keeps its trailing space");
    CHECK(r[1] == "ccc ddd", "remainder on the second");
    std::printf("  ok: wraps at spaces\n");
}

static void test_explicit_newlines_and_blank_lines() {
    auto r = w("one\ntwo", 500);
    CHECK(r.size() == 2 && r[0] == "one" && r[1] == "two", "newline splits");

    // A blank line is deliberate spacing between paragraphs and must survive —
    // collapsing it would silently reflow every multi-paragraph modal.
    auto b = w("a\n\nb", 500);
    CHECK(b.size() == 3, "blank line preserved");
    CHECK(b[1].empty(), "and it is genuinely empty");
    std::printf("  ok: explicit newlines and blank lines\n");
}

// THE CASE THAT MATTERS FOR CONFIRMATION MODALS. A file path has no spaces, so
// space-based wrapping alone leaves it as one enormous line running off-screen.
static void test_long_token_breaks_mid_word() {
    const std::string path = "save:/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA.dat";  // 41
    auto r = w(path, 100);                                                   // 10/line
    CHECK(r.size() > 1, "a space-free token still breaks");
    for (const auto& line : r)
        CHECK(measure(line) <= 100, "every line fits the width");
    std::string joined;
    for (const auto& line : r) joined += line;
    CHECK(joined == path, "and nothing is lost or duplicated");
    std::printf("  ok: long space-free token breaks mid-word\n");
}

static void test_long_token_after_normal_words() {
    auto r = w("see /aaaaaaaaaaaaaaaaaaaaaaaa now", 100);
    for (const auto& line : r)
        CHECK(measure(line) <= 100, "every line fits");
    std::string joined;
    for (const auto& line : r) joined += line;
    CHECK(joined == "see /aaaaaaaaaaaaaaaaaaaaaaaa now", "content preserved");
    std::printf("  ok: long token mixed with normal words\n");
}

// A break must never land inside a multi-byte glyph — that renders as mojibake,
// and a title with an accented character is entirely ordinary.
static void test_utf8_not_split() {
    const std::string s = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";  // 6x e-acute
    auto r = w(s, 30);   // 3 glyphs per line
    for (const auto& line : r) {
        CHECK(line.size() % 2 == 0, "no line ends mid-sequence");
        for (size_t i = 0; i < line.size(); i += TextWrap::utf8_len(line, i))
            CHECK((unsigned char)line[i] == 0xC3, "each line starts on a lead byte");
    }
    std::string joined;
    for (const auto& line : r) joined += line;
    CHECK(joined == s, "content preserved across the break");
    std::printf("  ok: UTF-8 sequences are never split\n");
}

// Degenerate inputs must not hang or lose text — a wrap loop that fails to make
// progress is an infinite loop on the main thread, which on hardware looks like
// the console freezing rather than a text bug.
static void test_degenerate_inputs() {
    auto e = w("", 100);
    CHECK(e.size() == 1 && e[0].empty(), "empty text gives one empty line");

    auto zero = w("abc", 0);
    CHECK(zero.size() == 1 && zero[0] == "abc", "zero width returns text unchanged");

    auto tiny = w("abcdef", 5);   // narrower than a single glyph
    std::string joined;
    for (const auto& line : tiny) joined += line;
    CHECK(joined == "abcdef", "impossibly narrow width still terminates, text intact");
    std::printf("  ok: degenerate inputs terminate safely\n");
}

// The actual NAND (System) confirmation body — the string that was reported
// running off the screen.
static void test_real_confirmation_body() {
    const std::string body =
        "NAND (System) holds the console's firmware. Writing to it incorrectly can "
        "make the console unbootable, and a recovery may not be possible.\n\n"
        "Browsing stays read-only and every change still asks for confirmation. "
        "Enable this only if you know why you need it.";
    auto r = w(body, 560);   // MODAL_W 640 minus padding
    CHECK(r.size() > 4, "it wraps into several lines rather than two long ones");
    for (const auto& line : r)
        CHECK(measure(line) <= 560, "every line fits inside the modal");
    std::printf("  ok: the real NAND (System) body fits the modal\n");
}

int main() {
    std::printf("text_wrap_test\n");
    test_no_wrap_needed();
    test_wraps_at_spaces();
    test_explicit_newlines_and_blank_lines();
    test_long_token_breaks_mid_word();
    test_long_token_after_normal_words();
    test_utf8_not_split();
    test_degenerate_inputs();
    test_real_confirmation_body();
    std::printf("text_wrap_test: %d checks passed\n", g_checks);
    return 0;
}
