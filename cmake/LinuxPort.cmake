# cmake/LinuxPort.cmake - the non-Windows (Linux) half of the build.
#
# The root CMakeLists.txt includes this file and stops when it is not building for Windows, so the
# Windows target list (hr / homrec) is never touched by anything in here.
#
# What it builds today:
#   hr_platform     static library: src/platform/linux/* (session detection, XDG paths, backend stubs)
#   hr_core         static library: the Windows-era engine files that already compile on Linux
#                   (.hrc read/write, settings table, app logic, input overlay, ls command ...)
#   homrec          the raw Linux CLI (doctor / paths / start)
#   hr              the GUI (wxGTK): main window, Settings and the windows that hang off it. Recording,
#                   preview, audio mixer and overlays are "not now" placeholders until their backends land.
#                   Needs libwxgtk3.2-dev; skipped with a message when wxWidgets is not found.
#   test_*          the platform-independent doctest suites from scripts/tests/ (run with `ctest`)
#
# What it will build later is listed in docs/LINUX_PORT_PLAN.md - new backends are added to
# HR_PLATFORM_SOURCES below (and their library to hr_platform's link line), nothing else moves.

option(HOMREC_BUILD_TESTS "Build the platform-independent unit tests" ON)
option(HR_GUI "Build the wxGTK GUI (hr) when wxWidgets is found" ON)

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

# ---- GUI (hr) ---------------------------------------------------------------------------------
# The shared wx files from src/ui/ (they build on both systems) + the Linux-only ones in src/ui/linux/.
set(HR_GUI_BUILT OFF)
if(HR_GUI)
    find_package(wxWidgets 3.2 COMPONENTS core base aui adv QUIET)
    if(wxWidgets_FOUND)
        include(${wxWidgets_USE_FILE})
        set(HR_GUI_SOURCES
            # Linux-only: app, main window, the small windows that hang off it
            src/ui/linux/app.cpp
            src/ui/linux/main_frame.cpp
            src/ui/linux/about.cpp
            src/ui/linux/welcome.cpp
            src/ui/linux/pc_info.cpp
            src/ui/linux/log_view.cpp
            src/ui/linux/console.cpp
            src/ui/linux/stubs.cpp
            # shared with Windows (ifdef'd where the systems differ)
            src/ui/settings_dialog.cpp
            src/ui/preset_dialog.cpp
            src/ui/hrc_config.cpp
            src/ui/language.cpp
            src/ui/theme.cpp
            src/ui/themed_widgets.cpp
            src/core/settings.cpp
            src/core/display_info.cpp
            # Linux versions of the engine helpers the windows above call
            src/platform/linux/log_linux.cpp
            src/platform/linux/mic_linux.cpp
        )
        add_executable(hr ${HR_GUI_SOURCES})
        target_include_directories(hr PRIVATE src src/ui src/core src/utils)
        target_link_libraries(hr PRIVATE hr_core hr_platform ${wxWidgets_LIBRARIES} ZLIB::ZLIB)
        target_compile_options(hr PRIVATE -Wall ${HR_OPT_LINUX})
        target_link_options(hr PRIVATE -Wl,--gc-sections)
        if(HR_LTO AND HR_IPO_OK)
            set_property(TARGET hr PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
        endif()
        set(HR_GUI_BUILT ON)
    else()
        message(STATUS "HomRec: wxWidgets 3.2 not found - skipping the GUI (hr). "
                       "Debian/Ubuntu: sudo apt install libwxgtk3.2-dev (scripts/linux/install-deps-debian.sh)")
    endif()
endif()

# ---- tests + bench ----------------------------------------------------------------------------
if(HOMREC_BUILD_TESTS)
    include(${CMAKE_CURRENT_LIST_DIR}/Tests.cmake)
endif()

message(STATUS "HomRec: Linux build -> homrec (+ GUI hr: ${HR_GUI_BUILT}, tests: ${HOMREC_BUILD_TESTS}). hr.exe / hom are Windows-only.")
