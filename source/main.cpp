// source/main.cpp
// GarageNX — entry point and main loop.
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
#include "ui/modal.hpp"
#include "config/config.hpp"
#include "lang/localization.hpp"
#include "core/album_mount.hpp"
#include "core/nand_mount.hpp"
#include "core/keys.hpp"
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
static const std::string ASSET_ROOT  = "romfs:";   // no trailing slash — see path helpers
#else
// On PC, look for assets relative to the binary (build dir)
static const std::string ROOT_PATH   = "./GarageNX_data";
static const std::string ASSET_ROOT  = "./assets";
#endif

static std::string config_path() { return ROOT_PATH + "/config.json"; }
static std::string lang_dir()    { return ROOT_PATH + "/lang";        }
static std::string asset_lang_dir() { return ASSET_ROOT + "/lang";    }

// Create the full app directory tree. Idempotent — safe to call every launch.
// Uses POSIX mkdir (libnx provides this over the sdmc: mount; standard on PC).
static void ensure_directories() {
    auto make = [](const std::string& path) {
        // mkdir returns -1 with errno=EEXIST if it already exists — that's fine.
        mkdir(path.c_str(), 0755);
    };

    // ROOT_PATH may be a device-prefixed path (sdmc:/switch/GarageNX).
    // mkdir doesn't create intermediates, so build the tree top-down.
    make(ROOT_PATH);
    make(ROOT_PATH + "/lang");
    make(ROOT_PATH + "/act_logs");
    make(ROOT_PATH + "/dumps");
    make(ROOT_PATH + "/backups");
    make(ROOT_PATH + "/logs");
}

// Poll live system data and push it to the status bar. Cheap enough to call on
// an interval (not every frame — see the loop's throttle).
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

    // Clock: date + time, formatted per the user's configured preferences
    // (Behavior::date_format order and Behavior::time_24h). show_clock still
    // governs whether it appears; show_seconds still governs seconds.
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
    // Initialize required Switch services
    romfsInit();
    nsInitialize();
    nssuInitialize();
    ncmInitialize();
    nifmInitialize(NifmServiceType_User);
    socketInitializeDefault();   // BSD sockets — required by M6 network services (FTP/HTTP)
    psmInitialize();
    setsysInitialize();
    setInitialize();
    Core::mount_album();   // expose album:/ for the file-manager transports
    Core::mount_nand();    // expose bis_user:/bis_system: (config-gated, read-only)

    // Load the keyset once, here, on the main thread before any server can start.
    // Title display names come from encrypted Control NCAs, so a transport listing
    // "Installed Titles" needs keys — and if each transport lazily loaded them from
    // its own worker thread it would race the UI reading the same global keyset.
    // Failure is fine and non-fatal: titles then fall back to id-based names.
    Core::Keys::load();
    setcalInitialize();       // calibration data (MACs, battery lot, etc.)
    pdmqryInitialize();
    spsmInitialize();
    timeInitialize();         // system clock (for NTP + status bar time)
    tsInitialize();           // temperature sensor (SoC temp)
    accountInitialize(AccountServiceType_Application);
    // NOTE: no manual hidInitialize() — SDL2 initializes and OWNS the hid service
    // on Switch, and GarageNX reads input exclusively through SDL (SDL_Joystick /
    // SDL keyboard events; no libnx hid* APIs are used). A manual hidInitialize()
    // here double-owned the service and was never matched by hidExit(), leaving a
    // dangling hid session at process exit — which prevented libnx's clean applet
    // teardown (contributing to exiting to hbmenu instead of HOME, and to unclean
    // shutdown). Letting SDL manage hid's full lifecycle fixes the leak.
#endif

    // ── Config ────────────────────────────────────────────────────────────────
    // Ensure the app's directory tree exists before any file I/O.
    ensure_directories();

    if (!Config::load(config_path())) {
        SDL_Log("startup — config load failed, continuing with defaults");
    }

    const auto& cfg = Config::get();

    // ── Theme ─────────────────────────────────────────────────────────────────
    Theme::set(cfg.app.theme == "light" ? Theme::Variant::Light : Theme::Variant::Dark);

    // ── Renderer + fonts ─────────────────────────────────────────────────────
    if (!Renderer::init(ASSET_ROOT)) {
        SDL_Log("startup — Renderer::init failed");
        return false;
    }

    // ── Input ─────────────────────────────────────────────────────────────────
    Input::init();
    Input::set_repeat_enabled(cfg.behavior.button_repeat_on_hold);
    Input::set_repeat_delay(400);
    Input::set_repeat_interval(80);

    // ── Localization ──────────────────────────────────────────────────────────
    // 1. Set the bundled baseline (romfs). This loads the permanent English
    //    fallback — every key resolves against it, always.
    Lang::set_baseline_dir(asset_lang_dir());

    // 2. Scan the user language dir (sdmc) for additional drop-in languages.
    std::vector<std::string> known = { cfg.app.language };
    auto scan = Lang::scan(lang_dir(), known);

    // TODO (Milestone 8): if scan.new_ones is non-empty, prompt language selection

    // 3. Activate the configured language (falls back to English if unavailable).
    Lang::load(cfg.app.language);

    // ── Title bar: real firmware + SDK versions ───────────────────────────────
    {
        const auto& fw = Core::System::firmware();
        TitleBar::Info tbinfo;
        tbinfo.fw_version  = fw.version.or_na();
        tbinfo.sdk_version = Core::System::sdk_version();
        TitleBar::set(tbinfo);
    }

    // ── NTP sync on launch (per design decision) ───────────────────────────────
    // Blocking with a short timeout so a dead server can't hang startup. This
    // sets the system clock to network time. On the PC stub it's a no-op query.
    {
        auto ntp = Core::Ntp::sync("pool.ntp.org", 3000);
        if (ntp.success) {
            SDL_Log("startup — NTP sync OK (offset %lld s)",
                    (long long)ntp.offset_seconds);
        } else {
            SDL_Log("startup — NTP sync failed: %s", ntp.error.c_str());
        }
    }

    // ── Status bar: first live read ────────────────────────────────────────────
    refresh_status_bar();

    SDL_Log("startup — complete");
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
    socketExit();
    nifmExit();
    ncmExit();
    nssuExit();
    nsExit();
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

    // Startup splash — shown once, after init so the renderer and assets are
    // ready, and before the first menu frame is drawn.
    Splash::show(ASSET_ROOT, 2000, 500);   // 2s hold, then fade into the menu backdrop

    // Push root screen
    ScreenStack stack;
    push(stack, std::make_unique<MainMenuScreen>());

    bool running = true;
    // Tracks a modal that is currently answering a ConfirmationBroker request, so
    // its result resolves the broker (not a screen). Reset when that modal closes.
    uint64_t g_pending_confirm_id  = 0;
    bool     g_confirm_modal_active = false;

    uint32_t last_status_refresh = 0;

    while (running && !stack.empty()) {
        // ── Input ─────────────────────────────────────────────────────────────
        if (!Input::poll()) {
            running = false;
            break;
        }

        // ── Periodic status bar refresh (once per second) ──────────────────────
        // Storage/battery/temp/clock don't change fast enough to warrant a
        // per-frame poll, and some of these reads aren't free.
        uint32_t now = SDL_GetTicks();
        if (now - last_status_refresh >= 1000) {
            refresh_status_bar();
            last_status_refresh = now;
        }

        // Keep the console awake while a Connectivity screen is open (refreshes
        // the idle timer so it neither dims nor sleeps mid-transfer).
        Core::SleepInhibit::tick();

        // Resolve one installed-title display name per frame, on this thread.
        // Transports read the results; they never decrypt anything themselves.
        Services::installed_titles_tick();

        // ── Update ────────────────────────────────────────────────────────────
        if (!stack.empty() && !Modal::is_active()) {
            bool do_pop = false;
            auto next = stack.back()->update(do_pop);

            if (next) {
                push(stack, std::move(next));
            } else if (do_pop) {
                pop(stack);
            }

            // An exit/power menu item requests a full app quit (from any submenu
            // depth). Honour it here so we end the loop regardless of stack depth,
            // rather than only popping the screen the item lived on.
            if (menu_quit_requested()) running = false;
        }

        // ── On-device confirmation bridge (NAND safety) ────────────────────────


        // If a transport worker is blocked in ConfirmationBroker::ask() waiting for
        // the user to approve a guarded operation (e.g. a NAND write), pop a modal
        // on the console. The worker stays blocked until the user answers here —
        // the PC->console->console-confirm->PC round trip IS the safety. Only raise
        // it when no other modal is active, so a screen's own dialog isn't stomped.
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
                // Remember which request this modal is answering, so the result
                // routes back to the right one even if it changes underneath.
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

        // Modal renders on top of everything (after bars)
        if (Modal::is_active()) {
            Modal::Result res = Modal::update_and_draw();
            if (res != Modal::Result::Pending) {
                if (g_confirm_modal_active) {
                    // This modal was a broker confirmation — resolve the worker.
                    Services::ConfirmationBroker::instance().resolve(
                        g_pending_confirm_id,
                        res == Modal::Result::Confirmed
                            ? Services::ConfirmResult::Allowed
                            : Services::ConfirmResult::Denied);
                    g_confirm_modal_active = false;
                } else if (!stack.empty()) {
                    // A screen's own modal — route to that screen.
                    stack.back()->on_modal_result(static_cast<int>(res));
                }
            }
        }

        Renderer::end_frame();

        // ── Frame cap (target 60fps) ──────────────────────────────────────────
        // vsync (SDL_RENDERER_PRESENTVSYNC) handles this on Switch.
        // On PC without vsync, cap at ~60fps to avoid pegging the CPU.
#ifdef PLATFORM_PC
        SDL_Delay(16);
#endif
    }

    // ── Teardown ──────────────────────────────────────────────────────────────
    // Release any transport worker blocked on a confirmation with Denied, before
    // we tear down the UI it was waiting on (the teardown discipline from the
    // cancel crash — never leave a worker parked on a dead main loop).
    Services::ConfirmationBroker::instance().shutdown();
    // Release any transport worker blocked waiting for the title cache.
    Services::installed_titles_shutdown();
    // Guaranteed restore of normal sleep behaviour, even if a screen guard
    // somehow outlived the stack — never leave the console unable to sleep.
    Core::SleepInhibit::force_release();

    stack.clear();

    Input::shutdown();
    Renderer::shutdown();
    shutdown_services();

    // exit(0), NOT return 0. SDL2 hijacks main() on Switch (SDL.h does
    // `#define main SDL_main` and provides its own real main that wraps ours), so
    // a `return` here goes back into SDL's wrapper rather than libnx's crt0 — and
    // that wrapper path lands on hbmenu instead of HOME. exit(0) runs the C
    // runtime's normal exit (atexit/__libnx_exit) and terminates the process the
    // way a real title does, which returns to the HOME menu and lets qlaunch
    // re-scan installed titles. This is the same exit(0) that plain-libnx
    // installers (e.g. Plutonium-based) use. Cleanup above has already run.
    // Unlike the earlier raw svcExitProcess() attempt, exit(0) is the SANCTIONED
    // exit path — it does the managed libnx teardown, so it does not crash.
    exit(0);
}
