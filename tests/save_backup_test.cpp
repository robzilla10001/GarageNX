// tests/save_backup_test.cpp
//
// Backup destination naming. Pure string work, and the part of save backup most
// likely to fail on exactly the saves a user cares about.
//
// Save folders are named after ACCOUNT NICKNAMES and GAME TITLES. Neither is
// constrained to anything: real Switch titles contain ':' ("Pokemon: Let's Go"),
// '?' ("Who Wants to Be a Millionaire?"), '/' in some regional names, and
// trailing dots. FAT32 and exFAT reject those characters outright, so an
// unsanitised path does not produce an ugly folder name — it produces a FAILED
// COPY, silently, on the titles most likely to be backed up.
//
// The copy itself needs a mounted save and stays on hardware. This does not.

#include "core/save_backup.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Core::SaveBackup;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static bool has_illegal(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || std::string("\\/:*?\"<>|").find((char)c) != std::string::npos)
            return true;
    return false;
}

static void test_real_title_names() {
    // Every one of these is a shape that exists on real hardware.
    const char* titles[] = {
        "Pokemon: Let's Go, Pikachu! [0100000000010000]",
        "Who Wants to Be a Millionaire? [0100ABCDEF012000]",
        "Nights of Azure 2: Bride of the New Moon [010012345678A000]",
        "Sonic Mania [0100DEADBEEF0000]",
    };
    for (const char* t : titles) {
        const std::string s = sanitize_component(t);
        CHECK(!s.empty(), "a real title never sanitizes to nothing");
        CHECK(!has_illegal(s), "no illegal character survives");
    }
    // The colon is the common one, and the one that would fail the copy.
    CHECK(sanitize_component("Pokemon: Let's Go").find(':') == std::string::npos,
          "colon is removed");
    std::printf("  ok: real title names are made safe\n");
}

static void test_illegal_characters() {
    CHECK(!has_illegal(sanitize_component("a\\b/c:d*e?f\"g<h>i|j")),
          "every illegal character is replaced");
    CHECK(!has_illegal(sanitize_component(std::string("tab\there") + '\n')),
          "control characters are replaced");
    // Runs collapse, so "A: B" does not become "A__B".
    CHECK(sanitize_component("A:/B") == "A_B", "runs of illegal chars collapse to one");
    std::printf("  ok: illegal and control characters\n");
}

// FAT silently DROPS trailing dots and spaces. A caller that built a path ending
// in one would then look for a directory that does not exist under that name —
// the copy target and the created directory disagree, which is a confusing
// failure to debug on a console.
static void test_trailing_dots_and_spaces() {
    CHECK(sanitize_component("Game.") == "Game", "trailing dot stripped");
    CHECK(sanitize_component("Game   ") == "Game", "trailing spaces stripped");
    CHECK(sanitize_component("Game...  ") == "Game", "both, repeatedly");
    CHECK(sanitize_component("  .Game") == "Game", "leading dots and spaces too");
    std::printf("  ok: trailing dots and spaces\n");
}

static void test_degenerate_names() {
    CHECK(sanitize_component("") == "unnamed", "empty gets a usable name");
    CHECK(sanitize_component("///") == "unnamed", "all-illegal gets a usable name");
    CHECK(sanitize_component("...") == "unnamed", "all-dots gets a usable name");
    CHECK(sanitize_component("   ") == "unnamed", "all-spaces gets a usable name");
    std::printf("  ok: degenerate names still produce a directory\n");
}

// A nickname can be any UTF-8. Truncating mid-sequence leaves a broken glyph in a
// FILENAME, which is worse than in a label: some tools refuse the name entirely.
static void test_truncation_is_utf8_safe() {
    std::string long_jp;
    for (int i = 0; i < 60; ++i) long_jp += "\xE3\x83\x86";   // 3-byte katakana TE
    const std::string s = sanitize_component(long_jp, 20);
    CHECK(s.size() <= 20, "respects the cap");
    CHECK(s.size() % 3 == 0, "cut on a code-point boundary, not mid-sequence");
    CHECK(!s.empty(), "and something survives");

    const std::string ascii = sanitize_component(std::string(200, 'a'), 96);
    CHECK(ascii.size() == 96, "ASCII truncates to exactly the cap");
    std::printf("  ok: truncation respects UTF-8 boundaries\n");
}

static void test_backup_dir_layout() {
    const std::string d = backup_dir_for("sdmc:/switch/GarageNX/backups",
                                         "Rob", "Zelda [0100000000010000]",
                                         "20260712-143005");
    CHECK(d == "sdmc:/switch/GarageNX/backups/Rob/Zelda [0100000000010000]/20260712-143005",
          "layout is root/user/title/stamp");

    // A trailing slash on the configured root must not double up.
    const std::string d2 = backup_dir_for("sdmc:/backups/", "Rob", "G [01]", "S");
    CHECK(d2 == "sdmc:/backups/Rob/G [01]/S", "trailing slash on the root is absorbed");

    // The device prefix must survive: "sdmc:" is not an illegal-character case.
    CHECK(d.compare(0, 5, "sdmc:") == 0, "device prefix is preserved");
    std::printf("  ok: backup directory layout\n");
}

// Two different awkward titles must not collapse onto the SAME directory, or one
// backup silently lands inside another game's folder.
static void test_distinct_titles_stay_distinct() {
    const std::string a = sanitize_component("Game: One [0100000000010000]");
    const std::string b = sanitize_component("Game: Two [0100000000020000]");
    CHECK(a != b, "different titles keep different folder names");

    // The application id is what actually disambiguates, and it is all legal
    // characters, so it always survives sanitization intact.
    CHECK(a.find("0100000000010000") != std::string::npos, "app id survives");
    CHECK(b.find("0100000000020000") != std::string::npos, "app id survives");
    std::printf("  ok: distinct titles stay distinct\n");
}

// ── Backup listing: what may be offered as a RESTORE SOURCE ─────────────────
//
// This filter is a safety boundary, not tidiness. An SD card is the user's own:
// stray folders, half-extracted archives, ".DS_Store" from a Mac, a directory
// they made by hand — all can sit beside real backups. Offering one of those as a
// restore source would REPLACE a save with garbage, and restore is the one
// operation here that cannot be undone from inside the game.
static void test_stamp_recognition() {
    CHECK(is_backup_stamp("20260712-143005"), "a real stamp is accepted");
    CHECK(is_backup_stamp("19700101-000000"), "any valid digits are accepted");

    CHECK(!is_backup_stamp(""), "empty rejected");
    CHECK(!is_backup_stamp("20260712"), "date alone rejected");
    CHECK(!is_backup_stamp("20260712-14300"), "too short rejected");
    CHECK(!is_backup_stamp("20260712-1430055"), "too long rejected");
    CHECK(!is_backup_stamp("20260712_143005"), "wrong separator rejected");
    CHECK(!is_backup_stamp("2026071x-143005"), "non-digit rejected");
    CHECK(!is_backup_stamp(".DS_Store"), "junk from a PC rejected");
    CHECK(!is_backup_stamp("my old save"), "a hand-made folder rejected");
    CHECK(!is_backup_stamp("20260712-143005 (1)"), "a duplicated folder rejected");
    std::printf("  ok: only real stamps are offered as restore sources\n");
}

// Newest first, because a restore list where the newest is not obvious is a list
// that invites picking the wrong one. Stamps are fixed-width and zero-padded, so
// descending lexicographic IS reverse chronological — the reason sortable_stamp()
// exists instead of reusing log_stamp(), which follows the user's date order.
static void test_sort_newest_first() {
    std::vector<std::string> v = {
        "20250101-000000", "20260712-143005", "20260712-143004", "20251231-235959",
    };
    sort_newest_first(v);
    CHECK(v[0] == "20260712-143005", "newest first");
    CHECK(v[1] == "20260712-143004", "one second earlier is second");
    CHECK(v[2] == "20251231-235959", "then the previous year end");
    CHECK(v[3] == "20250101-000000", "oldest last");

    // The case that would break a naive sort on a DMY-ordered stamp: a later day
    // in an earlier month must still sort older.
    std::vector<std::string> v2 = { "20260805-000000", "20261207-000000" };
    sort_newest_first(v2);
    CHECK(v2[0] == "20261207-000000", "December beats August regardless of day");
    std::printf("  ok: backups sort newest first\n");
}

static void test_title_dir_is_parent_of_run_dir() {
    const std::string root  = "sdmc:/switch/GarageNX/backups";
    const std::string title = backup_title_dir(root, "Rob", "Zelda [0100000000010000]");
    const std::string run   = backup_dir_for(root, "Rob", "Zelda [0100000000010000]",
                                             "20260712-143005");
    CHECK(run.compare(0, title.size(), title) == 0,
          "the run directory sits inside the title directory");
    CHECK(run == title + "/20260712-143005", "and differs only by the stamp");

    // Both must sanitize identically, or listing would look in a different place
    // than creating wrote to — a bug that only appears on awkward title names.
    const std::string t2 = backup_title_dir(root, "Rob", "Pokemon: Let's Go");
    const std::string r2 = backup_dir_for(root, "Rob", "Pokemon: Let's Go", "S");
    CHECK(r2.compare(0, t2.size(), t2) == 0,
          "listing and creating agree on awkward names too");
    std::printf("  ok: title dir is the parent of every run dir\n");
}

// The app-id parser recreation depends on. Once a save is deleted, the backup's
// title-label FOLDER NAME is the only surviving record of which title it belongs
// to, and the id inside "[...]" is what recreating the save needs. A wrong parse
// here means restoring an orphaned backup recreates the WRONG title's save, or
// none — so this is pinned hard.
static void test_app_id_from_label() {
    CHECK(app_id_from_label("Zelda [0100000000010000]") == 0x0100000000010000ULL,
          "the id inside the bracket");
    CHECK(app_id_from_label("Sonic Mania [0100DEADBEEF0000]") == 0x0100DEADBEEF0000ULL,
          "hex letters parse");
    CHECK(app_id_from_label("game [0100abcdef012000]") == 0x0100ABCDEF012000ULL,
          "lowercase hex parses too");

    // A game name that itself contains brackets: the id is the LAST group.
    CHECK(app_id_from_label("Ys [Memoire] [010012345678A000]") == 0x010012345678A000ULL,
          "the id is taken from the final bracket group, not the first");

    // Things that carry no id -> 0, so the caller refuses rather than guesses.
    CHECK(app_id_from_label("No Brackets Here") == 0, "no bracket at all");
    CHECK(app_id_from_label("Game [tooshort]") == 0, "not 16 hex digits");
    CHECK(app_id_from_label("Game [010000000001000g]") == 0, "a non-hex digit");
    CHECK(app_id_from_label("Game [01000000000100000]") == 0, "17 digits rejected");
    CHECK(app_id_from_label("") == 0, "empty");
    CHECK(app_id_from_label("[]") == 0, "empty brackets");

    // The ID-ONLY FALLBACK form. A backup taken while the title name was not
    // resolvable is written to "Title 0100.../" and keeps that folder name
    // forever. Recovering the id from it is what lets the Manage Backups list
    // display the real game name for backups already on the card, without
    // renaming anything on disk.
    CHECK(app_id_from_label("Title 0100000000010000") == 0x0100000000010000ULL,
          "id recovered from the fallback form");
    CHECK(app_id_from_label("Title abcdef0123456789") == 0xABCDEF0123456789ULL,
          "lowercase hex in the fallback form");
    CHECK(app_id_from_label("Title 010000000001000") == 0, "fallback, 15 digits");
    CHECK(app_id_from_label("Title 010000000001000g") == 0, "fallback, non-hex");
    CHECK(app_id_from_label("Titles") == 0, "a game actually called 'Titles'");
    CHECK(app_id_from_label("Title Quest") == 0, "a game actually called 'Title Quest'");
    std::printf("  ok: app id parsed from backup label\n");
}

// Auto-backup staleness — the pure decision behind 3e-c. A bug here is invisible
// in the worst way: too-eager backs up every save on every launch (SD thrash),
// too-lax never backs up and the "safety net" silently does nothing. Both are
// caught here rather than by watching a console.
static void test_stamp_day_number() {
    // Known day counts from the civil epoch (1970-01-01 == 0).
    CHECK(stamp_day_number("19700101-000000") == 0, "epoch day is 0");
    CHECK(stamp_day_number("19700102-000000") == 1, "next day is 1");
    CHECK(stamp_day_number("19700201-000000") == 31, "31 days into February");

    // Hour-of-day must NOT affect the day number — staleness is whole calendar
    // days, so two stamps on the same date are zero days apart no matter the time.
    CHECK(stamp_day_number("20260712-000001") == stamp_day_number("20260712-235959"),
          "same date, different time -> same day number");

    // Malformed -> -1.
    CHECK(stamp_day_number("") == -1, "empty");
    CHECK(stamp_day_number("2026071-143005") == -1, "too short");
    CHECK(stamp_day_number("20261301-000000") == -1, "month 13 rejected");
    CHECK(stamp_day_number("20260732-000000") == -1, "day 32 rejected");
    std::printf("  ok: stamp day number\n");
}

static void test_days_between_and_leap() {
    CHECK(days_between_stamps("20260101-120000", "20260108-080000") == 7,
          "one week apart, ignoring the earlier hour");
    CHECK(days_between_stamps("20251231-000000", "20260101-000000") == 1,
          "across a year boundary");
    // 2024 is a leap year: Feb 28 -> Mar 1 is 2 days, not 1.
    CHECK(days_between_stamps("20240228-000000", "20240301-000000") == 2,
          "leap-year February handled");
    // 2023 is not: Feb 28 -> Mar 1 is 1 day.
    CHECK(days_between_stamps("20230228-000000", "20230301-000000") == 1,
          "non-leap February handled");
    // A malformed newest is treated as very stale, so a broken backup name never
    // blocks a fresh backup.
    CHECK(days_between_stamps("garbage", "20260101-000000") > 1000000,
          "malformed newest is treated as ancient");
    std::printf("  ok: days between stamps (incl. leap years)\n");
}

static void test_save_is_stale() {
    const std::string now = "20260712-120000";

    // Feature off: never stale, whatever the history.
    CHECK(!save_is_stale("", now, 0), "threshold 0 = off, even with no backup");
    CHECK(!save_is_stale("19700101-000000", now, 0), "threshold 0 = off, ancient backup");
    CHECK(!save_is_stale("", now, -5), "negative threshold also off");

    // Enabled, never backed up: always stale.
    CHECK(save_is_stale("", now, 7), "no backup is stale once enabled");

    // Enabled, boundary behaviour: stale at exactly the threshold, not before.
    CHECK(!save_is_stale("20260710-120000", now, 7), "2 days old, 7-day threshold: fresh");
    CHECK(!save_is_stale("20260706-120000", now, 7), "6 days old: still fresh");
    CHECK(save_is_stale("20260705-120000", now, 7), "7 days old: stale");
    CHECK(save_is_stale("20260601-120000", now, 7), "much older: stale");

    // A backup made TODAY is never stale for any positive threshold.
    CHECK(!save_is_stale(now, now, 1), "backed up today is fresh at 1 day");
    std::printf("  ok: save staleness decision\n");
}

int main() {
    std::printf("save_backup_test (destination naming)\n");
    test_real_title_names();
    test_illegal_characters();
    test_trailing_dots_and_spaces();
    test_degenerate_names();
    test_truncation_is_utf8_safe();
    test_backup_dir_layout();
    test_distinct_titles_stay_distinct();
    test_stamp_recognition();
    test_sort_newest_first();
    test_title_dir_is_parent_of_run_dir();
    test_app_id_from_label();
    test_stamp_day_number();
    test_days_between_and_leap();
    test_save_is_stale();
    std::printf("save_backup_test: %d checks passed\n", g_checks);
    return 0;
}
