// tests/save_commit_discipline_test.cpp
//
// Mechanically enforces the one rule that has now silently eaten user data twice:
//
//     EVERY filesystem mutation in a file that can see save paths must either
//     go through Services::SaveWrite::* (which commits), commit explicitly
//     within a few lines, or be marked "// NO-COMMIT: <reason>".
//
// A Switch save filesystem is journalled — writes are discarded at unmount unless
// fsdevCommitDevice() runs — so an uncommitted mutation looks like it worked and
// then reverts. There is nothing to see at the time and no error anywhere.
//
// It has failed twice by being a convention rather than a check:
//   * FileBrowserScreen never committed. Correct for its whole life, because it
//     only saw SD and NAND — until the Save Manager pointed it at "save:/" and
//     on-device deletes started reverting.
//   * do_new_dir() and do_new_file() were still live when this test was written:
//     creating a folder or file inside a save quietly vanished at unmount.
//
// So this reads the actual source and fails the build. It is a lint, not a unit
// test, which is unusual here — but the invariant is one no unit test can reach
// (it lives in the relationship between a call and its follow-up) and one that
// costs a user their save data when it breaks.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef GNX_SOURCE_DIR
#error "GNX_SOURCE_DIR must be defined by the build so this test can read the source"
#endif

static int g_checks = 0;
static int g_failures = 0;

// Files that can be pointed at a save path. FileBrowserScreen is on this list
// because the Save Manager opens it on "save:/" — the exact fact that made its
// missing commits a bug.
static const char* kSaveCapableFiles[] = {
    "screens/file_browser.cpp",
    "services/ftp_server.cpp",
    "services/mtp_server.cpp",
    "services/save_surface.cpp",
    "core/save_backup.cpp",
};

// Mutating operations. Both spellings matter: the screens use the Fs:: wrappers,
// the FTP server uses raw POSIX, and a missing commit is equally silent either way.
static const char* kMutators[] = {
    "Fs::make_directory(", "Fs::create_empty_file(", "Fs::rename(",
    "Fs::remove_file(", "Fs::remove_directory_recursive(", "Fs::remove_many(",
    "::mkdir(", "::rmdir(", "::remove(", "::rename(",
};

// Evidence that the mutation is accounted for.
static bool line_is_exempt(const std::string& line) {
    return line.find("NO-COMMIT:") != std::string::npos;
}
static bool line_commits(const std::string& line) {
    return line.find("SaveWrite::") != std::string::npos ||
           line.find("save_commit_if_save_path") != std::string::npos ||
           line.find("SaveMount::commit") != std::string::npos ||
           line.find("save_wipe") != std::string::npos;
}

static bool is_comment(const std::string& line) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return true;
    return line.compare(i, 2, "//") == 0 || line.compare(i, 1, "*") == 0;
}

int main() {
    std::printf("save_commit_discipline_test\n");

    for (const char* rel : kSaveCapableFiles) {
        const std::string path = std::string(GNX_SOURCE_DIR) + "/" + rel;
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("FAIL: cannot open %s\n", path.c_str());
            return 1;
        }
        std::vector<std::string> lines;
        for (std::string l; std::getline(f, l); ) lines.push_back(l);

        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            if (is_comment(line)) continue;

            bool mutates = false;
            for (const char* m : kMutators)
                if (line.find(m) != std::string::npos) { mutates = true; break; }
            if (!mutates) continue;

            ++g_checks;

            // Look in a small window around the mutation, not just at it. A commit
            // often lands on the continuation line of the same expression (FTP's
            // shape), and an explanatory NO-COMMIT marker naturally sits on the
            // comment line above the statement it explains.
            const size_t lo = (i >= 3) ? i - 3 : 0;
            bool accounted = false;
            for (size_t j = lo; j < lines.size() && j <= i + 3; ++j) {
                if (line_is_exempt(lines[j]) || line_commits(lines[j])) {
                    accounted = true;
                    break;
                }
            }

            if (!accounted) {
                std::printf("FAIL: %s:%zu mutates without a commit or NO-COMMIT marker\n"
                            "      %s\n", rel, i + 1, line.c_str());
                ++g_failures;
            }
        }
    }

    if (g_failures) {
        std::printf("\n%d unaccounted mutation(s) of %d checked.\n"
                    "Use Services::SaveWrite::* (commits for you), commit explicitly,\n"
                    "or mark the line '// NO-COMMIT: <reason>' when not committing is correct.\n",
                    g_failures, g_checks);
        return 1;
    }
    std::printf("save_commit_discipline_test: %d mutation site(s) all accounted for\n",
                g_checks);
    return 0;
}
