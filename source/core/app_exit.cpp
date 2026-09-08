// source/core/app_exit.cpp

#include "core/app_exit.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

// The mechanism, confirmed from the switchbrew Homebrew ABI spec and libnx's
// env.c (an earlier attempt with __nx_applet_exit_mode was wrong):
//
//   g_loaderRetAddr, set once at startup in env.c:
//     - NSO (a real title):  &svcExitProcess -> process terminates, Horizon
//       returns to the HOME menu.
//     - NRO (homebrew/forwarder): the loader's return address -> control jumps
//       back into the loader stub, which is why a plain `return` lands on
//       hbmenu even in Application mode.
//
//   To exit to HOME regardless of launch context, call svcExitProcess()
//   ourselves instead of returning through the loader, exactly like libnx's
//   own applet path. Plain always-available syscall (svc 0x7), NX_NORETURN.

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
    // Terminate like a title. Does NOT return; the caller must have finished
    // cleanup already. Reaches HOME from both launch contexts (no loader-stub
    // return), and because the process actually terminates, qlaunch re-reads
    // its application records, so freshly installed titles show up on HOME
    // without the "close the app first" dance (Bug D).
    svcExitProcess();
    __builtin_unreachable();
#endif
}

} // namespace Core
