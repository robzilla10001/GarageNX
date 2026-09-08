# Third-Party Projects and Sources

GarageNX is a clean-room reimplementation. Code in this repository is written
for this project except where noted below; external projects are either linked
in, bundled as assets, or used as documented references for approaches, IPC
command IDs, and binary layouts. Nothing was decompiled from proprietary
sources, and no code from incompatibly-licensed projects was copied
(see CONTRIBUTING.md).

## A. Linked or vendored code (ships in the binary)

| Project | Origin | Where used | License / Distributable or Usable? |  
|---|---|---|---|
| libnx (switchbrew) | linked, devkitPro | entire `core/` platform layer | ISC License ✅ |  
| SDL2, SDL2_ttf, SDL2_image | linked, dkp-pacman | renderer, input, fonts, icons | [Unknown](https://github.com/dhustkoder/SDL2?tab=License-1-ov-file) ✅ |  
| nlohmann/json (Niels Lohmann, MIT; includes an Abseil-derived snippet) | vendored at `source/nlohmann/json.hpp` | config + language parsing | MIT License ✅ |  
| zstd (Facebook) | linked | NCZ/NSZ decompression (`install/ncz.cpp`) | BSD License ✅ |  
| libusbhsfs (DarkMatterCore) | linked | USB mass storage (`core/usb_mount.*`) | GNU GPL v2.0 ✅ |  
| libnfs (sahlberg, pinned @20b39fd) | cross-compiled with a Switch patch | NFS client (`services/net_surface.cpp`) | GNU GPL v3.0 ✅ |  
| libsmb2 (sahlberg, v4.0.0) | cross-compiled with a Switch patch | SMB client (`services/net_surface.cpp`) | GNU GPL v3.0 ✅ |  
| nxmp-portlibs (proconsule) | patch/tarball source for libnfs + libsmb2 | `tools/build-net-portlibs.sh` | GNU GPL v3.0 ✅ |  

The libnfs/libsmb2 patches and the build script live in this repository
(`tools/build-net-portlibs.sh`); neither library is available in dkp-pacman.
All licenses can be found in /licenses.

## B. Bundled assets

- Inter (Rasmus Andersson, SIL OFL 1.1) - UI text font
- DejaVu Sans Mono (Bitstream Vera / DejaVu license) - hex viewer font

See `assets/fonts/README.txt`.

## C. Referenced logic (approach/command IDs used, clean-room re-implemented)

1. ITotalJustice / Reset-Parental-Controls-NX - pctl raw-IPC command 1043 for
   parental-controls deletion, plus probes 1032/1031
   (`screens/tools_screen.cpp`).
2. ITotalJustice XCI installer - NS application-record sequence: cmd 27
   delete, cmd 16 push, HipcMapAlias buffer, record type 3 = Installed
   (`install/installer.cpp`, `tools/ns_probe/ns_probe.cpp`).
3. Sphaira / yati - RegisterNcasAndPushRecord install flow including zeroed
   storage_id conventions (`install/installer.cpp`), NCZ
   decompressFuncInternal mirroring (`install/ncz.cpp`), app-exit behavior
   notes (`core/app_exit.hpp`).
4. nxdumptool - titlekey-to-key-area NCA header conversion
   (generateEncryptedNcaKeyAreaWithTitlekey, `core/nca_modify.cpp`).
5. hactool / nstool - NCA/XCI binary layout offsets and key-file naming
   (`core/nca.cpp`, `install/xci_reader.cpp`, `core/keys.cpp`).
6. switch-time (3096) - NTP clock-setting approach: time:s service type +
   NetworkSystemClock (`core/ntp.cpp`).
7. NXMP (proconsule) - exit-to-HOME via __nx_applet_exit_mode
   (`screens/menu_dispatch.cpp`); also the portlibs patch source above.
8. Hekate - display-parity comparisons: eMMC CID readout, board model,
   battery design-capacity quirks (`core/storage.cpp`, `core/battery.cpp`,
   `screens/system_info.cpp`).
9. Tinfoil - split-NSP directory layout (`install/nsp_reader.cpp`,
    `core/dump.cpp`), ticketless NSZ approach (`install/ncz.cpp`).
10. Goldleaf - lead for accountextDeleteUser raw IPC (dropped feature,
    `docs/ARCHITECTURE.md` section 7); USB-library peer.
11. Awoo - USB-library peer (`core/usb_mount.hpp`).
12. NX-Activity-Log - reference for what accurate play stats require (the
    per-user play-log archive 80000000000000F0) (`core/activity.cpp`).
13. libhaze (Atmosphere) - explicitly NOT used: GPLv2-only vs this project's
    AGPLv3; the documented reason MTP is clean-room
    (`services/mtp_server.hpp`).
14. Atmosphere / exosphere - config items 65000-65010 for CFW detection,
    erpt_reports path (`core/atmosphere.cpp`).

## D. Documentation, specifications, and data sources (no code)

- switchbrew.org wiki - NCA_Format, CNMT, XCI, ETicket services (cmd 3
  DeleteTicket), Network Interface services (SfNetworkProfileBasicInfo,
  nifm:a cmd 10), Homebrew ABI spec.
- SwIPC - IETicketService command IDs (`core/es.cpp`).
- blawar/titledb - live runtime data source: versions.txt for title version
  lookup (`config/defaults.hpp`).
- ISO/IEC 18004 - the QR encoder is written clean-room to this standard
  (`core/qr.hpp`).

## License note

GarageNX is AGPLv3. License is available in full at ./LICENSE and /licenses/GNU AGPLv3.txt
