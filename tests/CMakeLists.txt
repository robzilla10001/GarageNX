# tests/CMakeLists.txt
#
# Standalone HOST build for the off-device unit tests. Deliberately NOT pulled in
# by the root CMakeLists, for three reasons:
#   1. The root defaults to the Switch toolchain, so a test target added there
#      would cross-compile to aarch64 and could not be run by the developer.
#   2. Every harness carries its own main(), which would collide with
#      source/main.cpp in the GarageNX target.
#   3. The root's PLATFORM=PC branch requires SDL2 and an empty `stubs`
#      directory; these tests need neither.
#
# Configure and run separately, with the host compiler:
#   cmake -S tests -B build-tests
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
#
# Nothing here is linked into the app, and no third-party test framework is used
# - the coding standard bars new dependencies without an approved task, so the
# harnesses are plain C++17 with asserts.
#
# ADMISSION RULE: a module belongs here only if it is free of libnx. NczWindow
# and mtp_data are written that way deliberately. Anything touching usb:ds, ncm,
# es, or fs can only ever be exercised on hardware - do not stub those to get a
# green light here; a passing stub proves the stub works.

cmake_minimum_required(VERSION 3.16)
project(GarageNXTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GARAGENX_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../source")

find_package(Threads REQUIRED)

enable_testing()

# Each suite builds twice: ThreadSanitizer for races and deadlocks, then
# Address/UB for bounds and lifetime. The two sanitizers are mutually exclusive,
# so this is two targets rather than one. Concurrency bugs are scheduling-
# dependent, so a single clean run means little - CTest is configured to repeat.
function(add_garagenx_test name)
    set(sources ${ARGN})
    foreach(san tsan asan)
        set(tgt ${name}_${san})
        add_executable(${tgt} ${sources})
        target_include_directories(${tgt} PRIVATE ${GARAGENX_SOURCE})
        target_link_libraries(${tgt} PRIVATE Threads::Threads)
        target_compile_options(${tgt} PRIVATE -g -O1 -Wall -Wextra)
        if(san STREQUAL "tsan")
            target_compile_options(${tgt} PRIVATE -fsanitize=thread)
            target_link_options(${tgt} PRIVATE -fsanitize=thread)
        else()
            target_compile_options(${tgt} PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${tgt} PRIVATE -fsanitize=address,undefined)
        endif()
        add_test(NAME ${tgt} COMMAND ${tgt})
        set_tests_properties(${tgt} PROPERTIES TIMEOUT 300)
    endforeach()
endfunction()

# ─── Suites ───────────────────────────────────────────────────────────────────

# NczWindow (slice 4b). Uses only std::mutex/std::condition_variable, so it
# compiles against source/ directly with no stub layer.
add_garagenx_test(ncz_window_test
    ncz_window_test.cpp
    ${GARAGENX_SOURCE}/install/ncz_window.cpp
)

# MTP wire format (slice 4c). mtp_data.hpp is free of USB and libnx by design,
# so it links alone. mtp_server (the transport) is hardware-only and is NOT here.
add_garagenx_test(mtp_protocol_test
    mtp_protocol_test.cpp
    ${GARAGENX_SOURCE}/services/mtp_data.cpp
)

# StreamInstaller's container front-end. This links ncz.cpp, whose
# !PLATFORM_SWITCH branch (ncz.cpp:438) stubs the decompressor out - which is
# why this suite tests container PARSING only and asserts nothing about NCZ.
# The stub satisfies the linker; it must never be mistaken for a decompressor.
# NczWindow above is the genuinely portable piece and is tested for real.
add_garagenx_test(stream_container_test
    stream_container_test.cpp
    link_stubs.cpp
    ${GARAGENX_SOURCE}/install/stream_installer.cpp
    ${GARAGENX_SOURCE}/install/ncz_window.cpp
    ${GARAGENX_SOURCE}/install/ncz.cpp
)

# StreamInstaller's XCI/XCZ front-end (slice 4c). Same link set as above and the
# same NCZ caveat: an .xcz entry here reaches ncz.cpp's stub, so that suite
# asserts a refusal and nothing about decompression.
add_garagenx_test(xci_container_test
    xci_container_test.cpp
    link_stubs.cpp
    ${GARAGENX_SOURCE}/install/stream_installer.cpp
    ${GARAGENX_SOURCE}/install/ncz_window.cpp
    ${GARAGENX_SOURCE}/install/ncz.cpp
)

# OverlapBuffer. Its host branch is pure std::thread/std::mutex - no libnx - so
# it meets the admission rule. Added for the MTP cancel crash (2347-0018): the
# teardown-ordering race lives in how recv_install quiesces this worker before
# abort() frees the state its sink touches. The quiesce() fence is the fix, and
# trial 3 is the reproduction - run under TSan, an ordering regression there is a
# reported race, not a silent pass.
add_garagenx_test(overlap_buffer_test
    overlap_buffer_test.cpp
    ${GARAGENX_SOURCE}/services/overlap_buffer.cpp
)

# The MTP cancel crash root cause: MtpServer destroyed members (m_install) before
# the base ~NetworkService joined the worker thread - a cross-thread UAF. Pure
# C++/threading, no libnx, so it reproduces on the host. The fixed variant (a
# derived dtor that stops first) must be clean under TSan/ASan.
add_garagenx_test(teardown_order_test
    teardown_order_test.cpp
)

# Sub-frame tap counting behind the file-browser navigation fix. input.cpp needs
# SDL (not host-linkable), so this mirrors the counting core and proves the
# property: N taps in one frame = N presses, not 1 (the "7 presses -> 5 lines"
# drop). A logic mirror - keep in sync with input.cpp poll().
add_garagenx_test(input_press_count_test
    input_press_count_test.cpp
)

# Pure core of the rendered-text cache (the file-browser stutter fix). Key
# equality/hash + eviction policy, tested without SDL; the texture glue in
# renderer.cpp is hardware-verified.
add_garagenx_test(text_cache_test
    text_cache_test.cpp
)

# RateMeter data-phase average behind the MTP screen's average-speed/ETA rework.
# Driven with real short sleeps (no clock injection into a working class); proves
# first-byte anchoring and re-anchoring after a counter reset.
add_garagenx_test(rate_meter_test
    rate_meter_test.cpp
    ${GARAGENX_SOURCE}/services/rate_meter.cpp
)

# StreamDriver: the transport-agnostic install keystone. Driven on the host with a
# synthetic in-memory PFS0 (no USB/sockets) to prove feed sequencing, size
# recovery from the container table, cancel teardown, and short-read handling.
add_garagenx_test(stream_driver_test
    stream_driver_test.cpp
    link_stubs.cpp
    ${GARAGENX_SOURCE}/install/stream_driver.cpp
    ${GARAGENX_SOURCE}/install/stream_installer.cpp
    ${GARAGENX_SOURCE}/install/ncz_window.cpp
    ${GARAGENX_SOURCE}/install/ncz.cpp
    ${GARAGENX_SOURCE}/services/overlap_buffer.cpp
)

# FTP install-path classifier: pure logic deciding STOR-installs-vs-writes and
# where the virtual install folders appear. No sockets - fully host-tested.
add_garagenx_test(ftp_paths_test
    ftp_paths_test.cpp
)

# HTTP install-path classifier: URL-based analog of ftp_paths. Pure, decides
# install-vs-plain-write and the target, plus percent-decoding of filenames.
add_garagenx_test(http_paths_test
    http_paths_test.cpp
)

# Shared ETA formatting for the MTP/FTP/HTTP status screens. Was two identical
# file-local copies before HTTP needed a third; pure, so its boundary behaviour
# (rollover, NaN, absurd values) is tested rather than eyeballed mid-transfer.
add_garagenx_test(stats_format_test
    stats_format_test.cpp
)

# LINT, not a unit test: reads the real source and fails the build if a filesystem
# mutation in a save-capable file is not followed by a commit. A journalled save
# discards uncommitted writes at unmount, so the failure is silent data loss with
# no error anywhere - an invariant no unit test can reach, because it lives in the
# relationship between a call and its follow-up.
add_garagenx_test(save_commit_discipline_test
    save_commit_discipline_test.cpp
)
# The helper builds one target per sanitiser (tsan/asan), so the source path has
# to be defined on each of them rather than on a bare target name.
foreach(san tsan asan)
    target_compile_definitions(save_commit_discipline_test_${san}
        PRIVATE GNX_SOURCE_DIR="${GARAGENX_SOURCE}")
endforeach()

# StorageCatalog: the shared definition of every storage surface all three
# transports consume (anti-drift). Pure description + config gating + the NAND
# safety policy - no libnx, fully host-tested.
add_garagenx_test(storage_catalog_test
    storage_catalog_test.cpp
    ${GARAGENX_SOURCE}/services/storage_catalog.cpp
)

# ConfirmationBroker: the cross-thread NAND-confirm state machine. Tested with real
# threads (worker asks + blocks, main resolves) under both sanitizers.
add_garagenx_test(confirmation_broker_test
    confirmation_broker_test.cpp
    ${GARAGENX_SOURCE}/services/confirmation_broker.cpp
)

# StoragePaths: the shared catalog-driven path resolver (transport-neutral path ->
# concrete VFS path, gated by the catalog). Header-only logic; links the catalog.
add_garagenx_test(storage_paths_test
    storage_paths_test.cpp
    ${GARAGENX_SOURCE}/services/storage_catalog.cpp
)

# SaveSurface: the PURE half of the shared Save Data surface - the synthetic path
# form MTP interns handles under, and the no-mount classification. The listing and
# mount halves are libnx-only and stay on hardware, per the admission rule above.
# Config: the per-transport surfaces split and its MIGRATION from the old shared
# "mtp" block. Pulls in config.cpp (pure C++ + vendored nlohmann, no libnx).
# host_stubs/ provides a working SDL_Log so config.cpp (pure C++ apart from its
# logging) can join the host suite. See the header in that file.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/host_stubs)

add_garagenx_test(config_surfaces_test
    config_surfaces_test.cpp
    ${GARAGENX_SOURCE}/config/config.cpp
)

# Text wrapping: header-only algorithm, no SDL. Pins where line breaks land,
# including mid-token breaks for space-free file paths and UTF-8 boundaries -
# things a hardware round checks badly and a unit test settles exactly.
add_garagenx_test(text_wrap_test
    text_wrap_test.cpp
)

# Save backup DESTINATION NAMING: header-only, no libnx. Save folders are named
# after account nicknames and game titles, which contain characters FAT rejects -
# unsanitised, the copy fails on exactly the titles a user wants backed up.
add_garagenx_test(save_backup_test
    save_backup_test.cpp
)

add_garagenx_test(save_surface_test
    save_surface_test.cpp
    ${GARAGENX_SOURCE}/services/storage_catalog.cpp
)

add_garagenx_test(write_guard_test
    write_guard_test.cpp
    ${GARAGENX_SOURCE}/services/storage_catalog.cpp
    ${GARAGENX_SOURCE}/services/write_guard.cpp
    ${GARAGENX_SOURCE}/services/confirmation_broker.cpp
)

add_garagenx_test(title_naming_test
    title_naming_test.cpp
    ${GARAGENX_SOURCE}/services/title_naming.cpp
)

add_garagenx_test(pfs0_layout_test
    pfs0_layout_test.cpp
    ${GARAGENX_SOURCE}/services/pfs0_layout.cpp
)

# NetSurface: the PURE half of the shared Network (SMB/NFS) surface - the
# protocol/port model, the display-path decomposition, the synthetic prefix MTP
# will intern handles under, and the in-memory session-credential store. The
# connect/mount half is libsmb2/libnfs and stays on hardware, per the admission
# rule above. Header-only under test; links nothing.
add_garagenx_test(net_surface_test
    net_surface_test.cpp
)

# NspRepair: the PURE half of the Tools "repair tickets with dump errors" op -
# PFS0 container reading, .cnmt.nca location (nested PFS0), cnmt-record title-id
# extraction, and repair-state classification. The Switch half (ES ticket
# lookup, the re-dump) is hardware-only per the admission rule above.
add_garagenx_test(nsp_repair_test
    nsp_repair_test.cpp
    ${GARAGENX_SOURCE}/tools/nsp_repair.cpp
)
