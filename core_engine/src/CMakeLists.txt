cmake_minimum_required(VERSION 3.18)
project(homrec_core VERSION 2.1.0 LANGUAGES CXX C)

# -- Compiler options ----------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    # /O2 = maximize speed, /arch:AVX2 for SIMD in color_convert.c
    add_compile_options(/O2 /arch:AVX2 /W3 /wd4100 /wd4267)
    # Suppress C4200 (zero-length array) and C4996 (deprecated POSIX names)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-O3 -march=native -Wall -Wextra -Wno-unused-parameter)
endif()

# -- Dependencies --------------------------------------------------------------

# Python (for pybind11)
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

# pybind11 — either installed or in third_party/
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
    add_subdirectory(third_party/pybind11 EXCLUDE_FROM_ALL)
endif()

# FFmpeg — locate headers and libraries
# Set FFMPEG_ROOT or put it on CMAKE_PREFIX_PATH
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
          PATHS ${FFMPEG_ROOT}/include ENV FFMPEG_ROOT
          REQUIRED)
find_library(AVCODEC_LIB  avcodec  PATHS ${FFMPEG_ROOT}/lib ENV FFMPEG_ROOT REQUIRED)
find_library(AVFORMAT_LIB avformat PATHS ${FFMPEG_ROOT}/lib ENV FFMPEG_ROOT REQUIRED)
find_library(AVUTIL_LIB   avutil   PATHS ${FFMPEG_ROOT}/lib ENV FFMPEG_ROOT REQUIRED)
find_library(SWSCALE_LIB  swscale  PATHS ${FFMPEG_ROOT}/lib ENV FFMPEG_ROOT REQUIRED)

# -- Source lists --------------------------------------------------------------

set(CORE_CXX_SOURCES
    engine.cpp
    capture.cpp
    encoder.cpp
    gpu_scaler.cpp
    monitor_enum.cpp
    bindings.cpp
)

set(CORE_C_SOURCES
    color_convert.c
    audio_mixer.c
)

set(CORE_HEADERS
    engine.hpp
    capture.hpp
    encoder.hpp
    ring_buffer.hpp
    frame_pool.hpp
    gpu_scaler.hpp
    monitor_enum.hpp
    color_convert.h
    audio_mixer.h
    perf_timer.h
)

# -- pybind11 extension module -------------------------------------------------
pybind11_add_module(homrec_core MODULE
    ${CORE_CXX_SOURCES}
    ${CORE_C_SOURCES}
)

target_include_directories(homrec_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(homrec_core PRIVATE
    ${AVCODEC_LIB}
    ${AVFORMAT_LIB}
    ${AVUTIL_LIB}
    ${SWSCALE_LIB}
    d3d11
    dxgi
    d3dcompiler
)

# -- Install -------------------------------------------------------------------
# Places homrec_core.pyd next to main.py so Python can import it directly.
install(TARGETS homrec_core
        LIBRARY DESTINATION ${CMAKE_SOURCE_DIR}/..
        RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/..)

# -- Optional: standalone test executable -------------------------------------
option(BUILD_TESTS "Build homrec_core C++ unit tests" OFF)
if(BUILD_TESTS)
    add_executable(test_color_convert tests/test_color_convert.c color_convert.c)
    target_include_directories(test_color_convert PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(test_audio_mixer tests/test_audio_mixer.c audio_mixer.c)
    target_include_directories(test_audio_mixer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_audio_mixer PRIVATE m)

    enable_testing()
    add_test(NAME ColorConvert COMMAND test_color_convert)
    add_test(NAME AudioMixer   COMMAND test_audio_mixer)
endif()

# -- Print summary -------------------------------------------------------------
message(STATUS "HomRec core ${PROJECT_VERSION}")
message(STATUS "  Python:  ${Python3_VERSION} at ${Python3_EXECUTABLE}")
message(STATUS "  FFmpeg:  ${FFMPEG_INCLUDE_DIR}")
message(STATUS "  pybind11: found")
