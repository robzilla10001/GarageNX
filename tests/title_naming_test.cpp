// tests/title_naming_test.cpp
//
// The round-trip is the property that matters: a name we generate for a listing
// must parse back to the same title when the client asks for it. If it doesn't,
// the user sees a file they cannot download.

#include "services/title_naming.hpp"

#include <cstdio>
#include <string>

static int g_checks = 0;
static int g_failed = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) { ++g_failed; std::printf("  FAIL: %s\n", what.c_str()); }
}

using namespace Services;
using Core::Ncm::Title;
using Core::Ncm::TitleType;

static Title mk(uint64_t meta, uint32_t ver, TitleType type, const std::string& name) {
    Title t;
    t.meta_id    = meta;
    t.program_id = meta;
    t.version    = ver;
    t.type       = type;
    t.name       = name;
    return t;
}

static void round_trip(const Title& t, const std::string& why) {
    const std::string fn = title_to_filename(t);
    const ParsedTitleName p = parse_title_filename(fn);
    check(p.ok, why + ": parses (" + fn + ")");
    check(p.meta_id == t.meta_id, why + ": meta_id survives");
    check(p.version == t.version, why + ": version survives");
    check(p.type    == t.type,    why + ": type survives");
}

int main() {
    std::printf("title_naming_test\n");

    // ── Ordinary cases ────────────────────────────────────────────────────────
    round_trip(mk(0x0100000000010000ull, 0, TitleType::Application, "Breath of the Wild"),
               "base title");
    round_trip(mk(0x0100000000010800ull, 65536, TitleType::Patch, "Breath of the Wild"),
               "update");
    round_trip(mk(0x0100000000011000ull, 3, TitleType::AddOnContent, "BOTW DLC Pack"),
               "dlc");

    // A title whose control NCA never resolved still has to be downloadable.
    round_trip(mk(0x0100ABCDEF012000ull, 0, TitleType::Application, ""), "unnamed title");

    // ── Hostile display names must not break the format ───────────────────────
    // Brackets delimit our metadata fields; a game name containing them must not be
    // able to forge a field and misdirect the parse to a different title.
    {
        Title t = mk(0x0100000000010000ull, 0, TitleType::Application,
                     "Fake [0100FFFFFFFFFFFF][BASE][v9]");
        const std::string fn = title_to_filename(t);
        const ParsedTitleName p = parse_title_filename(fn);
        check(p.ok, "spoofed-name title still parses");
        check(p.meta_id == 0x0100000000010000ull,
              "brackets in a display name cannot hijack the id (got the REAL id)");
    }
    round_trip(mk(0x0100000000010000ull, 0, TitleType::Application, "A/B:C*D?E\"F<G>H|I"),
               "name full of FAT-illegal characters");
    round_trip(mk(0x0100000000010000ull, 0, TitleType::Application,
                  std::string(400, 'X')), "absurdly long name");

    // Sanitiser guarantees.
    {
        const std::string s = sanitize_for_filename("bad/name:here");
        check(s.find('/') == std::string::npos && s.find(':') == std::string::npos,
              "sanitiser removes path separators");
        check(sanitize_for_filename("").size() > 0, "empty name becomes a placeholder");
        check(sanitize_for_filename("trailing...").back() != '.',
              "trailing dots stripped (FAT drops them silently)");
        check(sanitize_for_filename(std::string(400, 'X')).size() <= 128,
              "long names are capped");
    }

    // ── Things that are NOT ours must be rejected, not half-parsed ────────────
    check(!parse_title_filename("random.txt").ok,          "non-nsp rejected");
    check(!parse_title_filename("no-brackets.nsp").ok,     "missing fields rejected");
    check(!parse_title_filename("x [NOTHEX][BASE][v0].nsp").ok, "bad hex rejected");
    check(!parse_title_filename("x [0100][BASE][vX].nsp").ok,   "bad version rejected");
    check(!parse_title_filename("").ok,                    "empty rejected");
    check(!parse_title_filename(".nsp").ok,                "bare extension rejected");

    // Case-insensitive extension: some clients normalise it.
    {
        Title t = mk(0x0100000000010000ull, 0, TitleType::Application, "Game");
        std::string fn = title_to_filename(t);
        fn.replace(fn.size() - 4, 4, ".NSP");
        check(parse_title_filename(fn).ok, "uppercase .NSP accepted");
    }

    // ── Save folder naming ────────────────────────────────────────────────────
    {
        const std::string d = title_to_save_dirname(
            mk(0x0100000000010000ull, 0, TitleType::Application, "Breath of the Wild"));
        check(d.find(".nsp") == std::string::npos, "save dir has no file extension");
        check(d.find("0100000000010000") != std::string::npos,
              "save dir carries the application id");
    }

    std::printf("%s (%d checks", g_failed ? "FAILURES" : "ALL PASS", g_checks);
    if (g_failed) std::printf(", %d failed", g_failed);
    std::printf(")\n");
    return g_failed ? 1 : 0;
}
