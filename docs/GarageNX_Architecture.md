# GarageNX — Architecture & Design Reference
**Version:** 0.1.0-planning  
**Last Updated:** 2026-07-16 (rev 3)  
**Status:** Living reference — Milestones 1–5 complete and hardware-validated; Milestone 6 (Services) in progress (shared net/service foundation + FTP server landed).

---

## 1. Project Identity

| Property | Value |
|---|---|
| Name | GarageNX |
| Binary | `GarageNX.nro` |
| Root path | `sdmc:/switch/GarageNX/` |
| Language | C++17 |
| Platform SDK | libnx (latest devkitPro) |
| Renderer | SDL2 + SDL2_ttf + SDL2_image |
| Build system | CMake (devkitPro toolchain) |
| License | AGPLv3 (§13 source disclosure via `APP_SOURCE_URL`) |

### Post-parity additions (built after this reference's body was written)

This document's body describes the parity build. Since then the following shipped
and are hardware-verified; where they touch a subsystem below, that subsystem's
prose may predate them:

- **Browse Network (SMB/NFS).** A `net:` devoptab over libnfs/libsmb2
  (`source/services/net_surface.*`), a connection chooser + on-device editor
  (`source/screens/network_browser.*`, `network_edit.*`), read-ahead buffering and
  reconnect-through-drops, threaded non-freezing copy, and split-to-SD. Compiled
  only when `GARAGENX_NET_CLIENT` is defined and the two portlibs are installed —
  see `BUILDING_NETWORK_CLIENT.md`.
- **HTTP full read/write parity.** The web UI (`source/services/web_ui.hpp`) browses
  every catalog surface and now uploads-into-folder, deletes, and makes folders;
  the server added plain-PUT/DELETE/`POST /api/mkdir`, all routed through
  `write_guard` so NAND/save writes raise the on-device confirmation.
- **Exit to HOME** via libnx `__nx_applet_exit_mode = 1` (set in `menu_dispatch`/
  `main_menu` for the HOME exits; `= 0` for the explicit hbmenu exit). Returning to
  HOME is also what makes a freshly installed title appear.
- **Install → HOME-menu registration.** After the ncm meta commit and the ns record
  push, the installer calls `avmPushLaunchVersion` on 6.0.0+ and deliberately does
  NOT consume `nsGetApplicationRecordUpdateSystemEvent` (so qlaunch still rebuilds).
- **Behaviour settings.** Four never-consumed toggles removed; `action_logging`,
  `verify_hash_on_install`, and a split `screen_dim_seconds` / `screen_dim_seconds_net`
  (app-level idle dim, context by sleep-guard) wired and shown in Settings. A
  date-format selector was added (DD/MM/YYYY default, + MM/DD, YYYY/MM/DD); the
  unwired SDK field was removed from the title bar.
- **System Information — real-data completion.** `source/screens/system_info.cpp`
  went from mostly-N/A to fully wired: SD card details via a decoded CID
  (`Core::Storage::sd_cid()`, byte map `field[k]=cid[14-k]`), LCD vendor via
  `setcalGetLcdVendorId`, the MAX17050 gas gauge via direct i2c reads
  (`Core::Battery::max17050()`, RSENSE=10 mΩ), the full Charging section via a
  single `psmGetBatteryChargeInfoFields` call plus a BQ24193 i2c register read
  for `charging_config` (`Core::Battery::charge_info()`), and Game Activity via
  an ncm+pdm aggregate (`Core::Activity::summary()` — deliberately NOT the
  play-log save archive; see that file's header comment for why, and for why it
  must not call the name-resolving `title_play_stats()`). A batch of fields with
  no real data source (DRAM ID, device ID, fuses_burned, etc.) were removed
  rather than left as permanent placeholders — see `ROADMAP_to_parity.md` STATUS
  for the reasoning on each, especially `fuses_burned`.
- **Tools (maintenance) menu — IN PROGRESS.** `source/screens/tools_screen.*`,
  reached via `MenuItem::ToolsMenu` under System. Establishes a reusable
  destructive-operation pattern: read-only dry-run scan → Danger modal (2.0 s
  held confirm, `MODAL_HOLD_SECONDS` in `source/ui/modal.cpp`) → execute → result.
  Only "Clean install placeholders" is implemented so far; see
  `SESSION_HANDOFF.md` §4 for the full remaining op list and a cautionary note
  about a reverted orphaned-content attempt.

---

## 2. Repository Structure

> **This tree is the original plan, not the current repo, and has drifted.** Known
> divergences as of rev 3: `nsz_reader.{hpp,cpp}` shipped as `ncz.{hpp,cpp}`;
> `nca_writer.{hpp,cpp}` was never created — `stream_installer.cpp` calls the
> libnx `ncm*` C functions directly; and the M6 files (`stream_installer`,
> `overlap_buffer`, `mtp_data`, `mtp_server`, `rate_meter`, `net`, `keys`,
> `ncz_window`) are absent below, as is `tests/` (added rev 3).
> `find source -type f` is the authority; this section is intent. Trust it for
> *where a new module belongs*, not for what exists.
>
> **The PC stub build was REMOVED in rev 3 (Chief Architect ruling).** It had never
> worked: the root `CMakeLists.txt` PC branch did `include_directories(... stubs)`
> against a top-level `stubs/` that was never written (the repo had only an empty
> `source/stubs/`), so `-DPLATFORM=PC` demanded SDL2 and then died on the first
> libnx-touching `.cpp` in `SOURCES`. `PLATFORM_PC` was defined but never read by
> any source. Ruled superfluous rather than repaired: hardware iteration is faster
> than maintaining a second build of the app, and the transfer-and-test loop on
> device is short. `toolchain-pc.cmake` never existed and was never needed
> (`PLATFORM=PC` fell through to the host compiler). `PLATFORM` is retained with
> `Switch` as its only accepted value and now hard-errors otherwise, so the option
> cannot silently mislead. `source/stubs/` is deleted.
>
> This did **not** remove off-device testing, which never depended on it — see
> *Testing reality*.

```
GarageNX/
├── CMakeLists.txt                  ← root build definition
├── toolchain-switch.cmake          ← devkitPro Switch target
├── toolchain-pc.cmake              ← PC stub target for development
├── README.md
├── CONTRIBUTING.md
├── LICENSE
│
├── assets/
│   ├── fonts/
│   │   ├── Inter-Regular.ttf
│   │   └── Inter-Bold.ttf
│   ├── lang/
│   │   └── en.json                 ← bundled English baseline
│   └── icons/
│       └── icon.jpg                ← 256x256 NRO icon (required by hbloader)
│
├── source/
│   ├── main.cpp                    ← entry point, app loop, screen router
│   │
│   ├── core/                       ← all libnx system API wrappers
│   │   ├── CMakeLists.txt
│   │   ├── fs.hpp / fs.cpp         ← filesystem ops (SD, NAND, USB)
│   │   ├── title.hpp / title.cpp   ← title enumeration, install, delete, move
│   │   ├── ticket.hpp / ticket.cpp ← ticket enumeration and management
│   │   ├── saves.hpp / saves.cpp   ← save data enumeration and backup
│   │   ├── system.hpp / system.cpp ← firmware info, hardware info, SoC info
│   │   ├── battery.hpp / battery.cpp ← power, charging, max17050 registers
│   │   ├── network.hpp / network.cpp ← WiFi info, NTP, MAC addresses
│   │   ├── ntp.hpp / ntp.cpp       ← NTP sync implementation
│   │   ├── atmosphere.hpp / atmosphere.cpp ← CFW detection and info
│   │   ├── activity.hpp / activity.cpp ← game activity log (pdm:qry)
│   │   └── gc.hpp / gc.cpp         ← gamecard detection and reading
│   │
│   ├── services/                   ← background threaded services
│   │   ├── CMakeLists.txt
│   │   ├── ftp_server.hpp / ftp_server.cpp    ← clean-room FTP server (BSD sockets)
│   │   ├── http_server.hpp / http_server.cpp  ← clean-room HTTP server (BSD sockets)
│   │   ├── mtp_server.hpp / mtp_server.cpp    ← USB MTP responder
│   │   └── service_manager.hpp / service_manager.cpp ← thread lifecycle
│   │
│   ├── install/                    ← NSP/XCI/NSZ installation pipeline
│   │   ├── CMakeLists.txt
│   │   ├── installer.hpp / installer.cpp      ← orchestrator
│   │   ├── nsp_reader.hpp / nsp_reader.cpp    ← NSP/PFS0 parser
│   │   ├── xci_reader.hpp / xci_reader.cpp    ← XCI/HFS0 parser
│   │   ├── nsz_reader.hpp / nsz_reader.cpp    ← NSZ decompression (zstd)
│   │   ├── nca_writer.hpp / nca_writer.cpp    ← NCA installation to ES
│   │   ├── cnmt_parser.hpp / cnmt_parser.cpp  ← CNMT metadata extraction
│   │   └── forwarder.hpp / forwarder.cpp      ← NRO → forwarder NSP builder
│   │
│   ├── ui/                         ← renderer, theme, input, layout primitives
│   │   ├── CMakeLists.txt
│   │   ├── renderer.hpp / renderer.cpp        ← SDL2 context, frame lifecycle
│   │   ├── theme.hpp / theme.cpp              ← color tokens, dark/light swap
│   │   ├── font.hpp / font.cpp                ← Inter loader, size cache
│   │   ├── input.hpp / input.cpp              ← controller state, mapping, repeat
│   │   ├── layout.hpp / layout.cpp            ← grid, panels, scroll regions
│   │   ├── widgets.hpp / widgets.cpp          ← list, button, toggle, progress, QR
│   │   ├── status_bar.hpp / status_bar.cpp    ← persistent bottom bar
│   │   ├── title_bar.hpp / title_bar.cpp      ← persistent top bar
│   │   └── modal.hpp / modal.cpp              ← confirmation dialogs, warnings
│   │
│   ├── screens/                    ← one file per major view
│   │   ├── CMakeLists.txt
│   │   ├── screen.hpp              ← abstract base class
│   │   ├── main_menu.hpp / main_menu.cpp
│   │   ├── file_browser.hpp / file_browser.cpp     ← reused across all FS contexts
│   │   ├── network_browser.hpp / network_browser.cpp ← HTTP/FTP/GitHub navigator
│   │   ├── title_list.hpp / title_list.cpp          ← installed title manager
│   │   ├── homebrew_list.hpp / homebrew_list.cpp    ← NRO enumerator
│   │   ├── tools_menu.hpp / tools_menu.cpp
│   │   ├── system_info.hpp / system_info.cpp        ← full hardware/CFW dump
│   │   ├── ticket_list.hpp / ticket_list.cpp
│   │   ├── save_manager.hpp / save_manager.cpp
│   │   ├── activity_log.hpp / activity_log.cpp
│   │   ├── ftp_screen.hpp / ftp_screen.cpp          ← FTP mode UI + QR
│   │   ├── http_screen.hpp / http_screen.cpp        ← HTTP mode UI + QR
│   │   ├── mtp_screen.hpp / mtp_screen.cpp          ← MTP mode UI
│   │   └── settings.hpp / settings.cpp
│   │
│   ├── config/                     ← settings persistence
│   │   ├── CMakeLists.txt
│   │   ├── config.hpp / config.cpp             ← JSON read/write (nlohmann/json)
│   │   └── defaults.hpp                        ← all default values, one place
│   │
│   └── lang/                       ← localization
│       ├── CMakeLists.txt
│       └── localization.hpp / localization.cpp ← lang file loader, t() lookup
│
└── stubs/                          ← PC build stubs for libnx symbols
    └── libnx_stub.hpp              ← allows compiling core/ on desktop
```

---

## 3. Paths & Files at Runtime

```
sdmc:/switch/GarageNX/
├── GarageNX.nro
├── GarageNX.nro.bak          ← previous version after update
├── config.json
├── lang/                     ← user-dropped language files
│   ├── en.json               ← always present (bundled baseline)
│   ├── es.json               ← example user-added
│   └── br.json
├── act_logs/
│   └── YYYY-MM-DD.txt        ← one file per day, plain text
├── dumps/                    ← game dumps (user-configurable default)
├── backups/                  ← save data backups (user-configurable default)
└── logs/                     ← internal diagnostic logs (if action logging on)
```

---

## 4. Configuration Schema (`config.json`)

```json
{
  "app": {
    "language": "en",
    "theme": "dark",
    "update_check_url": "https://github.com/YOUR_ORG/GarageNX/releases/latest",
    "titledb_url": "https://github.com/blawar/titledb/raw/master/versions.txt"
  },
  "behavior": {
    "action_logging": true,           // gates writing the install log to SD
    "saves_ro_mode": false,
    "screen_dim_seconds": 30,         // menus/browsers: app-level dim after N s idle (0 = never)
    "screen_dim_seconds_net": 0,      // FTP/HTTP/MTP sessions: dim after N s (0 = never)
    "button_repeat_on_hold": true,
    "show_clock": true,
    "show_seconds": false,
    "date_format": "DMY",       // DMY | MDY | YMD  (clock + log names)
    "time_24h": true,           // 24h clock display; logs are always 24h
    "save_auto_backup_days": 0,
    "verify_hash_on_install": true    // stream SHA-256 per NCA vs content-id; fail install on mismatch
  },
  // Removed (never had a consumer): highlight_update_files, rotate_screen,
  // use_overclocking, show_cache_warming.
  "paths": {
    "save_backup": "sdmc:/switch/GarageNX/backups",
    "log_folder": "sdmc:/switch/GarageNX/logs",
    "dump_folder": "sdmc:/switch/GarageNX/dumps"
  },
  "visibility": {
    "browse_sd": true,
    "browse_system_partition": true,
    "browse_user_partition": true,
    "browse_usb": true,
    "install_from_cartridge": true,
    "browse_network": true,
    "view_installed_games": true,
    "tools": true,
    "view_tickets": true,
    "view_saves": true,
    "start_mtp": true,
    "start_ftp": true,
    "start_http": true
  },
  "mtp": {
    "sd_card": true,
    "nand_user": true,
    "nand_system": false,
    "installed_games": true,
    "sd_install": true,
    "nand_install": false,
    "saves": true,
    "album": true,
    "gamecard": false,
    "user_storages": true
  },
  "ftp": {
    "server_port": 5000,
    "allow_anonymous": true,
    "login_user": "garagenx",
    "login_pass": "garagenx",
    "start_access_point": false,
    "ssid": "GarageNX",
    "password": "",
    "use_5ghz": false,
    "hidden_ssid": false
  },
  "network": {
    "github_token": ""
  }
}
```

---

## 5. Localization System

### Canonicity rule
`assets/lang/en.json` is the **authoritative translation template**. It must always be 100% complete — no missing keys may ever be committed. All other language files are validated against it at startup; missing keys fall back to English silently. Translators need only `en.json` as their reference — no source code reading required.

### File format (`lang/en.json`)
```json
{
  "meta": {
    "language": "English",
    "author": "GarageNX Team",
    "version": "1"
  },
  "main_menu": {
    "browse_sd": "Browse SD Card",
    "browse_network": "Browse Network",
    "installed_titles": "Installed Titles",
    "homebrew": "Homebrew",
    "tools": "Tools",
    "tickets": "Installed Tickets",
    "saves": "Save Data",
    "mtp": "USB-MTP Connection",
    "ftp": "FTP Server",
    "http": "HTTP Server",
    "activity": "Activity Log",
    "settings": "Settings"
  }
}
```

### Resolution at runtime
```cpp
// Single call anywhere in the codebase
std::string label = t("main_menu.browse_sd");
```

On startup: enumerate `sdmc:/switch/GarageNX/lang/*.json`. If new files are found beyond the known set, prompt the user once to select a language. Selection is written to `config.json`. Language can be changed any time in Settings.

Fallback chain: selected language → `en.json` → key string itself (never a crash).

---

## 6. Theme System

### Color tokens (both themes)

| Token | Dark | Light |
|---|---|---|
| `bg_base` | `#1C1F26` | `#F2F4F7` |
| `bg_surface` | `#252830` | `#FFFFFF` |
| `bg_elevated` | `#2E3240` | `#E8ECF2` |
| `fg_primary` | `#E8EAF0` | `#1C1F26` |
| `fg_secondary` | `#8B90A0` | `#5A6070` |
| `fg_disabled` | `#4A4F60` | `#B0B8C8` |
| `accent` | `#4A90D9` | `#2970B8` |
| `accent_warn` | `#E8A020` | `#C07010` |
| `accent_danger` | `#D94A4A` | `#B83030` |
| `accent_ok` | `#4AB870` | `#2A9050` |
| `border` | `#33374A` | `#D0D8E8` |
| `status_bar_bg` | `#14161C` | `#E0E4EC` |

All colors defined as `SDL_Color` structs in `theme.hpp`. Swap is instantaneous — re-render next frame.

---

## 7. UI Layout

### Screen dimensions
- **Base:** 1280 × 720
- **Docked:** detected via `appletGetOperationMode()`, SDL render scale applied — UI layout stays at 720p coordinates throughout

### Persistent regions
```
┌─────────────────────────────────────────────────────────────┐
│  TITLE BAR  — GarageNX v0.1.0 │ FW 18.1.0 │ SDK 19.3.2    │ h=40
├─────────────────────────────────────────────────────────────┤
│                                                             │
│                                                             │
│                    SCREEN CONTENT                           │ h=620
│                                                             │
│                                                             │
├─────────────────────────────────────────────────────────────┤
│  STATUS BAR — SD: 47.2/128GB (36%) NAND: 2.1/32GB  42°C 🔋78% ▲ │ h=40
│  [optional: HH:MM:SS]                                       │
└─────────────────────────────────────────────────────────────┘
```

### File browser layout (default — ranger-style)
```
┌──────────┬────────────────────┬────────────────────────────┐
│          │                    │                            │
│ NAV MENU │  DIRECTORY LISTING │  DETAILS / PREVIEW PANEL  │
│  (col 1) │     (col 2)        │       (col 3)             │
│  ~18%    │      ~40%          │        ~42%               │
│          │                    │                            │
└──────────┴────────────────────┴────────────────────────────┘
```

### File browser — split view (press `-` to toggle)
```
┌──────────┬──────────────────────┬──────────────────────────┐
│          │  SOURCE              │  DESTINATION             │
│ NAV MENU │  directory listing   │  directory listing       │
│          │  ──────────────────  │  ──────────────────────  │
│          │  DETAILS             │  OPERATION CONTEXT MENU  │
└──────────┴──────────────────────┴──────────────────────────┘
```

### Button legend (context-sensitive, shown at bottom of content area)
```
[A] Open/Confirm  [B] Back  [X] Select  [Y] Mark All  [+] Context Menu  [-] Split View  [L/R] Page
```

---

## 8. File Browser — Feature Specification

### Navigation
- D-pad / left analog: move cursor
- A: open file or directory
- B: go up one level
- X: toggle selection on item (multi-select accumulates)
- Y: mark all / deselect all (useful for batch operations)
- `+`: open context menu
- `-`: toggle split-view mode
- R: next page (in file viewer)
- L: previous page (in file viewer)
- R3: toggle plain text / hex view (when file is open)

### Display columns (default list view)
```
[icon]  filename.ext                          [size / <DIR>]
```

### Context menu — single file
- Copy
- Cut
- Rename
- Delete *(confirmation required)*
- Create New Directory
- Create New File
- *(if .nsp/.xci/.nsz)* Install to SD / Install to NAND
- *(if .nro)* Copy to `sdmc:/switch/{AppName}/{AppName}.nro` + Create Forwarder NSP
  - Destination path shown to user before confirmation
  - `{AppName}` derived from NRO's embedded NACP `ApplicationName`, sanitized (spaces → underscores, special chars stripped); falls back to filename stem if NACP is absent or malformed
- *(if any file)* Open as Text / Open as Hex
- *(if .nsp/.xci)* Peek Contents

### Context menu — multi-select
- Copy
- Cut
- Delete *(confirmation required, count shown)*

### File viewer
- **Text mode** (default for: .txt .ini .json .xml .yaml .log .md .hpp .cpp .h .c .py .sh and any file ≤ header sniff reveals UTF-8 text)
  - Minimal syntax coloring: comments (grey), strings (accent), keywords (bold) — JSON, INI, C++ only
  - Chunked loading: 64KB pages, loaded on demand as user scrolls
- **Hex mode** (default for: .nca .nsp .xci .bin and any unrecognized binary)
  - Classic hex dump: offset | hex bytes | ASCII sidebar
  - Chunked loading: 4KB pages
- Toggle between modes: R3

### NSP/XCI peek view
- Title ID
- Version
- Required system version
- Content list: NCA ID, type (Program, Control, Meta, Data, etc.), size
- Icon thumbnail: 96×96, extracted from the control NCA and rendered in the corner of the details column; shown only when a valid icon is present

### Paged memory strategy
- File viewer never loads more than 2 pages (current + next) into memory
- Previous page evicted on forward scroll, next+1 prefetched on background thread
- Hard cap: 128KB in memory at any time from file content

---

## 9. Screen Inventory & Responsibilities

| Screen | Key responsibilities |
|---|---|
| `MainMenu` | Renders menu items per visibility config, routes to screens |
| `FileBrowser` | All FS contexts: SD, NAND, USB, saves, network-backed |
| `NetworkBrowser` | HTTP/FTP/GitHub URL prompt + FileBrowser adapter |
| `TitleList` | Enumerate NCM titles, show name (via TitleDB), size, version, type |
| `TitleDetail` | Per-title: delete, move, reset reqs, edit age rating, dump, repack |
| `HomebrewList` | Recursive NRO scan of sdmc:/switch, launch or manage |
| `ToolsMenu` | Routes to all tools submenu items |
| `SystemInfo` | All hardware/CFW/battery/SD/activity fields, scrollable |
| `TicketList` | Enumerate all tickets, filter by type, delete |
| `SaveManager` | Enumerate saves (installed, orphaned, backed up), backup/restore |
| `ActivityLog` | Read act_logs/ files, render by day and by user |
| `FTPScreen` | FTP server controls, IP/port/passkey display, QR code |
| `HTTPScreen` | HTTP server controls, IP/port/passkey display, QR code |
| `MTPScreen` | MTP connection status, storage toggles |
| `Settings` | All config booleans, paths, URLs, visibility toggles |

---

## 10. Service Architecture

All services run on dedicated threads managed by `ServiceManager`. The UI thread never blocks on service I/O.

```
Main Thread (UI)
    │
    ├── ServiceManager
    │       ├── FTPServer thread     (clean-room, BSD sockets)
    │       ├── HTTPServer thread    (clean-room, BSD sockets)
    │       └── MTPServer thread     (USB comms via libnx)
    │
    ├── Background Workers (thread pool, max 2)
    │       ├── File copy/move operations
    │       ├── NSP/XCI installation pipeline
    │       ├── Save backup jobs
    │       └── File viewer prefetch
    │
    └── Startup sequence (blocking, completes before menu renders)
            ├── Config load
            ├── Language enumeration
            ├── NTP sync (async, updates clock when complete)
            ├── TitleDB fetch/cache check
            └── App update check
```

Progress of background operations is surfaced via a thin shared-state struct the UI polls each frame — no callbacks crossing thread boundaries into the renderer.

---

## 11. Startup Sequence

```
1. SDL2 init (video, audio off, controller on)
2. Load Inter-Regular.ttf + Inter-Bold.ttf at base sizes
3. Apply theme from config (or dark if no config exists)
4. Load config.json → apply all settings
5. Enumerate lang/ → if new files found, prompt language selection
6. Mount sdmc:, nand_sys:, nand_user: via libnx
7. Spawn background: NTP sync
8. Spawn background: TitleDB cache check (compare ETag / last-modified)
9. Spawn background: App update check (GitHub releases API)
10. Render main menu (services and update check complete async)
```

---

## 12. Update Mechanism

### App self-update
1. On startup, fetch latest release info from `update_check_url`
2. Compare remote version tag to `APP_VERSION` compile-time constant
3. If newer: show non-intrusive notification in title bar
4. User selects "Update Available" → modal: version changelog + [Update Now] [Defer]
5. Download `.nro` to temp path on SD
6. Verify file size / optionally SHA256 (if release publishes hash)
7. Rename `GarageNX.nro` → `GarageNX.nro.bak`
8. Move downloaded file to `GarageNX.nro`
9. Display "Relaunch to apply update" → user confirms → `envSetNextLoad()` + exit

### TitleDB cache
1. On startup, check `sdmc:/switch/GarageNX/titledb_cache.json` age
2. If missing or > 24 hours old: fetch from `titledb_url`
3. Parse and cache locally as a flat `{tid: title_name}` map
4. All title display goes through this map with TID fallback

---

## 13. Localization Key Namespace (draft)

```
app.*           — app-level strings (name, version labels)
main_menu.*     — main menu item labels
file_browser.*  — file browser UI strings
title_list.*    — title manager strings
tools.*         — tools submenu strings
system_info.*   — system information labels
settings.*      — settings screen strings
modal.*         — confirmation dialog strings
status.*        — status bar strings
errors.*        — error messages
```

---

## 14. Third-Party Dependencies

| Library | Purpose | License |
|---|---|---|
| libnx | Nintendo Switch system API | ISC |
| SDL2 | Rendering, input, window | zlib |
| SDL2_ttf | TTF font rendering | zlib |
| SDL2_image | PNG/JPG loading (icons) | zlib |
| nlohmann/json | JSON config + lang parsing | MIT |
| zstd | NSZ decompression | BSD/GPL dual |
| libusbhsfs | USB mass-storage (FAT/exFAT/NTFS/ext) | GPLv2+ (GPL build) |
| Inter font | UI typeface | SIL OFL 1.1 |

GarageNX is licensed under AGPLv3; all bundled/linked components above are
license-compatible with AGPLv3 distribution. **libusbhsfs is built with
`BUILD_TYPE=GPL`**, which adds NTFS (NTFS-3G) and ext (lwext4) support and places
the library under GPLv2+. That is compatible — GPLv2-or-later may be taken as
GPLv3, and AGPLv3 permits linking with GPLv3 — but it does mean two further GPL
components ship in the binary, and the source-disclosure obligation already met
via `APP_SOURCE_URL` now covers them too. An `ISC` build of the same library would
give FAT32/exFAT only with no GPL entanglement, if that trade is ever preferred. The FTP and HTTP network services are clean-room implementations on libnx BSD sockets and add no server-library dependency; the MTP responder uses libnx USB comms.

---

## 15. Recommended Build Order (Implementation Milestones)

### Milestone 1 — Shell (no libnx, PC build)
- CMake dual-target setup (switch + pc stub)
- SDL2 renderer, theme, font loading
- Input handler
- Title bar + status bar (static placeholder data)
- Main menu rendering (no navigation yet)
- Config read/write (nlohmann/json)
- Localization loader

**Exit criteria:** App launches on PC, renders correctly, theme toggle works, config persists.

### Milestone 2 — File Browser
- FileBrowser screen (SD card only)
- Ranger-style 3-column layout
- Navigation, selection, context menu
- Text viewer (plain + hex, chunked)
- Copy/move/delete/rename operations
- Split-view mode

**Exit criteria:** Full file management on SD card works on hardware.

### Milestone 3 — System Data (libnx integration begins)
- Status bar with live data (SD/NAND free space, battery, temp, clock)
- System info screen (all firmware/hardware fields)
- NTP sync
- TitleDB fetch + cache

**Exit criteria:** Status bar is live, system info screen is complete.

### Milestone 4 — Title Management

**Design decisions (locked):**
- **Naming/icons via NCA decryption (DBI's method), not network TitleDB.** For
  each title, locate its Control NCA, decrypt it using the keyset at
  `sdmc:/switch/{prod,title,dev}.keys`, and parse the NACP for the display name
  and the 256×256 JPEG icon. No network dependency.
- **Keys required.** If the keyset is missing or incomplete, title management is
  BLOCKED with a clear on-screen message explaining which keys are needed and
  where to place them (`sdmc:/switch/prod.keys`). We do not fall back to `ns` or
  title IDs — better to be explicit than to show a half-broken list.
- **Full destructive parity in M4:** delete, dump-to-SD (as NSP), and move
  SD↔NAND are all implemented this milestone.
- **Delete confirmation:** simple yes/no modal that shows the title's resolved
  name (not just the ID) so the user sees exactly what they're removing.

**Build phases:**
- Phase A — `core/keys` (parse keyset), `core/nca` (NCA header + Control NCA
  decrypt → NACP), `core/ncm` (enumerate installed titles). Unlocks the deferred
  SDK-version read (SystemVersion title `0100000000000809`).
  **[COMPLETE — validated on hardware FW 21.0.1]** Full pipeline proven:
  keys → NCM enumeration → NCA3 header AES-XTS decrypt (header_key) →
  key-area AES-ECB decrypt (key_area_key_application) → section AES-CTR decrypt →
  IVFC superblock parse (RomFS lives at last level's logical_offset, NOT
  section byte 0) → RomFS file-table walk → control.nacp parse → display name +
  version. 100% of installed titles resolve correctly.
  Key crypto details nailed down during validation:
    - Titlekey-crypto titles: titlekey retrieved from the console's ES common
      ticket system (hand-written IPC: CountCommonTicket cmd 9,
      GetCommonTicketData cmd 16), decrypted with titlekek[keygen] (AES-ECB).
      title.keys is also honored (its value is the ENCRYPTED titlekey — must be
      titlekek-decrypted, not used raw).
    - AES-CTR reads MUST be 16-byte aligned. Reading at an unaligned offset
      desyncs the keystream and produces garbage. Round the offset down to a
      0x10 boundary, decrypt, then skip the head bytes. (This bug manifested as
      ~50% of titles failing depending on where their file-metadata table
      landed — a false-positive header decrypt masked it.)
    - Personalized tickets (RSA-wrapped, console-specific) are NOT yet handled;
      those need the eTicket device RSA key via SPL. Deferred — will show the
      program ID with a "personalized ticket" note if encountered.
- Phase B — TitleList + TitleDetail screens (icon, name, version, size, type,
  location). **[COMPLETE]** Installed-titles browser with resolved names/icons,
  progress-bar loading, and updates/DLC grouped under each app in TitleDetail.
- Phase C — delete, dump-to-SD, move SD↔NAND with confirmation flows.
  **[COMPLETE — all three validated on hardware]**
  **Design decisions (locked):**
  - **Delete first** (safest), then dump, then move — sequenced by risk.
  - **Delete scope:** deleting a base application ALSO removes its updates and
    DLC (they're useless without the base). Deleting a standalone update or DLC
    removes only that item.
  - **Delete confirmation:** hold-A-for-3-seconds on a modal that shows the
    resolved title name — real friction for an irreversible action, not a
    quick yes/no.
  - **Dump-to-SD: DONE (validated on hardware — installs + launches).**
    Standard NSP packaging: enumerate the meta's contents via
    ncmContentMetaDatabaseListContentInfo, build a PFS0 (NSP) container, and
    stream each NCA byte-for-byte from NCM content storage (4 MB chunks,
    multi-GB safe, no NCA modification so every hash stays valid). For
    titlekey-crypto titles, the ticket is retrieved from the console's ES system
    (GetCommonTicketData, cmd 16) and added as <rights_id>.tik. The .tik alone is
    sufficient for DBI install (cert not required). Runs on a background thread
    with a live progress bar + cancel. Output: sdmc:/switch/GarageNX/dumps/.
    Key IPC lesson: ES rights_id is RAW INPUT DATA (serviceDispatchInOut in-arg),
    NOT a buffer — passing it as a buffer yields LibnxError_BadInput (0xF601).
    ES ticket path is the fallback after title.keys in name resolution, which is
    why the IPC bug stayed latent until dump relied on ES directly.
    TICKET-LESS (standard-crypto re-encryption) remains a future enhancement;
    header-mod crypto already verified on host (nca_modify_roundtrip_test.cpp),
    but needs CNMT content-record rewrite + NCA hash recompute to install.
  - Delete uses ncmContentStorageDelete for each content id + ns record removal
    (nsDeleteApplicationRecord / ns application management).
  - **Dump-to-SD: TICKET-LESS NSPs** (per design decision). For titlekey-crypto
    NCAs: write the decrypted titlekey into key-area slot 2, re-encrypt the key
    area with key_area_key_application[keygen] (AES-ECB), clear the rights ID,
    and re-encrypt the 0xC00 header with header_key (AES-XTS, Nintendo tweak).
    Header-modification crypto verified on host via round-trip test
    (nca_modify_roundtrip_test.cpp): key-area ECB and header XTS are exact
    inverses of what core/nca.cpp reads; full flow confirmed (rights id cleared,
    recovered body key == titlekey, NCA3 magic intact). Remaining pieces —
    CNMT content-record handling, NCA header SHA-256 recompute, PFS0 packaging,
    streaming from NCM — built on top of this verified core.
    Output: sdmc:/switch/GarageNX/dumps/. Each meta (base/update/DLC) → own NSP.

- Title enumeration (NCM)
- TitleList screen with name/icon resolution (NCA-decrypt)
- TitleDetail: delete, dump to SD
- Move between SD ↔ NAND
- **Carried over from M3 (need NAND/NCM/save plumbing):**
  - **SDK version** — read the NintendoSDK version the firmware was built
    against (DBI shows e.g. `21.4.0` on FW `21.0.1`). Lives in the SystemVersion
    title `0100000000000809`; requires NCM read. Currently shows `—`.
  - **Playtime totals & activity stats** — total/active playtime, first-gameplay
    date, and session counts must be read from the per-user play-log save
    archive `SYSTEM:/save/80000000000000F0` (the source DBI and NX-Activity-Log
    use), tied to the account/user profile. The pdm query APIs
    (`pdmqryQueryAppletEvent`, `pdmqryQueryPlayStatisticsByApplicationId`) do NOT
    reproduce DBI's numbers on hardware — the raw event log's first entries
    predate the RTC being set (first-play reads ~2025-01-01) and session counts
    include system-applet churn. Needs the save-archive mount + parse
    infrastructure M4 introduces. Currently all N/A.

  **Resolved during M3 (no longer deferred):**
  - ~~True serial number~~ — DONE. Read from Atmosphère's pre-blank PRODINFO
    backup filename at `sdmc:/atmosphere/automatic_backups/<SERIAL>_PRODINFO.bin`
    (no NAND access needed). Accurate to the character on hardware.

**Exit criteria:** Can enumerate, view, delete, and dump installed titles.

**[MILESTONE 4 COMPLETE — all features hardware-validated on FW 21.0.1]**
Move SD↔NAND (Core::TitleOps::move_title): true full move via NCM install
pipeline (GeneratePlaceHolderId → CreatePlaceHolder → WritePlaceHolder streamed
→ Register), meta record copied via ncmContentMetaDatabaseGet/Set + Commit, then
source removed ONLY after the destination fully succeeds (strict phase ordering;
any earlier failure rolls back the destination and leaves the source intact).
Confirmed on hardware: moved title appears in the correct storage and launches.
The meta-DB write alone was sufficient — no nsPushApplicationRecord refresh
needed for HOS to see the new location. Delete confirmation and all destructive
file-browser deletes now use a hold-A-1.5s gate (built into Modal Kind::Danger).

**Selective update/DLC deletion — attempted, deferred.** We tried to let users
remove JUST an update or DLC while keeping the base. Content deletion worked
(NCAs removed, space reclaimed) and DeleteApplicationRecord (ns cmd 27) worked,
but rebuilding the application record to drop only the update via
PushApplicationRecord (ns cmd 26, HipcPointer buffer) consistently returned
NotFound (0xCE01) even when pushing back valid, unmodified records read via
ListApplicationRecordContentMeta (cmd 17). Empirically determined the correct
command IDs (17/27/26) and buffer type (HipcPointer for push) via an on-device
probe battery, but couldn't resolve the push NotFound without further guessing.
Decision: removed selective delete; whole-title delete
(nsDeleteApplicationCompletely) works cleanly and is the supported path. The
record-rebuild remains a candidate for a future revisit if the push semantics
are cracked (likely the last_modified_event value or additional service state).

### Milestone 5 — Installation Pipeline
- NSP/XCI/NSZ parser
- NCA writer + ES ticket install
- CNMT parsing
- Hash verification
- Installation from file browser context menu

**Exit criteria:** Can install NSP/XCI from SD card to both SD and NAND.

**[MILESTONE 5 COMPLETE — HARDWARE VALIDATED ✓]**

#### Validated install pipeline (ground truth from Sphaira/yati + hardware iteration)

**Container parsing:**
- PFS0/NSP: magic "PFS0", u32 file_count at 0x04, u32 strtab_size at 0x08, entries at 0x10 (0x18 each: u64 off, u64 size, u32 name_off, u32 rsvd), then strtab, then data. No alignment padding.
- HFS0/XCI: magic "HEAD" at file offset 0x100, root HFS0 byte offset at 0x130 (NOT 0x120). Root HFS0 lists partitions; "secure" partition contains installable NCAs. HFS0 entries are 0x40 bytes (0x38 for PFS0).

**CNMT NCA decryption:**
- NCA header: AES-XTS, sectors 0x200 each, header_key (validated)
- NcaFsEntry at 0x240: u32 start_block, u32 end_block (media units = 0x200 bytes) — NOT u64+u64
- key_gen = max(dec_hdr[0x206], dec_hdr[0x220]); if > 0, subtract 1 (offset-by-one)
- generation for CTR nonce: NcaFsHeader+0x140 (not 0x008, not 0x340)
- CTR nonce = generation(BE,4) || (abs_file_offset/0x10)(BE,8) — identical to nca.cpp
- Section data contains PFS0 (not CNMT-FS): magic "PFS0" confirmed
- PFS0 data region offset found via hash_info.data_offset at NcaFsHeader+0x008+0x038
- CNMT binary is the single file inside that PFS0

**CNMT header (on-disk format):**
- CnmtHeader is 0x20 bytes (NOT 0x18) — has 8 bytes reserved at end
- Content records (PackagedContentInfo) at sizeof(CnmtHeader) + extended_header_size
- PackagedContentInfo: hash[0x20] + content_id[0x10] + size_le[6] + type[1] + id_offset[1] = 0x38

**meta-DB blob format (from Sphaira/yati RegisterNcasAndPushRecord, hardware-validated):**

    NcmContentMetaHeader (libnx struct, 0x18 bytes)
      .extended_header_size = ext_sz from CNMT
      .content_count        = infos.size() + 1  // +1 for the CNMT NCA itself
      .content_meta_count   = 0
      .attributes           = 0
      .storage_id           = 0  // MUST be 0, not the destination storage
      (no install_type field in this struct)
    extended_header (raw bytes from CNMT binary, ext_sz bytes)
    NcmContentInfo for CNMT NCA (type=Meta, FIRST entry)
    NcmContentInfo[] for all other NCAs from CNMT content list

    NcmContentInfo is 0x18 bytes: content_id[0x10] + size[5 bytes, u40] + attr + type + id_offset
    Built by stripping 0x20-byte hash from PackagedContentInfo and truncating size from u48→u40.

**NS application record (from Sphaira ns.cpp + ITotalJustice, hardware-validated):**
- Delete cmd = 27 (serviceDispatchIn, u64 app_id)
- Push  cmd = 16 (serviceDispatchIn, {u8 lme=3, u8[7] pad, u64 tid}, HipcMapAlias buffer)
- lme = 3 = ApplicationRecordType_Installed
- CSR = {NcmContentMetaKey(0x10), u8 storage_id, u8[7] pad} = 0x18 bytes
- Do NOT call cmd 26 (wrong command), do NOT use HipcPointer (wrong buffer type)
- Read existing records first (cmd 17) to preserve non-GameCard entries
- PushApplicationRecord creates the NS record immediately — no reboot needed

**What does NOT work / caused corruption:**
- ncmExit()+ncmInitialize() during install session: causes NCM to reload stale index, corrupts other titles
- Passing raw CNMT PackagedContentMeta bytes to ncmContentMetaDatabaseSet: NCM stores them but GetContentIdByType reads wrong offsets
- Using HipcPointer instead of HipcMapAlias for the CSR buffer: NS rejects silently
- Using cmd 26 instead of cmd 16 for PushApplicationRecord: wrong command
- storage_id != 0 in NcmContentMetaHeader: causes GetContentIdByType index corruption

**NCM internal index behaviour (confirmed):**
- ncmContentMetaDatabaseSet updates the on-disk DB and the in-memory index together when called correctly
- Stale index from previous bad Set() calls persists until HOS uninstalls the title cleanly (Settings or DBI)
- ncmExit()/ncmInitialize() does NOT clear the stale index — NCM sysmodule stays resident

**Diagnostic tools:**
- `tools/ns_probe/ns_probe.cpp`: standalone NRO that probes NCM state for a specific title without reinstalling. Logs GetContentIdByType, GetPath, ListContentInfo, and NS record status to install_probe.txt. Build with `make ns_probe -j$(nproc)`.

**Files:** `source/install/nsp_reader.cpp/.hpp`, `source/install/xci_reader.cpp/.hpp`, `source/install/installer.cpp/.hpp`, `tools/ns_probe/ns_probe.cpp`

- Installation from file browser context menu

**Exit criteria:** Can install NSP from SD card to both SD and NAND.

#### NSZ/NCZ decompression (ground truth — hardware validated ✓)

An `.nsz`/`.xcz` is **not** a zipped NSP. Each `.nca` inside the PFS0/HFS0 is replaced by an `.ncz`: the original 0x4000 NCA header is kept verbatim, then section headers, then a zstd stream of the **decrypted** NCA body (encrypted data is high-entropy and will not compress — decrypting first is the whole point). Reconstruction = decompress the body, then **re-encrypt each section with its stored key** to reproduce the exact original NCA (so its SHA-256 matches its content-id and NCM accepts it, exactly like a verbatim NSP).

- On-disk NCZ: `[0x0000..0x3FFF]` original NCA header (verbatim) · `[0x4000]` `NczHeader{u64 magic "NCZSECTN", u64 total_sections}` · `Section[]` (0x40 each: `u64 offset,u64 size,u64 crypto_type,u64 pad,u8 key[0x10],u8 counter[0x10]`) · optional `NczBlockHeader{"NCZBLOCK", u8 ver=2, u8 type=1, u8 pad, u8 block_size_exp, u32 total_blocks, u64 decompressed_size}` + `u32 block_sizes[]` (block-compressed / random-access variant) · zstd stream(s).
- **Re-encryption is unconditional and per-section.** The body is stored decrypted for *both* titlekey NCAs (rights_id set) and standard-crypto NCAs. Sections with `crypto_type >= 3 (AesCtr)` are re-encrypted with the section key; `crypto_type < 3 (None)` passes through. There is **no** rights_id gating — an early misconception that skipped re-encryption for titlekey NCAs corrupted the body (SHA-256 mismatch).
- **CTR counter uses the ABSOLUTE NCA offset**, `counter = section.counter[0:8] || (abs_offset >> 4) (BE, 8)`. Create one CTR context per section at the section's absolute start and let `aes128CtrCrypt` auto-increment across chunks. Section-relative offsets, or recreating the context per output chunk (which truncates at non-16-aligned zstd boundaries), both corrupt the stream. Verified against Sphaira/yati `decompressFuncInternal`.
- **Header written verbatim** — any header patch (distribution bit, crypto conversion) changes the NCA hash and breaks the content-id match, so it is not done. The reconstructed NCA is byte-identical to the original.
- Placeholder is sized to the **decompressed** NCA size: block variant → `0x4000 + block_hdr.decompressed_size`; stream variant → NCA `content_size` at header 0x208.

**Ticket handling (hardware validated ✓).** A reconstructed titlekey NCA keeps its rights_id, so HOS needs a ticket to get the titlekey at launch (missing/bad ticket → "titlekey cannot be initialized", surfaced as fatal `2123-0011`). Rules, mirroring the working NSP path:
- Container `.tik` is **Common** → import verbatim with its real `.cert` (the titlekey block is already titlekek-encrypted and console-agnostic; needs no sig-patch). Do **not** rebuild it — an earlier destructive rebuild (zeroed RSA sig + empty cert) that also skipped the verbatim path was the launch-failure cause.
- Container `.tik` is **Personalised** → rebuild to Common from the raw NCZ section titlekey (titlekek-encrypt with `titlekek[master_key_revision]`, `title_key_type=0`, keep rights_id, real cert), else import verbatim.
- **No `.tik`** → fabricate a Common ticket per titlekey NCA from the NCZ section key (`make_common_ticket`). Standard-crypto NCAs (rights_id == 0) need no ticket.
- Ticket body offsets are computed from the signature type (RSA-2048 → body at 0x140): `title_key_block=body+0x40`, `title_key_type=body+0x141`, `master_key_revision=body+0x145`, `rights_id=body+0x160`. Import result is surfaced, never silently dropped.

**Verbose install log + persistent logs.** `Install::Progress` carries a mutex-guarded `log_lines` ring buffer (`push_log`/`log_snapshot`/`log_count`); the worker thread appends one line per checkpoint. The install overlay shows a live 6-line box that auto-follows the tail during install, then **stays open** on completion (no auto-dismiss), becomes scrollable (D-pad), and closes on A/B; on failure the title/error lines/final status render in the danger color. Every install (success or failure) is written to `<log_folder>/<timestamp>.log`.

**Milestone-5 housekeeping (UI/UX).**
- **Round-robin navigation** with a delayed edge-wrap (`Widgets::WrapNav`): in-range moves are instant, but at an edge the cursor arms and wraps to the opposite end only after ~450 ms of sustained contact, so holding-to-end or an accidental extra press won't wrap. Used by the `List` widget (file browser list + context menus), the installed-titles menu, and the title-detail action row. Content-scroll views (viewer, system info) deliberately do **not** wrap.
- **Input jank fix:** presses were read by per-frame state polling and diffing, which drops any press that begins and ends inside one (possibly stalled) frame. `Input::poll` now also captures `SDL_JOYBUTTONDOWN/UP` from the event queue and ORs those edges in, so every rapid tap registers as exactly one move.
- **File browser:** opening a folder resets the selector to the top (with a per-pane back-stack so **B** restores the selector to the folder you came from); pressing **A** on an installable file (`.nsp/.nsz/.xci/.xcz`) opens an **Install to SD / Install to NAND** submenu instead of the text viewer.

**Date/time preference.** New `Core::DateTime` utility centralizes all formatting. `behavior.date_format` (`DMY`/`MDY`/`YMD`, default `DMY`) and `behavior.time_24h` (default true) drive the status-bar clock. Log filenames use the user's date order but filesystem-safe separators and are **always 24-hour** (am/pm would muddy the name), e.g. `12-07-2026_14-30-05.log`. (Settings-screen selectors for these land with the Settings screen in M8; editable via `config.json` today.)

**Additional files (M5, actual):** `source/install/ncz.cpp/.hpp` (NSZ/NCZ decompressor — the doc's planned `nsz_reader` name; NCZ crypto lives here), `source/core/datetime.cpp/.hpp`, plus M5 UI edits to `installer.cpp/.hpp`, `file_browser.cpp/.hpp`, `ui/input.cpp`, `ui/widgets.cpp/.hpp`, `screens/title_list.cpp`, `screens/title_detail.cpp`, `config/*`, `main.cpp`.

**[MILESTONE 5 COMPLETE — NSP / XCI / NSZ / XCZ install + launch, HARDWARE VALIDATED ✓]**

---

### Milestone 6 — Services
**Goal:** three network/USB transfer services (FTP, HTTP, MTP), each with a status screen, plus QR codes and access-point mode. Clean-room (no ftpd/libmicrohttpd/ftpsrv dependency) — FTP and HTTP are implemented directly on BSD sockets (libnx provides them). *Originally planned:* the same code would compile as POSIX for a PC stub build to enable loopback testing. **That stub build never worked and was removed in rev 3**; all FTP/HTTP validation is on hardware.

**Shared foundation (implemented first):**
- `nifm` is already initialized in `main`; **add `socketInitialize()`/`socketExit()`** to the startup/shutdown sequence (BSD socket backing).
- `source/core/net.{hpp,cpp}` — `current_ip()` (Switch: `nifmGetCurrentIpAddressAndSubnetMask`; PC: UDP-connect trick), `hostname()`, `link_url(scheme, port, path)` helper for the on-screen address + future QR payload.
- `source/services/service_manager.{hpp,cpp}` — `NetworkService` abstract base owning the background thread + lifecycle (`start()/stop()/is_running()/status()/last_error()`), `Status{Stopped,Starting,Running,Error}`. Thread primitive is libnx `Thread` on Switch / `std::thread` on PC, hidden behind the base; concrete services only implement `run(should_stop)`.
- `source/screens/service_screen_base` pattern — status dot, `ip:port`, credentials, byte/session counters, QR panel, and a Start/Stop toggle; concrete screens (`ftp_screen`, `http_screen`, `mtp_screen`) fill in the specifics.

**FTP server (first service — implemented):** clean-room RFC 959 subset over BSD sockets. `select()` loop over the listener + client control sockets; passive mode (`PASV`/`EPSV`). Commands: `USER PASS SYST FEAT OPTS PWD/XPWD CWD CDUP TYPE PASV EPSV LIST NLST RETR STOR DELE RMD/XRMD MKD/XMKD RNFR RNTO SIZE NOOP QUIT`. Paths map POSIX `/…` ↔ the VFS (M6 initial root = `sdmc:/`; multi-mount `/sdmc`, `/nand` virtual roots are a follow-up). File transfers use streaming stdio (`fopen` on `sdmc:/…`); directory/metadata ops use `Core::Fs`. Config lives in `config.ftp` (`server_port` default 5000, `allow_anonymous`, `login_user`, `login_pass`).

**HTTP server (implemented, hardware validated ✓).** Clean-room HTTP/1.1 subset on BSD sockets. `GET` on a directory returns a JSON listing (`{"path":"/dir","entries":[{name,type,size}]}`, POSIX paths — the `sdmc:` VFS prefix is never exposed) for the in-app NetworkBrowser; `GET` on a file streams it with `Content-Length`; `PUT` uploads when `http.allow_upload`, else 403; other methods 405. Path traversal is clamped to the root exactly as FTP. `SO_RCVTIMEO` is set on each accepted socket — the server handles one connection at a time, and without a receive timeout a browser's idle preconnect socket wedges it for the full client keep-alive (~2 min, observed on hardware). Config: `http.server_port` (8080), `http.allow_upload`.

**Transfer stats (`services/rate_meter.{hpp,cpp}`).** Services publish monotonic byte counters and nothing else; `Services::RateMeter` is sampled once per frame by a screen and converts deltas into a smoothed bytes/sec figure. Keeping time out of the service threads means no service needs a timer and MTP gets speed for free by exposing the same counters. FTP counts **distinct peer addresses**, not sockets: one user routinely holds two control connections (most clients open a second on first directory entry), so counting sockets reported "2 clients" for one person.

**QR rendering (`core/qr.{hpp,cpp}` + `Widgets::draw_qr`, implemented ✓).** Clean-room encoder, deliberately scoped: byte mode, ECC level M, versions 1-6 only. v6-M holds 106 bytes against ~28 for `http://192.168.100.100:8080/`, and stopping at v6 avoids the version-information block entirely (it exists only from v7). For v1-6 at level M every block is equal-sized, so there is no group-1/group-2 split. All 8 masks are scored and the lowest-penalty one wins. `encode()` returns an invalid `Code` if the payload will not fit; screens fall back to plain text. Validated by fuzzing every payload length 1-106 through a real scanner (zbar) — 106/106 round-trip, over-capacity refuses cleanly. `Widgets::draw_qr` renders dark-on-white with the mandatory 4-module quiet zone and integer module scaling, deliberately ignoring the theme (an inverted or fractionally-scaled QR does not scan).

**AGPL §13 source disclosure (implemented ✓).** `APP_SOURCE_URL` is a compile definition (single source of truth; forks repoint it). Surfaced on the FTP and HTTP screens and in System Info under an Application section alongside version and licence.

**MTP responder — licence constraint (decided).** libhaze (Atmosphère's MTP responder, used by sphaira via a `haze::FileSystemProxyImpl`) is the obvious shortcut and is **not available to us**: its sources are licensed *"under the terms and conditions of the GNU General Public License, version 2"* — GPLv2-**only**, with no "or later" clause, which is legally incompatible with this project's AGPLv3. The responder is therefore clean-room. Verified against libnx (ISC, our toolchain): `usb:ds` supplies everything required — `usbDsRegisterInterface`, `usbDsInterface_AppendConfigurationData`, `usbDsInterface_RegisterEndpoint`, `usbDsEndpoint_PostBufferAsync`, `usbDsParseReportData`.

Unlike QR (where capping at v6 kept scope small) MTP has **no scope escape hatch** — a host will not enumerate a partial responder, so the minimum operation set is dictated by the host, not by us. It is therefore delivered in slices:
- **Slice 1 (done, hardware validated ✓):** USB transport + descriptors + `GetDeviceInfo` / `OpenSession` / `CloseSession` / `GetStorageIDs` / `GetStorageInfo`, plus `MTPScreen`. Confirmed on Debian: the console enumerated, the host loaded its MTP driver, read the `iProduct` string, parsed DeviceInfo, opened a session and located the storage — failing only at `GetObjectHandles` with our own `0x2005 OperationNotSupported` (surfaced by libgphoto2 as `-6 GP_ERROR_NOT_SUPPORTED`), i.e. exactly the slice boundary.
- **Slice 2 (done):** browse — `GetNumObjects` / `GetObjectHandles` / `GetObjectInfo` / `GetObject`. Object handles are interned on first sight during a listing and are stable for the session (handle N is `m_paths[N-1]`; 0 is reserved as invalid); the table is cleared on OpenSession/CloseSession. `GetObject` streams the payload across bulk transfers so a multi-GB read never needs a multi-GB buffer. **Known gaps:** `ObjectInfo.ObjectCompressedSize` is 32-bit in PTP, so a >4 GiB file on exFAT is misreported (exact on FAT32, which caps at 4 GiB) — 64-bit sizes need the MTP object-property path; and the `GetObjectHandles` format filter is ignored (all entries are returned).
- **Slice 3 (done):** write — `SendObjectInfo` / `SendObject` / `DeleteObject`. `SendObjectInfo` names the object and `SendObject` supplies the bytes, so the destination is carried between the two commands (`m_pending_*`), armed by exactly one SendObject and disarmed if the host disconnects. Uploads stream to disk and a short/aborted transfer removes the partial file rather than leaving it. Filenames containing a path separator, `.` or `..` are rejected — the host names the object but must not be able to escape the storage root. **Known gap:** a handle whose object was just deleted stays in the table until the session ends (hosts re-enumerate, so this is benign).
- **Slice 4a (COMPLETE, hardware validated ✓):** install storages. Verified end-to-end at 232 MB, ~4 GB and 4.8 GB — installs and launches. Two write-only drop zones are exposed alongside "SD Card" — `Install to SD` (0x00020001) and `Install to NAND` (0x00030001), each gated on `config.mtp.sd_install` / `nand_install`. Writing an NSP to one streams it into NCM; browsing them returns an empty list. Plain NSP only — an `.ncz` is refused up front with a message pointing at the file browser, never half-installed.

**MTP host detection (hardware-learned).** The USB `idProduct` decides which host stack picks the device up. With an arbitrary id (`057e:3000`) libmtp's device database does not recognise it, so Linux falls through to the generic PTP/camera backend (libgphoto2): the device mounts as `gphoto2://`, shows a camera icon, names storages `store_XXXXXXXX` instead of using `StorageDescription`, and `mtp-detect` reports "No raw devices found". Using **`057e:201d`** — the id libmtp knows as *"Nintendo Switch / Switch Lite"*, and the one DBI uses — puts the device on the libmtp/`mtp://` path. `DeviceFriendlyName` (device property 0xD402) is also answered; without any device property a host falls back to the USB `iProduct` string for the device label.

**Transfer overlap (`services/overlap_buffer.{hpp,cpp}`, implemented).** The receive paths used to read a chunk from USB, write it to storage, then read the next — strictly alternating, so the bulk endpoint idled for the whole write and the storage idled for the whole read. Measured on hardware that cost about half the achievable rate (~18 MB/s installing vs ~36 MB/s for a plain copy over the same link). `OverlapBuffer` lends the caller one of two page-aligned buffers to fill while a worker thread drains the other. The sink is a callback, so the same class serves an NCM placeholder write, an `fwrite` to SD, or a future FTP/HTTP path; the thread primitive follows `NetworkService` (libnx `Thread` on Switch, `std::thread` on PC). `flush()` fences the worker before `finish()` touches the installer from the calling thread, so `StreamInstaller` needs no locking of its own. If the buffers or worker cannot be created the callers fall back to the original direct path rather than failing. Was exercised during development for byte-exact in-order delivery, single-chunk-in-flight (no buffer reuse under the worker), latched sink failure without deadlock, prompt destruction mid-flight, and a measured ~54% of serial time, clean under ThreadSanitizer and ASan/UBSan — but **that harness was never committed and no longer exists** (see *Testing reality* below). Only the hardware validation below is reproducible today. **Hardware validated ✓** — install throughput went from ~18 MB/s to 24-38 MB/s, i.e. parity with a plain file copy over the same link, so installing now costs essentially nothing over transferring. The remaining variance is the SD card, not the transport: a ~4 GB title runs high-30s while a 5 GB title drops to mid-20s, the signature of pseudo-SLC write-cache exhaustion, and a 350 MB title averages low-30s because the fixed finalisation cost (CNMT parse, meta registration, ticket import) does not scale with size. Further transport tuning would be chasing the card; the path is now storage-bound.

**64-bit object sizes (implemented, hardware validated ✓).** MTP's `ObjectCompressedSize` and the data-container length are both 32-bit, so a >4 GiB object arrives with `0xFFFFFFFF` in both — which would have silently capped a large stream install at 4 GiB, the very limit the feature exists to defeat. Rather than implement MTP object properties to recover a 64-bit size, `StreamInstaller::container_size()` derives the true length from the PFS0 entry table (64-bit and exact) and `recv_install` adopts it as soon as the header lands — well before a large transfer nears the 4 GiB mark. Was checked during development at 764 MB, just under 4 GiB, just over 4 GiB, and a 14 GiB multi-entry container; **that harness was never committed** (see *Testing reality*). **Hardware validated ✓** — 232 MB, ~4 GB (the boundary that previously capped), and 4.8 GB all install and launch.

**Keys are not ambient (hardware-learned).** `Core::Keys::get()` is only valid after `Core::Keys::load()` — the keyset lives in a singleton that nothing loads implicitly. The file-browser install path calls `load()` before `get()`; a service thread that skips it receives an *empty* keyset and the install fails at "failed to decrypt/parse: CNMT", long after the transfer has completed. Any new install entry point must `load()` first, and should check `Core::Keys::available()` **before** the data phase so a keyless console fails instantly instead of after a multi-GB transfer.

**MTP structure.** `services/mtp_data.{hpp,cpp}` holds the wire format (container framing, UTF-16 strings incl. surrogate pairs, arrays, dataset builders) and is deliberately free of USB/libnx so it can be unit-tested off-device — the transport half can only ever be exercised on hardware, so everything testable is isolated from it. `services/mtp_server.{hpp,cpp}` owns `usb:ds` and operation dispatch, reusing `NetworkService` purely for its thread lifecycle (the base has no network-specific API). usb:ds requires **page-aligned DMA buffers**; transfers use bounded waits with `usbDsEndpoint_Cancel` + drain on timeout so `stop()` stays responsive when the host goes away. `MTPScreen` shows connection/session state rather than an address — there is nothing to scan over USB.

**Stream install (`install/stream_installer.{hpp,cpp}`, implemented).** `install()` is pull-based random-access — it reads the CNMT first (which may sit anywhere in the PFS0) and then reads each NCA by offset — while a USB stream only goes forwards. Buffering the whole NSP to disk would defeat the purpose, since the FAT32 4 GiB file ceiling is exactly why stream install exists.

The resolution turns on an existing property of `install()`: it **skips any NCA that is already registered, without ever calling `read()`**. So `StreamInstaller` parses the PFS0 header/table as it arrives (every entry's byte range is known before its data does), streams each NCA straight into an NCM placeholder and registers it as its last byte lands, and tees the small entries (`.cnmt.nca`, `.tik`, `.cert`) into RAM as they pass. `finish()` then hands the entry list to the **existing, hardware-validated `install()`** with those small entries served from RAM: every NCA hits the skip path, and meta registration plus ticket import run exactly as they do for a local NSP. **`install()` and `installer.hpp` are untouched** — the only change to the validated code was dropping `static` from `content_id_from_name` so both paths share one hex-decode rather than duplicating a security-relevant parse.

The state machine was exercised off-device during development against synthetic PFS0 containers fed at chunk sizes of 1, 7, 64, 4096 and whole-file, verifying entry boundaries hold across arbitrary transfer splits, that the RAM tee is byte-exact, and that `.ncz` and bad magic are rejected rather than half-installed. **That harness was never committed** (see *Testing reality*); the behaviour is currently guarded only by hardware runs.

**Access-point mode — SHELVED (Chief Architect ruling).** No implementation path exists: `nifm` exposes no access-point API, and `ldn` is Nintendo's *local network communications* for Switch-to-Switch play — tied to a `local_communication_id` that must match the NACP, carrying game `advertise_data`, and not joinable by a PC. Ruled superfluous: WiFi is ubiquitous and the feature was a nice-to-have, never a requirement. The `config.ftp` AP fields (`start_access_point`, `ssid`, `password`, `use_5ghz`, `hidden_ssid`) are now dead and should be removed when the Settings screen lands in M8.

**Testing reality (rev 3 — read this before trusting any "unit-tested" claim above).**
As of rev 3 this repo has **no PC build of the app, by decision** (see §2), and
until rev 3 had no tests at all. The
off-device suites this document credited for OverlapBuffer, the StreamInstaller
state machine, container sizing, and MTP wire format were written and run inside
disposable AI-session containers during development and **were never committed**;
they are gone. Everything M1-M6 was ultimately validated on Switch hardware, which
is real evidence but cannot catch a data race — a passing install proves the
scheduler happened to cooperate that run. Treat the qualified claims above as
development history, not as a live regression net.

`tests/` (rev 3) is the first committed test suite, and it sidesteps the broken PC
branch entirely. It is a **separate CMake project** with its own `project()`,
configured against the host compiler and never included by the root:

    cmake -S tests -B build-tests && cmake --build build-tests
    ctest --test-dir build-tests --output-on-failure --repeat until-fail:20

It is kept separate because the root defaults to the Switch toolchain (an
aarch64 test binary cannot be run by the developer), because each harness has its
own `main()` which would clash with `source/main.cpp`, and because the PC branch
drags in SDL2 that the tests do not need. Each suite is built twice — once under
ThreadSanitizer, once under Address/UB, since the two are mutually exclusive.
Harnesses are plain C++17 with asserts and no framework (the coding standard bars
new third-party dependencies without an approved task).

**`ncz.cpp` is not portable, and that matters for what these suites mean.** Its
`#else` branch (`ncz.cpp:438`) stubs the whole decompressor out on the host —
`get_decompressed_size()` returns 0, `decompress()` returns "not supported on
this platform" — because the real implementation uses libnx crypto
(`aes128CtrCrypt`, `aes128XtsDecrypt`). So `stream_installer.cpp` *links* on the
host, which is what makes `stream_container_test` possible, but **any host test
of NCZ decompression would be testing that stub**. `stream_container_test`
therefore asserts on container parsing only and says nothing about NCZ.
`NczWindow` is the genuinely portable piece and is tested for real.

**Link stubs vs behavioural stubs.** `tests/link_stubs.cpp` supplies
`Install::install()` (libnx-bound via `installer.cpp`) and
`Core::Keys::requirement_message()`. This is not a hole in the admission rule.
A *behavioural* stub stands in for logic while the test runs, so the result
depends on it — forbidden. A *link* stub satisfies the linker for code the test
never reaches. The safeguard is that `install()` **aborts**: if a suite ever
calls `finish()`, it dies loudly rather than passing against a fiction. Tests
assert that a refusal happened, never on stubbed message text.

**Suites as of rev 3** (`cmake -S tests -B build-tests`, 6 targets — each suite
built once under TSan and once under ASan/UBSan):
| Suite | Covers | Does NOT cover |
|---|---|---|
| `ncz_window_test` | `NczWindow` for real: races, deadlock, back-pressure, byte-exactness, loud failure on design violations | — |
| `mtp_protocol_test` | `mtp_data`: container header codec, UTF-16LE strings, ObjectPropList parsing, u64 sizes past 4 GiB | `mtp_server` — the transport is hardware-only |
| `stream_container_test` | PFS0 parsing, entry boundaries, gap skipping, `container_size()` arithmetic to 14 GiB, malformed-input refusal | anything NCZ (stubbed on host), `finish()` (aborts by design) |

**Admission rule:** a module belongs in `tests/` only if it is free of libnx.
`mtp_data` and `NczWindow` are written that way deliberately, so they compile
against `source/` with no stub layer. Anything touching `usb:ds`, `ncm`, `es`, or
`fs` is hardware-only — do not stub those to manufacture a green light, because a
passing stub proves only that the stub works. That boundary, not a PC build, is
what determines whether something is testable.

**Remaining M6 work:**
- **Slice 4b:** NSZ stream install. **Window landed (rev 3): `install/ncz_window.{hpp,cpp}` + `tests/ncz_window_test.cpp`. StreamInstaller wiring outstanding.**

  The conflict is not merely that the NCZ decompressor *pulls* while USB *pushes*; it pulls **by offset**:

      using ReadFn = std::function<size_t(uint64_t offset_in_entry, void* buf, size_t len)>;

  A plain producer/consumer pipe does **not** serve it, and the earlier "ring buffer + install thread" sketch is insufficient. Reading `ncz.cpp` establishes the real access pattern, which is worse than "the header is read twice":

  - `get_decompressed_size()` reads `NczHeader` at `0x4000`, then the optional `NczBlockHeader`, then seeks **back to offset 0** for `0xC00` bytes. `begin_entry` must call it before `CreatePlaceHolder`, because the placeholder is sized to the *decompressed* NCA.
  - `decompress()` then re-reads the entire region from scratch — offset 0 for `0x4000`, `0x4000` again, the section table, block header, block sizes — and finally sniffs 4 bytes of zstd magic at `compressed_start` before re-reading from `compressed_start` for real.
  - Above `compressed_start` both paths advance strictly monotonically (block loop: `block_read_off += cmp_size`; stream loop: `src_off += got`). Verified line by line — this is what makes a forward window viable at all.

  **`safe_read()` in `ncz.cpp` is `fn(off, buf, len) == len` — it does not loop on short reads.** So the window must serve a full request in one call, including one that straddles the prefix/window seam (a ~1 MB block read crossing the 8 MB prefix does exactly that). Short reads are legal only at end-of-stream.

  The shape that fits is a **retained prefix + forward-only sliding window** (`NczWindow`): hold the first 8 MB of the entry in RAM for its lifetime and serve every re-read from it; serve everything above from a window that blocks until the MTP thread has pushed that far, and hard-fail a below-watermark read rather than return wrong bytes. The prefix is sized by bound, not by parsing: the re-read region ends at `compressed_start` = `0x4000 + sizeof(NczHeader) + 0x40 * total_sections + sizeof(NczBlockHeader) + 4 * total_blocks`, which is ~74 KB for a 14 GiB NCA at the typical 1 MB block exponent and ~3.6 MB in the pathological 16 KB-exponent case. 8 MB clears both with margin; sizing it exactly would mean duplicating `ncz.cpp`'s header parse for no gain. Without the prefix the whole NCZ would need buffering — precisely what stream install exists to avoid (FAT32's 4 GiB ceiling).

  Sync is `std::mutex`/`std::condition_variable`, following `OverlapBuffer`, which uses them unguarded on both targets and is hardware-validated — so `NczWindow` needs no `PLATFORM_SWITCH` guard and no libnx stub, and is testable off-device as-is. Decompression runs on its own thread; reuse `OverlapBuffer`'s thread and sink-callback shape rather than inventing a second one. Keys are not ambient — `Core::Keys::load()` then `available()` **before** the data phase (see below).

  **Slice 4c groundwork — MTP 1.1 object properties (rev 3, NOT hardware-validated).** `container_size()` was the sole authority for the data phase (`recv_install`: *"if (m_install->container_size() > 0) payload = ..."*). That works only because a PFS0's last entry ends at the file's end. **It is false for XCI**, whose trailing padding — an untrimmed image is padded to the gamecard capacity — belongs to the transfer but to no entry. Coming up short there does not fail an install; it leaves unread bytes in the endpoint, and the next command parses file data as a container header. A desynced session is worse than a refused one.

  Fixed by asking the host instead of inferring: **`SendObjectPropList` (0x9808)** carries `ObjectCompressedSize` split high/low across command parameters 4 and 5 — the only route by which a host can declare an object of 4 GiB or more. (`SendObjectInfo`'s `ObjectCompressedSize` is u32 and saturates at `0xFFFFFFFF`.) **`GetObjectPropsSupported` (0x9801)** ships with it and is not optional: libmtp calls it *before* 0x9808 and gives up if it fails, so advertising 0x9808 alone leaves it unreachable.

  **And 0x9801 obligates `GetObjectPropDesc` (0x9802)** — found the hard way on hardware (`Error: could not get property description`, on plain NSP as well as NSZ). Answering 0x9801 is not the end of it: the host then asks for a *description* of **every** code that answer names, to learn its datatype before it can encode a value, and libmtp abandons the entire send if a single one is missing. The three operations are a package; implementing two of them is strictly worse than implementing none, because 0x9801 diverts the host onto a road that then dead-ends.

  So the advertised list carries a two-way obligation: every code named must be **describable** by `build_object_prop_desc()` *and* **decodable** by `parse_object_prop_list()`. Naming an undecodable property would make the host send a value we reject, taking the dataset with it. `tests/mtp_protocol_test.cpp` asserts both directions over exactly that list — including that each ObjectPropDesc dataset is consumed to the byte, since a wrong default-value width desynchronises the host on the rest of it.

  The authority now inverts: a host-declared 64-bit size outranks `container_size()`, which demotes to a cross-check that logs disagreements. The host's number describes the **transfer**; the container's table describes its **contents**. They coincide for PFS0, which is why the old approach worked, and they will not for XCI.

  `SendObjectInfo` and `SendObjectPropList` converge on `arm_incoming_object()`. That is deliberate: two copies of the install gate is how one of them silently stays open. **Risk on the record:** once 0x9808 is advertised, libmtp uses it for `.nsp` too — the validated install path now runs on this code. Re-test a plain NSP first. The log line *"Host declared an exact 64-bit size"* is the proof the new route is live; its absence means libmtp fell back to `SendObjectInfo`.

  **A successful stream install reported itself as a no-op (fixed rev 3).** Every line of an MTP install log came from `install()`, and read `[n/m] <hash>.nca - already installed, skipped` for every content — on titles downloaded minutes earlier. Nothing was broken; the log was.

  Two causes, one root. `install()` opened with `progress.reset()`, which clears `log_lines` under the mutex — erasing StreamInstaller's entire account of the transfer that had just happened. What survived was `install()`'s own view, and by design that view is a no-op: `finish()` hands it content **already registered**, so it correctly takes its "already installed" path for every entry (this is the trick that lets `install()` be reused unchanged — see `finish()`). The log was therefore structurally incapable of showing the real install and could only show the redundant second pass.

  `install()` gains `contents_preregistered` (defaulted false, so the file-browser path is untouched — it owns its Progress outright and for it "already installed" genuinely means pre-existing). When true: do not reset the caller's Progress, suppress the per-content skip lines (accurate, but reporting on work that happened elsewhere), and say *"Content already written by the transfer; registering metadata..."* once. **A function should not reset a Progress it did not start** — that is the general rule this violated.

  **Rejection reasons used to be discarded (fixed rev 3).** `save_install_log()` was only ever reached from `recv_install()`, so a `SendObjectInfo` refusal pushed its reason into `m_install_progress` and then returned — nothing ever read it. MTP can only answer the host with a bare response code, which libmtp renders as "Could not send object info", so the reason existed on neither side. `MtpServer::reject_install()` now handles all three refusals (folder, extension, keys): it `reset()`s the progress buffer (it may still hold the previous install's lines), records the reason, `SDL_Log`s it for nxlink, and writes `sdmc:/switch/GarageNX/logs/<stamp>_mtp.log`. **Any new pre-data-phase refusal must go through it**, or it will be silent in exactly the same way.

  **Two gates had to open, not one.** `mtp_server.cpp`'s `SendObjectInfo` tested `.nsp` and nothing else, rejecting `.nsz` before the data phase ever began — a host-side "Could not send object info" with the reason only in the on-screen install log. That gate is deliberate and correct in shape (refusing after several GB have been pushed is both rude and, since the endpoint stops being drained, a hang); it simply needed `.nsz` added. `.xci`/`.xcz` stay rejected there — an XCI is HFS0 and needs its own front-end (slice 4c). Note `SendObjectInfo` *already* required keys for any install, so the `parse_table()` keys check below is a second line of defence that matters for the future FTP/HTTP paths rather than for MTP.

  **StreamInstaller wiring (rev 3, NOT hardware-validated).** The `.ncz` rejection at `parse_table()` is gone, replaced by a keys precondition: an NSZ cannot be decompressed without `header_key`, so it is refused up front rather than partway through a multi-gigabyte transfer. The check reads `m_keys.has_header_key` on the keyset actually passed in, not the global `Core::Keys::available()`, which answers for whatever `load()` last cached and is not necessarily the same object.

  `begin_entry()` cannot create the placeholder for an `.ncz` entry: it must be sized to the *decompressed* NCA, and at that moment not one byte has arrived. So creation moves onto the worker, which blocks in `NczWindow::read()` until the header region is in. Division of labour:

  | Thread | Does |
  |---|---|
  | MTP | `begin_entry()` starts the worker; `write_entry()` pushes into the window (blocking = back-pressure onto USB); `end_entry()` calls `finish()`, joins, then `Register`s |
  | Worker | `get_decompressed_size()` → `CreatePlaceHolder(dec_size)` → `decompress()` → `WritePlaceHolder` from `write_cb` |

  Only one thread touches `m_cs` during an `.ncz` entry (the MTP thread pushes and nothing else), and `m_ncz_error`/`m_ph_open` cross threads only across the join, which is a happens-before edge. `write_cb`'s offset is the absolute NCA offset — `ncz.cpp` emits the verbatim `0x4000` header at 0 and body chunks at their true offsets — so it maps onto `WritePlaceHolder` with no cursor. `write_cb` also polls `m_progress.cancel`, which is how a UI cancel reaches a decompressor mid-NCA.

  **`abort()` ordering is load-bearing.** A worker may be inside `ncmContentStorageWritePlaceHolder` at that instant, so: abort the window (any blocked `read()`/`push()` returns) → worker unwinds → **join** → *only then* delete the placeholder. Deleting first races the delete against a live write on a handle the worker still holds. `ncz_join()` no-ops when nothing runs, so the plain-NCA path is unaffected, and the destructor calls `abort()` as a backstop. `m_ph_open` is the authority for whether a placeholder exists — it can no longer be inferred from `m_entry_open`, because on this path the *worker* creates it and may not have got that far.

  A `.cnmt.ncz` needs both paths: decompressed into a placeholder and registered, *and* teed to RAM as **compressed** bytes for `finish()`. That is correct — `ce.is_ncz` is set, `install()` decompresses it on the way in exactly as for a local NSZ, and RAM gives it the random access it needs. `install()` already handles NCZ for the file path (M5) and `stream_installer.cpp` already tagged `ce.is_ncz` — 4b supplies a `ReadFn`, it does not teach the installer about NCZ.

  **`abort()` validated on hardware (rev 3): PASSED.** USB pulled mid-transfer during a large NSZ: no hang, no crash, and the SD card's free space returned to its total — i.e. the placeholder was deleted. That is the full ordering working: window aborted → worker unwound out of `decompress()` → joined → *then* `DeletePlaceHolder`. A wrong order gives a hang or a crash; a wrong `m_ph_open` leaves an orphaned multi-gigabyte placeholder with no file to account for it. Still untested: **UI cancel**, which is a different route (`m_progress.cancel` → `write_cb` returns false → `decompress()` unwinds from the inside, rather than the window failing from the outside).

  **Hardware validation (rev 3): PASSED.** Three NSZ titles installed over MTP from a Debian host — one under 1 GB, one at ~4 GB (straddling the FAT32 file-size ceiling), one at 6 GB (above it). All clean. This exercises the retained prefix against real NCZ headers, thousands of real crossings of the 8 MB seam, placeholder sizing from `get_decompressed_size()` (a wrong size yields a `content_id` hash mismatch, not a crash — so a pass here is meaningful), and sustained `push()` back-pressure, since zstd decompression is slower than USB.

  **Still unproven, in rough order of risk:**
  - **`abort()` mid-NSZ.** The happy path never calls it. Cancel during a large NSZ, and yank the USB cable during one — this is the ordering the whole design turns on and it has never run.
  - **`.cnmt.ncz`.** The dual path (decompress → placeholder → register, *and* tee the **compressed** bytes to RAM for `install()`) only runs if the packer compressed the meta NCA. Most `nsz` builds leave `.cnmt.nca` uncompressed because it is tiny, in which case these three installs never touched that branch. Check an install log's entry list for a `.cnmt.ncz` before assuming it is covered.
  - **Prefix overflow.** A container built with a small block exponent (16 KB → ~3.6 MB of `block_sizes` for a 14 GiB NCA) approaches the 8 MB prefix. It fails loudly rather than corrupting, but it has never been seen.
  - **Keys missing.** Both the `SendObjectInfo` and `parse_table()` refusals are untested on device.

  **Prior verification status was syntax only.** Both the Switch and PC paths compile clean under `-Wall -Wextra` (the Switch path against a transcribed libnx surface, in scratch — not committed). That catches typos and arity errors, nothing more; the check validates against a hand-written stand-in, so it is weaker than a real build. The stronger guarantee is that every libnx call mirrors an existing hardware-validated call site in the same file, and the thread creation copies `OverlapBuffer`'s exact shape (`0x2C`, `-2`; stack raised `0x8000` → `0x10000` for zstd's frame). **The join ordering and every `ncm` interaction are unproven until they run on hardware.**
- **Slice 4c:** XCI/XCZ stream install. **DONE — HARDWARE VALIDATED ✓ (rev 4).**

  **Read this first: the rev 3 entry for this slice was false.** It recorded the
  collector refactor as landed. It was not in the tree, not in any commit, not in
  a stash, not on a branch. `86cfa99` — the tip — contained slice 4b, Option C
  and the keys precondition, and a `Phase{Header,Table,Data,Done,Failed}` that
  had never heard of a `Step`. The refactor was almost certainly done in a
  session sandbox that then evaporated; the *document* came back because it was
  copied out and committed, and the code did not. So the doc described a
  collector that did not exist, and anyone resuming cold — which is this
  document's entire purpose — would have built the XCI front-end on top of it.

  It cost a re-cut. The lesson is cheap by comparison and is now policy: **an
  entry here claims only what has been read back out of the tree.** "The tests
  pass" is not evidence a refactor landed — `stream_container_test`'s 65 checks
  are green *in both worlds*, by design, because a characterization test pins
  observable behaviour and that refactor was defined as changing none. Only
  reading `Phase` distinguishes them. See the verification ledger below, which
  exists so this cannot happen again quietly.

  **The collector.** `Phase{Header,Table,Data}` could not express XCI: it
  hard-codes "one contiguous table at offset 0". `Phase` is now
  `{Collect,Data,Done,Failed}` plus a `Step`, over one general rule: **collect N
  bytes at absolute offset X, then run step S**, discarding everything in
  between. `want()` refuses a backwards `off` outright — a stream cannot rewind,
  and asking is a caller bug, not a runtime condition.

  Two things surfaced that review would not have:
  - `parse_table()` **re-read `count` and `sts` from `m_hdr`** (at lines 99–100;
    rev 3 said 117–118). The old collector kept header and table buffers alive at
    once; one general blob cannot. Now stashed into `m_ent_count`/`m_str_size` at
    header-parse time. Note what this bug would have done: not an error, but a
    plausible-looking parse of counts read out of entry bytes.
  - The tail of `parse_table()` — sort into stream order, key precondition,
    `container_size()`, NCA count, enter `Phase::Data` — is
    **format-independent**. Extracted as `finalize_entries()`. Below it, nothing
    knows whether the bytes came from a PFS0 or an XCI's secure partition. That
    is what made 4c a front-end rather than a rewrite.

  **It is five steps, not four.** Rev 3 said four. An HFS0's table length is only
  knowable once its header has been read, so each of the two partitions costs two
  collections: `XciHead` (0x38 at 0x100) → `XciRootHeader` → `XciRootTable`
  (locates `secure`) → `XciSecureHeader` → `XciSecureTable` → `finalize_entries`.
  The RSA signature, the gamecard header and the update/normal partitions
  discard themselves in `Phase::Collect` for free, as predicted.

  **The root HFS0 offset is at `0x130`.** `0x120` is the gamecard IV. The dead
  `XciHeader` struct that documented `0x120` lived in `xci_reader.**cpp**:16`,
  not the `.hpp` as rev 3 said; it was unreferenced and is now deleted, replaced
  by a note saying why there is no struct. `XciReader::parse()` reads `0x130` and
  always did — check any reference against `parse()`, not against a field list.

  **XCZ needed no XCI-side code.** Classification is by name and the key
  precondition is shared, so it fell out of the front-end for free.

  **`container_size()` returns 0 for XCI, deliberately.** For PFS0 it is exact
  (the last entry ends at the file's end); an XCI continues past `secure` into
  gamecard padding that belongs to the transfer but to no entry. The host's
  declared size is the only authority. An XCI arriving **without** an exact
  declaration is **refused** at `SendObjectInfo` — coming up short does not fail
  an install, it leaves unread bytes in the endpoint and desyncs the session.

  The refusal predicate turned out cleaner than rev 3 assumed: `size_exact` is
  already `comp_size != 0xFFFFFFFF`, so `is_xci && !size_exact` refuses
  *precisely* the ≥ 4 GiB images from hosts that never sent
  `SendObjectPropList`. A 3 GB XCI over plain `SendObjectInfo` carries an exact
  size and is unaffected. This is what Option C bought.

  **Two defects fixed in passing (rev 4):**
  - **String-table size was unbounded on the PFS0 path.** `count` was bounded
    from the start; `m_str_size` never was. Both are host-supplied u32s feeding
    straight into a `reserve()`, so a 4-byte edit to an NSP header asks for a
    4 GiB allocation and takes the process out via `bad_alloc` — a crash, on a
    console, from a bad file. Confirmed real: removing the guard makes ASan
    report `out of memory`. Now bounded on both PFS0 and HFS0 by
    `count * kMaxNameBytes` — **derived**, not picked: a string table holds
    `count` NUL-terminated names and nothing else, so a three-file container gets
    a three-file bound.
  - **The keyless refusal said "NSZ" to XCZ users.** Both formats reach it by the
    same route. It now names the container actually sent. This string is the
    entire explanation a user gets; a wrong noun reads as a bug in us. The two
    suites pin each other — the PFS0 suite asserts `NSZ`, the XCI suite asserts
    `XCZ` and the *absence* of `NSZ`.

  ### Verification ledger

  Every line below is either something a console did, or something only a host
  test asserts, or something nothing has touched. Nothing is recorded here on the
  strength of anyone's recollection.

  **A console did this (rev 4).** Both suites 5×, then a real device:
  - NSP install — the regression check. The refactor and the new bound both sit
    on the path that already installed titles. It still does.
  - XCI — identified and installed.
  - NSZ > 4 GB — identified and installed.
  - XCZ > 4 GB — identified and installed.

  The XCI install also verifies the size authority end to end, indirectly but
  meaningfully: `container_size()` returns 0, so `payload` came entirely from the
  host's 64-bit declaration. Had that path been wrong the session would have
  desynced rather than merely mis-reported a number.

  **Only a host test asserts this — synthetic images, never a console:**
  - Every malformed-image refusal: bad `HEAD`, bad root HFS0, bad secure HFS0,
    no `secure` partition.
  - Wild root offsets: past-the-end, and backwards (the `want()` rewind guard,
    which nothing else exercises).
  - Hostile counts and string-table sizes, on both PFS0 and HFS0.
  - `container_size() == 0` at every point in a transfer.
  - Note the asymmetry: `stream_container_test` is a **characterization** suite —
    it pins behaviour that pre-existed it. `xci_container_test` is **not**. There
    was no prior behaviour to pin, so every assertion in it is a claim about what
    the code should do, written by whoever wrote the code. It is worth less than
    the PFS0 suite and should be read that way. It was mutation-tested (7 seeded
    defects, all caught) which is the best available substitute, not a
    replacement.

  **Not proven here — with each one's disposition (rev 4):**
  - **UI cancel.** Now has hardware data, and it is not clean: see the dedicated
    MTP-cancel item in §16. Two of four container types still crash (2168-0002).
    This is the live blocker, tracked there, not here.
  - **The XCZ-vs-NSZ noun.** PARKED. The rev 4 hardware pass had keys, so the
    keyless refusal never fired; the fix is asserted by tests only. Decision:
    not worth manufacturing a keyless fixture to prove — it will surface in
    normal use if wrong, and the two suites pin the noun against each other.
  - **The ≥ 4 GiB XCI exact-size refusal.** CLOSED as a non-issue. It needs an
    MTP-1.0-only host to fire, and MTP 1.1 has been ubiquitous for ~15 years. The
    refusal code stays (correct, costs nothing) but is not something to chase a
    repro for. Defensive, not expected to fire.
  - **`.cnmt.ncz`.** PARKED. The dual path only runs if the packer compressed the
    meta NCA, which most `nsz` builds do not (it is tiny). Decision: leave until
    it appears in a real file — the code has the branch, nothing has exercised
    it, and manufacturing one is not worth the cycle now. Reopen if a production
    file trips it.

  **Known cosmetic defect:** the container log line reads
  `Container: XCI/XCZ (gamecard image)` for both. Technically correct, useless
  for telling them apart, and the same class of defect as the NSZ/XCZ noun fixed
  above. `m_format` is already available at the call site.


**Exit criteria:** FTP, HTTP, and MTP transfer modes functional; the network screens (FTP/HTTP) show a scannable address. MTP has no address to scan; access-point mode is shelved.

### Milestone 7 — Tools & Remaining Screens
- Tools menu: all garbage clean operations
- SaveManager screen
- TicketList screen
- HomebrewList + forwarder NSP builder
- ActivityLog screen
- NetworkBrowser (HTTP/FTP/GitHub)

**Exit criteria:** Feature-complete against DBI parity.

### Milestone 8 — Polish
- App self-update mechanism
- Settings screen (all toggles)
- Light theme
- Overclocking support
- Screen dim timer
- Button repeat
- All modal warnings (destructive operations)
- Full localization key coverage

**Exit criteria:** v1.0.0 release candidate.

---

## 16. Open Items (pinned for later)

- [ ] **Exit to HOME menu (parked from M3)** — `appletRequestExitToSelf()` fires
      but has no visible effect on hardware; app neither exits nor transitions.
      DBI achieves this from applet mode, so it's possible. Investigate: (1) log
      `appletGetAppletType()` to confirm we're a LibraryApplet; (2) whether the
      close needs `appletMainLoop()` pumping vs SDL_QUIT surfacing; (3) whether
      DBI uses a different primitive. Exit-to-hbmenu works fine meanwhile.
- [ ] **Menu Reformat**

| Menu | Sub-Menu Items|  
|---|---|
|Installed Titles              |← kept top-level  |  
|Browse...                     |→ SD Card, System Partition, User Partition,  |  
|                              |  USB Drives, Network, Homebrew, Tickets, Save Data  |  
|Install from Cartridge        |← kept top-level  |  
|Connectivity...               |→ USB-MTP, FTP Server, HTTP Server  |  
|System...                     |→ Tools, Settings, Activity Log  |  
|Exit...                       |→ Home Menu, hbmenu, Bootloader, Shutdown  |  

- [ ] **Activity stats (deferred to M4)** — first-gameplay date, session counts,
      playtime totals must come from the per-user save archive
      `SYSTEM:/save/80000000000000F0` (DBI/NX-Activity-Log source), not the pdm
      event-log APIs (which give near-epoch dates + inflated counts on hardware).
      Needs M4's save-mount + parse infra. All fields N/A until then.
- [ ] **SDK version (deferred to M4)** — read from SystemVersion title
      `0100000000000809` via NCM. Shows `—` for now.
- [ ] Title deletion procedure — pull from ITotalJustice's documented process when implementing Milestone 4
- [ ] SD ↔ NAND move — validate against established practice before implementing
- [ ] NRO forwarder template blob — source from ITotalJustice reference when implementing Milestone 7
- [x] ~~NSZ decompression — confirm zstd block format specifics against nsz spec~~ **DONE (M5):** stream + block (`NCZBLOCK`) variants both handled; per-section re-encryption with absolute-offset persistent CTR context. See Milestone 5 notes.
- [ ] **M6 sockets** — `socketInitialize()` must be added to startup (before any service) and `socketExit()` to shutdown; `nifm` is already initialized. FTP/HTTP are clean-room BSD-socket implementations (the PC stub target was removed in rev 3; validation is on hardware).
- [ ] **Date/time selectors** — `behavior.date_format` / `behavior.time_24h` exist and drive the clock + log names now; the settings-screen UI to pick them lands in M8 (currently `config.json`-editable).
- [ ] GitHub browser — pagination handled via GitHub REST API (`Link` header); authenticated (token present) uses 5,000 req/hr, unauthenticated uses 60 req/hr with graceful rate-limit messaging in UI
- [x] **MTP cancel crash — FIXED (rev 4), hardware-confirmed.** C++ destruction-
      order use-after-free: MtpServer had no destructor, so members (m_install)
      were destroyed before the base ~NetworkService joined the worker thread —
      the worker used m_install after free. The crash report's real exception was
      Data Abort @ 0x0 (null deref); the 2168-0002 that misled six rounds was a
      stale result register. FIX: ~MtpServer() { stop(); } joins the worker before
      members die. HARDWARE: 10 cancels of the same NSZ at 100 MB–2.5 GB, every one
      a clean drop to GarageNX's own menu, zero crashes; the abort trace shows all
      21 abort sequences reaching a clean exit. A full NSZ install also completed
      (Installation complete, ticket rc=0x0) in the same session. Proven on host
      too: teardown_order_test races under TSan on the no-dtor variant, clean on
      the fixed. Diagnostic instrumentation has been removed; the real fixes
      (~MtpServer dtor, atomic abort guard, ncz_join-before-guard ordering,
      OverlapBuffer quiesce) remain.
      ABI-break theory ruled out (libnx 4.12.0 / AMS 1.11.2, both current). Two
      real cancel bugs fixed en route (OverlapBuffer teardown race; double-abort).
      Remaining fatal (2168-0002) fires at Close, which returns void — so the
      Result comes from the preceding DeletePlaceHolder (now traced) or a bad
      session handle. One NSZ cancel + the `after DeletePlaceHolder rc=` line
      picks the fix. See the Milestone-6 cancel note for the three hypotheses.** Pressing B during a streaming install
      crashed to HOME; space was reclaimed on relaunch. The crash code decodes to libnx module 347 / desc 18
      — a newlib `BadReent`, i.e. a threading fault, not install-logic
      corruption (consistent with the clean reclaim: `abort()` ran and deleted the
      placeholder; the fault came *after*, in thread teardown).

      Root cause: a teardown-ordering race between two worker threads.
      `recv_install` overlaps USB reads with placeholder writes via
      `OverlapBuffer`, whose worker runs the sink `m_install->feed()` — which on
      the NSZ path pushes into the decompress window. On cancel, the transfer loop
      exits on `should_stop()` and calls `m_install->abort()`, which joins the
      decompression worker and **frees the window** (`m_ncz_win.reset()`). But the
      OverlapBuffer worker was never stopped first — it was only joined by `ov`'s
      destructor at function-scope exit, *after* `abort()`. So the overlap worker
      could be inside `feed()`/`push()` on a window `abort()` was destroying:
      cross-thread use-after-free, surfacing in newlib as `BadReent`.

      Fix: `OverlapBuffer::quiesce()` — stop the worker and join it, blocking
      until any in-flight sink call returns; idempotent; the destructor now calls
      it. `recv_install` calls `quiesce()` **before** `abort()` on every teardown
      path, so the sink is provably not running when the window is freed. The
      upload path (`recv_object` → SD `fwrite`) was hardened the same way — it was
      safe only by the incidental ordering of `flush()` then `fclose()`, and now
      says so explicitly.

      **What is proven, and where:** `OverlapBuffer` had *no test at all*; it now
      has one (`overlap_buffer_test`, host, TSan+ASan). Trial 3 reproduces the
      race shape — a sink touching heap state a concurrent teardown frees — and
      was validated by mutation: reverting to the old order (free state, then
      quiesce) makes **TSan report a data race, including one in `operator
      delete`**, which is the freed window. Correct order is clean. So the fix is
      proven against the *class* of bug on the host.

      **HARDWARE RESULT (rev 4): first crash fixed, second crash exposed.** The
      `BadReent` (2347-0018) is gone. Cancelling now behaves as:

      | Cancel | Result | ncz window | placeholder owner |
      |---|---|---|---|
      | NSP | clean drop to HOME | none | this thread |
      | XCZ | clean drop to HOME | yes | worker |
      | NSZ | **crash 2168-0002** | yes | worker |
      | XCI | **crash 2168-0002** | none | this thread |

      `quiesce()` was correct and necessary — NSP and XCZ (including the exact
      NSZ-window path it targeted) now cancel cleanly. But NSZ and XCI still
      crash, with a **different** code: 2168-0002 = **ncm module 168, desc 2**
      (`ResultPlaceHolderAlreadyExists` territory), an ncm-internal fatal, not a
      memory fault — exactly the "no guarantee this was the only cause" caveat
      above, now realised. Space is still reclaimed on all four.

      This is a distinct, non-threading bug the first one was masking. No single
      code-reading explanation covers both crashing cases: NSZ (worker-created
      placeholder) and XCI (this-thread placeholder) share neither the window nor
      the placeholder owner, and cancel-percentage does not split the four. Every
      `ncm` result in `stream_installer.cpp` is checked with `R_FAILED` — nothing
      in the tree does `R_ABORT_UNLESS`/`fatalThrow` — so the fatal is raised
      *inside* ncm when we call it with a handle/id in a state it treats as a
      programming error. Which call, on which entry, is not determinable by
      reading, and cannot be reproduced off-device.

      **ABI-break theory (rev 4): RULED OUT.** Checked and clean — toolchain
      libnx 4.12.0 (≥ 4.10.0), console AMS 1.11.2 / FW 21.0.1. Past the TLS-ABI
      fix on both sides, no stale-libnx corruption. The crash is a real code bug,
      not an environment mismatch. Recorded here because the migrating crash code
      (below) made memory corruption look plausible; the version check is the
      cheap disproof, and it disproved it.

      **NARROWED (rev 4): the fatal is at ncmContentStorageClose, and Close
      returns `void`.** The trace's last line before death was `before Close`,
      with `after DeletePlaceHolder` already printed (delete completed, ph_open→0).
      Per libnx ncm.h line 55, `void ncmContentStorageClose(NcmContentStorage*)`
      — it does a local svcCloseHandle, no Result-returning IPC. So the
      2168-0002 (ncm desc 2) Result cannot originate in Close itself; it is either
      set by the preceding DeletePlaceHolder (whose Result the code discarded, now
      captured) or the svcCloseHandle faults on a session left in a bad state. ncm
      init/exit is single-lifecycle (main.cpp 122/218), so the session is not
      double-exited; m_cs open/close is balanced. Three live hypotheses, which the
      rev-4 trace now separates:
        1. DeletePlaceHolder returns 2168-0002 (ignored) → Close faults downstream
           → trace shows `after DeletePlaceHolder rc=0x4A8`.
        2. Delete succeeds (rc=0x0), Close svcCloseHandle faults on a valid-looking
           handle → storage state issue not visible in source.
        3. Session already broken before abort() entered.
      HARDWARE (rev 4), and the decode that reframes everything:
      **2168-0002 = 0x4A8 = ncm PlaceHolderAlreadyExists.** Not a Close bug, not a
      generic handle fault — a specific ncm error meaning a CreatePlaceHolder ran
      for a placeholder that already exists. And `after DeletePlaceHolder rc=0x0`
      confirms abort()'s OWN delete succeeds, so the offending create is elsewhere:
      the ncz WORKER (stream_installer.cpp ~865), which creates the placeholder on
      the NSZ/XCI path. NSP has no worker — which is exactly why NSP originally
      cancelled clean and NSZ/XCI did not.

      Because a RETURNED R_FAILED from CreatePlaceHolder is handled (it calls
      fail(), no crash), the fatal must be ncm raising the error where we do not
      check it — most likely LATCHED on the session by a failed async placeholder
      op and surfaced when Close tears the session down. Working hypothesis: a
      stale placeholder (from a prior cancelled attempt of the same NCA — note the
      test method cancels the SAME file repeatedly) collides with the worker's
      create; nothing in the codebase cleans stale placeholders at startup.

      **MISSTEP THIS ROUND, recorded so it is not repeated:** a candidate called
      ncmContentStorageCleanupAllPlaceHolder before Close. That is WRONG — per
      switchbrew it "closes/flushes all resources for the storage and causes all
      future IPC to the session to return 0xC805", i.e. it POISONS the session. It
      appeared to fix NSZ/XCZ (the crash merely moved to where the SD trace lost
      its tail) and it REGRESSED the previously-clean NSP cancel. Reverted. If
      Cleanup is ever used it belongs at install START on a fresh storage handle,
      never at teardown.

      HARDWARE (rev 4, cont.): `worker: CreatePlaceHolder rc=0x0` — the worker's
      create is CLEAN. Stale-placeholder-collision theory is dead too. Confirmed
      facts now: PlaceHolderAlreadyExists is latched from neither the worker create
      (0x0) nor abort's delete (0x0); the crash is at `before Close` with NO
      `after ncz_join` line; and ncz_running=1 when abort enters (worker alive).

      The missing `after ncz_join` is the load-bearing clue. Either the SD trace
      loses that line as the fatal races the flush, OR threadWaitForExit never
      returns because the worker is parked in a BLOCKING ncmContentStorageWritePlaceHolder
      that m_ncz_win->abort() cannot unblock — leaving a worker still holding m_cs
      when Close runs. The rev-4 build brackets the join with "join: waiting" /
      "join: worker exited" to decide which.

      METHODOLOGY NOTE: four hypotheses have now been wrong (double-abort-only;
      libnx ABI break; CleanupAllPlaceHolder; stale-placeholder collision). Each
      died to a hardware trace or a doc lookup, not to more reasoning. The pattern
      is clear — this crash is only yielding to instrumentation, so the discipline
      is: trace, read one fact, change one thing. No bundled speculative fixes.

      **ROOT CAUSE, CONFIRMED (rev 4) — a C++ destruction-order use-after-free.**
      The crash report is the key: Exception Type is **Data Abort, Fault Address
      0x0** — a NULL deref. The 2168-0002 in the report header is a STALE result
      register, not the cause; chasing it as an ncm error (six rounds) was chasing
      a ghost. The real fault: MtpServer : NetworkService has NO ~MtpServer, so C++
      destroys it as (1) empty derived dtor, (2) MEMBERS incl. m_install, (3) base
      ~NetworkService which calls stop() to join the worker. Members die in step 2,
      the worker is joined in step 3 — so m_install is destroyed while the MTP
      worker thread is still inside recv_install using it. Two threads run abort()
      on one half-destroyed installer (the abort#1-stuck-in-ncz_join /
      abort#2-does-teardown inversion in the trace is exactly this), and one
      dereferences what the other freed. Every abort()-level fix this session (the
      double-abort guard, the ncz_join reorder, the atomics, the handle-leak
      theory) was treating symptoms of THIS.

      **FIX: `~MtpServer() override { stop(); }`** — join the worker BEFORE any
      member is destroyed. One line. Proven on the host: tests/teardown_order_test
      models the base/member/thread skeleton; under TSan the no-derived-dtor
      variant reports a data race in operator delete (the crash), the derived-dtor
      variant is clean across 200 trials. This is the first fix this session with
      a host repro of the actual mechanism, not just a plausible story.

      VERIFY on hardware: the very cancel that always crashed should now exit clean,
      first try — no accumulation needed, because the bug was never accumulative
      (that reading was wrong; see below, kept for the record). The trace should
      show abort running to completion once, no concurrent abort#1/abort#2.

      ── SUPERSEDED analysis, kept for the record (all treated symptoms of the
         destruction-order bug above; each was disproven by the next hardware trace,
         which is the process working even when slow): ──

      BREAKTHROUGH (rev 4): the crash is ACCUMULATIVE. Hardware report — an NSZ
      cancelled 4× in one session dumped to HOME cleanly 3×, then crashed on the
      4th (same file, same position). A pure ordering bug fires every time; "clean
      N times then fatal" is a RESOURCE LEAK hitting a limit. This is the fact the
      four dead theories all failed to predict, and it points straight at a
      concrete defect:

      threadClose(&m_ncz_thread) exists in exactly ONE place — inside ncz_join().
      The rev-4 abort() idempotency guard (added to fix the double-abort) sat
      BEFORE ncz_join and returned early on the second call — so any path that
      reached abort() with the guard already set SKIPPED ncz_join entirely,
      leaking the worker's Thread handle. Horizon's per-process handle table is
      finite; a few leaked handles later, the next handle-creating syscall faults.
      That is precisely "clean 3×, crash on the 4th", and it is NSZ/XCI-only
      because only those paths spawn the worker. It also explains why the crash
      code MIGRATED when the guard was added two rounds ago: the guard introduced
      the leak.

      FIX (rev 4): abort() reordered so ncz_join() ALWAYS runs (it is idempotent —
      early-returns when no worker), and only the ncm teardown (Delete/Close) is
      guarded exactly-once, via an atomic exchange, AFTER the join. Thread cleanup
      can no longer be skipped. m_aborted is now std::atomic.

      CONFIDENCE: this is the fifth hypothesis; four were wrong. But it is the only
      one consistent with the accumulation evidence, and the leak is structurally
      confirmed (single threadClose site, provably skippable by the old guard).
      Trace files are now PER-RUN: each app launch resolves the first free
      sdmc:/switch/GarageNX/logs/abort_trace_N.log (numbered, not timestamped —
      no RTC syscall during teardown), so 6-8 cancels produce abort_trace_0.log
      .. abort_trace_7.log with no reconnect-and-delete between runs. The join-trace ("join: waiting"/"join: worker exited") is retained to confirm
      the worker is now joined on every cancel. VERIFY on hardware: cancel the same
      NSZ 6–8 times in one session — the old bug crashed by the 4th; the fix should
      survive all of them. Only then is this FIXED, not FIXED-PENDING.

      ── Prior analysis, still on record (the double-abort was real and is fixed,
         but was NOT the whole crash — see above): ──

      **ROOT CAUSE FOUND (rev 4), via the trace.** The hardware abort_trace.log
      from an NSZ cancel showed **two `enter` lines for one cancel** — abort() ran
      twice. Cause: every cancel site in `recv_install` does `m_install->abort();
      m_install.reset();`, and `~StreamInstaller()` *also* calls `abort()`
      (`stream_installer.cpp:88`). So abort ran explicitly, then again from the
      destructor. The second pass re-entered ncm — `DeletePlaceHolder`/`Close` on
      a placeholder/handle the first pass had already freed — which ncm treats as
      a programming error and turns fatal (2168-0002). NSP and XCZ escaped only by
      timing: whether the second pass landed inside ncm's sensitive window was a
      race, so they crashed on some runs, not the ones tested.

      This vindicates the decision not to guess a fix from code-reading: the two
      crashing cases (NSZ worker-created placeholder, XCI this-thread placeholder)
      shared no structural feature, and no static reading separated them from the
      clean cases — because the discriminator was runtime timing, which only the
      trace could show.

      **FIX: abort() is now idempotent** — a `m_aborted` guard makes every call
      after the first a no-op (no second ncz_join, DeletePlaceHolder, or Close).
      The guard is the real fix and stays; the trace and its per-call counter stay
      *only until a clean hardware run confirms it*, then both come out (the guard
      remains).

      **What is proven, and what is not.** Host tests cannot prove this: the
      dangerous second pass is `#ifdef PLATFORM_SWITCH` (ncm), and off-device
      abort()'s only action (`ncz_join`) is already self-guarded, so a double
      abort was harmless on the host even before the fix — `stream_container_test`
      has a call-path smoke test that says so in its own comment. **The proof is
      the next hardware run:** cancel an NSZ (and an XCI) and confirm
      abort_trace.log now shows a single `abort#1 enter` with an `abort#2 re-entry
      (no-op)`, and no crash. Until that trace is in hand this is FIXED-PENDING,
      not FIXED.

- [x] **Input dropped presses during fast navigation — FIXED (rev 4).** Observed
      as ~7 D-pad taps advancing ~5 lines, worse in the file browser than the main
      menu. My first analysis (a poll/event coalescing guess) named the wrong
      mechanism; a frame-by-frame trace found the real one: poll() OR-combined
      button-down events into a bitmask (`event_pressed |= mask`), so two taps of
      one button inside a single frame collapsed to ONE press. It bit hardest in
      the file browser because drawing two full panes lengthens frames, widening
      the window for multiple taps to land together. FIX: poll() now COUNTS
      down-events per button (s_press_count[]) instead of OR-ing; Input::press_count()
      exposes it; List::handle_input() steps once per counted tap (max'd with
      repeat() so a held press does not double-step). Proven on host:
      input_press_count_test mirrors the counting core and asserts 7 taps -> 7
      presses, single tap -> 1, sustained hold -> 0, analog edge -> 1. The SDL glue
      (event drain) is hardware-verified separately. NOTE: this corrects a wrong
      earlier entry — the "~28%" and the poll-coalescing mechanism were both
      mis-stated; the real cause was the event-queue OR.
- [x] **File-browser navigation stutter — FIXED (rev 4): per-frame text
      rasterisation.** After the input press-count fix, navigation STILL stuttered
      (~1 in 4 taps dropped, holds not smooth). The tell was that the main menu —
      same List widget, same handle_input — felt fine while the file browser did
      not. The difference is render load, not input: List::draw() called
      Font::render() (a full glyph rasterisation) for every visible row every
      frame, and Renderer::blit()/Widgets::draw_text() did
      SDL_CreateTextureFromSurface + SDL_DestroyTexture per call — so the two
      costliest steps (rasterise + GPU upload) ran and were thrown away every
      frame. The browser draws two panes of rows plus a details column, nav column
      and hints: 40+ rasterise+upload+destroy cycles per frame, dropping below
      60fps. Long frames cause BOTH symptoms — multiple taps land per frame
      (drops) and repeat() fires at most once per long frame (choppy holds).
      FIX: a rendered-text cache. New source/ui/text_cache.hpp holds the pure key
      (text+size+weight+family+colour) and eviction policy (hard cap + frame-age
      LRU); Renderer::draw_text()/measure_text() rasterise+upload once and reuse
      the SDL_Texture across frames, keyed by content. List::draw() and
      Widgets::draw_text() (the file browser's title/path/details/hints helper)
      now route through it, so steady-state cost is one RenderCopy per row.
      text_cache_advance_frame() (in begin_frame) ages out unused entries;
      text_cache_clear() (in shutdown, before SDL_DestroyRenderer) frees textures.
      Proven on host: text_cache_test (19 checks). SDL texture glue needs hardware
      (no SDL on host); lifetime/ordering invariants hand-audited. The input
      press-count fix (prior item) is still correct and retained, but this render
      fix is the primary cause of the stutter. HARDWARE-CONFIRMED: all menus are
      responsive; caching in the two lowest-level text helpers (List::draw and
      Widgets::draw_text) lifted every screen, not just the browser.
- [ ] **Repeat delay one frame late** — `Input::repeat()` does
      `s_repeat_map[bit]`, and `std::map::operator[]` inserts on read. `poll()`
      iterates the map, so a button acquires repeat state only *after* `repeat()`
      has asked about it once, making `first_press` one frame late and the
      effective delay `s_repeat_delay_ms + 1 frame`. Small, but it biases any
      future tuning of that constant. Fix alongside the coalescing item above;
      they are the same code.
- [~] **MTP screen: stats rework — DONE (host-tested where possible, hardware-
      pending).** Stats changed from `requests | ↑ sent | ↓ recv | rate` to
      `↑ sent | ↓ recv | Now | Avg | ETA`; `request_count()` dropped. All the
      design constraints below were honoured:
      - 1 Hz is a DISPLAY LATCH, not a frame-rate change: update() samples the
        RateMeter every frame (accuracy) and rebuilds the on-screen strings only
        once per second via SDL_GetTicks (legibility + avoids churning the text
        cache). The render loop is untouched, so input is not starved.
      - ETA denominator is WIRE bytes: MtpServer::current_wire_size()/
        current_wire_recv() publish the transfer's wire (compressed) size and the
        file payload received. ETA = (wire_size − wire_recv) / current rate; "—"
        until a size is known and a rate exists.
      - HARDWARE FIX (rev 4, post-first-test): the wire counters were first added
        only to recv_file_data() — the plain file-COPY path. But an install goes
        through recv_install(), which was NOT instrumented, so during an actual
        install current_wire_size() stayed 0 and the ETA was permanently "—".
        Fixed by adding the wire accounting to recv_install() (size resolved from
        the host's 64-bit declaration or the container table; payload counted as
        it is fed). Second bug from the same misread: the screen sampled the
        RateMeter on bytes_sent()+bytes_recv(), which counts ALL MTP protocol
        traffic — so the data-phase average anchored the instant the host
        connected, not when a transfer began ("average starts when the screen is
        drawn"). Fixed by sampling current_wire_recv() (file payload only), which
        also re-anchors per object. Both traced to putting the accounting in the
        copy path instead of the install path; RateMeter logic was correct and
        unchanged.
      - Average is over the DATA PHASE only: RateMeter now has
        average_bytes_per_sec() + data_phase_started(), anchored at the first byte
        moved (not at reset), so pre-transfer idle and post-transfer meta
        registration don't drag it down. Re-anchors on a counter reset (server
        restart). Proven on host: rate_meter_test (11 checks) — idle exclusion,
        first-byte anchoring, re-anchor after reset, reset clears all state.
        Writing the test caught two real bugs first: the average inherited the
        windowed m_last_t (idle leaked in), and reset detection used the coarse
        250 ms m_last_bytes; fixed by giving the average its own every-sample
        tracker decoupled from the rate window.
      - Layout: 5 columns × kColW=180 from cx=60 → rightmost ~900 px of 1280, fits.
      - **Per-direction rate: deliberately NOT split.** The spec floated separate
        ↑/↓ speeds. Kept the combined RateMeter (samples sent+recv) for Now/Avg:
        the ↑sent/↓recv columns already show cumulative per-direction totals, and
        the live speed is dominated by whichever direction is active (recv during
        an install, send during a pull). Two meters for a split rate is marginal
        value; revisit only if a real need appears.
      - **Language-file gap found & fixed (was NOT just "adding fields"):** the MTP
        screen referenced mtp.* keys (title/status/host/session/hint_*), but
        en.json had NO top-level `mtp` block at all — only an unrelated
        `mtp_screen` subtree (storage labels). The working FTP/HTTP screens use a
        flat top-level block (http.*, ftp.*); MTP's was simply missing, so those
        lines had been rendering raw keys on hardware. Added the full 15-key `mtp`
        block modelled on `http`, including the new speed_now/speed_avg/eta.
      HARDWARE-PENDING: verify the five columns render, ETA counts down against
      wire bytes and shows "—" before size is known, and the labels resolve
      (no raw "mtp.status" text). The SDL/libnx glue (screen draw, server wire
      counters) can't compile on the host; API contracts hand-audited.
- [ ] Full `en.json` — to be completed key-by-key as each screen is implemented; must be 100% complete before v1.0.0

---

*This document is the living reference for GarageNX development. All design decisions made prior to this document are captured above. Subsequent decisions should be appended to the relevant section with a date note.*

## Transport-agnostic install driver (StreamDriver) — the parity keystone

Extracted the install *driver loop* from `MtpServer::recv_install` into
`Install::drive()` (`source/install/stream_driver.{hpp,cpp}`) so MTP, and soon FTP
and HTTP, share ONE install path instead of three copies. The install *engine*
(`StreamInstaller`) was already transport-agnostic; what was welded to MTP was the
loop around it — size correction from the PFS0/HFS0 table, the OverlapBuffer, the
feed sequencing, and the load-bearing teardown order (quiesce the overlap worker
BEFORE abort(), the 4c UAF). All of that now lives in the driver, parameterised by
three injected callbacks: a byte source (`read`), a cancel predicate (`stop`), and a
transport unwedge (`drain`). The transport keeps only its own framing (MTP's 12-byte
data-container header) and its drain.

`MtpServer::recv_install` is now a thin adapter (~65 lines, was ~160), HARDWARE-CONFIRMED
transparent (all four formats install + cancel identically post-refactor): read the first
transfer, strip the header, hand off to `drive()` with `ep_read` as the source,
`should_stop` as cancel, `drain_data` as drain, and the wire atomics wired to the
`WireSink`.

TESTABILITY WIN — and a real bug it caught. Because the byte source is injected, the
whole driver runs on the host against a synthetic in-memory PFS0
(`tests/stream_driver_test.cpp`, 17 checks: size recovery from the table, exact-size
declaration, chunk-size invariance 1..8192, mid-stream cancel → Cancelled + drain,
early-EOF → ShortRead). Running it under TSan surfaced a data race that had been in
the SHIPPING MTP install path since 4c: `StreamInstaller::complete()`/`ok()` read
`m_phase` on the main thread while the overlap worker writes it in `feed()`. No host
test had driven the overlap worker through completion before, so TSan never saw it.
Fixed by making `m_phase` a `std::atomic<Phase>` (relaxed; it is a status flag, not a
lock). TSan now clean. The extraction improved coverage of code already on hardware.

NOTE (host-invisible guard): `stream_driver.hpp` explicitly includes `<sys/types.h>`
for `ssize_t` rather than relying on transitive visibility — the exact class of
devkitPro-only break (cf. the `fileno`/`Result` issues) that the host build cannot
catch.


## Install over FTP (Slice A) — first consumer of the keystone

FTP now mirrors MTP's STORAGE-CHOOSER model. The FTP root "/" is a chooser holding
only storage folders — no real files — exactly like picking a drive in an MTP client:

    /              → chooser: "SD Card", "SD Install", ["NAND Install"]
    /SD Card/...   → the real SD filesystem (files live HERE, one level down)
    /SD Install/x  → drop x to install it to the SD card
    /NAND Install/x→ drop x to install it to NAND

This corrects a first-cut bug where the install folders were OVERLAID on top of the
SD-card contents at the root (files and install folders hybridised into one listing).
Now the root is a clean menu and the filesystem lives inside "SD Card", matching how
MTP presents separate storages.

Gating is shared with MTP via Config::get().mtp: "SD Card" and "SD Install" show by
default; "NAND Install" is opt-in (nand_install defaults false). A STOR into a
disabled or non-storage location is rejected. So FTP and MTP always show identical
storages because they read one config.

Pieces:
- `source/services/ftp_paths.hpp` — pure path model: Root / Filesystem (under SD
  Card, with the relative remainder) / SdInstall / NandInstall / Invalid (a bare
  path under no storage root). Host-tested (tests/ftp_paths_test.cpp, 28 checks,
  incl. bare-path-is-Invalid and name-collision safety).
- `FtpServer::to_vfs` is the choke point: only "/SD Card/..." maps to "sdmc:/...";
  every other root returns "" so all filesystem commands (RETR/SIZE/DELE/MKD/RMD/…)
  reject it uniformly. That single mapping is what keeps SD contents out of the root.
- `FtpServer::ftp_install()` — the StreamDriver adapter (socket recv = byte source,
  should_stop = cancel, no framing, size recovered from the container table). Same
  hardware-validated install path as MTP.

HARDWARE-CONFIRMED: install (into SD Install), clean cancel/exit teardown, and
MTP-matching stats all verified on device. Slice A complete.

## Working with ncm — read the existing callers first

Wave 3 lost roughly six hardware rounds to the same mistake, in four different
places: writing a fresh ncm caller when this codebase already contained a proven
one, then debugging the divergence.

  * Bulk resolve_control() from a transport thread crashed the console —
    screens/title_list.cpp had always done exactly one per frame on the main thread.
  * ncmContentMetaDatabaseGetSize() was used for a title's size; it returns the
    ~100-byte META RECORD size, so every title displayed as "100 B".
  * ncmContentStorageGetSizeFromContentId() per NCA made enumeration 10x slower
    (2s -> 20s) — the size is already in the NcmContentInfo record via
    ncmContentInfoSizeToU64(), which core/title_ops.cpp already used.

RULE: before calling an ncm API, grep for an existing caller in this repo and copy
its usage — thread, pacing, and which API it chose. The compiler cannot check any
of that, and neither the host test build nor the -fsyntax-only guard covers these
files. A manual API audit against the real headers has caught several errors that
nothing else would have.

## MTP file-manager parity (browse + open) — DONE, hardware-confirmed

MTP exposes SD + Album (catalog-driven, storage derived from each object's path
prefix) and now supports opening files in place from the PC, not just installing.
The open-in-place fix took several rounds; the decisive tool each time was WIRE
LOGGING, not reasoning. Final root cause: GetObjectPropValue (0x9803) was declared
but never dispatched or advertised, so gvfs — which reads file SIZE through the
object-property path, not GetObjectInfo — got OperationNotSupported for all 261 of
its per-object size queries and rendered every file blank without ever issuing a
read. Fix: implement + advertise GetObjectPropValue, returning each property in the
exact datatype build_object_prop_desc declares (ObjectSize as UInt64 is the one
that matters). Also advertised the android.com extension + GetPartialObject64,
though the property fix was the real unblock.

BUILD-SAFETY NOTE: mtp_server.cpp is libnx-only and excluded from the host test
build, so structural edits (a str_replace eating a case header; a dropped wire
field) pass "all tests" yet fail the real devkitPro build. Guard: run
`g++ -fsyntax-only -DPLATFORM_SWITCH` on the file after every edit to catch
switch/brace/structure breaks the test suite cannot see.


## Exit to HOME (Bugs E + D) — DONE, hardware-confirmed

The app exited to hbmenu instead of HOME, and installed titles didn't appear on HOME
until close. Root cause was three interacting bugs, none of them the actual exit call:
(1) a manual hidInitialize() leaked the hid service (SDL already owns it) — caused an
unclean teardown/crash; (2) running_under_hbloader() tested envHasNextLoad(), which is
true even for a forwarder launch, so B always took the "arm next-load = hbmenu.nro"
branch; (3) the home branch called appletRequestExitToSelf(), which libnx gates to
AppletType_LibraryApplet, so it silently no-op'd in Application mode. Fixes: drop the
manual hidInitialize; detect context via appletGetAppletType(); for a real Application
exit-to-HOME is a clean process termination (pop the loop, teardown, exit(0)) WITHOUT
arming next-load; keep exit(0) (not return 0) because SDL wraps main via SDL_main.
Fixing the clean HOME exit also made qlaunch re-scan records, closing Bug D for free.
HARDWARE-CONFIRMED on both forwarder and applet launches.


*Rev 4 (cycle-accurate to hardware test): Slice 4c XCI/XCZ stream install is COMPLETE and hardware-validated — NSP/NSZ/XCI/XCZ install and cancel cleanly. Work this cycle: collector refactor re-cut (the rev-3 entry claiming it was done proved false); XCI/XCZ HFS0 front-end added; PFS0/HFS0 string-table bound; NSZ/XCZ refusal noun; WrapNav wrap-delay 450→300. The MTP cancel crash took the most effort and six wrong theories before the crash report's exception type (Data Abort @ 0x0, not the ncm result 2168-0002) revealed the true cause: a C++ destruction-order use-after-free fixed by giving MtpServer a destructor that joins the worker before members are destroyed. Three real bugs were fixed en route (OverlapBuffer teardown race, double-abort, and the destruction order). Lesson recorded: when a fault has a crash dump, read the exception type, not the result register. Full detail in §16 Open Items.*


## Save Data as a shared surface (3d-b FTP, 3d-c MTP)

Save data is the one surface the catalog's "one vfs_root" model does not
describe: `save:` only becomes valid once a specific (user, title) pair is
mounted, and only one may be mounted at a time. The top two levels
(`/Save Data/<User>/<Title>/...`) are synthesized; only the third is real.

`services/save_surface.{hpp,cpp}` is the single definition of that surface — the
two synthesized listings and, crucially, the SINGLE-SLOT MOUNT POLICY. Every
request from every transport routes through `save_resolve()`, which mounts a
file-level path's title (releasing whatever was mounted) and releases for
anything shallower. That is what makes "navigate away from a title and it
unloads" true without any transport tracking navigation state.

It was extracted from `ftp_server.cpp` when MTP became the second consumer,
unifying onto the implementation that had already been hardware-verified rather
than the reverse — the same discipline as folding `dump.cpp` onto `NspStream`.

### Why a save object cannot be interned by its real path

This is the difference between Save Data and Installed Titles, and it is not
obvious. A virtual NSP has no path on disk, but its NAME is unique, so
`titles:/<name>` is a sufficient MTP handle key. A save file's real path is
`save:/slot1.dat` — and that names a DIFFERENT FILE depending on which title is
mounted right now. MTP handles must stay valid for a whole session, so interning
by the real path would let two titles' files collide on one handle and hand the
host the wrong bytes, silently, with no error raised anywhere.

Save objects are therefore interned as:

    savedata:/<User>/<Title>/<rest...>

carrying the full three-level identity, with the concrete path derived at the
moment of use. The part after the prefix is exactly the `rel` that
`sp_split_save()` already parses, so no second decomposition exists to drift.

The prefix deliberately does NOT begin with `save:`. `StorageCatalog::
surface_for_vfs()` and `mtp_storage_for_path()` both identify a surface by a
plain prefix compare against `vfs_root`, so a `save:`-prefixed synthetic path
would be mistaken for a mounted one by the write guard. `save_surface_test`
asserts that property directly rather than trusting the spelling.

### Browsing must not mount

An MTP host requests an ObjectInfo for EVERY object it lists. Answering "is this
a directory?" for a title folder by mounting it would mount and unmount every
save on the console just to browse one user's folder — the bulk-churn pattern
that crashed the console in 3b. User and title folders are directories BY
CONSTRUCTION, so `save_synth_is_synthesized_dir()` answers them without touching
the mount. Only paths inside a title resolve, and all children of one listing
belong to the same title, so the slot stays put.

### Concurrency: verified, not assumed

`SaveMount`'s slot globals carry no mutex, which would be a cross-thread hazard
if FTP and MTP could run at once. They cannot: `FTPScreen::on_exit()` and
`MTPScreen::on_exit()` both `m_server.reset()`, and each server's destructor
stops and joins its worker. One connectivity screen is on top at a time, so one
transport exists at a time. This is load-bearing and invisible — if a future
change lets two services run concurrently (a "keep serving in the background"
option, say), the mount slot needs a lock and an owner BEFORE that ships.


## The syntax guard was checking nothing (fixed)

The documented guard for libnx-only files was

    g++ -fsyntax-only -std=c++17 -I source -DPLATFORM_SWITCH <file>
    # header-not-found errors are EXPECTED and should be ignored

and it never checked anything at all. A missing header is a FATAL error, not an
ignorable one: the compiler prints `fatal error: switch.h: No such file or
directory / compilation terminated.` and STOPS. The parser never reaches the
function bodies, so the exact class of breakage the guard was added to catch —
a `str_replace` eating a `case` header, a dropped wire field — passed it every
time. On `mtp_server.cpp` the guard produced six lines of output and zero
coverage.

Fixed by `tools/syntax_guard.sh` + `tools/stubs/`: EMPTY stand-ins for
`switch.h` and the SDL headers let the preprocessor complete so the PARSER runs
over the whole file. Undeclared types then produce ordinary non-fatal errors,
which are filtered out; what survives the filter is structure. Reparsing
`mtp_server.cpp` this way goes from 6 lines to 628, and the brace-balance check
runs alongside.

The filter is deliberately NARROW. A first draft also matched `expected ';'
before X`, which fires on every `u32 n = 0;` in the tree because the stubs leave
libnx's typedefs undeclared — it flagged four perfectly well-formed files. That
is a type symptom, not a structural one.

The stubs are NOT a PC build and must never grow declarations to silence errors:
a stub complete enough to type-check would be a second, lying definition of
libnx, which is precisely what rev 3 deleted the PC stub build for.

LIMITS ARE UNCHANGED, and a pass still means only "this file will parse":
wrong types and wrong field names pass, and a manual audit of every libnx call
against its real header is still required.


## Transport parity is about ORDER, not just outcome

Save Data over MTP shipped refusing every mutation, while FTP raised the
on-device confirmation and performed the operation on Allow. Both transports read
the same catalog and called the same guard; the difference was entirely in
**where each one resolved relative to where it guarded**.

FTP's `to_vfs()` resolves a Save Data path to its mounted `save:/...` form before
any command touches the guard. So `guard_write()` sees a path the catalog claims
(Saves, ReadOnly + `Confirm::OnDevice`), raises the modal, and the command
proceeds if the user allows it. MTP guarded the *synthetic* `savedata:/...` key,
which no surface claims — so it default-denied, and no modal could ever appear.

The rule that follows: **resolve, then guard.** Every MTP mutation
(`DeleteObject`, `SetObjectPropValue`, `arm_incoming_object`) now resolves a
synthetic save path through `save_resolve_synth()` — which mounts the title the
handle names — and guards the concrete result.

Two things stay deliberately asymmetric, and both are still parity:

* **The handle key stays synthetic.** Filesystem work uses `save:/...`; the
  interned handle remains `savedata:/<User>/<Title>/<leaf>`. A handle keyed on
  the concrete path would name a different file as soon as another title mounted.
* **The storage root and synthesized levels refuse with no prompt**, because FTP
  refuses them too (`to_vfs()` returns `""`, the command answers 550). There is no
  (user, title) at those levels, so a prompt could not name a real destination.

### The window MTP has and FTP does not

FTP's `STOR` resolves and writes within one command. MTP splits the same work
across `SendObjectInfo` (which armed `m_pending_path`) and `SendObject` (which
wrote it), and a client may issue anything in between — including a browse of
another title, which remounts the single save slot. A bare `save:/<leaf>` armed
in the first command would then have written into a **different game's save**,
silently, after the user approved a prompt naming the right one.

`m_pending_save_synth` holds the synthetic key across that gap and `SendObject`
re-resolves from it. No second confirmation is asked: the key pins user, title
and leaf, so re-resolution cannot reach a destination the user did not already
approve — it can only fail, and failure is a refusal.

Generalise this when adding any transport: **a two-command protocol turns a
resolved path into a stale one.** Anything resolved in a first command and used
in a second must be re-resolved, or carry enough identity to be re-resolved.


## Save writes must be COMMITTED, and capacity must be honest

Two failures with one root: a surface's advertised properties and its write
mechanics were both inherited from a read-only surface, and stayed wrong after
Save Data stopped being read-only.

**Journalled saves.** A Switch save filesystem journals writes made through the
`save:` mount and DISCARDS them at unmount unless `fsdevCommitDevice("save")` is
called. An uncommitted write reports success, reads back correctly for as long as
the mount is live, and is gone when the game next opens it. `Core::SaveMount::
commit()` and `Services::save_commit_if_save_path()` are called after every
successful mutation on every transport, and BEFORE success is reported to the
client — a failed commit fails the operation instead of lying about it. The
helper takes the path rather than a flag so a caller cannot forget which surface
it wrote to; a non-save path is a no-op. Note that this was missing from FTP as
well, since 3d-b: browse and pull never touch the journal, so the hardware pass
that verified them could not have caught it.

**Capacity is a wire field whose truth depends on policy.** MTP's StorageInfo
`FreeSpaceInBytes` was 0 for Save Data, copied from Installed Titles where
nothing can ever be written. Hosts check that field BEFORE opening a transfer, so
once confirmed writes were allowed, every write was refused client-side — "not
enough space" for a 553-byte file, with no request reaching the device and
nothing in a wire log to see. The surface to copy was NAND (User): read-only by
policy, writable per operation after a confirmation, real capacity figures.

The general rule: **when a surface's ACCESS changes, re-audit every value derived
from the old access.** Nothing in the type system or the host tests can catch
this class — the value is a protocol field whose correctness depends on a policy
decision made in a different file.

### Known-dead setting

`Config::Behavior::saves_ro_mode` is declared, persisted and read by nothing.
It looks like the DBI "mount saves read-only" option. Either wire it into
`save_resolve()` (mount read-only, and make the guard refuse regardless of
confirmation) or delete it — a setting that silently does nothing is worse than
an absent one, especially a SAFETY setting a user may believe is protecting them.


## "Read-only" does not mean unwritable

Three surfaces are `Access::ReadOnly` **and** `Confirm::OnDevice`: NAND (User),
NAND (System) and Save Data. That combination means read-only by POLICY —
writable per operation once the user confirms on the console. Only
`ReadOnly + Confirm::None` (Game Card, Installed Titles) is genuinely unwritable.

Missing that distinction produced the same bug twice. MTP's StorageInfo reported
zero free space for Save Data and for NAND (System), reasoning that neither was
writable. Hosts check `FreeSpaceInBytes` *before* opening a transfer, so every
confirmed write was refused client-side with "not enough space" — for a 553-byte
file, on a console with tens of gigabytes free, with no request reaching the
device and nothing in a wire log to look at.

`StorageCatalog::is_writable()` now states the rule once, and
`storage_catalog_test` pins the classification of all nine surfaces with a
failure message that points at capacity reporting. Give a surface a confirmation
path in future and a host test will fail and say what else to check.

The general shape, worth carrying to the HTTP transport: **a protocol field whose
correct value depends on a policy decision in another file cannot be verified
locally.** Comments do not survive the change that invalidates them. Where the
dependency is real, express it in a shared helper and pin it in a test.


## Config ordering, and toggles that are not global

`Core::mount_nand()` is config-gated. It used to run at `main.cpp:141`;
`Config::load()` runs at 168. So it gated on compile-time defaults, and
`bis_system:` could never be mounted however `config.json` was set.

The bug was invisible for a precise reason: `nand_user` defaults to **true**, so
`bis_user:` always mounted and NAND (User) always worked. `nand_system` is the
only surface defaulting to **false**, so it was the only one that could expose
the ordering — and being off by default, nobody hit it until someone deliberately
turned it on.

`Config::get()` now logs once, loudly, when called before `Config::load()`.
Reading config early yields defaults that are indistinguishable from a user's
real choices, so the mistake surfaces much later as "the app ignores my config".
Nothing in the type system can catch an ordering bug; a log line can. Anything
config-gated must be called below the load, and there is a marker comment at the
old site saying so.

### Mounts are global; toggles are per-transport

Surface toggles now live per transport (`Config::Surfaces`, one each in MTP, FTP
and HTTP), so saves can be served over FTP without being exposed over MTP. But a
**mount is process-wide**. Anything that mounts, initialises a service, or makes
any other global decision must ask
`Config::any_transport_exposes(&Surfaces::field)` rather than any single
transport's block — otherwise enabling NAND for FTP alone leaves the partition
unmounted, and the folder appears but refuses to open.

Migration is behaviour-preserving by construction: a legacy `config.json` has
flat keys under `"mtp"` and no `"surfaces"` object, and those flat keys seed all
three transports — exactly what the shared block used to do. Precedence per key
is: this transport's block, then the legacy flat key, then the compile-time
default. `"surfaces"` present means new format; absent means legacy, so no
version stamp is needed.

### The mount probe belongs to the catalog, not to a transport

FTP probed a surface's mount root and hid it when absent. MTP did not, and
advertised storages whose enumeration then failed — the host reports "could not
get object handles" and the user has a storage they cannot open. One config, one
catalog, two behaviours. `StorageCatalog::mount_available()` is now the single
definition, with the Save Data and non-Filesystem exemptions stated once.

Any future transport gets this for free, which is the whole reason the catalog
exists. A per-transport copy of a rule about a shared resource will drift; the
only question is which copy is wrong when it does.


## Writing config is the risky part of a settings screen

The UI is the easy half. `Config::save()` sat unused for the whole project, and
giving it a caller changed two things from harmless to dangerous.

**A field missing from the serialiser silently resets.** Anything present in the
`All` struct but absent from `to_json`/`from_json` would be quietly returned to
its default the first time a user touched any setting. All 56 fields were audited
before the screen was wired, and `test_every_field_round_trips` now pins them —
enumerated by hand deliberately, because a reflection-style loop would pass while
proving nothing. The tedium is the feature: adding a config field should mean
adding a line to that test.

**A wholesale rewrite deletes what it does not model.** `save()` used to write
`to_json()` over the file, which would remove any key the struct has no field
for — one added by hand, or written by a newer build. It now overlays our values
onto the file *as loaded* (RFC 7386 `merge_patch`), so unknown keys survive. The
single deliberate exception is the pre-split flat surface keys under `"mtp"`,
which have been migrated into `"surfaces"`; leaving a stale `"nand_system": true`
beside `"surfaces": {"nand_system": false}` would read as though the surface were
on, so removing them is what makes the migration one-way and finished.

### When a protection was only ever inconvenience

NAND (System) was guarded by being awkward to reach: a user had to hand-edit
JSON. That is not a safety property, it is friction, and a menu toggle removes it
completely. So enabling it now raises a Danger modal that names the real risk.
Disabling never asks — removing access is not the dangerous direction, and
confirming it would only train the user to dismiss the dialog unread.

The general point for any future UI over an existing setting: **ask what was
actually protecting the dangerous option before you make it convenient.** If the
answer is "it was hard to find", the protection has to be rebuilt explicitly in
the same change that makes it easy, or the feature ships as a net safety
regression.

### Save-on-exit is really flush-on-boundary

`push()` calls `on_exit()` on the parent screen, so a settings screen's
`on_exit()` fires when navigating deeper, not only when leaving. That makes the
persistence rule "flush pending changes at any navigation boundary" — which is
the safer reading: a change cannot be lost to an unexpected exit, and a
non-destructive save costs only a rewrite. A failed save keeps the dirty flag so
the next boundary retries it; clearing it would turn one failed write into a
setting the user believes they made.


## Two "measure text" functions, on purpose

`Renderer::measure_text()` and `Font::measure_width()` both return the width of a
string and are NOT duplicates. Unifying them is the obvious cleanup and it is
wrong.

`Renderer::measure_text()` is part of the cached-text path: it rasterises and
uploads the string, populating the texture cache so the subsequent `draw_text()`
is free. It is for "I am about to draw exactly this".

`Font::measure_width()` is pure TTF metrics with no texture. It is for
measurement that is NOT going to be drawn — above all, word wrapping, which
measures dozens of trial prefixes ("NAND (Sys", "NAND (Syst", ...) per call.
Routing wrapping through the cached path would fill the cache with strings that
never appear on screen and evict the ones that do.

`ui/text_wrap.hpp` holds the wrapping algorithm itself, header-only and free of
SDL, with the measurement function injected. That is what lets it be host-tested
with an exact synthetic metric ("every glyph is 10px") and assert precise break
columns, rather than being eyeballed on a console.

### Wrap once, not per frame

`Modal::show()` wraps and stores the lines; `update_and_draw()` only consumes
them. The body cannot change while a modal is up, and the main thread is the
thread a transport worker is blocked on while waiting for a confirmation — so
per-frame wrapping would be waste in the one place waste is least affordable.

The body is also clamped to what fits the screen. An over-tall modal hides its
own buttons, which for a broker confirmation means no way to answer and a
transport blocked until timeout.


## Save Manager: a fourth consumer, not a fourth implementation

`SaveManagerScreen` is the on-device path to save data, alongside FTP, MTP and
(later) HTTP. It implements none of the save handling itself.

The two synthesized levels come from `Services::save_user_names()` and
`save_title_labels()`. Entering a title mounts through
`Services::save_resolve()` — the same choke point the transports use — so the
single-slot rule holds on the on-device path too, and navigating back to the
user list releases the mount for free, because `save_user_names()` releases as
part of listing. Browsing is `FileBrowserScreen` pointed at `save:/`, which is
what that screen was built for.

This is the payoff for having extracted `save_surface` when MTP became the second
consumer rather than copying FTP's logic: the third and fourth consumers cost
almost nothing, and none of them can drift on the mount policy.

### Filename sanitisation is a correctness problem, not a cosmetic one

Backup directories are named after **account nicknames and game titles**. Neither
is constrained to anything. Real Switch titles contain `:` ("Pokemon: Let's Go"),
`?` ("Who Wants to Be a Millionaire?"), and trailing dots. FAT32 and exFAT reject
those characters outright, so an unsanitised path does not produce an ugly folder
name — it produces a **failed copy**, silently, on exactly the titles a user is
most likely to want backed up.

Three subtleties beyond replacing illegal characters, all host-tested:

* FAT silently **drops** trailing dots and spaces, so a caller that built a path
  ending in one would then look for a directory that does not exist under that
  name. Strip them before building the path, not after the copy fails.
* Truncating a long name must land on a **UTF-8 code-point boundary**. A broken
  sequence in a filename is worse than in a label: some tools reject the name
  entirely rather than drawing a replacement glyph.
* Two different awkward titles must not collapse onto the same sanitised name, or
  one game's backup lands inside another's folder. The application id in brackets
  is all legal characters, so it always survives intact and does the
  disambiguating.

### Timestamps that sort

`Core::DateTime::log_stamp()` honours the user's configured date order, which is
right for a log file a human opens by name. It is wrong for backups: for a DMY
user, `12-07-2026` sorts before `05-08-2026`, so "newest" is not the bottom of
the list. `sortable_stamp()` is fixed year-month-day for names that a machine
orders and a human scans.

The general rule: **ask whether a formatted string is read by a person or sorted
by a machine before reusing a formatter.**


## Restore is designed around the rollback, not the action

Save restore is the only operation in GarageNX that destroys something a user
cannot get back from anywhere else. The flow reflects that:

1. A pre-restore snapshot is taken **before the user is asked**.
2. The Danger modal **names that snapshot's path**, so the way back is visible
   while deciding rather than offered afterwards as a consolation.
3. Only then is the save replaced.

If the snapshot fails, the restore is never offered. A restore with no way back is
not a decision worth putting in front of someone.

The cost is that a cancelled restore has spent one wasted copy to the SD card.
That is the right trade: a few seconds and a few megabytes, against a
confirmation that would otherwise have to promise a backup and hope. A cancelled
restore also **keeps** its snapshot — it is a real backup, and deleting it to tidy
up would discard something the user may want.

### Why this one does NOT use ConfirmationBroker

The broker exists so a **transport-initiated** write has to be approved on the
console: the PC-to-console round trip *is* the safety property. A restore started
from the Save Manager already has the user at the console, so the Danger modal
(hold-to-confirm) is that gate. Routing it through the broker as well would be two
dialogs for one decision, and two dialogs for one decision is how people learn to
dismiss both.

### Ordering and refusals

The **source is validated before the save is touched**. Deleting the save and
then discovering the backup is unreadable is the worst possible ordering, and it
is the one you get for free if you do not check first.

Two things are refused outright:

* A directory that does not match the stamp shape is never listed as a restore
  source. An SD card is the user's own — stray folders, half-extracted archives,
  a Mac's `.DS_Store` all sit beside real backups, and restoring one would replace
  a save with garbage.
* An **empty** backup is refused. Restoring one would delete the save and copy
  nothing back. That almost certainly means an interrupted backup rather than a
  user who wants to erase their progress — and if they do want that, deleting in
  the file browser is the honest way to ask for it, and does not look like a
  restore that quietly did nothing.

The commit happens **once, over the whole replace**. Per-file commits would make
each intermediate state durable, which is not what "replace this save" means. A
failed commit fails the restore, because the journal will discard the writes at
unmount and reporting success would describe a save that is about to revert to a
half-deleted state.


## A mount root is not an object

"/Save Data/<User>/<Title>" resolves to exactly `save:/`. That is a legal path to
**list** — it is why browsing a save works at all — and an illegal target for
**delete or rename**, because it is the filesystem rather than a file in it.

This is not a theoretical distinction. An ordinary FTP or MTP client deletes a
folder by clearing its contents and then removing the folder, so deleting a save
produced two confirmation dialogs, both approved, followed by an rmdir of a mount
point that could never succeed.

The failure mode worth naming: **a confirmation dialog for an operation that
cannot succeed is worse than no dialog at all.** It teaches people that approving
these prompts does nothing, and this project's entire safety model rests on those
prompts being meaningful. `Services::save_is_mount_root()` refuses such mutations
with no prompt, on every transport.

## When a surface gains a consumer, re-check its invariants

Save writes must be committed or the journal discards them at unmount. That was
fixed for FTP and MTP when those were the only writers. Then the Save Manager
pointed `FileBrowserScreen` at `save:/` — and the file browser's delete, rename
and paste had gone straight to `Fs::` since the day they were written, which was
correct for every surface they had ever seen.

The result: an on-device delete appeared to work, and the file came back as soon
as the user left the save and the mount was released.

The general lesson: **giving an existing component access to a new surface is a
change to that component**, even when not a line of it is edited. Every invariant
the surface imposes has to be re-checked against it, not just the ones the new
wiring touches.

The durable fix is structural rather than per-caller: nothing should mutate a save
except through a helper that commits. Four call sites all remembering
independently is a bug waiting for the fifth.


## Four ways to fail identically

`classify_write()` returns `Deny` for four different reasons, and two more
refusals now happen before the guard is consulted at all. From outside the
console every one of them looks the same: the operation fails and no dialog
appears.

1. empty or unresolved path
2. no surface claims the path (including a synthetic `savedata:/...` path that
   reached the guard unresolved)
3. the surface is **disabled for that transport**
4. read-only with no confirmation path
5. the save mount root, refused before guarding
6. a synthetic save path that failed to resolve

Reason 3 became newly confusing when surfaces went per-transport: the same save
write can be confirmable over MTP and silently denied over FTP, with nothing on
screen to explain the difference.

Six causes with one observable symptom is a diagnosis problem, not a logic
problem, and no amount of reasoning from the symptom will separate them. Every
one of them now writes a line to `logs/write_guard.log` naming the reason, the
path, the matched surface and whether it is enabled — plus a `FAILED` line
carrying `errno` when the filesystem call itself is what refused, which is the
one outcome the guard could never account for.

This is §5.1 applied to a subsystem rather than a bug: **when several distinct
causes share one symptom, the fix is to make them distinguishable before
attempting a diagnosis, not after the second wrong guess.**


## "Delete the save folder" means "wipe the save"

The bug that presented as "deletes fail with no modals" was not a delete bug at
all. The log added the round before named it in one line: the target was always
"save:/", the synthesized save mount root. The user was deleting the title folder
to clear a save, and an earlier over-eager refusal was rejecting that intent
silently.

The title folder is the mount point, not a real directory, so it cannot be
removed with rmdir — but emptying the save is a real, common operation.
`Services::save_wipe()` deletes every child of the mounted save and commits, and
FTP `DELE`/`RMD` and MTP `DeleteObject` on the save root route to it THROUGH the
write guard, so a wipe over a transport gets the same on-device confirmation as
any other save write. The on-device Save Manager offers the same operation as
"Erase Save", snapshot-first like restore.

The wider lesson is about diagnosis, not saves. Three rounds were spent here: two
guesses that each changed the symptom without fixing it, then a log that settled
it immediately — and the answer was a category the guesses never considered,
because the operation the user wanted was not the operation the UI names. When a
refusal is firing "correctly" and the user still cannot do what they set out to
do, the question is not "why is the check wrong" but "what did they actually
mean", and only an observation of the real request answers that.


## Clear vs delete: two operations the UI must not blur

A save has two destructive operations that look similar and are not:

* **Wipe / Erase** empties the save's contents through the mounted `save:`
  filesystem and commits. The save-data RECORD survives, so the title stays in the
  list with an empty save — the equivalent of Nintendo's in-console "reset".
* **Delete** removes the record itself, by `save_data_id`, through the fs service
  (not through a mount). The title leaves the list and the game creates a fresh
  save next launch.

The delete bug three rounds back came from having only wipe and presenting it as
"delete": the folder returned empty and the user expected the title to vanish.
Both operations now exist and are named for what they do.

Deletion is **by `save_data_id`**, never by application id — the fs API has no
by-app deletion, and the id from `FsSaveDataInfo` is the only stable handle. It
runs with nothing mounted, because deleting a filesystem out from under a live
mount is undefined.

### Why delete is on-device only

Wipe is exposed over transports because a client deleting a folder has an obvious
meaning: clear its contents. Deleting a save *record* has no folder gesture — it
is a Switch concept a file-manager client cannot form intentionally — so a
transport delete could only ever be a wrong-target accident, and with no on-device
snapshot behind it. Delete is therefore console-only, where the snapshot-first
confirmation lives. Same principle as the ConfirmationBroker: put the dangerous
operation where the human already is.

### The one unverified call

`fsDeleteSaveDataFileSystemBySaveDataSpaceId` could not be checked against a libnx
header in the build sandbox, so per §5.4 it is isolated in a single primitive,
logs its Result, and the call site records the fallback spellings. That is the
honest shape for a call taken on knowledge rather than a verified header: contain
it, log it, and write down what to try if it is wrong.


## Backups must outlive the saves they protect

A backup exists precisely to survive the loss of its save. So a save manager whose
only path to a backup runs THROUGH the live save is self-defeating: delete the
save and the backup becomes unreachable at the exact moment it matters. This was a
real hole — snapshot-first delete took a backup the user then could not get to.

The fix is that the backup TREE is browsable on its own terms. The directory
layout `<root>/<user>/<title>/<stamp>` is a complete index by itself, so
`backup_users()` / `backup_titles()` enumerate it straight from the filesystem
with no reference to live saves, reached from a pinned "Manage Backups" entry that
is present even when no live saves exist.

Restoring an orphaned backup then has to RECREATE the deleted save, not just mount
it. `save_resolve()` only matches live saves by design; `save_resolve_for_restore()`
recreates the record first, keyed on the application id recovered from the backup's
own folder label — which, once the live save is gone, is the only surviving record
of which title the backup belongs to. That is why the id is embedded in the label
at backup time and parsed back out here (`app_id_from_label`, pure and tested):
the backup has to be self-describing enough to restore with no help from the system
it is restoring into.

Two consequences fall out of this and are easy to get wrong:

* Restoring from the backup tree takes NO pre-restore snapshot. The current save
  may not exist, so there is nothing to protect and a snapshot attempt would fail
  and abort the restore. The snapshot is for protecting a save that is about to be
  overwritten; there isn't one.
* The recreation call (`fsCreateSaveDataFileSystem`) is another §5.4 unverifiable,
  handled the same way as delete: probe-then-create, isolated, logged, with the
  fallbacks written at the call site.


## Automatic backup: the feature the platform won't let you build, and the one it will

"Pre-play snapshot" assumes the app can act at game-launch time. It can't:
GarageNX is homebrew, not resident while a game runs — once a title launches,
GarageNX is gone from memory. There is no launch hook to hang a snapshot on, so
the literal feature is unbuildable on this platform.

The intent behind it survives translation, though: "my saves get backed up on a
schedule without me remembering." The buildable form is a staleness sweep at
app-open — back up any live save whose newest backup is older than a configured
number of days. Same guarantee, a trigger that actually exists.

### Staleness is pure, and tested like it matters

The decision (`save_is_stale`) is the kind of logic that fails silently in both
directions: too eager thrashes the SD card on every launch, too lax quietly backs
up nothing and the safety net is a no-op. So it is pure and host-tested hard —
including leap years, year boundaries, and the exact threshold boundary.

It compares by **calendar day number** (Hinnant's civil-from-days algorithm), not
by subtracting two `time_t` values. Backup stamps are local wall-clock; "now" is
local wall-clock; the question is "how many days ago", so day-number arithmetic is
both correct and free of every timezone and DST hazard that `time_t` subtraction
would invite. Hour-of-day never enters the comparison.

### One label definition, or auto-backups vanish

Auto-backup must write to the exact directory the manual restore path later reads,
which means both must build the `"<Name> [APPID]"` label identically. This was
already duplicated between two functions; adding a third copy in the sweep would
have been a latent silent failure — a label that differed by one character would
make every auto-backup invisible to restore, with no error anywhere.

`Services::save_build_label()` is now the single definition, and
`save_title_labels()` routes through it. This is the same anti-drift move as
`StorageCatalog` and `NspStream`, applied to a string: the moment a second
consumer appears, the derivation becomes shared rather than copied.

### The trigger runs after the first frame, never during startup

The sweep is synchronous and can touch every save on the console (one mount slot,
so strictly sequential), and `save_enumerate_all()` blocks priming the name cache.
Running any of that before the menu is on screen would be an unbounded freeze on a
black screen. So it fires as a one-shot on the *second* loop iteration — the first
has already drawn the UI — guarded by the feature being enabled. When it is off
(the default) the whole path is a single comparison that returns immediately.

Because the sweep blocks the main loop, it draws its OWN frames: it reports phase
and per-save progress through a callback, and the trigger draws a "checking /
backing up N of M" overlay on each. This matters more than it looks — the slow
part is the *enumeration* (priming the ncm name cache), not the copying, so a
naive "back up N saves" spinner would show nothing during the very phase that
takes the time. The overlay names the enumerate phase explicitly. A blocking
sweep with no self-drawn frames is indistinguishable from a hang; the reported
"~10s freeze for 7 saves" was exactly that, and exactly the enumeration cost.


## HTTP install: the third adapter on one driver

Install over HTTP is a thin adapter on `Install::drive()`, exactly like FTP and
MTP. The engine, the cancel semantics, and the teardown order that took real
device crashes to get right in 4c are shared, not re-implemented — the whole point
of extracting the driver was that the third and fourth transports would cost an
adapter, not a re-derivation.

HTTP's adapter differs from FTP's in two small, favourable ways. Content-Length is
an exact wire size, so the ETA runs off a real total from the first byte rather
than being recovered from the container's own table. And the body bytes that
arrived alongside the headers are handed to the driver as its FirstChunk, so the
header read never costs a lost byte.

The route is chosen by a pure classifier (`http_paths.hpp`), host-tested before any
socket code depended on it, the same discipline as `ftp_paths`. It uses a
slash-delimited `/install/sd` / `/install/nand` prefix rather than FTP's
space-containing folder names: a URL with a space must be percent-encoded, and a
client that forgets would silently mis-route, so the transport gets the spelling
that is unambiguous in a URL while the target meanings stay identical across
transports.

### Applying a scar before it reopens

`~HttpServer() { stop(); }` was added the moment `HttpServer` gained an `m_install`
member — not after a crash. Cancelling a transfer crashed both MTP and FTP with
the same cross-thread use-after-free (members destroyed before the base class
joined the worker still inside the install) until each got that one-liner. The
rule from the delete-commit bug generalises here: giving a component a new member
that a worker thread touches is a change to that component's destruction
requirements, whether or not the crash has happened yet. This time the invariant
was re-checked at the moment the consumer appeared, and the scar was applied
before it could reopen.


## The web UI, and the constraint that shaped it

The front-end is one embedded string (`web_ui.hpp`), not files on the SD card.
That is a correctness choice, not a packaging one: the page must work while the SD
card is being browsed, rewritten, or is the very target of the install the page
just started. Reading UI assets off the card the app is mutating gives you a UI
that breaks precisely when it is needed. It also removes the "asset missing"
failure mode entirely — if GarageNX runs, its web UI runs — and keeps the page
working on a LAN with no internet, since there are no CDN fonts or frameworks to
fetch.

### Progress had to come from the browser, and that turned out to be honest

`HttpServer` runs a strictly serial accept loop: one connection, handled inline,
no threading. During an install the loop is inside `http_install()` for minutes, so
a browser polling a server-side progress endpoint would have its connection sit
unanswered in the listen backlog until the install finished. A progress bar that
updates once, after the thing it measures is over.

So live progress comes from the browser's own `xhr.upload.onprogress`. For a
*streaming* install this is not a workaround but an accurate measure: the console
consumes bytes as it receives them, so TCP backpressure makes "bytes the console
accepted" track "bytes the console installed" closely. `/api/status` is polled only
while idle, for state and the last result.

Making the server concurrent would allow true console-side stats mid-install — and
would put a second thread next to the install object whose destruction order took
real device crashes to get right. That is not a trade worth making for a progress
bar. The constraint was found by reading the accept loop before designing the
progress channel, which is the cheap place to find it.

### A new consumer exposes the old bugs, again

The request path was never percent-decoded before filesystem use. Every file whose
name contains a space — which is most real title names — was undownloadable over
HTTP. It stayed invisible because HTTP was curl-only, where you type the name you
meant; the moment a browser became a client, it would have failed on the first
click.

This is the third time this session the pattern has appeared: the delete-commit bug
when the file browser gained save paths, the destruction-order UAF when HTTP gained
an install member, and now this. **Giving an existing component a new kind of
consumer is a change to that component**, and its latent assumptions have to be
re-checked against the new caller even though not a line of it was edited.


## A stale roadmap entry is a hazard, not just untidiness

Slice C ("MTP file-manager parity") was listed as unstarted long after it was
finished — the entry predated the save-data rounds that built out MTP's
enumeration, object properties, partial reads and plain-file push. Taken at face
value it would have prompted a second implementation of a working, hardware-
verified subsystem.

The check is cheap and the failure is expensive, so the rule is: **before building
from a roadmap item, verify the item against the code.** The entry now records what
was audited and when, rather than what someone once intended to do.

## Three transports, one stat set

`format_eta()` lived twice as byte-identical file-local copies. Two identical
copies are not yet a bug; they are a bug waiting for the first person to fix one of
them. Adding HTTP as a third consumer was the moment to extract it
(`ui/stats_format.hpp`), which is the same rule already applied to the save label
and the mount probe: the second consumer is the warning, the third is the
obligation.

Extracting it also fixed something the copies hid. The guard read
`seconds < 0 || seconds > 359999`, and **both comparisons are false for NaN**, so a
NaN estimate fell through to `(int)(NaN + 0.5)` — `INT_MIN` — and would have
rendered as a nonsense duration. It was unreachable because every call site happens
to guard with `cur > 1.0`, which is exactly the kind of safety that holds until
someone adds a fourth call site. The shared version is NaN-safe at the source.

### What the meter measures matters more after a UI exists

The HTTP screen sampled `bytes_sent() + bytes_recv()` — every byte the server
moved. That was merely imprecise while HTTP was curl-driven. Once the web UI
existed, those totals included the HTML page on every load, a JSON listing on every
navigation, and a status poll every five seconds, so the speed readout would have
presented page chatter as install throughput. It now samples the install's wire
bytes, like MTP and FTP.

The general shape, seen yet again: **adding a consumer does not just exercise
existing code, it changes what the existing code means.**


## Manual and automatic backup: one sweep, two policies

The main-menu "Back Up Saves" action and the automatic staleness sweep are the
same operation with different selection rules, so they share one implementation
(`sweep_impl`) and one progress overlay (`ui/backup_overlay.hpp`). They differ in
exactly one line: whether a save is filtered by `save_is_stale()`.

The manual action deliberately ignores staleness. A button wired to the stale
policy would do nothing when auto-backup is off — which is the default — or when
nothing happened to be stale, and **a control that silently no-ops is worse than
no control**, because the user cannot tell "it worked" from "it ignored me".
Pressing a button means "do it now".

Zero backed up is reported as "nothing backed up", never as success: zero covers
both "this console has no save data" and "every copy failed", and claiming success
for the second is the kind of lie that costs someone their progress later.

## Never use a translated string as a printf format

The completion message was first written as
`snprintf(buf, n, Lang::t("...").c_str(), count)`. That is the classic
format-string hazard wearing ordinary clothes: a lang file is **data**, and a
translation that carries `%s` where the caller passes an `int` is undefined
behaviour on the console, reachable through nothing worse than a bad translation
or a corrupted asset.

The safe shape — used everywhere else in this codebase, which is why the mistake
stood out on audit — is a **literal** format with the translated text as an
argument: `snprintf(f, n, "%s: %d", Lang::t("...").c_str(), value)`. Counts are
concatenated rather than formatted through translated text.

The general rule: a format string is code. Anything loaded from disk is not.


## When a label becomes a filename, a cache miss becomes permanent

`save_build_label()` produces `"<Name> [APPID]"`, or `"Title <APPID>"` when the
name cache cannot resolve the title. In the Save Manager that fallback is a
transient display state — the row re-labels a second later as names arrive.

In the backup path it is **not** a display string. It becomes the backup
directory's name on the SD card. A name that was merely unresolved at that instant
is written to the filesystem and stays there forever.

That is what made the §5.5 mirror bug worse the second time it appeared.
`save_enumerate_all()` blocked on `installed_titles_list()` from the main thread —
the thread that resolves names — so it timed out with nothing resolved, and every
backup either sweep produced was named `Title 0100000000010000/`. The comment on
that line asserted "not a UI path", which was simply wrong, and a wrong comment is
worse than none: it argues against the reader noticing.

The rule this leaves behind: **before persisting a derived string, ask whether the
derivation can fail transiently.** A display can be wrong for a frame. A filename
is wrong until someone deletes it.

The fix has two halves, and both were necessary:

* New backups: `save_enumerate_all()` takes a `pump`. Supplying it declares the
  caller to be the main thread, so the function drives the resolver itself and
  calls back between units for a redraw, rather than blocking on a loop it has
  parked. It waits on `installed_titles_names_resolved()` — enumeration finished
  *and* every name through the resolver — because "enumerated" alone is true while
  names are still arriving one per tick.
* Existing backups: their folders keep their names. Renaming would invalidate
  every path already recorded, and is not the app's to do. The Manage Backups list
  instead recovers the application id from the folder name and re-resolves it for
  **display only**, leaving the path untouched. This is why `app_id_from_label()`
  parses both the bracket form and the id-only fallback: the fallback is not just
  a legacy artifact, it is the thing that makes the old data recoverable.


## Committing a save is part of the mutation, not a step after it

A Switch save filesystem is journalled: writes are discarded at unmount unless
`fsdevCommitDevice()` runs. Every save mutation therefore needs a commit — and for
most of this project that requirement lived in fourteen call sites across four
files, each remembering independently.

That is a convention, not an invariant, and it failed twice. `FileBrowserScreen`
never committed — correct for its whole life, since it only saw SD and NAND, until
the Save Manager pointed it at `save:/`. And writing the fix found two more still
live: `do_new_dir()` and `do_new_file()` created a folder or file in the active
pane with no commit, so making a folder inside a save quietly vanished.

`Services::SaveWrite::*` folds the commit into the operation. It is a no-op
success on every other surface, so a caller never has to know which surface it is
on — which is exactly the knowledge callers turn out not to have.

### A lint, because the invariant is between two statements

`tests/save_commit_discipline_test.cpp` reads the source and fails the build if a
mutation in a save-capable file is not accompanied by a commit or marked
`// NO-COMMIT: <reason>`. It is the only lint in the suite, and it earns that
exception twice over: the invariant lives in the *relationship* between a call and
its follow-up, where no unit test can reach it, and the failure mode is silent loss
of a user's save data with no error anywhere.

It was verified to fail — reintroducing the `do_new_dir` bug makes it name the file
and line. A check that cannot fail is decoration.

### The exemption that is easy to "fix" wrongly

Reviewing every flagged site produced one genuinely subtle case worth recording.
MTP's partial-write cleanup removes a half-written file and does **not** commit. On
a save that is correct and deliberate: the failed write and its removal are both
uncommitted, so the journal discards them together and the save returns to its last
committed state. **Not committing is the rollback.** Adding a commit there would
make a failed transfer durable — a plausible-looking "fix" that would introduce the
bug. The reasoning now sits at the line rather than in someone's memory, which is
the other thing the lint bought: it forced every mutation site to be adjudicated
and its answer written down.


## The host build does not compile the screens

`settings_screen.cpp` — like every screen and transport — is excluded from the
host test build, which compiles only the pure modules. A clean `ctest` run says
nothing about whether a screen's member pointers name real fields.

That mattered when Settings round 2 wired roughly thirty config fields through
`bool Config::Behavior::*` and `bool Config::Visibility::*` member pointers. A
typo would be a compile error **on the Switch build only** — days later, on
someone else's machine. The syntax guard parsed the file happily, exactly as
documented: it proves structure, not names.

So every field was verified by hand against `config.hpp` before shipping. The
general rule this reinforces: **"the tests pass" is a claim about the modules the
tests compile.** For anything outside that set, the manual audit is not belt and
braces, it is the only check there is.

The corollary took a build break to learn. A duplicate `case` label in
`menu_dispatch.cpp` reached the devkitPro build even though the syntax guard had
compiled the file and seen g++ report it — the guard only surfaces errors matching
its `STRUCTURAL` pattern, and that string was not in the list, so it printed a
green tick over a real error. The pattern now includes `duplicate case value`,
`redefinition of` and `multiple definition of`: none can be produced by a missing
libnx header, so they are safe to trust without reintroducing the noise the filter
exists to suppress.

For files outside the host build the guard is the *only* mechanical check they
get. Anything it can be taught to catch, it should be — and the teaching has to be
verified by reintroducing the bug, because a filter that matches nothing looks
identical to a file that is clean.

The same reasoning added a second check. Wrong field names surface as g++'s "has
no member named", which the filter discards — correctly, since a libnx struct
reduced to an empty stub genuinely has no members and the message is meaningless
there. But when the type is in **our** namespace the stubs are irrelevant and the
error is real, so the guard now surfaces that subset specifically. It was verified
by planting a bad field on `Services::SaveRef`.

The blind spot that remains, stated rather than papered over: wrong field names on
*libnx* types still pass, because the stubs make that error indistinguishable from
noise. Those are what the manual audit against real headers is for.

### A deferral can go stale

Round 2 was deferred because it needed the software keyboard, and §5.4 says a
guessed applet call has already crashed this console once. By the time it came up,
`ui/keyboard.hpp` already wrapped swkbd with both `get_text()` and `get_number()`,
and the file browser's new-file prompt had been hardware-verified. The risk was
retired; the note recording it was not.

Checking a deferral against the code before honouring it costs one grep. Honouring
a stale one costs either a redundant implementation or an unnecessary refusal —
the same failure that nearly rebuilt Slice C.


## A config field is not evidence of a feature

Settings round 2 exposed every behaviour toggle that had a config field and a lang
key. Seven of them — `action_logging`, `highlight_update_files`,
`verify_hash_on_install`, `show_cache_warming`, `rotate_screen`,
`use_overclocking`, `screen_dim_seconds` — were at that time read by no code at
all. They were aspirational fields from an earlier design, and their presence
looked like evidence that something honoured them.

It did not. **Before exposing a setting, grep for a consumer.** A toggle that
changes nothing is worse than an absent one: it presents as a feature, and the user

_Resolution (later session):_ the seven were dealt with, not left. Four with no
plausible cheap consumer — `highlight_update_files`, `show_cache_warming`,
`rotate_screen`, `use_overclocking` — were **removed** from Config entirely.
The other three were **wired** and are now shown in Settings: `action_logging`
gates the install-log write, `verify_hash_on_install` streams a per-NCA SHA-256
check, and `screen_dim_seconds` became a menu timer plus a `screen_dim_seconds_net`
timer driving an app-level idle dim. The principle stands regardless.
cannot distinguish it from a broken one — the same reasoning that made the manual
backup button back up everything rather than run a policy that could silently
no-op.

The fields stay in `Config` (harmless, round-tripped, ready if implemented); they
are simply not offered.

## Fall-through groups are a trap for new cases

`case MenuItem::Settings:` was added to the head of an existing group of
unimplemented menu cases that shared a single `return nullptr`. The new case
brought its own `return SettingsScreen::root()` with it — and the four cases
already in the group fell into it. Browsing the NAND partitions opened the Settings
screen, and had done since that round.

The switch now carries a warning at the point of edit. The general shape is worth
naming: **adding a label to a shared branch changes the behaviour of every label
already on it**, and a `case` list is the one place in C++ where adding a line
above your own code silently rewrites someone else's.

## Menu entries are a promise

An entry that dispatches to `nullptr` does nothing when pressed, which is
indistinguishable from broken. Five of them were shipping — including
`InstallCartridge`, a top-level item nobody had reported, found by auditing the
whole dispatch rather than only the items named in the report.

Entries are now listed only when dispatch returns a real screen. The enum values
and their `nullptr` cases remain, so re-adding one is a single line the day its
screen exists.


## Changing a default is not a migration

`Defaults::UPDATE_CHECK_URL` was corrected from a placeholder to the real
repository, and nothing changed on device. `from_json` reads a stored key whenever
it is present, so a default only applies to a config that has never seen that key —
which, after the first run, is no config at all.

The fix is an exact-match migration: if the stored value is precisely the old
placeholder, replace it; anything else, including a URL the user chose, is left
untouched. The placeholder constant stays in `defaults.hpp` purely so `from_json`
can recognise it.

The rule: **when correcting a default, ask what existing configs already hold.**
Usually they hold the old value, and only a migration reaches them. The
per-transport surfaces split needed the same treatment for the same reason.

## Two activity queries, one reliable

`Core::Activity::summary()` reports N/A for everything on hardware, deliberately.
The pdm event stream includes system-applet churn and its first entries predate the
RTC being set, so aggregate session counts and "first played" dates do not match
what other tools show; accurate totals need the play-log save archive at
`SYSTEM:/save/80000000000000F0`, a system save in an undocumented format.

Per-title statistics are a different query — addressed directly by application id
rather than derived from the event log — and are the figures other homebrew
activity tools display. Unreliable aggregates and correct per-title numbers coexist
in the same service, which is why the Activity Log screen is built on the second
and the System Information screen still honestly says N/A for the first.

The screen enumerates *installed* titles, so a game played and then deleted has
statistics pdm still holds that the list will not show. That limitation is stated
in the header rather than papered over, because the alternative — enumerating pdm's
own title set — needs the same save archive that put the aggregates out of reach.


## Tickets: claim only what the code can check

The Installed Tickets screen derives a title id from bytes 0-7 of each rights ID
(big-endian) and resolves it to a name. It does **not** derive key generation from
the rights ID, even though the layout is well known — this codebase reads key
generation from the NCA header, so taking it from the rights ID would be asserting
something nothing else here corroborates.

The display is self-validating. A correct title id resolves to an installed
title's name; one that does not resolve renders as the **raw rights ID**. So a
wrong derivation would show up as "unresolved" rather than as a confidently wrong
title name — and "not installed" is a legitimate, common state in any case, because
a ticket outlives the title it belongs to.

This is the general shape for reading a format the project does not own: derive the
part you can check, display the raw bytes for the part you cannot, and let a
failure present as absence rather than as fiction.

The screen is read-only. Deleting a ticket can make an installed title
unlaunchable, and destructive operations here arrive with their own confirmation
and rollback story — the Save Manager pattern — rather than riding in as a button
on a list.


## HTTP was the transport that never consumed the catalog

`StorageCatalog` and `sp_resolve` exist so that FTP, MTP and HTTP cannot disagree
about which surfaces exist or where they live. HTTP never used them: its
`resolve_vfs()` was `m_prefix + posix`, hardcoding `sdmc:`. The result was a
transport that could install to SD and NAND but could only *browse* the SD card,
while the other two served Installed Titles, Save Data, NAND and Album.

That is the exact drift the shared definition was written to prevent, and sharing a
definition does not help if a consumer quietly declines to consume it. Worth
remembering when adding a fourth transport: the catalog is not automatic.

Listing now dispatches on `PathKind` the way FTP's `LIST` does — root chooser,
Save Data's two synthesized levels, the synthesized Installed Titles list, real
directories — and downloading an installed title streams the same
`Core::NspStream` that FTP `RETR` and MTP `GetObject` use. One builder, three
transports, which is what extracting it bought.

## A false positive is still a cost

The new JSON listing tripped the syntax guard's brace counter, which does not strip
string literals and saw unbalanced `{`/`}` inside JSON fragments. Its own comment
predicts this ("a smell test, not a proof"), so the easy response was to shrug.

The response taken instead was to give the JSON envelope one definition each for
open and close, which balances the literal braces in the file. **A guard that cries
wolf is a guard people stop reading**, and the whole value of these mechanical
checks is that a FAIL means something. Two lines of refactor is cheap next to
training yourself to ignore a red line.

The same instinct applies to tests. `test_save_preserves_key_order` passed on its
first run — vacuously, because the test's own `json` alias alphabetised the input
before it ever reached the file. A test that cannot fail for the reason it claims
is worse than no test, because it reports confidence that is not there.


## A shared catalog helps generic consumers, not hand-written ones

When the Game Card surface finally gained a mount, it appeared on FTP and HTTP
immediately and was missing from MTP and the console menu. Nothing was broken in
either — the difference is how each consumes the catalog.

FTP and HTTP iterate `StorageCatalog::enabled_surfaces()` generically, so a new
surface costs them nothing. MTP maps catalog ids onto **wire** storage ids by hand
(the protocol requires stable numeric ids), and the console Browse menu lists its
entries by hand. Neither had an entry for a surface that had never been mountable.

The rule for adding a surface: **the catalog is necessary but not sufficient.**
Grep for everywhere that enumerates surfaces by hand.

That rule was written and then immediately broken. Adding Gamecard to MTP needed
four edits — the wire-id constant, both mapping functions, and a hardcoded
eight-line `if` chain in `GetStorageIDs`. Three were made and the fourth was
missed, so the card appeared on FTP and HTTP and not on MTP.

`GetStorageIDs` now **derives** its list from `StorageCatalog::all()` through
`surface_to_mtp_id()`, which deletes the fourth place permanently. The remaining
hand-maintained parts — the id constant and the two mapping functions — cannot be
derived, because MTP requires stable numeric wire ids that outlive any reordering
of the catalog. But they are a compile-visible pair now, not a list that silently
omits.

The general form: when a rule says "remember to update N places", the fix is to
make it fewer places, not to remember harder.

### MTP sends no change events

There is an interrupt endpoint in the descriptor (the PTP class mandates one) and
nothing ever writes to it. So a host that has already enumerated storages will not
learn about one that appears later — a game card inserted mid-session is invisible
until the host re-enumerates, typically on reconnect. Proper `StoreAdded`/
`StoreRemoved` events would fix it and are not implemented.

The Browse Game Card entry is gated on the *storage surface* rather than its own
visibility flag. "Can this console see game cards" is one decision, and a second
toggle could disagree with the first, offering a menu entry into a surface the
transports have been told to hide.


## A fallback that impersonates is worse than one that refuses

`build_storage_info()` described every unrecognised MTP storage as the SD card —
its name *and* its capacity figures. When the game card arrived, hosts showed two
storages both called "SD Card", told apart only by a numeric id.

The bug was not the missing case. It was that the default branch produced a
**plausible wrong answer** instead of an error. Only the real SD id gets the SD
block now; anything else returns empty and the caller answers `InvalidStorageId`,
so the next surface added without a case fails loudly.

Note the same number meaning opposite things in two places. Reporting zero free
space is a **bug** for NAND System — the surface is writable through the
confirmation broker, and a host reads zero free as "full" and refuses the write
client-side. It is **correct** for a game card, which is physically read-only, and
is exactly what stops a host attempting a write it could never complete. What
decides it is whether writes are possible at all, not the value itself.

### Three lists in three rounds

Adding one surface to MTP surfaced three separate hand-maintained places: the wire
id mapping, `GetStorageIDs`, and `build_storage_info`. `GetStorageIDs` was fixed by
deriving it from the catalog. The other two cannot be derived — MTP needs stable
numeric ids, and capacity/type/access genuinely differ per surface — but they can
be made to fail loudly rather than silently omit or silently impersonate. When a
list cannot be eliminated, make its failure mode obvious.


## A ternary has no default branch

`Core::Ncm::Storage` was `{ BuiltIn, SdCard }`, and six files converted it to an
NCM storage id with the same expression:
`storage == SdCard ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser`.

Adding `GameCard` to that enum would have compiled everywhere and been wrong
everywhere: a cartridge would silently resolve to built-in storage, in the code
that chooses which content database to open. No warning, no error — a ternary's
"else" swallows every value you did not think of, which is precisely what makes it
the wrong shape for a mapping that can grow.

`Ncm::to_ncm_storage_id()` is now the single definition, and it is a `switch` with
an explicit default. One edit adds a storage; every caller follows.

This had to land *before* dump and install were built on top of it. Six
silently-wrong mappings would have produced failures that pointed anywhere except
the cause — and diagnosing that from a console, with no host repro, is exactly the
kind of multi-round hunt this project has already paid for more than once.


## Adding an enum value can break code that never changed

`Core::Ncm::Storage` gained a third value, `GameCard`. Two things broke that were
nowhere near the edit.

Six files converted `Storage` to an NCM id with a **ternary**, whose else-branch
silently swallowed the new value and mapped cartridges to built-in storage. That
was fixed by deriving the mapping once, in a `switch` with a default.

`title_ops::move_title()` chose its destination the same way —
`storage == SdCard ? BuiltInUser : SdCard` — which now *accepts* a GameCard source
and picks SD. Plausible, until the delete half of the move tries to remove content
from a physical read-only cartridge. The path did not exist while the enum had two
values; adding a third made it reachable. It now refuses a GameCard source.

Neither file was touched by the feature work. **Widening a type widens the input
domain of every function that consumes it**, and the dangerous ones are those that
handled the old domain exhaustively by accident — a two-value enum makes an
if/else exhaustive, and a third value makes it a silent default.

Grep for every `?:` and every `if/else` over the type, not just the switches. A
`switch` at least warns; a ternary never will.


## Gamecard content is not readable through NCM

`ncmContentStorageReadContentIdFile()` fails on `NcmStorageId_GameCard` with
2002-2964 (FS, unsupported operation). `NspStream` therefore reads cartridge NCAs
as files from the mounted secure partition — `gamecard:/<content_id>.nca`, or
`.cnmt.nca` for meta contents — and keeps the NCM path unchanged for every other
storage.

The diagnosis is worth remembering more than the fix. The failing Result alone
would have left several candidates open. What settled it was an **asymmetry**: on
the same storage handle and the same content id,
`ncmContentStorageGetSizeFromContentId` succeeded while the read failed. A bad
handle or a bogus id fails *both*. One accessor working and another failing can
only mean that accessor is unimplemented for that storage.

That asymmetry also disproved the standing suspicion that the once-a-second
game-card presence poll was disturbing the card handle. It was plausible enough to
have been "fixed" on a guess — which would have cost a round and left the real
cause untouched.

**When one call fails, check whether a neighbouring call on the same handle
succeeds.** It converts "something is wrong" into "this specific operation is
unsupported" in a single observation.

### A fixed bug with a second call site is a half-fixed bug

The rights-id probe read NCA headers through the same unsupported call. Left
alone, every cartridge dump would have silently reported "no rights id" and
produced an NSP that looked complete and would not install — a worse failure than
the original, because it fails later and quieter. `nca_rights_id()` was split into
a storage-aware read and a shared parse.


## Two long-operation shapes, and picking the wrong one freezes the console

GarageNX has two patterns for work that takes minutes, and they are not
interchangeable:

* **Callback-driven, synchronous.** The save-backup sweep blocks the main thread
  and calls back per unit so the caller can paint a frame. Correct when the API
  offers a progress callback.
* **Worker thread, polled.** The NSP dump publishes into an atomic `Progress`
  struct and runs on a libnx thread; the UI polls it each frame. Correct when the
  API offers no callback — and an atomic progress struct is the tell that this is
  the intended shape.

`GamecardScreen` first used the wrong one: it called `dump_title_to_nsp()`
synchronously under a comment asserting it "draws its own frames". It has no
callback with which to do that, so the console froze for the whole dump. **Read
what the API offers before describing what it does** — the comment was confident
and wrong, which is worse than absent.

### Copy the caller, but audit its cleanup

`TitleDetailScreen` was the correct model for the threading and an incomplete one
for teardown: its destructor freed an icon texture and never waited for the dump
thread. Backing out mid-dump is blocked by the UI, but **app exit is not** —
`main.cpp` clears the screen stack, and the worker would write into freed members.
That is the same cross-thread use-after-free that crashed MTP and FTP on cancel.

Both destructors now cancel *and* wait. The cancel shortens the wait; it does not
replace it. And the wider point: copying a proven pattern means copying what it
does right, not inheriting what it forgot.


## Split NSP output, and not guessing at errors

Dumps larger than ~4 GiB are written as a **directory** named `<name>.nsp`
containing numbered parts (`00`, `01`, ...). That is the layout DBI and Tinfoil
produce and consume, and the Switch treats such a directory as a single archive.
Smaller dumps stay ordinary single files.

This is not optional polish. A Switch SD card is FAT32 unless its owner
deliberately reformatted it, and FAT32 cannot hold a file of 4 GiB or more — which
most cartridge dumps exceed. Without splitting, large dumps fail partway through
with a short write.

The bug that exposed it is the more useful part. The code reported
`Write failed (SD full?)` — with a question mark — whenever `fwrite` came up short,
without consulting `errno`. It shipped one plausible cause as if it were the
finding, and sent someone to check free space on a card with 120 GB free.

**A message with a question mark in it is a hypothesis.** Where the real cause is
knowable — `errno` was available at the point of failure — the message should
report it: `ENOSPC` really is a full card, `EFBIG` is the size limit, anything else
gets its number printed. Speculation in an error string costs a user their time
before it costs them anything else, and it is *more* expensive than no message,
because it actively misdirects.

The failure's shape was also evidence in itself: it failed **partway**, not
immediately. A full card fails when it fills; a fixed size limit fails at the same
boundary regardless of capacity.


## Split archives: output and input are one feature

Dumps over 4 GiB are written as a directory of numbered parts. That change is only
half a feature on its own — and the missing half was shipped, so a dump completed
and then could not be installed.

`NspReader` now treats a directory of parts as one contiguous stream (`pread_at`
spans part boundaries; the concatenation *is* the PFS0, with no per-part header),
and the file browser recognises a split archive as a directory whose name ends in
an installable extension **and** that contains a part `00`. Both conditions are
needed: the name alone would offer Install on any folder someone called
`backup.nsp`, and the part alone gives no reason to look for it.

The general lesson, which this project has now paid for twice: **when a change
alters what a system produces, find everything that consumes it.** The rights-id
probe was the same shape — one call site fixed, a second left reading through an
unsupported path — and both failures share the property that makes them expensive:
the operation *reports success* and the damage surfaces later, somewhere else,
looking like a different bug.


## The game card is just another byte source

Installing from a cartridge has no cartridge-specific install path. `NspStream`
presents a gamecard title as a sequential NSP byte stream, `Install::drive()`
consumes any byte source, and `StreamInstaller` does the rest — the same machinery
MTP, FTP and HTTP use. The adapter is about thirty lines.

That is the return on extracting the driver in 4c. Each new install surface has
cost an adapter rather than a re-derivation: FTP, then MTP, then HTTP, now a
physical cartridge. The only thing that made the card special was that `NspStream`
could not read its content — and once that single accessor was routed through the
mounted secure partition, the card stopped being special at all.

Worth noting what this means for "Install from Cartridge" as a menu item: it is
now redundant rather than missing. The Game Card screen lists what is on the card
and installs it, which is one route to one operation instead of two.

### A second worker means revisiting teardown

`GamecardScreen` now owns two worker threads (dump and install). Its destructor
cancels and waits for **both**. Adding a thread to a screen is a change to that
screen's destruction requirements, not just to its update loop — the same lesson
as `~HttpServer()`, and one that has now been applied proactively three times
rather than after a crash.


## The guard is the only check most of this codebase gets

The host suite compiles the pure modules. Screens, transports and most of `core`
are not in it, so a passing `ctest` says nothing about them — for those files the
syntax guard is the entire mechanical safety net.

It was covering 24 files. An audit found 52 more that compile cleanly and simply
were not listed, including several edited in the same session. Coverage is now 78:
every compilable source file in the project.

Two lessons came out of closing the last two gaps.

**A stub must carry what the file needs from the header, and nothing more.** The
first `zstd.h` stub defined `ZSTD_FRAME_MAGIC`; `ncz.cpp` declares its own
`constexpr` of that name, and the macro expanded inside the declaration and
mangled it. Extra declarations in a stub are not free — each one is a chance to
break code that was fine.

**A stub that silently fails to declare is worse than an absent one.** The
`switch.h` stub never defined libnx's `u8`/`u32`/`u64`. Structs added to it
referenced those aliases, never parsed, and every one of *our* structs containing
such a member then reported "has no member named ..." — an error in our namespace
whose real cause was three files away. It hid because undefined-type errors are
filtered as expected noise.

### Verify a widening by reintroducing a real bug

The own-type field check listed `Services|Config|Install|UI|...` and **not
`Core::`**, where most of this project's types live. It had shipped two rounds
earlier and was half-blind the entire time. That was found by planting a bad field
on `Core::Ncm::Title` and observing that the guard passed.

Every widening here gets tested that way, because a filter that matches nothing is
indistinguishable from a codebase that is clean — and the second reading is the
comfortable one.


## A stub you wrote cannot verify that an API exists

`core/usb_mount.cpp` called `ueventWait()`. libnx has no such function — events
are waited on through `waiterForUEvent()` plus `waitMulti`/`waitSingle`. The
syntax guard passed the file anyway, because the same hands that wrote the call
had written `Result ueventWait(...)` into `tools/stubs/switch.h`.

That is the sharpest limitation of this whole approach. A stub confirms that the
code agrees with **your belief about the API**, and nothing more. Elsewhere in this
project §5.4 caveats mark calls taken on knowledge because no header was
obtainable; those at least announce themselves. Inventing the declaration is worse,
because it converts an unverified call into an apparently-verified one.

Two rules follow, and both are now written at the stub:

* **Copy stub declarations from something authoritative** — a real header, or
  working example code from the library itself. If you cannot, say so at the
  declaration rather than letting it look checked.
* **Prefer the form you can see working over the one you believe in.** The fix uses
  `waitMulti` with a single waiter because libusbhsfs's own `example_event` does,
  and it avoids `waitSingle` — which is almost certainly real, but appears nowhere
  in anything readable from here. `waitSingle` is deliberately left undeclared in
  the stub too: declaring the unverified is exactly how this started.


## An unasserted edit is indistinguishable from a successful one

Browse USB detected drives correctly, showed and hid its menu entry correctly, and
did nothing when pressed. The dispatch case had never been added: the edit intended
to replace the "unimplemented" case group used a plain string replacement with no
assertion that the pattern matched, and the group had since gained another case. It
silently did nothing.

The result was the most confusing shape a bug can take — a feature that looks
half-wired because half of it genuinely is. Detection worked, visibility worked,
the file compiled, tests passed.

The discipline that catches this costs one line: **assert the match count before
writing.** Nearly every edit in this project does, so a stale pattern fails loudly
at the moment of editing rather than presenting as a runtime mystery days later.
The one edit that skipped it is the one that cost a hardware round.
