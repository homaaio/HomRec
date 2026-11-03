# cmake/LinuxPort.cmake - the non-Windows (Linux) half of the build.
#
# The root CMakeLists.txt includes this file and stops when it is not building for Windows, so the
# Windows target list (hr / hom / homrec) is never touched by anything in here.
#
# What it builds today:
#   hr_platform     static library: src/platform/linux/* (session detection, XDG paths, backend stubs)
#   hr_core         static library: the Windows-era engine files that already compile on Linux
#                   (.hrc read/write, settings table, app logic, input overlay, ls command ...)
#   homrec    the raw Linux CLI (doctor / paths / start)
#   test_*          the platform-independent doctest suites from scripts/tests/ (run with `ctest`)
#
# What it will build later is listed in docs/LINUX_PORT_PLAN.md - new backends are added to
# HR_PLATFORM_SOURCES below (and their library to hr_platform's link line), nothing else moves.

option(HOMREC_BUILD_TESTS "Build the platform-independent unit tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(HR_PLATFORM_SOURCES
    src/platform/linux/session_detect.cpp
    src/platform/linux/paths_linux.cpp
    src/platform/linux/backend_stubs.cpp
    # phase 2: src/platform/linux/x11/x11_capture.cpp  src/platform/linux/x11/x11_windows.cpp ...
    # phase 3: src/platform/linux/wayland/portal_capture.cpp ...
)

# Same optimisation set as the Windows targets: -O3, per-function sections + --gc-sections, LTO when the
# toolchain supports it (turn off with -DHR_LTO=OFF).
option(HR_LTO "Link-time optimisation" ON)
set(HR_OPT_LINUX -O3 -ffunction-sections -fdata-sections)
include(CheckIPOSupported)
check_ipo_supported(RESULT HR_IPO_OK OUTPUT HR_IPO_MSG LANGUAGES C CXX)

add_library(hr_platform STATIC ${HR_PLATFORM_SOURCES})
target_include_directories(hr_platform PUBLIC src src/platform)
target_compile_options(hr_platform PRIVATE -Wall -Wextra ${HR_OPT_LINUX})

# Files from the original (Windows) tree that need no change to build here. Move more in as they
# get fixed - see "Moved to Linux" in docs/LINUX_PORT_PLAN.md.
set(HR_CORE_SOURCES
    src/core/profile_io.cpp
    src/core/settings_registry.cpp
    src/core/app_logic.cpp
    src/core/input_overlay.cpp
    src/core/input_overlay_registry.cpp
    src/commands/ls.cpp
    src/utils/system_integration.cpp
    src/utils/ui_utils.cpp
)
find_package(ZLIB REQUIRED)   # profile_io.cpp (.hrc compression)
add_library(hr_core STATIC ${HR_CORE_SOURCES})
target_include_directories(hr_core PUBLIC src src/core src/utils src/ui)
target_link_libraries(hr_core PUBLIC ZLIB::ZLIB)
target_compile_options(hr_core PRIVATE -Wall ${HR_OPT_LINUX})

add_executable(homrec src/platform/linux/main.cpp)
target_link_libraries(homrec PRIVATE hr_platform)
target_compile_options(homrec PRIVATE -Wall -Wextra ${HR_OPT_LINUX})
target_link_options(homrec PRIVATE -Wl,--gc-sections)
if(HR_LTO AND HR_IPO_OK)
    set_property(TARGET homrec hr_platform hr_core PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ---- tests + bench ----------------------------------------------------------------------------
if(HOMREC_BUILD_TESTS)
    include(${CMAKE_CURRENT_LIST_DIR}/Tests.cmake)
endif()

message(STATUS "HomRec: Linux build -> homrec (+ tests: ${HOMREC_BUILD_TESTS}). hr.exe / hom / homrec are Windows-only.")
