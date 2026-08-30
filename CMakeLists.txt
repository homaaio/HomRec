cmake_minimum_required(VERSION 3.15)
project(HomRec LANGUAGES C CXX)

# This file is a 1:1 port of the existing ./Makefile - same source list,
# same compiler flags, same libraries. It does NOT try to "improve" or
# reorganize anything. If you later add/remove a .cpp file, add/remove it
# in BOTH this file and Makefile until you decide to retire one of them.

# ---------------------------------------------------------------------------
# Paths that were hardcoded in the Makefile (LUA_CFLAGS/LUA_LDFLAGS, wx
# include/lib dirs). Kept as cache variables here so you can override them
# on the command line instead of editing the file, e.g.:
#   cmake -B build -G "MinGW Makefiles" -DLUA_INCLUDE_DIR=D:/lua54/include
# ---------------------------------------------------------------------------
set(LUA_INCLUDE_DIR "C:/lua54/include" CACHE PATH "Lua headers")
set(LUA_LIB_DIR     "C:/lua54/lib"     CACHE PATH "Lua import lib dir")
set(WX_INCLUDE_DIR  "C:/msys64/mingw64/lib/wx/include/msw-unicode-3.2" CACHE PATH "wx build-specific headers")
set(WX_INCLUDE_DIR2 "C:/msys64/mingw64/include/wx-3.2" CACHE PATH "wx headers")
set(WX_LIB_DIR      "C:/msys64/mingw64/lib" CACHE PATH "wx/mingw64 lib dir")

enable_language(RC)

# ---------------------------------------------------------------------------
# hr.exe - the main app. Source list copied verbatim from Makefile's $(OBJS),
# just with .o -> .cpp/.c and stripping the src/ prefix duplication.
# ---------------------------------------------------------------------------
set(HR_SOURCES
    src/win_main.cpp
    src/ui/main_frame.cpp
    src/ui/themed_widgets.cpp
    src/ui/theme.cpp
    src/ui/language.cpp
    src/ui/recording_controller.cpp
    src/ui/audio_panel.cpp
    src/ui/settings_dialog.cpp
    src/ui/hrc_config.cpp
    src/ui/win32_theme.cpp
    src/ui/overlay_add_dialogs.cpp
    src/ui/welcome_dialog.cpp
    src/ui/custom_messagebox.cpp
    src/ui/console_window.cpp
    src/ui/pc_analytics_dialog.cpp
    src/ui/log_viewer_dialog.cpp
    src/ui/window_picker_dialog.cpp
    src/ui/hide_window_dialog.cpp
    src/ui/overlays_dock_panel.cpp
    src/ui/overlay_placement_dialog.cpp
    src/hr_log.cpp
    src/hr_log_paths.cpp
    src/hr_plugin_log.cpp
    src/hr_pc_log.cpp
    src/hr_system_integration.cpp
    src/hr_crash_handler.cpp
    src/hr_archive.cpp
    src/hr_input_overlay.cpp
    src/hr_input_overlay_registry.cpp
    src/plugins/lua_engine.cpp
    src/plugins/lua_api.cpp
    src/hr_display_info.cpp
    src/hr_profile_io.cpp
    src/hr_app_logic.cpp
    src/hr_capture_ctl.cpp
    src/hr_pipeline.cpp
    src/hr_overlay_render.cpp
    src/hr_webcam_enum.cpp
    src/hr_ffmpeg_runner.cpp
    src/hr_tools.cpp
    src/hr_ui_utils.cpp
    src/hr_audio.cpp
    src/hr_dxgi_capture.cpp
    src/hr_stopwatch.cpp
    src/hr_settings.cpp
    src/hr_hotkey.cpp
    src/hr_mic_enum.cpp
    src/hr_settings_registry.cpp
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
    -O2 -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-municode>
)

target_compile_definitions(hr PRIVATE
    UNICODE _UNICODE
    _WIN32_WINNT=0x0601
    WXUSINGDLL
    __WXMSW__
)

target_link_directories(hr PRIVATE ${WX_LIB_DIR} ${LUA_LIB_DIR})

target_link_options(hr PRIVATE
    -municode -mwindows -static-libgcc -static-libstdc++
    -Wl,-Bstatic,-lpthread,-Bdynamic
)

target_link_libraries(hr PRIVATE
    comctl32 comdlg32 gdi32 shell32 user32 psapi wininet
    d3d11 dxgi pdh winmm ole32 uuid lua dwmapi uxtheme
    windowscodecs strmiids oleaut32
    wx_mswu_xrc-3.2 wx_mswu_html-3.2 wx_mswu_qa-3.2
    wx_mswu_core-3.2 wx_baseu_xml-3.2 wx_baseu_net-3.2 wx_baseu-3.2
)

# ---------------------------------------------------------------------------
# hom.exe - standalone plugin-manager CLI tool. Deliberately NOT municode
# (plain `main`, not `wWinMain`) and does NOT link wx/Lua - see the comment
# in the original Makefile explaining why it has its own flag set.
# ---------------------------------------------------------------------------
add_executable(hom tools/hom/hom.cpp)

target_compile_features(hom PRIVATE cxx_std_17)

target_compile_options(hom PRIVATE -O2 -Wall -Wextra -static-libgcc -static-libstdc++)

target_compile_definitions(hom PRIVATE UNICODE _UNICODE)

target_link_options(hom PRIVATE -static-libgcc -static-libstdc++)

target_link_libraries(hom PRIVATE winhttp shlwapi dbghelp)
