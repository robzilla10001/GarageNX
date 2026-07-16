// source/ui/keyboard.cpp

#include "ui/keyboard.hpp"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

namespace Keyboard {

bool get_text(const Options& opts, std::string& out) {
#ifdef PLATFORM_SWITCH
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) {
        SDL_Log("Keyboard::get_text — swkbdCreate failed: 0x%x", rc);
        return false;
    }

    swkbdConfigMakePresetDefault(&kbd);

    if (!opts.header.empty())
        swkbdConfigSetHeaderText(&kbd, opts.header.c_str());
    if (!opts.ok_text.empty())
        swkbdConfigSetOkButtonText(&kbd, opts.ok_text.c_str());
    if (!opts.initial_text.empty())
        swkbdConfigSetInitialText(&kbd, opts.initial_text.c_str());

    swkbdConfigSetStringLenMax(&kbd, opts.max_length);
    // Disable the OK button when the field is empty, unless explicitly allowed.
    if (!opts.allow_empty)
        swkbdConfigSetStringLenMin(&kbd, 1);

    // Buffer for the result
    char buffer[FS_MAX_PATH];   // generous; swkbd caps at max_length anyway
    std::memset(buffer, 0, sizeof(buffer));

    rc = swkbdShow(&kbd, buffer, sizeof(buffer));
    swkbdClose(&kbd);

    if (R_FAILED(rc)) {
        // User cancelled, or an error occurred. Both treated as "no input".
        return false;
    }

    out = buffer;
    // Guard against a confirmed-but-empty result when empties aren't allowed.
    if (!opts.allow_empty && out.empty()) return false;
    return true;

#else
    // PC fallback — read a line from stdin so development flows work.
    printf("\n[keyboard] %s", opts.header.c_str());
    if (!opts.initial_text.empty())
        printf(" (default: %s)", opts.initial_text.c_str());
    printf("\n[keyboard] > ");
    fflush(stdout);

    char line[1024];
    if (!fgets(line, sizeof(line), stdin)) return false;

    // Strip trailing newline
    size_t len = std::strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
        line[--len] = '\0';
    }

    out = (len == 0 && !opts.initial_text.empty()) ? opts.initial_text : std::string(line);
    if (!opts.allow_empty && out.empty()) return false;
    return true;
#endif
}

bool get_number(const std::string& header, int initial, int& out) {
    Options opts;
    opts.header       = header;
    opts.initial_text = std::to_string(initial);
    opts.max_length   = 10;

#ifdef PLATFORM_SWITCH
    // swkbd supports a numeric preset; set it up inline here rather than in
    // get_text so the general path stays text-oriented.
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, opts.initial_text.c_str());
    swkbdConfigSetStringLenMax(&kbd, opts.max_length);

    char buffer[32];
    std::memset(buffer, 0, sizeof(buffer));
    Result rc = swkbdShow(&kbd, buffer, sizeof(buffer));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;

    out = atoi(buffer);
    return true;
#else
    std::string s;
    if (!get_text(opts, s)) return false;
    out = atoi(s.c_str());
    return true;
#endif
}

} // namespace Keyboard
