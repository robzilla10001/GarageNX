# GarageNX

**Taking care of everything under the hood.**

GarageNX is an all-in-one homebrew management tool for the Nintendo Switch — a clean-room, open-source successor in spirit to tools like DBI. It aims to be a solid, polished, mature utility for power users: file management, title management, save data handling, system information, network file transfer, and a broad set of maintenance tools, all in one place.

Why? DBI has a shady reputation, and the author is notoriously hostile towards non-russian speakers. He has gone so far as to threaten bricking the consoles of anyone using an unofficial translation. I personally have not seen this happen, nor do I know of anyone who has; however, it's a bold enough claim to be taken seriously. It may not be malware in the strictest terms, but it's close enough to make a lot of people feel a particular kind of way. So, in an effort to alleviate those issues, I'm attempting to make a 1 for 1 replacement for DBI. There are other installers out there, with their strengths and weaknesses, but none of them really does exactly what DBI does. While researching what is necessary to make this piece of software work, I've come to realize that there isn't much that DBI does that is unique. Challenging? Sure. There's a lot of work that has gone into this package, I'm not going to pretend that it's not a well thought out, well executed group of tools. But it's not impossible to replicate, and I aim to prove that. And, as the code is not available, I will not be reusing any of the author of DBI's original code; everything is built from the ground up (unless otherwise noted) specifically for this project. I did not go out of my way to reinvent the wheel everywhere; SDL2 is used for rendering and input, and nlohmann/json for config and language parsing. Everything else — including the FTP, HTTP, and MTP network services — is written from scratch, using libnx, ITotalJustice's gists and other open source projects as reference materials. There has been no reverse engineering or disassembly done here: I want absolutely no ties whatsoever to DBI. We appreciate what you've done for the community thus far, but we have it from here. Your services are no longer required.  

> **Status:** Moderate progress. Milestone 6 (services) is partially complete; HTTP/FTP builds. See [architecture](https://github.com/robzilla10001/GarageNX/blob/main/docs/GarageNX_Architecture.md).

---

## Philosophy

GarageNX is built to be the opposite of a closed, mysterious tool. It is:

- **Open** — full source, permissively documented, meant to be read and understood.
- **Forkable** — modular architecture, clear separation of concerns, welcoming to contributors and re-branders (within the terms of the license).
- **Professional** — a clean, high-contrast interface with no clutter. Legibility and predictable, safe behavior are the primary design goals.
- **Stable** — paged file loading, background workers off the UI thread, and strict confirmation on every destructive operation. This tool has a lot of power; it treats that power with respect.

---

## Features

Planned and in-progress functionality includes:

- **File browser** — ranger-style three-column navigation with split-pane copy/move, text and hex viewers (paged for large files), and archive peeking (NSP/XCI content listing).
- **Network browser** — navigate HTTP(S), FTP, and GitHub repositories using the same browser interface.
- **Title management** — enumerate installed titles; uninstall, move between SD and NAND, reset version requirements, edit metadata, dump, and repack.
- **Homebrew management** — recursively scan and launch NRO files; automatic forwarder generation.
- **Maintenance tools** — clean orphaned records, old updates, placeholders, unused tickets, and more; firmware dumping; NTP sync; version manifests.
- **System information** — comprehensive firmware, CFW, hardware, battery, and activity reporting.
- **Connectivity** — USB-MTP, FTP server, and HTTP server with QR-code network sharing.
- **Full localization** — drop-in language files; no language is second-class.

---

## Building

### Requirements

- [devkitPro](https://devkitpro.org/) with the `switch-dev` group installed
- `switch-sdl2`, `switch-sdl2_ttf`, `switch-sdl2_image` (via `dkp-pacman`)
- CMake 3.16+

### Compile

```bash
mkdir build && cd build
cmake .. -DPLATFORM=Switch
make -j$(nproc)
```

The output `GarageNX.nro` will be in the `build/` directory. Copy it to `sdmc:/switch/GarageNX/GarageNX.nro` on your SD card.

---

## Project Structure

```
source/
  core/        System API wrappers (filesystem, titles, battery, etc.)
  services/    Background services (FTP, HTTP, MTP, NTP)
  install/     NSP/XCI/NSZ installation pipeline
  ui/          Renderer, theme, input, layout, widgets
  screens/     One file per major view
  config/      JSON-backed settings
  lang/        Localization loader
assets/
  fonts/       Inter TTF (bundled)
  lang/        Language files (en.json is the canonical template)
  icons/       Application icon (bundled)
```

The full architecture reference lives in `docs/` (see `GarageNX_Architecture.md`).

---

## Localization

`assets/lang/en.json` is the translation template — it always contains every key. To add a language:

1. Copy `en.json`, rename it (e.g. `es.json`, `pt-br.json`).
2. Update the `meta.language` and `meta.author` fields.
3. Translate the values. Any key you leave out falls back to English automatically.
4. Drop the file into `sdmc:/switch/GarageNX/lang/` on your device.

GarageNX will detect it on next launch and offer to switch. Contributions of translation files back to this repository are very welcome.

---

## License

GarageNX is licensed under the **GNU Affero General Public License v3.0** (AGPLv3). See [`LICENSE`](LICENSE) for the full text.

### What this means for you

- You are free to **use, study, modify, and redistribute** this software.
- If you **distribute** GarageNX or a modified version, you must make your **complete corresponding source code** available under the same AGPLv3 license.
- **Network use is distribution.** If you run a modified version of GarageNX (or software incorporating its code) as a service that users interact with over a network — including its built-in FTP or HTTP servers — you **must** make the complete corresponding source of your modified version available to those users.

### Source-availability requirement for network deployments

Per AGPLv3 §13, any network-accessible deployment of GarageNX or a derivative **must provide users a way to obtain its complete source code.** In keeping with the spirit of this project, we ask (and the license effectively requires) that:

> **Any network deployment or fork must maintain a clear, visible link to the original source code repository:**
> **https://github.com/robzilla10001/GarageNX**
>
> Forks must link to their own corresponding source, and are encouraged to also credit and link back to this upstream repository.

The application itself surfaces its source URL in the network server screens and system information view, so this requirement is satisfied by default when using the software unmodified. If you modify GarageNX, keep that link present and pointed at *your* corresponding source.

---

## Contributing

Contributions are welcome — code, translations, bug reports, and documentation alike. This project explicitly aims to be easy to understand and build upon. Please read the architecture document before proposing structural changes, and keep the `en.json` template complete when adding user-facing strings.

By contributing, you agree that your contributions will be licensed under AGPLv3.

---

## Acknowledgements

GarageNX is a clean-room reimplementation. It stands on the shoulders of the broader Switch homebrew community's documentation and reverse-engineering work. It bundles or links the following open-source components: [libnx](https://github.com/switchbrew/libnx), [SDL2](https://www.libsdl.org/), [nlohmann/json](https://github.com/nlohmann/json), and [Inter](https://rsms.me/inter/). See each component's license for details.

## Credits / Thanks  

- `ITotalJustice` : just a boatload of documentation that has proved endlessly helpful. Thank you for your contributions the community.
- `switchbrew team` : obligatory shoutout for creating and maintaing libnx. Without it, none of this would be possible.
- `devkitpro team` : obligatory shoutout for setting up an excellent sdk/build environment. Without your docker container, I would be absolutely lost.
- `duckbill` : for a great application and the inspiration to make something just a little better (I hope).
- more to come as I remember them. 
