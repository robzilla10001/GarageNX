// source/screens/main_menu.cpp

#include "screens/main_menu.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/widgets.hpp"
#include "ui/input.hpp"
#include <SDL2/SDL.h>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

// Forward declarations for screens we'll implement in later milestones.
// Stubs return nullptr (i.e. "not yet implemented") until each milestone lands.
// Uncomment each include as the corresponding screen is implemented.
//
#include "screens/file_browser.hpp"
#include "screens/system_info.hpp"
#include "screens/title_list.hpp"
#include "screens/ftp_screen.hpp"
#include "screens/http_screen.hpp"
#include "screens/mtp_screen.hpp"
// #include "screens/network_browser.hpp"
// #include "screens/homebrew_list.hpp"
// #include "screens/tools_menu.hpp"
// #include "screens/ticket_list.hpp"
// #include "screens/save_manager.hpp"
// #include "screens/mtp_screen.hpp"
// #include "screens/activity_log.hpp"
// #include "screens/settings.hpp"

// ─── Menu item IDs ────────────────────────────────────────────────────────────
// Stable integer IDs for each menu entry so we can switch on them
// independently of list position (some items may be hidden).

enum class MenuItem {
    BrowseSD,
    BrowseSystemPartition,
    BrowseUserPartition,
    BrowseUSB,
    InstallCartridge,
    BrowseNetwork,
    InstalledTitles,
    Homebrew,
    Tools,
    Tickets,
    Saves,
    MTP,
    FTP,
    HTTP,
    ActivityLog,
    Settings,
    // ── Exit group ──────────────────────────────────────────────────────────
    ExitToHome,
    ExitToHBMenu,
    RestartToBootloader,
    Shutdown,
};

struct MenuEntry {
    MenuItem    id;
    const char* lang_key;   // looked up via Lang::t()
};

// Master list — order defines display order.
// Visibility is filtered at runtime against Config::Visibility.
static const MenuEntry MENU_ENTRIES[] = {
    { MenuItem::BrowseSD,              "main_menu.browse_sd"              },
    { MenuItem::BrowseSystemPartition, "main_menu.browse_system_partition"},
    { MenuItem::BrowseUserPartition,   "main_menu.browse_user_partition"  },
    { MenuItem::BrowseUSB,             "main_menu.browse_usb"             },
    { MenuItem::InstallCartridge,      "main_menu.install_from_cartridge" },
    { MenuItem::BrowseNetwork,         "main_menu.browse_network"         },
    { MenuItem::InstalledTitles,       "main_menu.installed_titles"       },
    { MenuItem::Homebrew,              "main_menu.homebrew"               },
    { MenuItem::Tools,                 "main_menu.tools"                  },
    { MenuItem::Tickets,               "main_menu.tickets"                },
    { MenuItem::Saves,                 "main_menu.saves"                  },
    { MenuItem::MTP,                   "main_menu.mtp"                    },
    { MenuItem::FTP,                   "main_menu.ftp"                    },
    { MenuItem::HTTP,                  "main_menu.http"                   },
    { MenuItem::ActivityLog,           "main_menu.activity_log"           },
    { MenuItem::Settings,              "main_menu.settings"               },
    { MenuItem::ExitToHome,            "main_menu.exit_to_home"           },
    { MenuItem::ExitToHBMenu,          "main_menu.exit_to_hbmenu"         },
    { MenuItem::RestartToBootloader,   "main_menu.restart_to_bootloader"  },
    { MenuItem::Shutdown,              "main_menu.shutdown"               },
};

// ─── Visibility check ─────────────────────────────────────────────────────────

// True when GarageNX is running under hbloader (launched from hbmenu / Album).
static bool running_under_hbloader() {
#ifdef PLATFORM_SWITCH
    return envHasNextLoad();
#else
    return false;
#endif
}

static bool item_visible(MenuItem id, const Config::Visibility& vis) {
    switch (id) {
        case MenuItem::BrowseSD:              return vis.browse_sd;
        case MenuItem::BrowseSystemPartition: return vis.browse_system_partition;
        case MenuItem::BrowseUserPartition:   return vis.browse_user_partition;
        case MenuItem::BrowseUSB:             return vis.browse_usb;
        case MenuItem::InstallCartridge:      return vis.install_from_cartridge;
        case MenuItem::BrowseNetwork:         return vis.browse_network;
        case MenuItem::InstalledTitles:       return vis.view_installed_games;
        case MenuItem::Tools:                 return vis.tools;
        case MenuItem::Tickets:               return vis.view_tickets;
        case MenuItem::Saves:                 return vis.view_saves;
        case MenuItem::MTP:                   return vis.start_mtp;
        case MenuItem::FTP:                   return vis.start_ftp;
        case MenuItem::HTTP:                  return vis.start_http;

        // Exit-to-home works in both contexts (appletRequestExitToSelf tears
        // down the applet session, dropping to HOME even under hbloader), so it
        // is always offered. Exit-to-hbmenu only makes sense when a loader is
        // actually present to return to.
        case MenuItem::ExitToHome:   return true;
        case MenuItem::ExitToHBMenu: return running_under_hbloader();

        // Always visible
        default: return true;
    }
}

// ─── Separator groups ─────────────────────────────────────────────────────────
// Draw a subtle divider before the first item in each group.

static bool is_group_start(MenuItem id) {
    return id == MenuItem::InstalledTitles   // content group
        || id == MenuItem::Tools             // tools group
        || id == MenuItem::MTP              // connectivity group
        || id == MenuItem::ActivityLog      // log group
        || id == MenuItem::Settings         // settings group
        || id == MenuItem::ExitToHome;      // exit group
}

// ─── Implementation ───────────────────────────────────────────────────────────

// Map from list index → MenuItem id (rebuilt when items change)
static std::vector<MenuItem> s_id_map;

MainMenuScreen::MainMenuScreen() {
    rebuild_items();
}

void MainMenuScreen::on_enter() {
    rebuild_items();
}

void MainMenuScreen::rebuild_items() {
    const auto& vis = Config::get().visibility;
    std::vector<Widgets::ListItem> items;
    s_id_map.clear();

    for (auto& entry : MENU_ENTRIES) {
        if (!item_visible(entry.id, vis)) continue;

        Widgets::ListItem item;
        item.label = Lang::t(entry.lang_key);
        items.push_back(item);
        s_id_map.push_back(entry.id);
    }

    m_list.set_items(std::move(items));
}

std::unique_ptr<Screen> MainMenuScreen::update(bool& pop) {
    pop = false;

    // B on the main menu exits GarageNX, so the user doesn't have to scroll to
    // the Exit entry every time. Uses the same reliable path as the Exit items:
    // under a homebrew loader, return to hbmenu; otherwise attempt exit-to-home.
    if (Input::pressed(Input::Button::B)) {
#ifdef PLATFORM_SWITCH
        if (running_under_hbloader()) {
            // Let hbloader relaunch its default (hbmenu) when we exit.
            if (envHasNextLoad())
                envSetNextLoad("sdmc:/hbmenu.nro", "sdmc:/hbmenu.nro");
            pop = true;   // popping the last screen ends the main loop
        } else {
            // Launched as an application/forwarder — request return to HOME.
            appletRequestExitToSelf();
        }
#else
        pop = true;
#endif
        return nullptr;
    }

    if (m_list.handle_input()) {
        int idx = m_list.cursor();
        if (idx < 0 || idx >= static_cast<int>(s_id_map.size()))
            return nullptr;

        MenuItem chosen = s_id_map[idx];

        switch (chosen) {
            case MenuItem::BrowseSD:
                return std::make_unique<FileBrowserScreen>(
                    "sdmc:/", Lang::t("file_browser.title_sd"));

            // NAND partitions (bis_system:/bis_user:) require explicit mounting
            // with elevated FS permissions. That harness lands with Milestone 4
            // (title management). Until then these are stubbed.
            case MenuItem::Tools:
                // Until the Tools submenu lands (Milestone 7), route directly to
                // System Information so the new data layer is testable.
                return std::make_unique<SystemInfoScreen>();

            case MenuItem::InstalledTitles:
                // Milestone 4 Phase B: the real installed-titles browser.
                return std::make_unique<TitleListScreen>();

            case MenuItem::FTP:
                // Milestone 6: FTP service screen.
                return std::make_unique<FTPScreen>();

            case MenuItem::HTTP:
                // Milestone 6: HTTP service screen.
                return std::make_unique<HTTPScreen>();

            case MenuItem::MTP:
                // Milestone 6: MTP (USB) service screen.
                return std::make_unique<MTPScreen>();

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
                // TODO: return the appropriate screen
                return nullptr;

            case MenuItem::ExitToHome:
#ifdef PLATFORM_SWITCH
                // Return to the Switch HOME menu, bypassing hbmenu.
                //
                // KNOWN ISSUE (parked — revisit before release): on hardware
                // this currently does nothing. appletRequestExitToSelf() fires
                // but the app neither exits nor transitions. DBI's README
                // confirms a home-exit IS achievable from applet mode, so the
                // mechanism exists — we just haven't matched it yet. Leads to
                // investigate when we return:
                //   1. Log appletGetAppletType() at runtime. We assume
                //      LibraryApplet; if hbloader/SDL hand us another type the
                //      call is effectively a no-op.
                //   2. The applet-close may require pumping appletMainLoop()
                //      until it returns false, rather than relying on SDL to
                //      surface SDL_QUIT. Our loop is SDL-event-driven; the
                //      close signal may not be converted to SDL_QUIT by the SDL
                //      Switch backend.
                //   3. Check whether DBI uses a different primitive entirely.
                // For now it's a harmless no-op rather than a wrong action;
                // exit-to-hbmenu is the reliable path under a loader.
                appletRequestExitToSelf();
#endif
                return nullptr;

            case MenuItem::ExitToHBMenu:
#ifdef PLATFORM_SWITCH
                // Return to hbmenu. Clearing the next-load path makes hbloader
                // fall back to its default (sdmc:/hbmenu.nro) on our exit; if a
                // loader is present we simply let it relaunch. When NOT under a
                // loader there's nothing to go back to, so this item is hidden
                // (see item_visible) and exit-to-home is offered instead.
                if (envHasNextLoad()) {
                    envSetNextLoad("sdmc:/hbmenu.nro", "sdmc:/hbmenu.nro");
                }
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
        }
    }

    return nullptr;
}

void MainMenuScreen::draw() {
    SDL_Renderer* r = Renderer::get();
    const int x = 0;
    const int y = Layout::CONTENT_Y;
    const int w = Layout::SCREEN_W;
    const int h = Layout::CONTENT_H;

    // Content background
    Theme::apply(r, Theme::Token::BgBase);
    Renderer::fill_rect(x, y, w, h);

    // Left accent panel (subtle brand strip)
    Theme::apply(r, Theme::Token::BgSurface);
    Renderer::fill_rect(x, y, 4, h);

    // List (indented slightly from the accent strip)
    Widgets::ListStyle style;
    style.row_height    = Layout::MENU_ITEM_H;
    style.indent_x      = Layout::MENU_INDENT_X;
    style.show_checkbox = false;
    style.show_dividers = true;

    m_list.draw(x, y, w, h - 36 /* room for button legend */, style);

    // Button legend
    std::vector<Widgets::ButtonHint> hints = {
        { "A", Lang::t("hints.open")     },
        { "B", Lang::t("hints.back")     },
    };
    Widgets::draw_button_legend(x, y + h - 32, w, hints);
}
