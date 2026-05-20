cmake_minimum_required(VERSION 3.14)
project(homrec_engine CXX)

set(CMAKE_CXX_STANDARD 17)

# Release build — max speed, small binary
if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /GL /GS-")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native")
endif()

add_library(homrec_engine SHARED
    src/capture_engine.cpp
)

target_include_directories(homrec_engine PRIVATE src)

if(WIN32)
    target_link_libraries(homrec_engine PRIVATE
        gdi32 user32 winmm ole32
    )
    # Static runtime so DLL works without MSVC/MinGW runtime installed
    if(MINGW)
        target_link_options(homrec_engine PRIVATE
            -static-libgcc -static-libstdc++ -static -lpthread)
    endif()
endif()

# Copy DLL next to homrec.py after build
add_custom_command(TARGET homrec_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:homrec_engine>
        ${CMAKE_SOURCE_DIR}/homrec_engine.dll
    COMMENT "Copied homrec_engine.dll to project root"
)
