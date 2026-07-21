// source/core/app_exit.cpp

#include "core/app_exit.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

// The mechanism, confirmed from the switchbrew Homebrew ABI spec and libnx's own
// env.c (not guessed — an earlier attempt with __nx_applet_exit_mode was wrong):
//
//   The address the program returns to on exit is g_loaderRetAddr, set once at
//   startup in env.c:
//     - NSO (a real title):  g_loaderRetAddr = &svcExitProcess  -> process
//       terminates, Horizon returns to the HOME menu.
//     - NRO (homebrew/forwarder): g_loaderRetAddr = the loader's return address
//       -> control jumps back into the loader stub (hbmenu / forwarder stub),
//       which is why a plain `return` lands on hbmenu even in Application mode:
//       the forwarder stub embeds a loader and intercepts the return.
//
//   Homebrew ABI spec: "Original LR given to entrypoint should be returned to...
//   If original LR is NULL, svcExitProcess should be used."
//
//   So to exit to HOME regardless of how we were launched, we do what a title
//   does: call svcExitProcess() ourselves instead of returning through the
//   loader. libnx itself ends the applet path with `svcExitProcess();
//   __builtin_unreachable();`. This is a plain, always-available syscall
//   (svc 0x7), NX_NORETURN — no fragile globals, no applet-type dependence.

namespace Core {

bool in_applet_mode() {
#ifdef PLATFORM_SWITCH
    switch (appletGetAppletType()) {
        case AppletType_Application:
        case AppletType_SystemApplication:
            return false;
        default:
            return true;
    }
#else
    return false;
#endif
}

void exit_to_home() {
#ifdef PLATFORM_SWITCH
    // Terminate like a title. This does NOT return. The caller must have already
    // done its cleanup (renderer/services/romfs teardown) before calling this,
    // because execution stops here — HOME regains foreground immediately.
    //
    // Why this reaches HOME from BOTH launch contexts:
    //   - Forwarder / Application: we skip the loader-stub return that would send
    //     us to hbmenu, and terminate directly -> HOME.
    //   - hbmenu/Sphaira applet mode: same -- we don't hand back to the loader.
    // And because the process actually terminates (as a real title exit does),
    // qlaunch re-reads its application records, so freshly installed titles show
    // up on HOME without the old "close the app first" dance (Bug D).
    svcExitProcess();
    __builtin_unreachable();
#endif
}

} // namespace Core
