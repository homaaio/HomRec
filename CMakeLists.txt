cmake_minimum_required(VERSION 3.15)
project(HomRec LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Anything that is not Windows (i.e. Linux) is built by cmake/LinuxPort.cmake and stops here:
# the rest of this file is the Windows build (hr.exe / homrec.exe) and needs windres, wxMSW, the
# Windows SDK import libraries ... none of which exist elsewhere. See docs/LINUX_PORT_PLAN.md.
if(NOT WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LinuxPort.cmake)
    return()
endif()

# The Windows build (CMake is the only build system now - the old Makefile is gone).
#
#   cmake -S . -B build -G "MinGW Makefiles"            (MSYS2 UCRT64 shell)
#   cmake --build build -j                              -> hr.exe and homrec.exe in the repo root
#   cmake --build build --target hr                     just hr.exe (also: homrec)
#   cmake -S . -B build -DHOMREC_BUILD_TESTS=ON         + unit tests (ctest --test-dir build) and the bench target
#
# When you add or remove a source file, update the lists below (HR_SOURCES / homrec).

# Where the build looks for libraries. Cache variables, so you can override them
# on the command line instead of editing the file, e.g.:
#   cmake -S . -B build -DLUA_INCLUDE_DIR=D:/lua/include -DLUA_LIB_DIR=D:/lua/lib
set(LUA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/lua54/include" CACHE PATH "Lua headers")
set(LUA_LIB_DIR     "C:/lua54/lib"     CACHE PATH "Lua import lib dir")
# wxWidgets and the C++ runtime must come from the SAME MSYS2 environment as the compiler.
# Mixing .../mingw64 (MSVCRT) with .../ucrt64 (UCRT) headers and import libraries produces
# link errors like undefined `__imp__snprintf_s`, `vswprintf_s` or `std::istream::seekg(fpos)`.
# Derive the prefix from the compiler CMake picked, so C:/msys64/ucrt64/bin/c++.exe uses
# C:/msys64/ucrt64 (the environment the shipped DLLs and C:/lua54 are built for).
set(HOMREC_MSYS_PREFIX "C:/msys64/ucrt64")
if(CMAKE_CXX_COMPILER MATCHES "^(.*)/bin/c\\+\\+\\.exe$")
    set(HOMREC_MSYS_PREFIX "${CMAKE_MATCH_1}")
endif()
set(WX_INCLUDE_DIR  "${HOMREC_MSYS_PREFIX}/lib/wx/include/msw-unicode-3.2" CACHE PATH "wx build-specific headers")
set(WX_INCLUDE_DIR2 "${HOMREC_MSYS_PREFIX}/include/wx-3.2" CACHE PATH "wx headers")
set(WX_LIB_DIR      "${HOMREC_MSYS_PREFIX}/lib" CACHE PATH "wx + MinGW runtime lib dir")
if(WX_INCLUDE_DIR AND NOT WX_INCLUDE_DIR MATCHES "^${HOMREC_MSYS_PREFIX}/")
    message(WARNING
        "WX_INCLUDE_DIR=${WX_INCLUDE_DIR} is outside the compiler's MSYS2 prefix "
        "(${HOMREC_MSYS_PREFIX}). Mixing mingw64 and ucrt64 normally fails to link - "
        "reconfigure with --fresh (and without old -DWX_* values) to let the defaults follow the compiler.")
endif()
if(NOT EXISTS "${WX_INCLUDE_DIR2}/wx/wx.h")
    message(WARNING
        "wxWidgets headers not found at ${WX_INCLUDE_DIR2}. The shipped DLLs and C:/lua54 are UCRT64: "
        "build from the MSYS2 UCRT64 shell with mingw-w64-ucrt-x86_64-wxwidgets3.2-msw installed, "
        "or point -DWX_INCLUDE_DIR/-DWX_INCLUDE_DIR2/-DWX_LIB_DIR at a wxWidgets built the same way as the compiler.")
endif()

enable_language(RC)

# hr.exe / homrec.exe land in the repo root, like they always did: installer/HomRec.iss and
# tools/homrec_build.py pick them up from there (hom.exe is not built here any more: it comes from
# github.com/homaaio/hom/releases - drop it in the repo root to bundle it). -DHR_EXE_IN_ROOT=OFF keeps them in the build folder.
option(HR_EXE_IN_ROOT "Put the .exe files in the source folder instead of the build folder" ON)
if(HR_EXE_IN_ROOT)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# Shared optimisation flags (hr + homrec). Turn LTO off with -DHR_LTO=OFF if the link gets slow/fails.
option(HR_LTO "Link-time optimisation" ON)
set(HR_OPT -O3 -ffunction-sections -fdata-sections)
set(HR_OPT_LINK -Wl,--gc-sections)
if(HR_LTO)
    list(APPEND HR_OPT -flto=auto)
    list(APPEND HR_OPT_LINK -flto=auto)
endif()

# hr.exe - the main app.
set(HR_SOURCES
    src/win_main.cpp
    src/commands/ls.cpp
    src/ui/main_frame.cpp
    src/ui/themed_widgets.cpp
    src/ui/theme.cpp
    src/ui/language.cpp
    src/ui/recording_controller.cpp
    src/ui/audio_panel.cpp
    src/ui/settings_dialog.cpp
    src/ui/preset_dialog.cpp
    src/ui/hrc_config.cpp
    src/ui/win32_theme.cpp
    src/ui/overlay_add_dialogs.cpp
    src/ui/welcome_dialog.cpp
    src/ui/custom_messagebox.cpp
    src/ui/console_window.cpp
    src/ui/pc_analytics_dialog.cpp
    src/ui/log_viewer_dialog.cpp
    src/ui/window_picker_dialog.cpp
    src/ui/capture_source_dialog.cpp
    src/ui/hide_window_dialog.cpp
    src/ui/overlays_dock_panel.cpp
    src/ui/scenes_panel.cpp
    src/ui/overlay_editor_dialog.cpp
    src/utils/log.cpp
    src/utils/log_paths.cpp
    src/utils/plugin_log.cpp
    src/utils/pc_log.cpp
    src/utils/wgc_log.cpp
    src/utils/system_integration.cpp
    src/utils/crash_handler.cpp
    src/utils/archive.cpp
    src/core/input_overlay.cpp
    src/core/input_overlay_registry.cpp
    src/plugins/lua_engine.cpp
    src/plugins/lua_api.cpp
    src/core/display_info.cpp
    src/core/profile_io.cpp
    src/core/app_logic.cpp
    src/core/capture_ctl.cpp
    src/core/pipeline.cpp
    src/core/overlay_render.cpp
    src/core/webcam_enum.cpp
    src/core/webcam_capture.cpp
    src/core/encoder_helpers.c
    ffmpeg/ffmpeg_runner.cpp
    src/core/tools.cpp
    src/utils/ui_utils.cpp
    src/core/audio.cpp
    src/core/audio_pipe.cpp
    src/core/overlay_feed.cpp
    src/core/dxgi_capture.cpp
    src/core/gpu_overlay.cpp
    src/core/mf_encoder.cpp
    src/core/wgc_capture.cpp
    src/utils/stopwatch.cpp
    src/core/settings.cpp
    src/core/hotkey.cpp
    src/core/mic_enum.cpp
    src/core/app_audio_enum.cpp
    src/ui/icons.cpp
    src/core/settings_registry.cpp
    src/utils/update.cpp
    resource.rc
)

add_executable(hr WIN32 ${HR_SOURCES})
set_target_properties(hr PROPERTIES OUTPUT_NAME "hr")

target_include_directories(hr PRIVATE
    src
    ${WX_INCLUDE_DIR}
    ${WX_INCLUDE_DIR2}
    ${LUA_INCLUDE_DIR}
)

target_compile_features(hr PRIVATE cxx_std_17)

target_compile_options(hr PRIVATE
    ${HR_OPT} -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-municode>
)

target_compile_definitions(hr PRIVATE
    UNICODE _UNICODE
    _WIN32_WINNT=0x0601
    WXUSINGDLL
    __WXMSW__
)

target_link_directories(hr PRIVATE ${WX_LIB_DIR} ${LUA_LIB_DIR})

# hr.map: linker map. Keep it next to every released hr.exe - it turns a crash offset into a function name
# (see src/utils/crash_handler.cpp).
target_link_options(hr PRIVATE
    -mwindows -static-libgcc -static-libstdc++
    -Wl,-Bstatic,-lpthread,-Bdynamic ${HR_OPT_LINK}
    -Wl,-Map=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/hr.map
)

# hr.exe may still be running (the app lives in the tray) and Windows refuses to overwrite a running exe:
# ld then exits with "returned 5" (ERROR_ACCESS_DENIED). A running exe can be renamed, so move it aside first.
add_custom_command(TARGET hr PRE_LINK
    COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:hr> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/free_exe.cmake
    VERBATIM)

target_link_libraries(hr PRIVATE
    comctl32 comdlg32 gdi32 shell32 user32 psapi wininet winhttp
    d3d11 dxgi pdh winmm ole32 uuid lua dwmapi uxtheme
    windowscodecs strmiids oleaut32
    mfplat mfreadwrite mf mfuuid
    wx_mswu_aui-3.2 wx_mswu_xrc-3.2 wx_mswu_html-3.2 wx_mswu_qa-3.2
    wx_mswu_core-3.2 wx_baseu_xml-3.2 wx_baseu_net-3.2 wx_baseu-3.2
)

# homrec.exe - standalone command-line interface (tools/homrec). No wx/Lua, and a wide wmain(), so
# it IS built with -municode. 'homrec start' records with the same RecordingController as hr.exe,
# so the wx-free engine sources are compiled into it too (the list below).
add_executable(homrec
    tools/homrec/homrec.cpp
    ffmpeg/ffmpeg_runner.cpp
    src/core/audio.cpp
    src/core/audio_pipe.cpp
    src/core/capture_ctl.cpp
    src/core/display_info.cpp
    src/core/dxgi_capture.cpp
    src/core/encoder_helpers.c
    src/core/gpu_overlay.cpp
    src/core/input_overlay.cpp
    src/core/mf_encoder.cpp
    src/core/overlay_feed.cpp
    src/core/overlay_render.cpp
    src/core/pipeline.cpp
    src/core/settings_registry.cpp
    src/core/tools.cpp
    src/core/webcam_capture.cpp
    src/core/webcam_enum.cpp
    src/core/wgc_capture.cpp
    src/ui/hrc_config.cpp
    src/ui/recording_controller.cpp
    src/ui/win32_theme.cpp
    src/ui/window_picker_dialog.cpp
    src/utils/log.cpp
    src/utils/log_paths.cpp
    src/utils/stopwatch.cpp
    src/utils/ui_utils.cpp
    src/utils/wgc_log.cpp
)

target_include_directories(homrec PRIVATE src)

target_compile_features(homrec PRIVATE cxx_std_17)

target_compile_options(homrec PRIVATE ${HR_OPT} -Wall -Wextra -municode -static-libgcc -static-libstdc++)

target_compile_definitions(homrec PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601)

target_link_options(homrec PRIVATE -municode -static-libgcc -static-libstdc++ ${HR_OPT_LINK})

target_link_libraries(homrec PRIVATE
    comctl32 comdlg32 gdi32 shell32 user32 psapi wininet winhttp d3d11 dxgi pdh
    winmm ole32 uuid dwmapi uxtheme windowscodecs strmiids oleaut32
    mfplat mfreadwrite mf mfuuid)

# ---- unit tests + benchmark (off by default on Windows; ON on Linux) --------------------------------------
option(HOMREC_BUILD_TESTS "Build the unit tests (scripts/tests) and the bench target" OFF)
if(HOMREC_BUILD_TESTS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Tests.cmake)
endif()
