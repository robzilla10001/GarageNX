// source/main.cpp
// GarageNX - entry point and main loop.
// Owns the screen stack, drives the frame lifecycle, applies startup sequence.

#include <SDL2/SDL.h>
#include <memory>
#include <vector>
#include <string>
#include <cstdlib>   // exit()
#include <sys/stat.h>
#include <ctime>

#ifdef PLATFORM_SWITCH
#include <switch.h>
#endif

#include "ui/renderer.hpp"
#include "ui/theme.hpp"
#include "ui/input.hpp"
#include "ui/title_bar.hpp"
#include "ui/status_bar.hpp"
#include "ui/widgets.hpp"
#include "ui/font.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "core/save_backup.hpp"
#include "ui/backup_overlay.hpp"
#include "core/album_mount.hpp"
#include "core/nand_mount.hpp"
#include "core/gamecard_mount.hpp"
#include "core/usb_mount.hpp"
#include "core/save_mount.hpp"
#include "core/keys.hpp"
#include "services/net_surface.hpp"
#include "services/title_surface.hpp"
#include "core/sleep_inhibit.hpp"
#include "services/confirmation_broker.hpp"
#include <thread>
#include <condition_variable>
#include <mutex>
#include "screens/menu_dispatch.hpp"
#include "core/system.hpp"
#include "core/storage.hpp"   // also declares Core::Thermal
#include "core/datetime.hpp"
#include "ui/splash.hpp"
#include "core/battery.hpp"
#include "core/atmosphere.hpp"
#include "core/ntp.hpp"
#include "screens/screen.hpp"
#include "screens/main_menu.hpp"

// ─── Path helpers ─────────────────────────────────────────────────────────────

#ifdef PLATFORM_SWITCH
static const std::string ROOT_PATH   = "sdmc:/switch/GarageNX";
static const std::string ASSET_ROOT  = "romfs:";   // no trailing slash - see path helpers
#else
// On PC, look for assets relative to the binary (build dir)
static const std::string ROOT_PATH   = "./GarageNX_data";
static const std::string ASSET_ROOT  = "./assets";
#endif

static std::string config_path() { return ROOT_PATH + "/config.json"; }
static std::string lang_dir()    { return ROOT_PATH + "/lang";        }
static std::string asset_lang_dir() { return ASSET_ROOT + "/lang";    }

// Create the app directory tree. Idempotent; mkdir does not create
// intermediates, so build top-down.
static void ensure_directories() {
    auto make = [](const std::string& path) {
        mkdir(path.c_str(), 0755);   // EEXIST is fine
    };

    make(ROOT_PATH);
    make(ROOT_PATH + "/lang");
    make(ROOT_PATH + "/act_logs");
    make(ROOT_PATH + "/dumps");
    make(ROOT_PATH + "/backups");
    make(ROOT_PATH + "/logs");
}

// Poll live system data and push it to the status bar (1 Hz, not per-frame).
static void refresh_status_bar() {
    StatusBar::Info sb;

    auto sd   = Core::Storage::sd_card();
    auto nand = Core::Storage::nand_user();
    sb.sd_free_gb    = sd.valid   ? sd.gb_free()    : 0.f;
    sb.sd_total_gb   = sd.valid   ? sd.gb_total()   : 0.f;
    sb.nand_free_gb  = nand.valid ? nand.gb_free()  : 0.f;
    sb.nand_total_gb = nand.valid ? nand.gb_total() : 0.f;

    auto temp = Core::Thermal::soc();
    sb.soc_temp_c = temp.valid ? temp.celsius : 0.f;

    auto pwr = Core::Battery::power();
    sb.battery_pct  = pwr.valid ? pwr.charge_fraction : 1.f;
    sb.is_charging  = pwr.charging;

    const auto& cfg = Config::get();
    if (cfg.behavior.show_clock) {
        sb.clock_str = Core::DateTime::clock_string_now();
    }

    StatusBar::set(sb);
}

// ─── Screen stack ─────────────────────────────────────────────────────────────

using ScreenStack = std::vector<std::unique_ptr<Screen>>;

static void push(ScreenStack& stack, std::unique_ptr<Screen> screen) {
    if (!stack.empty()) stack.back()->on_exit();
    stack.push_back(std::move(screen));
    stack.back()->on_enter();
}

static void pop(ScreenStack& stack) {
    if (stack.empty()) return;
    stack.back()->on_exit();
    stack.pop_back();
    if (!stack.empty()) stack.back()->on_enter();
}

// ─── Startup ──────────────────────────────────────────────────────────────────

static bool startup() {
#ifdef PLATFORM_SWITCH
    romfsInit();
    nsInitialize();
    nssuInitialize();
    ncmInitialize();
    nifmInitialize(NifmServiceType_User);
    socketInitializeDefault();   // BSD sockets - required by M6 network services (FTP/HTTP)
    psmInitialize();
    setsysInitialize();
    setInitialize();
    Core::mount_album();   // album:/ for the file-manager transports
    // NOTE: mount_nand() is config-gated and must not be called before
    // Config::load() below.

    // Keyset on the main thread before any server can start: title display names
    // come from encrypted Control NCAs, and lazy per-thread loading would race
    // the UI. Failure is non-fatal (titles fall back to id-based names).
    Core::Keys::load();
    setcalInitialize();
    pdmqryInitialize();
    spsmInitialize();
    timeInitialize();         // system clock (NTP + status bar time)
    tsInitialize();
    accountInitialize(AccountServiceType_Application);
    // NOTE: no manual hidInitialize() - SDL2 owns the hid service on Switch;
    // a manual init here double-owned the session and broke clean teardown.
#endif

    // ── Config ────────────────────────────────────────────────────────────────
    // Ensure the app's directory tree exists before any file I/O.
    ensure_directories();

    if (!Config::load(config_path())) {
        SDL_Log("startup - config load failed, continuing with defaults");
    }

    const auto& cfg = Config::get();

#ifdef PLATFORM_SWITCH
    // Config-gated mounts go AFTER Config::load() - before this point they would
    // gate on compile-time defaults (this is how bis_system: was never mounted).
    Core::mount_nand();    // bis_user:/bis_system: (config-gated, read-only)

    // Unconditional: init failure is worth knowing at startup (usually the
    // deprecated fsp-usb sysmodule, which libusbhsfs will not coexist with).
    Core::UsbMount::init();
#endif

    // ── Theme ─────────────────────────────────────────────────────────────────
    Theme::set(cfg.app.theme == "light" ? Theme::Variant::Light : Theme::Variant::Dark);

    // ── Renderer + fonts ─────────────────────────────────────────────────────
    if (!Renderer::init(ASSET_ROOT)) {
        SDL_Log("startup - Renderer::init failed");
        return false;
    }

    // ── Input ─────────────────────────────────────────────────────────────────
    Input::init();
    Input::set_repeat_enabled(cfg.behavior.button_repeat_on_hold);
    Input::set_repeat_delay(400);
    Input::set_repeat_interval(80);

    // ── Localization ──────────────────────────────────────────────────────────
    Lang::set_baseline_dir(asset_lang_dir());   // bundled English fallback

    std::vector<std::string> known = { cfg.app.language };
    auto scan = Lang::scan(lang_dir(), known);

    // TODO (Milestone 8): if scan.new_ones is non-empty, prompt language selection

    Lang::load(cfg.app.language);

    // ── Title bar: real firmware + SDK versions ───────────────────────────────
    {
        const auto& fw = Core::System::firmware();
        TitleBar::Info tbinfo;
        tbinfo.fw_version  = fw.version.or_na();
        tbinfo.sdk_version = Core::System::sdk_version();
        TitleBar::set(tbinfo);
    }

    // ── NTP sync on launch ────────────────────────────────────────────────────
    // Blocking with a short timeout so a dead server can't hang startup.
    {
        auto ntp = Core::Ntp::sync("pool.ntp.org", 3000);
        if (ntp.success) {
            SDL_Log("startup - NTP sync OK (offset %lld s, %s)",
                    (long long)ntp.offset_seconds, ntp.detail.c_str());
        } else {
            SDL_Log("startup - NTP sync failed: %s", ntp.error.c_str());
        }
    }

    // ── Status bar: first live read ────────────────────────────────────────────
    refresh_status_bar();

    SDL_Log("startup - complete");
    return true;
}

static void shutdown_services() {
#ifdef PLATFORM_SWITCH
    accountExit();
    tsExit();
    timeExit();
    spsmExit();
    pdmqryExit();
    setcalExit();
    setExit();
    setsysExit();
    psmExit();
    // Release any mounted SMB/NFS share BEFORE socketExit(): a dangling session
    // breaks libnx's clean applet teardown. No-op if nothing is mounted.
    Services::net_surface_release();
    socketExit();
    nifmExit();
    ncmExit();
    nssuExit();
    nsExit();
    // Never leave a save filesystem mounted after we exit.
    Core::SaveMount::release();
    Core::UsbMount::exit();
    Core::gamecard_unmount();
    Core::unmount_nand();
    Core::unmount_album();
    romfsExit();
#endif
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    if (!startup()) {
        return 1;
    }

    Splash::show(ASSET_ROOT, 2000, 500);   // 2s hold, then fade into the menu backdrop

    ScreenStack stack;
    push(stack, std::make_unique<MainMenuScreen>());

    bool running = true;
    // Tracks a modal currently answering a ConfirmationBroker request.
    uint64_t g_pending_confirm_id  = 0;
    bool     g_confirm_modal_active = false;

    uint32_t last_status_refresh = 0;

    // Auto-backup runs once, deferred until the first frame is on screen so the
    // user sees the menu rather than a frozen startup. Off by default.
    bool auto_backup_done = (Config::get().behavior.save_auto_backup_days <= 0);
    uint32_t first_frame_at = 0;

    // Idle-dim state (app-level screensaver). 0 = never.
    uint32_t dim_last_activity = SDL_GetTicks();
    uint32_t dim_last_mask     = 0;
    bool     dim_on            = false;

    while (running && !stack.empty()) {
        // ── Input ─────────────────────────────────────────────────────────────
        if (!Input::poll()) {
            running = false;
            break;
        }

        // ── Screen dim (idle) ─────────────────────────────────────────────────
        // Connectivity sessions (sleep guard held) use screen_dim_seconds_net;
        // everything else uses screen_dim_seconds. The wake press is swallowed.
        bool woke_from_dim = false;
        {
            const uint32_t nowd = SDL_GetTicks();
            const uint32_t mask = Input::held_mask();
            const bool activity = (mask != 0) || (mask != dim_last_mask);
            dim_last_mask = mask;
            if (activity) {
                dim_last_activity = nowd;
                if (dim_on) { dim_on = false; woke_from_dim = true; }
            }
            const auto& beh = Config::get().behavior;
            const int secs = Core::SleepInhibit::active() ? beh.screen_dim_seconds_net
                                                          : beh.screen_dim_seconds;
            if (!dim_on && secs > 0 &&
                (nowd - dim_last_activity) >= static_cast<uint32_t>(secs) * 1000u)
                dim_on = true;
        }

        // ── Periodic status bar refresh (once per second) ──────────────────────
        uint32_t now = SDL_GetTicks();
        if (now - last_status_refresh >= 1000) {
            refresh_status_bar();

            // A card can be inserted or ejected at any moment; the surface has
            // to appear and disappear with it.
            Core::gamecard_refresh();

            Core::UsbMount::refresh();

            last_status_refresh = now;
        }

        // Keep the console awake while a Connectivity screen is open.
        Core::SleepInhibit::tick();

        // Resolve one installed-title display name per frame, on this thread.
        Services::installed_titles_tick();

        // ── One-shot auto-backup of stale saves ────────────────────────────────
        if (!auto_backup_done) {
            if (first_frame_at == 0) {
                first_frame_at = SDL_GetTicks();
            } else if (SDL_GetTicks() - first_frame_at >= 1) {
                auto_backup_done = true;

                const int n = Core::SaveBackup::auto_backup_stale(UI::draw_backup_overlay);

                if (n > 0) {
                    Modal::Options o;
                    o.kind          = Modal::Kind::Info;
                    o.title         = Lang::t("auto_backup.done_title");
                    o.body          = Lang::t("auto_backup.done_body");
                    o.confirm_label = Lang::t("common.ok");
                    Modal::show(o);
                }
            }
        }

        // ── Update ────────────────────────────────────────────────────────────
        if (!stack.empty() && !Modal::is_active() && !woke_from_dim) {
            bool do_pop = false;
            auto next = stack.back()->update(do_pop);

            if (next) {
                push(stack, std::move(next));
            } else if (do_pop) {
                pop(stack);
            }

            // An exit/power menu item requests a full app quit from any depth.
            if (menu_quit_requested()) running = false;
        }

        // ── On-device confirmation bridge (NAND safety) ────────────────────────
        // A transport worker blocked in ConfirmationBroker::ask() gets a modal
        // here; only when no other modal is active, so a screen's own dialog
        // isn't stomped.
        {
            auto& broker = Services::ConfirmationBroker::instance();
            Services::ConfirmRequest req;
            if (!Modal::is_active() && broker.pending(req)) {
                Modal::Options o;
                o.title = req.transport + " requests a change";
                o.body  = req.operation + ": " + req.target +
                          "\n\nThis modifies protected system storage (NAND). "
                          "Allow this operation?";
                o.kind          = Modal::Kind::Danger;
                o.confirm_label = "Allow";
                o.cancel_label  = "Deny";
                Modal::show(o);
                g_pending_confirm_id = req.id;
                g_confirm_modal_active = true;
            }
        }

        // ── Draw ──────────────────────────────────────────────────────────────
        Renderer::begin_frame();

        TitleBar::draw();

        if (!stack.empty()) {
            stack.back()->draw();
        }

        StatusBar::draw();

        // Idle dim overlay: before the modal so a prompt stays readable.
        if (dim_on) {
            SDL_SetRenderDrawColor(Renderer::get(), 0, 0, 0, 165);
            Renderer::fill_rect(0, 0, Renderer::BASE_WIDTH, Renderer::BASE_HEIGHT);
        }

        // Modal renders on top of everything (after bars)
        if (Modal::is_active()) {
            Modal::Result res = Modal::update_and_draw();
            if (res != Modal::Result::Pending) {
                if (g_confirm_modal_active) {
                    // This modal was a broker confirmation - resolve the worker.
                    Services::ConfirmationBroker::instance().resolve(
                        g_pending_confirm_id,
                        res == Modal::Result::Confirmed
                            ? Services::ConfirmResult::Allowed
                            : Services::ConfirmResult::Denied);
                    g_confirm_modal_active = false;
                } else if (!stack.empty()) {
                    // A screen's own modal - route to that screen.
                    stack.back()->on_modal_result(static_cast<int>(res));
                }
            }
        }

        Renderer::end_frame();

        // ── Frame cap (PC only; Switch uses vsync) ────────────────────────────
#ifdef PLATFORM_PC
        SDL_Delay(16);
#endif
    }

    // ── Teardown ──────────────────────────────────────────────────────────────
    // Release any worker still blocked on a broker/title-cache wait before the
    // UI it is waiting on goes away.
    Services::ConfirmationBroker::instance().shutdown();
    Services::installed_titles_shutdown();
    // Never leave the console unable to sleep.
    Core::SleepInhibit::force_release();

    stack.clear();

    Input::shutdown();
    Renderer::shutdown();
    shutdown_services();

    // exit(0), NOT return 0: SDL2's SDL_main wrapper makes a plain return land
    // on hbmenu instead of HOME. exit(0) runs the C runtime's normal exit - the
    // sanctioned path that performs the managed libnx teardown.
    exit(0);
}
