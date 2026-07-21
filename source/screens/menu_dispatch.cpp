// source/screens/menu_dispatch.cpp

#include "screens/menu_dispatch.hpp"
#include "screens/submenu_screen.hpp"
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
        { MenuItem::InstallCartridge, "main_menu.install_from_cartridge" },
        { MenuItem::InstalledTitles,  "main_menu.installed_titles"       },
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
        { MenuItem::BrowseUSB,             "main_menu.browse_usb"              },
        { MenuItem::BrowseNetwork,         "main_menu.browse_network"          },
        { MenuItem::Homebrew,              "main_menu.homebrew"                },
        { MenuItem::Tickets,               "main_menu.tickets"                 },
        { MenuItem::Saves,                 "main_menu.saves"                   },
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
        { MenuItem::Tools,       "main_menu.tools"        },
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
        case MenuItem::BrowseSystemPartition: return vis.browse_system_partition;
        case MenuItem::BrowseUserPartition:   return vis.browse_user_partition;
        case MenuItem::BrowseUSB:             return vis.browse_usb;
        case MenuItem::BrowseNetwork:         return vis.browse_network;
        case MenuItem::InstallCartridge:      return vis.install_from_cartridge;
        case MenuItem::InstalledTitles:       return vis.view_installed_games;
        case MenuItem::Tools:                 return vis.tools;
        case MenuItem::Tickets:               return vis.view_tickets;
        case MenuItem::Saves:                 return vis.view_saves;
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

        case MenuItem::Tools:
            // Until a dedicated Tools screen lands, route to System Information.
            return std::make_unique<SystemInfoScreen>();

        case MenuItem::FTP:  return std::make_unique<FTPScreen>();
        case MenuItem::HTTP: return std::make_unique<HTTPScreen>();
        case MenuItem::MTP:  return std::make_unique<MTPScreen>();

        // Not yet implemented leaves — no-op until their screens land.
        case MenuItem::BrowseSystemPartition:
        case MenuItem::BrowseUserPartition:
        case MenuItem::BrowseUSB:
        case MenuItem::BrowseNetwork:
        case MenuItem::InstallCartridge:
        case MenuItem::Homebrew:
        case MenuItem::Tickets:
        case MenuItem::Saves:
        case MenuItem::ActivityLog:
        case MenuItem::Settings:
            return nullptr;

        case MenuItem::ExitToHome:
#ifdef PLATFORM_SWITCH
            // Clean process termination without arming a next-load: libnx returns
            // to HOME and qlaunch re-scans records. (See app_exit notes.) Request a
            // full app quit so this works from inside the Exit submenu too.
            menu_request_quit();
            pop = true;
#endif
            return nullptr;

        case MenuItem::ExitToHBMenu:
#ifdef PLATFORM_SWITCH
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
