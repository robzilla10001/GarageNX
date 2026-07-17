// tests/link_stubs.cpp
//
// LINK-TIME stubs for symbols the host suites reference but must never execute.
//
// Read this before adding anything here. tests/CMakeLists.txt states the rule:
// do not stub a dependency to manufacture a green light, because a passing stub
// proves only that the stub works. This file does not break that rule, and the
// distinction is worth being precise about:
//
//   A BEHAVIOURAL stub stands in for logic while the test runs, and the test's
//   result then depends on the stub. That is forbidden here — it is how you end
//   up "testing" ncm or usb:ds on a machine that has neither.
//
//   A LINK stub satisfies the linker for code the test never reaches. The
//   result depends on nothing it does.
//
// The safeguard is that these abort. If a test ever actually reaches one, it
// dies loudly instead of quietly passing against a fiction. A stub that could
// return a plausible value is a stub that could hide a bug.
//
// Install::install() lives in installer.cpp, which is bound to libnx (es, ncm,
// fs) and cannot link on the host. Its only caller is StreamInstaller::finish(),
// which the container suite deliberately never calls: registering meta and
// importing tickets is hardware work and belongs on the device.

#include "core/keys.hpp"
#include "install/installer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Install {

bool install(std::vector<ContentEntry>, Core::Ncm::Storage,
             const Core::Keys::Keyset&, Progress&, bool) {
    std::fprintf(stderr,
        "\n*** link stub reached: Install::install() ***\n"
        "The host suites must not call finish(). Meta registration and ticket\n"
        "import are hardware paths — test them on the device, not here.\n");
    std::abort();
}

} // namespace Install

namespace Core::Keys {

// Reached, unlike install(): parse_table() calls this to build the refusal text
// when an NSZ arrives without a header key. So it cannot abort — but it returns
// an obviously synthetic marker rather than an imitation of the real message.
// Tests assert that the refusal HAPPENED, never what this says. If an assertion
// here ever depends on this string, the assertion is testing this file.
std::string requirement_message() {
    return "<link stub: not the real keys requirement message>";
}

} // namespace Core::Keys
