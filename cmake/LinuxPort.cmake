# cmake/LinuxPort.cmake - the non-Windows (Linux) half of the build.
#
# The root CMakeLists.txt includes this file and stops when it is not building for Windows, so the
# Windows target list (hr / hom / homrec) is never touched by anything in here.
#
# What it builds today:
#   hr_platform     static library: src/platform/linux/* (session detection, XDG paths, backend stubs)
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

add_library(hr_platform STATIC ${HR_PLATFORM_SOURCES})
target_include_directories(hr_platform PUBLIC src src/platform)
target_compile_options(hr_platform PRIVATE -Wall -Wextra)

add_executable(homrec src/platform/linux/main.cpp)
target_link_libraries(homrec PRIVATE hr_platform)
target_compile_options(homrec PRIVATE -Wall -Wextra)

# ---- tests -------------------------------------------------------------------------------------
if(HOMREC_BUILD_TESTS)
    enable_testing()

    # encoder_helpers.c is plain C (BGRA <-> YUV/NV12 converters, SSE2/SSE4.1 where available).
    add_library(hr_encoder_helpers STATIC src/core/encoder_helpers.c)
    target_include_directories(hr_encoder_helpers PUBLIC src/core)
    target_compile_options(hr_encoder_helpers PRIVATE -O2 -Wall)

    foreach(t wav_util audio_mixer overlay_canvas replay_ring encoder_helpers)
        add_executable(test_${t} scripts/tests/test_${t}.cpp)
        target_include_directories(test_${t} PRIVATE src src/core src/utils scripts/tests)
        target_compile_options(test_${t} PRIVATE -Wall -Wextra)
        add_test(NAME ${t} COMMAND test_${t})
    endforeach()
    target_link_libraries(test_encoder_helpers PRIVATE hr_encoder_helpers)
endif()

message(STATUS "HomRec: Linux build -> homrec (+ tests: ${HOMREC_BUILD_TESTS}). hr.exe / hom / homrec are Windows-only.")
