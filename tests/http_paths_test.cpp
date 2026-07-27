// tests/http_paths_test.cpp
//
// The HTTP install-path classifier. Pure string work that decides whether a PUT
// installs or writes a plain file, and to which target — so a bug here either
// silently drops an upload on the SD as a raw file (no install) or routes it to
// the wrong storage. Mirrors ftp_paths_test.

#include "services/http_paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Services;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static void test_install_targets() {
    std::string leaf;
    CHECK(http_classify("/install/sd/Game.nsz", leaf) == HttpTarget::SdInstall,
          "/install/sd routes to SD");
    CHECK(leaf == "Game.nsz", "and keeps the filename");

    CHECK(http_classify("/install/nand/Title.nsp", leaf) == HttpTarget::NandInstall,
          "/install/nand routes to NAND");
    CHECK(leaf == "Title.nsp", "filename kept");
    std::printf("  ok: install targets\n");
}

static void test_plain_filesystem() {
    std::string leaf;
    CHECK(http_classify("/some/file.txt", leaf) == HttpTarget::Filesystem,
          "an ordinary path is filesystem");
    CHECK(leaf.empty(), "no leaf for a filesystem path");
    CHECK(http_classify("/", leaf) == HttpTarget::Filesystem, "root is filesystem");
    // A folder literally named "install" that is NOT our prefix chain still needs
    // a valid target to be an install; "/install" alone is invalid, not a write.
    CHECK(http_classify("/installer/x.nsz", leaf) == HttpTarget::Filesystem,
          "'installer' is not the install prefix");
    std::printf("  ok: plain filesystem paths\n");
}

static void test_invalid_install_paths() {
    std::string leaf;
    CHECK(http_classify("/install/sd/", leaf) == HttpTarget::Invalid,
          "install target with no filename is invalid");
    CHECK(http_classify("/install/sd", leaf) == HttpTarget::Invalid,
          "install target with no trailing file is invalid");
    CHECK(http_classify("/install/bogus/x.nsz", leaf) == HttpTarget::Invalid,
          "unknown install target is invalid");
    CHECK(http_classify("/install", leaf) == HttpTarget::Invalid,
          "bare /install is invalid");
    std::printf("  ok: invalid install paths\n");
}

static void test_nested_path_keeps_leaf() {
    std::string leaf;
    // Installs take a single file; a nested path keeps only the leaf so a client
    // that sends a directory-ish path still installs the file it named.
    CHECK(http_classify("/install/sd/sub/dir/Game.nsz", leaf) == HttpTarget::SdInstall,
          "nested path still routes to SD");
    CHECK(leaf == "Game.nsz", "leaf is the last component");
    std::printf("  ok: nested path keeps the leaf\n");
}

static void test_percent_decoding() {
    // Real title names contain spaces and punctuation that a browser or curl
    // percent-encodes. The leaf must be decoded so the install log and any
    // filename handling see the true name.
    std::string leaf;
    CHECK(http_classify("/install/sd/Super%20Game.nsz", leaf) == HttpTarget::SdInstall,
          "encoded space routes fine");
    CHECK(leaf == "Super Game.nsz", "%20 decodes to space");

    CHECK(http_classify("/install/nand/A%2BB%20%5BUSA%5D.nsp", leaf) == HttpTarget::NandInstall,
          "several escapes");
    CHECK(leaf == "A+B [USA].nsp", "%2B %20 %5B %5D decode");

    // A malformed escape is left literal, never dropped — corrupting a filename
    // silently is worse than an ugly one.
    CHECK(http_percent_decode("bad%2") == "bad%2", "truncated escape left literal");
    CHECK(http_percent_decode("bad%zz") == "bad%zz", "non-hex escape left literal");
    std::printf("  ok: percent decoding\n");
}

static void test_query_string_stripped() {
    std::string leaf;
    CHECK(http_classify("/install/sd/Game.nsz?token=abc", leaf) == HttpTarget::SdInstall,
          "a query string does not break routing");
    CHECK(leaf == "Game.nsz", "and is not glued onto the filename");
    std::printf("  ok: query string stripped\n");
}

static void test_is_install_path() {
    CHECK(http_is_install_path("/install/sd/x.nsz"), "sd install recognised");
    CHECK(http_is_install_path("/install/nand/x.nsp"), "nand install recognised");
    CHECK(!http_is_install_path("/regular/file.bin"), "plain path is not install");
    CHECK(!http_is_install_path("/install/sd/"), "no-filename install is not install");
    std::printf("  ok: is_install_path predicate\n");
}

// The web-UI routing helpers. These decide whether a request is the UI page, an
// API call, or a file — so a bug sends a browser a raw file where the app should
// be, or hands an API URL to the filesystem.
static void test_path_only_and_query() {
    CHECK(http_path_only("/api/list?path=/foo") == "/api/list", "query stripped");
    CHECK(http_path_only("/api/list") == "/api/list", "no query is unchanged");
    CHECK(http_path_only("/") == "/", "root unchanged");

    CHECK(http_query_param("/api/list?path=/foo", "path") == "/foo", "value read");
    CHECK(http_query_param("/api/list?a=1&path=/x&b=2", "path") == "/x",
          "value read from the middle");
    CHECK(http_query_param("/api/list", "path").empty(), "absent query gives empty");
    CHECK(http_query_param("/api/list?other=1", "path").empty(), "absent key gives empty");

    // A directory with a space: the browser encodes it, we must decode it back or
    // the listing would 404 on exactly the folders users name casually.
    CHECK(http_query_param("/api/list?path=/My%20Games", "path") == "/My Games",
          "%20 decodes in a query value");
    CHECK(http_query_param("/api/list?path=/My+Games", "path") == "/My Games",
          "'+' is a space inside a query string");
    std::printf("  ok: path_only and query_param\n");
}

static void test_route_predicates() {
    CHECK(http_is_ui_path("/"), "root serves the UI");
    CHECK(http_is_ui_path("/index.html"), "index.html serves the UI");
    CHECK(!http_is_ui_path("/api/list"), "an API path is not the UI");
    CHECK(!http_is_ui_path("/some/file.bin"), "a file is not the UI");

    CHECK(http_is_api_path("/api/list?path=/"), "api list");
    CHECK(http_is_api_path("/api/status"), "api status");
    CHECK(!http_is_api_path("/"), "root is not api");
    CHECK(!http_is_api_path("/apifoo"), "a lookalike prefix is not api");
    CHECK(!http_is_api_path("/install/sd/x.nsz"), "install is not api");
    std::printf("  ok: route predicates\n");
}

// The exact composition the server uses to turn a request target into a
// filesystem path: strip the query, THEN percent-decode. Pinned because getting
// the order or the presence of either step wrong is a silent 404 on ordinary
// files — the web UI made this obvious (every title name with a space failed to
// download) but it was latent the whole time HTTP was curl-only.
static void test_request_target_to_fs_path() {
    auto fs = [](const std::string& t) {
        return http_percent_decode(http_path_only(t));
    };
    CHECK(fs("/My%20Game.nsp") == "/My Game.nsp", "a space decodes");
    CHECK(fs("/Games/Zelda%20%5BUSA%5D.nsp") == "/Games/Zelda [USA].nsp",
          "brackets decode, slashes preserved");
    CHECK(fs("/plain.bin") == "/plain.bin", "an unencoded path is unchanged");
    CHECK(fs("/file.bin?x=1") == "/file.bin", "query removed before decoding");

    // Order matters: decoding first would turn "%3F" into '?' and then the query
    // strip would truncate a legitimate filename.
    CHECK(fs("/weird%3Fname.nsp") == "/weird?name.nsp",
          "an encoded '?' survives as part of the NAME, not as a query");
    std::printf("  ok: request target to filesystem path\n");
}

int main() {
    std::printf("http_paths_test\n");
    test_install_targets();
    test_plain_filesystem();
    test_invalid_install_paths();
    test_nested_path_keeps_leaf();
    test_percent_decoding();
    test_query_string_stripped();
    test_is_install_path();
    test_path_only_and_query();
    test_route_predicates();
    test_request_target_to_fs_path();
    std::printf("http_paths_test: %d checks passed\n", g_checks);
    return 0;
}
