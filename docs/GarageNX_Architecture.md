# GarageNX — Architecture & Design Reference
**Version:** 0.1.0-planning  
**Last Updated:** 2026-07-08 (rev 2)  
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
| License | MIT (FOSS, fork-friendly) |

---

## 2. Repository Structure

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
    "action_logging": true,
    "highlight_update_files": true,
    "rotate_screen": false,
    "use_overclocking": false,
    "saves_ro_mode": false,
    "show_cache_warming": false,
    "screen_dim_seconds": 30,
    "button_repeat_on_hold": true,
    "show_clock": true,
    "show_seconds": false,
    "date_format": "DMY",       // DMY | MDY | YMD  (clock + log names)
    "time_24h": true,           // 24h clock display; logs are always 24h
    "save_auto_backup_days": 0,
    "verify_hash_on_install": true
  },
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
| Inter font | UI typeface | SIL OFL 1.1 |

GarageNX is licensed under AGPLv3; all bundled/linked components above are license-compatible with AGPLv3 distribution. The FTP and HTTP network services are clean-room implementations on libnx BSD sockets and add no server-library dependency; the MTP responder uses libnx USB comms.

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
**Goal:** three network/USB transfer services (FTP, HTTP, MTP), each with a status screen, plus QR codes and access-point mode. Clean-room (no ftpd/libmicrohttpd/ftpsrv dependency) — FTP and HTTP are implemented directly on BSD sockets (libnx provides them; the same code compiles as POSIX for the PC stub build, enabling loopback testing).

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
- **Slice 4a (done):** install storages. Two write-only drop zones are exposed alongside "SD Card" — `Install to SD` (0x00020001) and `Install to NAND` (0x00030001), each gated on `config.mtp.sd_install` / `nand_install`. Writing an NSP to one streams it into NCM; browsing them returns an empty list. Plain NSP only — an `.ncz` is refused up front with a message pointing at the file browser, never half-installed.
- **Slice 4b:** NSZ stream install. Needs a producer/consumer ring buffer and an install thread, because the NCZ decompressor *pulls* through a `ReadFn` while USB *pushes*.

**MTP host detection (hardware-learned).** The USB `idProduct` decides which host stack picks the device up. With an arbitrary id (`057e:3000`) libmtp's device database does not recognise it, so Linux falls through to the generic PTP/camera backend (libgphoto2): the device mounts as `gphoto2://`, shows a camera icon, names storages `store_XXXXXXXX` instead of using `StorageDescription`, and `mtp-detect` reports "No raw devices found". Using **`057e:201d`** — the id libmtp knows as *"Nintendo Switch / Switch Lite"*, and the one DBI uses — puts the device on the libmtp/`mtp://` path. `DeviceFriendlyName` (device property 0xD402) is also answered; without any device property a host falls back to the USB `iProduct` string for the device label.

**MTP structure.** `services/mtp_data.{hpp,cpp}` holds the wire format (container framing, UTF-16 strings incl. surrogate pairs, arrays, dataset builders) and is deliberately free of USB/libnx so it can be unit-tested off-device — the transport half can only ever be exercised on hardware, so everything testable is isolated from it. `services/mtp_server.{hpp,cpp}` owns `usb:ds` and operation dispatch, reusing `NetworkService` purely for its thread lifecycle (the base has no network-specific API). usb:ds requires **page-aligned DMA buffers**; transfers use bounded waits with `usbDsEndpoint_Cancel` + drain on timeout so `stop()` stays responsive when the host goes away. `MTPScreen` shows connection/session state rather than an address — there is nothing to scan over USB.

**Stream install (`install/stream_installer.{hpp,cpp}`, implemented).** `install()` is pull-based random-access — it reads the CNMT first (which may sit anywhere in the PFS0) and then reads each NCA by offset — while a USB stream only goes forwards. Buffering the whole NSP to disk would defeat the purpose, since the FAT32 4 GiB file ceiling is exactly why stream install exists.

The resolution turns on an existing property of `install()`: it **skips any NCA that is already registered, without ever calling `read()`**. So `StreamInstaller` parses the PFS0 header/table as it arrives (every entry's byte range is known before its data does), streams each NCA straight into an NCM placeholder and registers it as its last byte lands, and tees the small entries (`.cnmt.nca`, `.tik`, `.cert`) into RAM as they pass. `finish()` then hands the entry list to the **existing, hardware-validated `install()`** with those small entries served from RAM: every NCA hits the skip path, and meta registration plus ticket import run exactly as they do for a local NSP. **`install()` and `installer.hpp` are untouched** — the only change to the validated code was dropping `static` from `content_id_from_name` so both paths share one hex-decode rather than duplicating a security-relevant parse.

The state machine is unit-tested off-device against synthetic PFS0 containers fed at chunk sizes of 1, 7, 64, 4096 and whole-file, verifying entry boundaries hold across arbitrary transfer splits, that the RAM tee is byte-exact, and that `.ncz` and bad magic are rejected rather than half-installed.

**Access-point mode — SHELVED (Chief Architect ruling).** No implementation path exists: `nifm` exposes no access-point API, and `ldn` is Nintendo's *local network communications* for Switch-to-Switch play — tied to a `local_communication_id` that must match the NACP, carrying game `advertise_data`, and not joinable by a PC. Ruled superfluous: WiFi is ubiquitous and the feature was a nice-to-have, never a requirement. The `config.ftp` AP fields (`start_access_point`, `ssid`, `password`, `use_5ghz`, `hidden_ssid`) are now dead and should be removed when the Settings screen lands in M8.

**Remaining M6 work:** MTP slice 4b (NSZ stream install).

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
- [ ] **M6 sockets** — `socketInitialize()` must be added to startup (before any service) and `socketExit()` to shutdown; `nifm` is already initialized. FTP/HTTP are clean-room BSD-socket implementations (compile as POSIX for the PC stub → loopback-testable).
- [ ] **Date/time selectors** — `behavior.date_format` / `behavior.time_24h` exist and drive the clock + log names now; the settings-screen UI to pick them lands in M8 (currently `config.json`-editable).
- [ ] GitHub browser — pagination handled via GitHub REST API (`Link` header); authenticated (token present) uses 5,000 req/hr, unauthenticated uses 60 req/hr with graceful rate-limit messaging in UI
- [ ] Full `en.json` — to be completed key-by-key as each screen is implemented; must be 100% complete before v1.0.0

---

*This document is the living reference for GarageNX development. All design decisions made prior to this document are captured above. Subsequent decisions should be appended to the relevant section with a date note.*
