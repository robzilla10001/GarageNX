# GarageNX - session notes for Claude

Read `docs/ARCHITECTURE.md` (especially §9, "Working on this codebase") before changing code.

## Build environment: ALWAYS use the devkitPro Docker container

All Switch builds, cross-compiles and anything needing portlibs (curl, mbedtls,
zlib, minizip, libnfs, libsmb2, SDL2, ...) MUST run inside the maintained
devkitPro container, never on the host. The host has no `/opt/devkitpro/portlibs`.

- Container: `switch_dev_build` (image `devkitpro/devkita64:latest`). Start it if
  it is not running (`docker start switch_dev_build`).
- The host's `~/projects` is mounted at `/projects`, so this repo is at
  `/projects/GarageNX-qwen` inside the container. Run commands with
  `docker exec switch_dev_build bash -c '...'`.
- `DEVKITPRO=/opt/devkitpro` inside the container.
- If something is missing from the container (a package, a tool), do NOT install
  or download it on your own: tell the user what is needed and ask them to confirm
  the download/install command before running it.
- Host-only work is limited to the pure host test suite
  (`cmake -S tests -B <scratch dir> && cmake --build <scratch dir> && ctest --test-dir <scratch dir>`).

## Build directories

- `build-container/` - the Switch build (configure/build it inside the container,
  see above). Output: `build-container/GarageNX.nro`.
- `build-tests/` - host unit tests only (`cmake -S tests -B build-tests`).
- `build-switch/` - release archives only (`GarageNX-<version>.7z`). Its CMake
  cache is stale (host paths) and unusable from the container.

## NRO naming: the NRO is ALWAYS `GarageNX.nro`

Never put a version in the NRO's file name (no `GarageNX-1.0.0b.nro`): forwarders
point at that exact path, so a renamed NRO breaks them. Versioning lives in the
release archive name (`GarageNX-<version>.7z`) and in the code (`APP_VERSION` in
`CMakeLists.txt`). Release archives contain `switch/GarageNX/GarageNX.nro` and
`switch/GarageNX/lang/`. Do not copy or rename build output to a versioned `.nro`.
