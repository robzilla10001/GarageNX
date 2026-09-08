# toolchain-switch.cmake
# devkitPro Nintendo Switch toolchain for CMake.
# This file is automatically picked up by CMakeLists.txt when PLATFORM=Switch.

set(DEVKITPRO $ENV{DEVKITPRO})
if(NOT DEVKITPRO)
    set(DEVKITPRO "/opt/devkitpro")
endif()

set(DEVKITARM "${DEVKITPRO}/devkitARM")  # kept for reference; Switch uses A64
set(DEVKITA64 "${DEVKITPRO}/devkitA64")

set(CMAKE_SYSTEM_NAME      "Generic")
set(CMAKE_SYSTEM_PROCESSOR "aarch64")

set(CMAKE_C_COMPILER   "${DEVKITA64}/bin/aarch64-none-elf-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITA64}/bin/aarch64-none-elf-g++")
set(CMAKE_AR           "${DEVKITA64}/bin/aarch64-none-elf-ar")
set(CMAKE_RANLIB       "${DEVKITA64}/bin/aarch64-none-elf-ranlib")
set(CMAKE_STRIP        "${DEVKITA64}/bin/aarch64-none-elf-strip")

# Tell CMake not to try to run test executables on the host
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Switch-specific compile flags (mirrors what devkitPro Makefiles set)
set(ARCH_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE")

set(COMMON_FLAGS
    "${ARCH_FLAGS}"
    "-ffunction-sections"
    "-fdata-sections"
    "-D__SWITCH__"
)

string(JOIN " " COMMON_FLAGS_STR ${COMMON_FLAGS})

set(CMAKE_C_FLAGS_INIT   "${COMMON_FLAGS_STR}")
set(CMAKE_CXX_FLAGS_INIT "${COMMON_FLAGS_STR} -fno-rtti")

# Linker flags — spec file handles the Switch-specific CRT setup
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-specs=${DEVKITPRO}/libnx/switch.specs ${ARCH_FLAGS} -Wl,-Map,GarageNX.map"
)

# pkg-config must point at the Switch portlibs, not the host system
set(ENV{PKG_CONFIG_PATH}        "${DEVKITPRO}/portlibs/switch/lib/pkgconfig")
set(ENV{PKG_CONFIG_LIBDIR}      "${DEVKITPRO}/portlibs/switch/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${DEVKITPRO}/portlibs/switch")

# Prevent CMake from searching host paths for libraries
set(CMAKE_FIND_ROOT_PATH
    "${DEVKITA64}"
    "${DEVKITPRO}/libnx"
    "${DEVKITPRO}/portlibs/switch"
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
