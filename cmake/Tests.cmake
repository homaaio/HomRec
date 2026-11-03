# cmake/Tests.cmake - the platform-independent unit tests (scripts/tests) and the bench target.
# Included by the Windows build (CMakeLists.txt, -DHOMREC_BUILD_TESTS=ON) and by cmake/LinuxPort.cmake.
#
#   cmake --build build                      builds the tests
#   ctest --test-dir build --output-on-failure
#   cmake --build build --target bench       builds bench(.exe); run it and save the output:
#       build/bench > before.txt     ...change code...     build/bench > after.txt
#       python tools/bench_compare.py before.txt after.txt     (or: python tools/bench_compare.py after.txt)

enable_testing()

# encoder_helpers.c is plain C (BGRA <-> YUV/NV12 converters, blend, thumbnail; SSE4.1 / AVX2 picked at run time).
add_library(hr_encoder_helpers STATIC ${CMAKE_SOURCE_DIR}/src/core/encoder_helpers.c)
target_include_directories(hr_encoder_helpers PUBLIC ${CMAKE_SOURCE_DIR}/src/core)
target_compile_options(hr_encoder_helpers PRIVATE -O3 -Wall)

foreach(t wav_util audio_mixer overlay_canvas replay_ring pic_pool encoder_helpers settings_registry)
    add_executable(test_${t} ${CMAKE_SOURCE_DIR}/scripts/tests/test_${t}.cpp)
    target_include_directories(test_${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/core ${CMAKE_SOURCE_DIR}/src/utils ${CMAKE_SOURCE_DIR}/scripts/tests)
    target_compile_options(test_${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
target_link_libraries(test_encoder_helpers PRIVATE hr_encoder_helpers)
target_sources(test_settings_registry PRIVATE ${CMAKE_SOURCE_DIR}/src/core/settings_registry.cpp)

# CPU hot-spot timings ("[bench] name ms" + "[pair] label old new"). Not part of `all`: build it when you want it.
add_executable(bench EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/scripts/tests/bench.cpp)
target_include_directories(bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(bench PRIVATE -O3)
target_link_libraries(bench PRIVATE hr_encoder_helpers)
