# GarageNX - Architecture Reference

One-page map of the codebase, the invariants that hold it together, and the
process rules that were each learned by breaking something on real hardware.
Resumes sessions should read the "Working on this codebase" section first.

| Property | Value |
|---|---|
| Name | GarageNX |
| Binary | `GarageNX.nro` |
| Root path | `sdmc:/switch/GarageNX/` |
| Language | C++17 |
| Platform SDK | libnx (devkitPro), Switch only - no PC build |
| Renderer | SDL2 + SDL2_ttf + SDL2_image |
| Build system | CMake (`toolchain-switch.cmake`) |
| Version | 1.0.0a (`APP_VERSION` define in `CMakeLists.txt`) |
| License | AGPLv3 (source disclosure via `APP_SOURCE_URL`) |

---

## 1. What it is

A Nintendo Switch homebrew management tool: file browser, title management
(install/uninstall/move/dump), save backup and restore, system information,
network transfer (FTP/MTP/HTTP), SMB/NFS network browsing, USB mass storage,
game card support, and a maintenance Tools menu. C++17, devkitPro + libnx,
SDL2, zstd.

## 2. Repository layout

```
GarageNX/
├── CMakeLists.txt              ← build definition (SOURCES list is manual, not a glob)
├── toolchain-switch.cmake      ← devkitPro Switch target
├── assets/                     ← fonts (Inter), lang/ templates (shipped to SD), icon
├── source/
│   ├── main.cpp                ← entry point, startup(), app loop
│   ├── core/                   ← libnx system API wrappers
│   ├── services/               ← transports (FTP/HTTP/MTP), shared surfaces, guards
│   ├── install/                ← NSP/XCI/NSZ installation pipeline
│   ├── ui/                     ← renderer, theme, input, widgets
│   ├── screens/                ← one file per major view
│   ├── config/                 ← JSON-backed settings
│   └── tools/                  ← nsp_repair (host-testable dump repair)
├── tests/                      ← host test suite (pure modules only)
└── tools/                      ← syntax_guard.sh, build-net-portlibs.sh
```

Key modules:

- `core/ncm`, `core/es`, `core/nca`, `core/nsp_stream` - title/content
  enumeration, ticket handling, NCA header work, and the ONE NSP builder.
- `core/dump` - title and firmware dumping; `enumerate_firmware_content()` is
  the single firmware enumeration shared by the Tools scan and the dump.
- `services/storage_catalog` - the single definition of every storage surface
  (id, name, vfs root, access, confirm policy). All transports consume it.
- `services/write_guard` + `confirmation_broker` - one enforcement point for
  "may this mutation proceed?"; guarded writes block a worker thread while the
  console shows a confirmation modal.
- `services/save_surface` - shared Save Data surface (FTP/MTP/browser) with
  the single-slot mount policy in `save_resolve()`.
- `services/net_surface` + `screens/network_browser` - SMB/NFS client and
  chooser over a `net:` devoptab (needs the libnfs/libsmb2 portlibs; see
  "Building the network client" below).
- `install/stream_driver` + `stream_installer` + `ncz_window` -
  transport-agnostic streaming install (MTP/FTP/HTTP all adapt onto one
  driver); NSZ/NCZ decompression with re-encryption.
- `screens/tools_screen` - the maintenance ops; `ToolsScreen::Op` has two
  shapes: scan/run (dry-run → held-confirm → execute) or `push` (a dedicated
  screen, e.g. `WifiProfileScreen`).

## 3. Runtime layout and logs

```
sdmc:/switch/GarageNX/
├── GarageNX.nro
├── config.json
├── lang/            ← user language files
├── dumps/           ← title and firmware dumps
├── backups/         ← save backups
├── act_logs/        ← action log (when enabled)
└── logs/            ← per-subsystem diagnostics: firmware_dump.log, wifi.log,
                       pctl.log, nsp_stream.log, save.log, titles.log, install logs
```

Diagnostics always go to a file on the SD card, never only to stdout
(`SDL_Log` output exists on hardware only over nxlink). Every subsystem with
hardware-visible behaviour has its own log.

## 4. Core invariants (do not silently change)

- **One implementation per concept.** StorageCatalog for storages, NspStream
  for NSP building, save_surface for Save Data, title_surface for Installed
  Titles, stream_driver for installs, `enumerate_firmware_content()` for
  firmware. A second copy of any of these is the bug.
- **Safety model:** NAND user/system and saves are read-only by policy; any
  mutation requires an on-device confirmation (the PC → console → PC round
  trip is the safety property). NAND system is config-OFF by default.
  Default-deny: unknown or disabled paths are refused. Game card is physically
  read-only. Pinned by tests (`write_guard_test`, `storage_catalog_test`).
- **Config::save() is non-destructive** (merges onto the file as loaded). New
  config fields go into `to_json`/`from_json` AND the round-trip test.
  Surfaces are PER TRANSPORT (`Config::get().ftp.surfaces`, etc.); global
  checks use `Config::any_transport_exposes()`.
- **Save writes commit.** A Switch save filesystem is journalled; every
  mutation of a save path goes through `SaveWrite::*`, which commits. A
  discipline test fails the build if a raw `Fs::` mutation reappears in a file
  that can see save paths (exempt lines carry `// NOT-A-SAVE: <reason>`).
- **Snapshots by value across threads.** Never hand a transport a reference to
  shared mutable state - that was a real cross-thread use-after-free.
- **Multi-observer signals use generation counters**, not booleans
  (`titles_dirty`).
- **Destructors stop workers first.** Any component whose member a worker
  thread touches needs `~T() { stop(); }` so the worker joins before members
  die (the MTP/FTP/HTTP cancel-crash class). `OverlapBuffer::quiesce()`
  before `abort()` on every teardown path.
- **Synthesized surfaces:** MTP interns handles under synthetic prefixes
  (`titles:/`, `savedata:/`, `network:/`) that deliberately do NOT begin with
  the real mount root, so prefix-matching cannot mistake them for real paths.
- **Anti-drift copy rule:** before calling any libnx/ncm API, grep this repo
  for an existing caller and copy its usage - thread, pacing, choice of API.
- **Fail closed.** Enumerate-failure means "touch nothing", not "nothing
  exists" (unused tickets, deleted-user saves).

## 5. Testing reality

The host suite (`cmake -S tests -B build-tests && cmake --build build-tests &&
ctest --test-dir build-tests`) builds ONLY the pure, libnx-free modules - 52
tests, all under sanitizers. It says nothing about screens, transports, or
most of core. Their only mechanical check is the syntax guard:

```
tools/syntax_guard.sh                 # all files
tools/syntax_guard.sh source/...      # one file
```

The guard proves STRUCTURE only (brace balance, `#if` depth, parse). It stops
before type-checking, so wrong field names and calls to libnx functions that
do not exist both pass. The rule: **before using any libnx symbol not already
used in this codebase, verify the signature against the real header**
(`$DEVKITPRO/libnx/include/switch/services/*.h`). Do not write the call and
hope. A text-flattened wiki snippet or a secondhand API summary is a lead to
verify, never a source.

The real Switch build runs in the devkitPro container (`switch_dev_build`,
repo mounted at `/workspace`): configure once with the portlibs pkg-config
env, then `cmake --build build-fresh`.

## 6. Building the SMB/NFS network client

`switch-libnfs`/`switch-libsmb2` are not in dkp-pacman and must be
cross-compiled (the Switch lacks `getuid`/`getgid` and needs endian/socket
patches). The repo ships a self-contained script. The client is ON by
default; configure fails with instructions if the headers are missing:

```
tools/build-net-portlibs.sh
cmake -- -DPLATFORM=Switch        # -DGARAGENX_NET_CLIENT=OFF to build without it
```

Manual routes and details: `docs/BUILDING_NETWORK_CLIENT.md`.

## 7. Tools menu - status of each op

The scan/dry-run/hold-confirm flow is shared; statuses as of 2026-09-07:

- **Install placeholders, old game updates, erpt_reports** - done,
  hardware-verified.
- **Orphaned content, downloaded system updates** - negative path
  hardware-confirmed; positive path not exercised (needs a staged condition).
  Uses `ncmContentMetaDatabaseLookupOrphanContent` and
  `nssuDestroySystemUpdateTask` - real, verified functions.
- **Unused tickets** - code complete, host-tested, hardware-untested.
  `Core::Es::delete_ticket()` (es cmd 3) has zero hardware mileage; test
  against an easily reacquired ticket first. Fails closed.
- **Repair tickets with dump errors** - implemented, hardware test owed. Finds
  dumped NSPs missing a ticket whose title has a common ticket NOW, and
  re-dumps through the proven pipeline (background thread). The pure half
  (`source/tools/nsp_repair`) is host-tested.
- **Deleted-user saves** - code complete; positive path is narrow (standard
  user deletion removes saves too). Direct delete by design.
- **Parental controls** - hardware-confirmed (2026-09-07). Root cause of the
  old "nothing to tidy": `pctlInitialize()` was never called, so commands hit
  a zeroed session. Delete is raw IPC cmd 1043 (per
  ITotalJustice/Reset-Parental-Controls-NX); detection probes cmd 1032/1031;
  logs to `logs/pctl.log`.
- **Wi-Fi profiles** - hardware-confirmed (2026-09-07). Deletion toggles nifm
  to the Admin session for cmd 10 (RemoveNetworkProfile) and restores User;
  SSIDs resolved from the profile's BasicInfo (Ssid is a 0x21-byte struct,
  byte 0 = length); logs to `logs/wifi.log`. The screen is a per-item picker,
  not a bulk op.
- **Firmware dump** - hardware-confirmed (2026-09-07). Runs on a background
  thread with live progress; B cancels; failures halt with a notification.
  Two earlier root causes: diagnostics went only to stdout (now
  `logs/firmware_dump.log` via `fw_log()`), and single-level mkdir could never
  create `dumps/firmware/<ver>/` (now `make_directory_recursive`). The dump
  logs a storage cross-check (present-in-storage vs meta-referenced NCA
  counts) - the settled answer to the historical 119/234/238 count questions.
- **Dropped, with reasons:** "clear ticket cache" (no ES operation does that;
  this codebase has no ticket cache), "delete users" (stock acc.h is entirely
  read-only; Goldleaf's working `accountextDeleteUser` raw-IPC approach is the
  lead to pursue - get its real command ID from source first).
- **Deliberately absent:** Installed Tickets screen (listing invites deleting;
  removal can unlaunch titles), `use_overclocking` (needs clkrst/pcv code that
  does not exist), `fuses_burned` (bootloader/secure-monitor-only; unreachable
  from a HOS application).

## 8. Known deferred items

- Aggregate activity stats (first-play date, session counts) need the
  per-user play-log save archive `SYSTEM:/save/80000000000000F0`; per-title
  stats (the Activity Log screen) work via pdm now.
- SDK version display needs the SystemVersion title (0100000000000809) via
  NCM; shows `-`.
- NSP/XCI peek view in the file browser (roadmap M7).

## 9. Working on this codebase

1. **Observe, don't guess.** If a fix does not land on the first try, add
   logging (to the SD-card log) instead of theorising, and read state back to
   prove which call is innocent.
2. **Copy the existing caller** before writing a new one (see invariants).
3. **New .cpp files must be added to CMakeLists.txt's SOURCES by hand.**
   Host tests do not catch a missing source file; the link step does.
4. **Verify libnx APIs against real headers first** (§5).
5. **Run the verification loop after every change:** syntax guard on touched
   libnx-only files, host suite, JSON validity of `assets/lang/en.json`, and
   the container Switch build before handing anything to hardware.
6. **Hardware-verify each step before building on it.** Be explicit about
   what is verified versus inferred.
7. **Localization:** `assets/lang/en.json` is the canonical template and must
   always be complete; missing keys in other languages fall back to English.
   Language files are read only from `sdmc:/switch/GarageNX/lang` - never
   embedded in the NRO - so translations never require a rebuild.
