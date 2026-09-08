cmake_minimum_required(VERSION 3.16)

# ─── Toolchain must be set before project() ───────────────────────────────────
# Switch is the only supported target. The PC stub build was removed in rev 3:
# it had never worked (its `stubs/` tree was never written, so it failed on the
# first libnx-touching source), PLATFORM_PC was defined but never read, and
# hardware iteration is faster than maintaining a second build of the app.
#
# This did NOT remove off-device testing. tests/ is a standalone host CMake
# project that does not read this file and needs no stubs or SDL2 — it exists
# because hardware cannot detect a data race. See tests/CMakeLists.txt.
#
# PLATFORM is retained (rather than deleted) so the guards below stay untouched,
# but the only accepted value is Switch.
if(NOT DEFINED PLATFORM)
    set(PLATFORM "Switch")
endif()
if(NOT PLATFORM STREQUAL "Switch")
    message(FATAL_ERROR
        "PLATFORM=${PLATFORM} is not supported. Switch is the only target; the PC "
        "stub build was removed in rev 3. For off-device unit tests use the "
        "standalone host project: cmake -S tests -B build-tests")
endif()

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain-switch.cmake"
    CACHE STRING "Toolchain file" FORCE)

# CMake project() VERSION must be strictly numeric; the "a" label lives in
# APP_VERSION below (shown in the title bar, System Info, and MTP DeviceVersion).
project(GarageNX VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Compile-time constants ───────────────────────────────────────────────────
add_compile_definitions(
    APP_NAME="GarageNX"
    APP_VERSION="1.0.0a"
    APP_VERSION_MAJOR=1
    APP_VERSION_MINOR=0
    APP_VERSION_PATCH=0
    # AGPLv3 section 13: a network-facing deployment must offer users its
    # corresponding source. The service screens and System Info surface this.
    # Forks MUST repoint this at their own corresponding source.
    APP_SOURCE_URL="https://github.com/robzilla10001/GarageNX"
)

# ─── Platform-specific setup ──────────────────────────────────────────────────
if(PLATFORM STREQUAL "Switch")
    add_compile_definitions(PLATFORM_SWITCH=1)

    # devkitPro environment
    set(DEVKITPRO $ENV{DEVKITPRO})
    if(NOT DEVKITPRO)
        set(DEVKITPRO "/opt/devkitpro")
    endif()

    set(PORTLIBS      "${DEVKITPRO}/portlibs/switch")
    set(LIBNX         "${DEVKITPRO}/libnx")

    # SDL2 and friends are in portlibs
    list(APPEND CMAKE_PREFIX_PATH "${PORTLIBS}" "${LIBNX}")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2       REQUIRED sdl2)
    pkg_check_modules(SDL2_TTF   REQUIRED SDL2_ttf)
    pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

    # For a fully static Switch build we need the transitive deps that SDL2_ttf
    # and SDL2_image pull in (freetype, harfbuzz, png, bz2, brotli, etc.).
    # pkg_check_modules without STATIC only gives the top-level -l flag, so we
    # call pkg-config --static manually and capture the full library list.
    execute_process(
        COMMAND pkg-config --libs --static SDL2_ttf
        OUTPUT_VARIABLE SDL2_TTF_STATIC_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND pkg-config --libs --static SDL2_image
        OUTPUT_VARIABLE SDL2_IMAGE_STATIC_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND pkg-config --libs --static sdl2
        OUTPUT_VARIABLE SDL2_STATIC_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Convert space-separated -lfoo flags into a CMake list for target_link_libraries
    separate_arguments(SDL2_TTF_STATIC_LDFLAGS_LIST   UNIX_COMMAND "${SDL2_TTF_STATIC_LDFLAGS}")
    separate_arguments(SDL2_IMAGE_STATIC_LDFLAGS_LIST UNIX_COMMAND "${SDL2_IMAGE_STATIC_LDFLAGS}")
    separate_arguments(SDL2_STATIC_LDFLAGS_LIST        UNIX_COMMAND "${SDL2_STATIC_LDFLAGS}")

    message(STATUS "SDL2_ttf static flags: ${SDL2_TTF_STATIC_LDFLAGS}")
    message(STATUS "SDL2_image static flags: ${SDL2_IMAGE_STATIC_LDFLAGS}")
    message(STATUS "SDL2 static flags: ${SDL2_STATIC_LDFLAGS}")

    # nlohmann/json — bundled as a single header in source/nlohmann/json.hpp

    include_directories(
        ${LIBNX}/include
        ${PORTLIBS}/include
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_TTF_INCLUDE_DIRS}
        ${SDL2_IMAGE_INCLUDE_DIRS}
        source   # makes #include <nlohmann/json.hpp> resolve to source/nlohmann/json.hpp
    )

    link_directories(
        ${LIBNX}/lib
        ${PORTLIBS}/lib
    )

endif()

# ─── Source files ─────────────────────────────────────────────────────────────
set(SOURCES
    source/main.cpp

    source/config/config.cpp

    source/lang/localization.cpp

    source/core/fs.cpp
    source/core/system.cpp
    source/core/storage.cpp
    source/core/battery.cpp
    source/core/atmosphere.cpp
    source/core/ntp.cpp
    source/core/activity.cpp
    source/core/datetime.cpp
    source/core/net.cpp
    source/core/qr.cpp
    source/core/keys.cpp
    source/core/album_mount.cpp
    source/core/nand_mount.cpp
    source/core/gamecard_mount.cpp
    source/core/usb_mount.cpp
    source/core/sleep_inhibit.cpp
    source/core/nsp_stream.cpp
    source/core/save_mount.cpp
    source/core/save_backup.cpp
    source/core/nca.cpp
    source/core/ncm.cpp
    source/core/es.cpp
    source/core/title_ops.cpp
    source/core/dump.cpp

    source/tools/nsp_repair.cpp

    source/install/nsp_reader.cpp
    source/install/xci_reader.cpp
    source/install/installer.cpp
    source/install/ncz.cpp
    source/install/ncz_window.cpp
    source/install/stream_installer.cpp
    source/install/stream_driver.cpp

    source/services/service_manager.cpp
    source/services/ftp_server.cpp
    source/services/http_server.cpp
    source/services/rate_meter.cpp
    source/services/overlap_buffer.cpp
    source/services/storage_catalog.cpp
    source/services/net_surface.cpp
    source/services/write_guard.cpp
    source/services/title_naming.cpp
    source/services/pfs0_layout.cpp
    source/services/title_surface.cpp
    source/services/save_surface.cpp
    source/services/confirmation_broker.cpp
    source/services/mtp_data.cpp
    source/services/mtp_server.cpp
    
    source/ui/renderer.cpp
    source/ui/theme.cpp
    source/ui/font.cpp
    source/ui/input.cpp
    source/ui/layout.cpp
    source/ui/widgets.cpp
    source/ui/modal.cpp
    source/ui/keyboard.cpp
    source/ui/title_bar.cpp
    source/ui/status_bar.cpp
    source/ui/splash.cpp

    source/screens/main_menu.cpp
    source/screens/submenu_screen.cpp
    source/screens/settings_screen.cpp
    source/screens/save_manager.cpp
    source/screens/save_backup_screen.cpp
    source/screens/activity_log.cpp
    source/screens/gamecard.cpp
    source/ui/backup_overlay.cpp
    source/screens/menu_dispatch.cpp
    source/screens/file_browser.cpp
    source/screens/network_browser.cpp
    source/screens/network_edit.cpp
    source/screens/file_viewer.cpp
    source/screens/system_info.cpp
    source/screens/tools_screen.cpp
    source/screens/wifi_profile_screen.cpp
    source/screens/title_list.cpp
    source/screens/title_detail.cpp
    source/screens/ftp_screen.cpp
    source/screens/http_screen.cpp
    source/screens/mtp_screen.cpp

)

# ─── zstd (for NSZ/XCZ decompression) ────────────────────────────────────────
include(FetchContent)
set(ZSTD_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_COMPRESSION OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DECOMPRESSION ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.5
    SOURCE_SUBDIR  build/cmake
)
FetchContent_MakeAvailable(zstd)

# ─── Executable ───────────────────────────────────────────────────────────────
# Browse Network SMB/NFS client. ON by default - the portlibs are a standard
# part of this project's build environment (installed by
# tools/build-net-portlibs.sh, which produces switch-libnfs + switch-libsmb2).
# The build fails at CONFIGURE time with instructions if the portlib headers
# are missing. To build without the client:
#     cmake .. -DPLATFORM=Switch -DGARAGENX_NET_CLIENT=OFF
option(GARAGENX_NET_CLIENT "Build the SMB/NFS network client (needs switch-libnfs + switch-libsmb2)" ON)
set(GARAGENX_NET_LIBS "")
if(GARAGENX_NET_CLIENT)
    # The client is ON — but it needs the portlib HEADERS at compile time. Check
    # for them here and fail with an actionable message, so an enabled-without-
    # portlibs build stops at configure with instructions rather than deep inside
    # the compile with "nfsc/libnfs.h: No such file or directory". devkitPro
    # portlibs always install under $DEVKITPRO/portlibs/switch/include.
    set(_gnx_portlib_inc "$ENV{DEVKITPRO}/portlibs/switch/include")
    if(NOT EXISTS "${_gnx_portlib_inc}/nfsc/libnfs.h" OR
       NOT EXISTS "${_gnx_portlib_inc}/smb2/smb2.h")
        message(FATAL_ERROR
            "GARAGENX_NET_CLIENT=ON, but the SMB/NFS portlib headers were not found "
            "under ${_gnx_portlib_inc} (nfsc/libnfs.h and/or smb2/smb2.h).\n"
            "  • To install the portlibs, run the repo's build script:\n"
            "        tools/build-net-portlibs.sh\n"
            "  • To build WITHOUT the network client, turn it off:\n"
            "        cmake .. -DPLATFORM=Switch -DGARAGENX_NET_CLIENT=OFF\n")
    endif()
    add_compile_definitions(GNX_NET_CLIENT)
    set(GARAGENX_NET_LIBS nfs smb2)
    message(STATUS "Browse Network client ENABLED (linking nfs smb2)")
else()
    message(STATUS "Browse Network client OFF (build with -DGARAGENX_NET_CLIENT=ON after installing switch-libnfs + switch-libsmb2)")
endif()

add_executable(GarageNX ${SOURCES})

if(PLATFORM STREQUAL "Switch")
    target_link_libraries(GarageNX
        ${SDL2_TTF_STATIC_LDFLAGS_LIST}
        ${SDL2_IMAGE_STATIC_LDFLAGS_LIST}
        ${SDL2_STATIC_LDFLAGS_LIST}
        libzstd_static
        # libusbhsfs GPL build: the library references NTFS-3G and lwext4 symbols,
        # which live in SEPARATE archives. Its own README is explicit —
        # "if you're using a GPLv2+ licensed build, you'll also need to link your
        # application against both NTFS-3G and lwext4: -lusbhsfs -lntfs-3g -llwext4"
        # — and its example Makefiles do exactly that. An ISC build would need
        # usbhsfs alone.
        #
        # Order matters: these must follow usbhsfs (which references them) and
        # precede nx.
        usbhsfs
        ntfs-3g
        lwext4
        # Browse Network (services/net_surface.cpp): nfs + smb2, injected here only
        # when -DGARAGENX_NET_CLIENT=ON. Like usbhsfs they must precede nx; the var
        # is empty otherwise, so the tree links without the portlibs present.
        ${GARAGENX_NET_LIBS}
        nx
        m
    )

    # ── NRO generation ────────────────────────────────────────────────────────
    # devkitPro provides nx-elf2nro via the switch_tools package.
    # We call it as a post-build step so `cmake --build` produces the .nro
    # directly without needing a separate Make wrapper.

    set(NRO_OUTPUT  "${CMAKE_BINARY_DIR}/GarageNX.nro")
    set(ELF_OUTPUT  "${CMAKE_BINARY_DIR}/GarageNX")
    set(ICON_PATH   "${CMAKE_SOURCE_DIR}/assets/icons/icon.jpg")
    set(NACP_PATH   "${CMAKE_BINARY_DIR}/GarageNX.nacp")

    # RomFS: everything the app loads at runtime (fonts, lang) must be embedded
    # into the NRO so it's available on hardware under romfs:/.
    # We stage a clean romfs dir (assets minus the icon, which is baked separately).
    set(ROMFS_DIR "${CMAKE_BINARY_DIR}/romfs")

    # Stage romfs contents: fonts + lang only (not the icon — that's a separate flag)
    add_custom_command(TARGET GarageNX POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ROMFS_DIR}/fonts"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ROMFS_DIR}/lang"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ROMFS_DIR}/bg"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/fonts" "${ROMFS_DIR}/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/lang"  "${ROMFS_DIR}/lang"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/bg"    "${ROMFS_DIR}/bg"
        COMMENT "Staging RomFS (fonts, lang, bg)..."
    )

    # Build NACP (application metadata)
    add_custom_command(TARGET GarageNX POST_BUILD
        COMMAND "${DEVKITPRO}/tools/bin/nacptool"
            --create "GarageNX" "GarageNX Team" "${PROJECT_VERSION}"
            "${NACP_PATH}"
        COMMENT "Generating NACP metadata..."
    )

    # Convert ELF → NRO, embedding the RomFS
    add_custom_command(TARGET GarageNX POST_BUILD
        COMMAND "${DEVKITPRO}/tools/bin/elf2nro"
            "${ELF_OUTPUT}"
            "${NRO_OUTPUT}"
            --nacp="${NACP_PATH}"
            --icon="${ICON_PATH}"
            --romfsdir="${ROMFS_DIR}"
        COMMENT "Converting ELF to NRO (with RomFS)..."
    )

    message(STATUS "Switch build configured. Output: ${NRO_OUTPUT}")

endif()

# ─── Assets ───────────────────────────────────────────────────────────────────
# Copy assets to the binary directory so the app can find them at runtime
# regardless of where it's launched from.
add_custom_command(TARGET GarageNX POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "${CMAKE_BINARY_DIR}/assets"
    COMMENT "Copying assets to build directory..."
)

# ─── ns_probe tool ────────────────────────────────────────────────────────────
if(PLATFORM STREQUAL "Switch")
    add_executable(ns_probe tools/ns_probe/ns_probe.cpp)
    target_include_directories(ns_probe PRIVATE
        ${DEVKITPRO}/libnx/include
        ${DEVKITPRO}/portlibs/switch/include
    )
    target_compile_definitions(ns_probe PRIVATE PLATFORM_SWITCH=1)
    target_compile_options(ns_probe PRIVATE
        -march=armv8-a+crc+crypto -mtune=cortex-a57
        -ffunction-sections -fdata-sections
    )
    target_link_options(ns_probe PRIVATE
        -Wl,-Map,${CMAKE_BINARY_DIR}/ns_probe.map
    )
    target_link_libraries(ns_probe nx m)

    set(PROBE_NRO  "${CMAKE_BINARY_DIR}/ns_probe.nro")
    set(PROBE_ELF  "${CMAKE_BINARY_DIR}/ns_probe")
    set(PROBE_NACP "${CMAKE_BINARY_DIR}/ns_probe.nacp")

    add_custom_command(TARGET ns_probe POST_BUILD
        COMMAND "${DEVKITPRO}/tools/bin/nacptool"
            --create "ns_probe" "GarageNX" "0.1.0" "${PROBE_NACP}"
        COMMAND "${DEVKITPRO}/tools/bin/elf2nro"
            "${PROBE_ELF}" "${PROBE_NRO}" --nacp="${PROBE_NACP}"
        COMMENT "Building ns_probe.nro"
    )
    message(STATUS "ns_probe tool configured. Output: ${PROBE_NRO}")
endif()

# ─── Off-device tests ─────────────────────────────────────────────────────────
# The host test suite lives in tests/ and is a SEPARATE CMake project, not an
# add_subdirectory() of this one. It must be configured with the host compiler:
#
#   cmake -S tests -B build-tests && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure --repeat until-fail:20
#
# It is kept out of this build on purpose: this file defaults to the Switch
# toolchain (a test cross-compiled to aarch64 cannot be run), and each harness
# has its own main(), which would clash with source/main.cpp. See
# tests/CMakeLists.txt for the rule about which modules are admissible.
