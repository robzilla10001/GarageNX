// source/core/app_exit.hpp
//
// Correct "exit to HOME" for BOTH launch methods, chosen at runtime.
//
// GarageNX can be launched two ways, and each needs a different exit:
//   - Forwarder / own HOME icon  -> runs as AppletType_Application. A normal
//     return already lands on HOME, and because a real application exited,
//     qlaunch re-reads its application records (so freshly installed titles show
//     up without a manual relaunch).
//   - Sphaira / hbmenu (an NRO)  -> runs in APPLET MODE (AppletType_LibraryApplet
//     et al.) under nx-hbloader. A plain return hands control back to hbloader,
//     which shows hbmenu — NOT HOME. To reach HOME we ask libnx to exit the
//     process to the menu instead of returning into the loader.
//
// The context is detected with appletGetAppletType() — the same signal nx-hbmenu
// uses for its "Applet Mode" indicator and that Goldleaf uses to report how it
// was launched. See app_exit.cpp for the mechanism and the hardware-verify notes.

#pragma once

namespace Core {

// True when running in applet mode (launched as an NRO under hbloader in applet
// context). Informational — the exit path below no longer depends on it.
bool in_applet_mode();

// Terminate the process so the console returns to the HOME menu, regardless of
// launch method (forwarder, title-takeover, or hbmenu). Call this INSTEAD of
// returning from main(), AFTER all cleanup — it does not return.
//
// Confirmed mechanism (switchbrew Homebrew ABI + libnx env.c): a normal return
// jumps to g_loaderRetAddr, which for homebrew/forwarders is the loader stub ->
// hbmenu. Calling svcExitProcess() ourselves terminates like a real title ->
// HOME, and makes qlaunch re-scan installed-title records on the way out.
[[noreturn]] void exit_to_home();

} // namespace Core
