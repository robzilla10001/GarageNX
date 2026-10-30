// tests/lockpick_fetch_test.cpp
//
// The pure decisions behind the Lockpick_RCM_Pro download: which release asset to
// fetch, and which zip entry is the payload. The entry match matters for safety -
// the destination is fixed, so an archive path like "../../x/Lockpick_RCM_pro.bin"
// must resolve to the same file name and nothing else.

#include "services/lockpick_fetch.hpp"

#include <cstdio>
#include <cstdlib>

namespace LF = Services::LockpickFetch;

static int g_checks = 0;
#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL: %s (%s:%d)\n", (what), __FILE__, __LINE__);       \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

int main() {
    std::string tag;
    const char* rel = R"({"tag_name":"v1.2.3","assets":[
        {"name":"notes.txt","browser_download_url":"https://x/notes.txt"},
        {"name":"Other.zip","browser_download_url":"https://x/other.zip"},
        {"name":"Lockpick_RCM_Pro_v1.2.3.ZIP","browser_download_url":"https://x/lp.zip"}]})";
    CHECK(LF::pick_zip_url(rel, &tag) == "https://x/lp.zip", "prefers the lockpick zip");
    CHECK(tag == "v1.2.3", "tag read");

    CHECK(LF::pick_zip_url(R"({"assets":[{"name":"a.zip","browser_download_url":"https://x/a.zip"}]})")
              == "https://x/a.zip", "falls back to any zip");
    CHECK(LF::pick_zip_url(R"({"assets":[{"name":"a.zip","browser_download_url":"http://x/a.zip"}]})")
              .empty(), "plain http refused");
    CHECK(LF::pick_zip_url(R"({"assets":[{"name":"a.bin","browser_download_url":"https://x/a.bin"}]})")
              .empty(), "no zip asset");
    CHECK(LF::pick_zip_url("not json").empty(), "malformed json");
    CHECK(LF::pick_zip_url("[]").empty(), "wrong json shape");
    CHECK(LF::pick_zip_url(R"({"assets":[{"name":5}]})").empty(), "bad asset fields");

    CHECK(LF::find_payload_entry({"README.md", "payloads/Lockpick_RCM_pro.bin"}) == 1, "nested entry");
    CHECK(LF::find_payload_entry({"lockpick_rcm_pro.BIN"}) == 0, "case-insensitive");
    CHECK(LF::find_payload_entry({"a\\b\\Lockpick_RCM_pro.bin"}) == 0, "backslash path");
    CHECK(LF::find_payload_entry({"../../evil/Lockpick_RCM_pro.bin"}) == 0, "traversal resolves to base name");
    CHECK(LF::find_payload_entry({"Lockpick_RCM.bin", "x_Lockpick_RCM_pro.bin"}) == -1, "near misses rejected");
    CHECK(LF::find_payload_entry({}) == -1, "empty archive");

    std::printf("lockpick_fetch_test: %d checks passed\n", g_checks);
    return 0;
}
