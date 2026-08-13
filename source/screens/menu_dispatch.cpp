// source/screens/menu_dispatch.cpp

#include "screens/menu_dispatch.hpp"
#ifdef PLATFORM_SWITCH
// libnx exit-destination control (u32): 1 = exit to HOME menu, 0 = loader/hbmenu.
extern "C" unsigned __nx_applet_exit_mode;
#endif
#include "screens/submenu_screen.hpp"
#include "screens/settings_screen.hpp"
#include "screens/save_manager.hpp"
#include "screens/save_backup_screen.hpp"
#include "screens/file_browser.hpp"
#include "screens/network_browser.hpp"
#include "screens/activity_log.hpp"
#include "screens/gamecard.hpp"
#include "core/fs.hpp"
#include "core/usb_mount.hpp"
#include "screens/screen.hpp"
#include "lang/localization.hpp"
#include "config/config.hpp"

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

// Leaf screens the menu can open.
#include "screens/file_browser.hpp"
#include "screens/title_list.hpp"
#include "screens/system_info.hpp"
#include "screens/ftp_screen.hpp"
#include "screens/http_screen.hpp"
#include "screens/mtp_screen.hpp"

// ─── Item lists (display order) ───────────────────────────────────────────────

const std::vector<MenuEntry>& menu_top_level() {
    static const std::vector<MenuEntry> v = {
        // NOTE: entries are listed here ONLY when dispatch returns a real screen.
        // An item that dispatches to nullptr looks broken when pressed, so it stays
        // out of the menu until its screen exists — the enum value and its
        // (nullptr) dispatch case remain, so re-adding it is one line.
        // Currently withheld for that reason: InstallCartridge.
        { MenuItem::InstalledTitles,  "main_menu.installed_titles"       },
        { MenuItem::BackupSaves,      "main_menu.backup_saves"           },
        { MenuItem::BrowseMenu,       "main_menu.browse"                 },
        { MenuItem::ConnectivityMenu, "main_menu.connectivity"           },
        { MenuItem::SystemMenu,       "main_menu.system"                 },
        { MenuItem::ExitMenu,         "main_menu.exit"                   },
    };
    return v;
}

const std::vector<MenuEntry>& menu_browse_items() {
    static const std::vector<MenuEntry> v = {
        { MenuItem::BrowseSD,              "main_menu.browse_sd"               },
        { MenuItem::BrowseSystemPartition, "main_menu.browse_system_partition" },
        { MenuItem::BrowseUserPartition,   "main_menu.browse_user_partition"   },
        { MenuItem::Gamecard,              "main_menu.gamecard"                },
        { MenuItem::BrowseUSB,             "main_menu.browse_usb"              },
        { MenuItem::BrowseNetwork,         "main_menu.browse_network"          },
        { MenuItem::BrowseGamecard,        "main_menu.browse_gamecard"         },
        { MenuItem::Homebrew,              "main_menu.homebrew"                },
        { MenuItem::Saves,                 "main_menu.saves"                   },
        // BrowseUSB is listed above now that libusbhsfs backs it; BrowseNetwork is
        // listed now that it opens a real chooser (services/net_surface). With no
        // connections configured the chooser shows a "how to add one" message
        // rather than nothing, so it is not a dead button.
        // REMOVED DELIBERATELY: Tickets. Listing tickets invites deleting them,
        // deleting one can make an installed title unlaunchable, and almost no
        // homebrew uses titlekey crypto — so the feature carried real risk for
        // very little reach. Core::Es::list_common_tickets() stays (it is proven
        // and harmless) in case a future need is clearer.
    };
    return v;
}

const std::vector<MenuEntry>& menu_connectivity_items() {
    static const std::vector<MenuEntry> v = {
        { MenuItem::MTP,  "main_menu.mtp"  },
        { MenuItem::FTP,  "main_menu.ftp"  },
        { MenuItem::HTTP, "main_menu.http" },
    };
    return v;
}

const std::vector<MenuEntry>& menu_system_items() {
    static const std::vector<MenuEntry> v = {
        { MenuItem::SystemInfo,       "main_menu.system_info"        },
        { MenuItem::Settings,    "main_menu.settings"     },
        { MenuItem::ActivityLog, "main_menu.activity_log" },
    };
    return v;
}

const std::vector<MenuEntry>& menu_exit_items() {
    static const std::vector<MenuEntry> v = {
        { MenuItem::ExitToHome,          "main_menu.exit_to_home"          },
        { MenuItem::ExitToHBMenu,        "main_menu.exit_to_hbmenu"        },
        { MenuItem::RestartToBootloader, "main_menu.restart_to_bootloader" },
        { MenuItem::Shutdown,            "main_menu.shutdown"              },
    };
    return v;
}

// ─── Classification / visibility ──────────────────────────────────────────────

bool menu_is_submenu(MenuItem id) {
    return id == MenuItem::BrowseMenu || id == MenuItem::ConnectivityMenu ||
           id == MenuItem::SystemMenu || id == MenuItem::ExitMenu;
}

namespace {
bool running_under_hbloader() {
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
} // namespace

bool menu_item_visible(MenuItem id) {
    // Exit-to-hbmenu only makes sense when a loader is present to return to.
    if (id == MenuItem::ExitToHBMenu) return running_under_hbloader();

#ifdef PLATFORM_SWITCH
    // Respect the user's per-item visibility settings (Settings lets items be
    // hidden). Submenu openers and exit/power items are always shown.
    const auto& vis = Config::get().visibility;
    switch (id) {
        case MenuItem::BrowseSD:              return vis.browse_sd;
        // Gated on the STORAGE SURFACE, not a separate visibility flag. "Can this
        // console see game cards" is one decision, and duplicating it as a second
        // toggle would let the two disagree — a menu entry that opens a surface the
        // transports have been told to hide. It also disappears with the card,
        // because an unmounted surface fails the same probe FTP and MTP use.
        case MenuItem::BrowseGamecard:
            // BOTH conditions. The surface being enabled is a user preference; the
            // card being INSERTED is a fact about the world, and the entry must
            // follow the fact too. Gating only on the preference left a row that
            // was always visible and did nothing when no card was in — the
            // dead-button problem, reintroduced by me in the round that was
            // supposed to remove dead buttons.
            return Config::any_transport_exposes(&Config::Surfaces::gamecard)
                   && Fs::is_directory("gamecard:/");
        case MenuItem::BrowseSystemPartition: return vis.browse_system_partition;
        case MenuItem::BrowseUserPartition:   return vis.browse_user_partition;
        // Shown only when a drive is actually attached AND the user has the item
        // enabled — the same both-conditions rule the game card uses. An entry that
        // is always visible and opens nothing is the dead-button problem.
        case MenuItem::BrowseUSB:
            return vis.browse_usb && !Core::UsbMount::volumes().empty();
        case MenuItem::BrowseNetwork:         return vis.browse_network;
        case MenuItem::InstallCartridge:      return vis.install_from_cartridge;
        case MenuItem::InstalledTitles:       return vis.view_installed_games;
        case MenuItem::SystemInfo:            return vis.tools;
        case MenuItem::Tickets:               return vis.view_tickets;
        case MenuItem::Saves:                 return vis.view_saves;
        case MenuItem::BackupSaves:           return vis.backup_saves;
        case MenuItem::MTP:                   return vis.start_mtp;
        case MenuItem::FTP:                   return vis.start_ftp;
        case MenuItem::HTTP:                  return vis.start_http;
        default:                              return true;
    }
#else
    return true;
#endif
}

// ─── App quit request ─────────────────────────────────────────────────────────

namespace {
bool g_quit_requested = false;
}

bool menu_quit_requested() { return g_quit_requested; }
void menu_request_quit()   { g_quit_requested = true; }

// ─── Activation ───────────────────────────────────────────────────────────────

static std::unique_ptr<Screen> open_submenu(MenuItem id) {
    switch (id) {
        case MenuItem::BrowseMenu:
            return std::make_unique<SubMenuScreen>(
                Lang::t("main_menu.browse"), menu_browse_items());
        case MenuItem::ConnectivityMenu:
            return std::make_unique<SubMenuScreen>(
                Lang::t("main_menu.connectivity"), menu_connectivity_items());
        case MenuItem::SystemMenu:
            return std::make_unique<SubMenuScreen>(
                Lang::t("main_menu.system"), menu_system_items());
        case MenuItem::ExitMenu:
            return std::make_unique<SubMenuScreen>(
                Lang::t("main_menu.exit"), menu_exit_items());
        default:
            return nullptr;
    }
}

std::unique_ptr<Screen> menu_activate(MenuItem id, bool& pop) {
    pop = false;

    if (menu_is_submenu(id)) return open_submenu(id);

    switch (id) {
        case MenuItem::BrowseSD:
            return std::make_unique<FileBrowserScreen>(
                "sdmc:/", Lang::t("file_browser.title_sd"));

        case MenuItem::InstalledTitles:
            return std::make_unique<TitleListScreen>();

        case MenuItem::SystemInfo:
            return std::make_unique<SystemInfoScreen>();

        case MenuItem::FTP:  return std::make_unique<FTPScreen>();
        case MenuItem::HTTP: return std::make_unique<HTTPScreen>();
        case MenuItem::MTP:  return std::make_unique<MTPScreen>();

        // CAUTION: every case below returns its OWN screen. Do not add a new case
        // to an existing fall-through group without checking what that group
        // returns — a `case X:` dropped into the "unimplemented" list silently
        // inherits its return. That is exactly how BrowseSystemPartition,
        // BrowseUserPartition, BrowseUSB and BrowseNetwork all ended up opening
        // the SETTINGS screen: MenuItem::Settings was added to the head of their
        // shared group and they fell into its return.
        case MenuItem::Settings:
            return SettingsScreen::root();

        // NAND partitions, read-only unless the write guard says otherwise. The
        // mount is config-gated (mount_nand), so if the surface is disabled the
        // browser opens on a device that is not there — hence the mount check.
        case MenuItem::BrowseSystemPartition:
            if (!Fs::is_directory("bis_system:/")) return nullptr;
            return std::unique_ptr<Screen>(
                new FileBrowserScreen("bis_system:/", Lang::t("main_menu.browse_system_partition")));

        case MenuItem::BrowseUserPartition:
            if (!Fs::is_directory("bis_user:/")) return nullptr;
            return std::unique_ptr<Screen>(
                new FileBrowserScreen("bis_user:/", Lang::t("main_menu.browse_user_partition")));

        case MenuItem::Gamecard:
            return std::unique_ptr<Screen>(new GamecardScreen());

        case MenuItem::BrowseGamecard:
            // Same mount check the partitions use: the surface can be enabled while
            // no card is inserted, and opening a browser on an absent device gives
            // an empty folder that errors rather than an honest "nothing here".
            if (!Fs::is_directory("gamecard:/")) return nullptr;
            return std::unique_ptr<Screen>(
                new FileBrowserScreen("gamecard:/", Lang::t("main_menu.browse_gamecard")));

        case MenuItem::Homebrew:
            return std::unique_ptr<Screen>(
                new FileBrowserScreen("sdmc:/switch/", Lang::t("main_menu.homebrew")));

        case MenuItem::Saves:
            return std::unique_ptr<Screen>(new SaveManagerScreen());

        case MenuItem::BackupSaves:
            return std::unique_ptr<Screen>(new SaveBackupScreen());

        // Genuinely unimplemented: no screen and no backing service yet. These are
        // HIDDEN from the menu by default (see menu_item_visible) rather than left
        // clickable, because a menu entry that does nothing when pressed is
        // indistinguishable from one that is broken.
        case MenuItem::ActivityLog:
            return std::unique_ptr<Screen>(new ActivityLogScreen());

        case MenuItem::BrowseUSB: {
            // Open the attached USB volume. Reached only when one is present —
            // menu_item_visible() gates the entry on volumes() being non-empty —
            // but re-checked here because visibility is evaluated a frame earlier
            // and a drive can be pulled in between.
            const auto& vols = Core::UsbMount::volumes();
            if (vols.empty()) return nullptr;
            // One volume is the common case; open it directly rather than make the
            // user pick from a list of one. A chooser for multi-volume drives is
            // the noted follow-up.
            return std::unique_ptr<Screen>(new FileBrowserScreen(
                vols[0].mount + "/", vols[0].label));
        }

        case MenuItem::BrowseNetwork:
            // Opens the connection chooser. Reached whenever the item is visible;
            // the chooser handles the empty-connections case itself, so unlike USB
            // there is no device-present precondition to re-check here.
            return std::unique_ptr<Screen>(new NetworkBrowserScreen());

        case MenuItem::InstallCartridge:
        case MenuItem::Tickets:
            return nullptr;

        case MenuItem::ExitToHome:
#ifdef PLATFORM_SWITCH
            // libnx reads __nx_applet_exit_mode during its exit sequence: 1 = exit to
            // the HOME menu (where qlaunch re-scans records and shows freshly
            // installed titles), 0 = return to the loader/hbmenu. This is the
            // mechanism NXMP uses. Works even when launched from hbmenu.
            __nx_applet_exit_mode = 1;
            menu_request_quit();
            pop = true;
#endif
            return nullptr;

        case MenuItem::ExitToHBMenu:
#ifdef PLATFORM_SWITCH
            __nx_applet_exit_mode = 0;   // default: return to the loader/hbmenu
            if (envHasNextLoad())
                envSetNextLoad("sdmc:/hbmenu.nro", "sdmc:/hbmenu.nro");
            menu_request_quit();
            pop = true;
#endif
            return nullptr;

        case MenuItem::RestartToBootloader:
#ifdef PLATFORM_SWITCH
            spsmShutdown(true);
#endif
            return nullptr;

        case MenuItem::Shutdown:
#ifdef PLATFORM_SWITCH
            spsmShutdown(false);
#endif
            return nullptr;

        // Submenu openers handled above.
        case MenuItem::BrowseMenu:
        case MenuItem::ConnectivityMenu:
        case MenuItem::SystemMenu:
        case MenuItem::ExitMenu:
            return nullptr;
    }
    return nullptr;
}
