// tests/host_stubs/SDL2/SDL.h
//
// A REAL SDL_Log for the host test build, not a lie.
//
// config.cpp is otherwise pure C++ and belongs in the host suite (its migration
// logic is exactly the kind of thing that must be pinned by a test), but it logs
// through SDL_Log and SDL2 is not present in the test environment. This provides
// a working implementation that writes to stderr.
//
// This is DIFFERENT in kind from tools/stubs/, which are empty files that exist
// only so the parser runs. This one actually does the thing its name promises,
// which is why a test that reaches it behaves sensibly rather than silently.
//
// Do NOT grow this into a general SDL shim. If a file needs more of SDL than a
// log call, it is not a pure file and does not belong in the host suite — see the
// admission rule at the top of tests/CMakeLists.txt.
#pragma once

#include <cstdarg>
#include <cstdio>

static inline void SDL_Log(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
