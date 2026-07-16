# Contributing to GarageNX

Thanks for your interest in improving GarageNX. This project is built to be understood, extended, and forked — contributions of all kinds are welcome.

## Ways to contribute

- **Code** — bug fixes, new features, refactoring toward the architecture in `docs/GarageNX_Architecture.md`.
- **Translations** — add or improve language files. See below.
- **Testing** — report bugs, especially edge cases in file operations or title management. Hardware-specific behavior is valuable.
- **Documentation** — clarify build steps, document undocumented system behavior, improve inline comments.

## Before you start

Read `docs/GarageNX_Architecture.md`. It captures every design decision, the module layout, and the milestone plan. Structural changes should be discussed against that document first — open an issue before a large PR.

## Ground rules

1. **Stability first.** This tool has destructive capabilities. Every destructive operation must have a clear confirmation modal. Never load an unbounded amount of data into memory — page it.
2. **No hardcoded colors or strings.** Colors come from `Theme::Token`. User-facing text comes from `Lang::t()`. Add new keys to `assets/lang/en.json`.
3. **`en.json` stays complete.** It is the canonical translation template. Never commit a user-facing string without adding its English key.
4. **Keep the UI thread responsive.** Long operations (copy, install, dump, network) run on background workers, not the render loop.
5. **Match the existing style.** Clear names, section-commented files, no clever one-liners where a readable block will do.

## Translations

1. Copy `assets/lang/en.json` to a new file named with the language code (e.g. `es.json`, `pt-br.json`, `de.json`).
2. Update `meta.language` and `meta.author`.
3. Translate the values, not the keys. Leave the `meta.notes` and any key you're unsure about — untranslated keys fall back to English automatically.
4. Test on-device by dropping the file in `sdmc:/switch/GarageNX/lang/`.
5. Open a PR adding the file to `assets/lang/`.

## Building and testing

See the [README](README.md) for build instructions. Use the PC stub target (`-DPLATFORM=PC`) for fast UI iteration; verify on hardware or an emulator before submitting anything touching `core/`, `services/`, or `install/`.

## Licensing

By submitting a contribution, you agree it will be licensed under the project's **AGPLv3** license. Do not submit code you don't have the right to license this way, and don't paste code from incompatibly-licensed projects (including the original DBI, which this is a clean-room reimplementation of — work from public documentation and specifications, not from decompiled or proprietary source).

## Commit and PR conventions

- Keep commits focused; one logical change per commit where practical.
- Reference the milestone or module in the PR description.
- If your change is user-facing, note it plainly so it can go in a changelog.
