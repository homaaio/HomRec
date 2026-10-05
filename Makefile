# `make LUA_CFLAGS="-IC:/lua54/include" LUA_LDFLAGS="-LC:/lua54/lib"` (C:\lua54 folder) or your lua54 path. download from lua.org

CXX := g++
CC  := cc

LUA_CFLAGS  ?= -IC:/lua54/include
LUA_LDFLAGS ?= -LC:/lua54/lib

# hom (tools/hom/hom.cpp) is a separate standalone tool (the HomRec
# plugin package manager) - NOT linked into hr.exe, doesn't need
# wxWidgets or Lua, and its main() is a plain narrow int main(argc, argv)
# rather than hr.exe's wWinMain/wmain. Because of that last point it must
# NOT get -municode: with -municode the linker looks for a wide entry
# point (wWinMainCRTStartup/wmainCRTStartup) that a plain main() doesn't
# provide, and the link fails. So hom gets its own flag variables instead
# of reusing CXXFLAGS/LDFLAGS above.
HOM_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -static-libgcc -static-libstdc++
HOM_LDLIBS   := -lwinhttp -lshlwapi -ldbghelp

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 \
            -Isrc -IC:/msys64/mingw64/lib/wx/include/msw-unicode-3.2 \
            -IC:/msys64/mingw64/include/wx-3.2 -DWXUSINGDLL -D__WXMSW__ $(LUA_CFLAGS)

CFLAGS := -O2 -Wall

LDFLAGS := -mwindows -static-libgcc -static-libstdc++ -Wl,-Bstatic,-lpthread,-Bdynamic $(LUA_LDFLAGS) \
           -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -luser32 -lpsapi -lwininet -lwinhttp -ld3d11 -ldxgi -lpdh \
           -lwinmm -lole32 -luuid -llua -ldwmapi -luxtheme -lwindowscodecs -lstrmiids -loleaut32 \
           -lmfplat -lmfreadwrite -lmf -lmfuuid \
           -LC:/msys64/mingw64/lib -lwx_mswu_aui-3.2 -lwx_mswu_xrc-3.2 -lwx_mswu_html-3.2 -lwx_mswu_qa-3.2 \
           -lwx_mswu_core-3.2 -lwx_baseu_xml-3.2 -lwx_baseu_net-3.2 -lwx_baseu-3.2

OBJS := \
	src/win_main.o \
	src/commands/ls.o \
	src/ui/main_frame.o \
	src/ui/themed_widgets.o \
	src/ui/theme.o \
	src/ui/language.o \
	src/ui/recording_controller.o \
	src/ui/audio_panel.o \
	src/ui/settings_dialog.o \
	src/ui/preset_dialog.o \
	src/ui/hrc_config.o \
	src/ui/win32_theme.o \
	src/ui/overlay_add_dialogs.o \
	src/ui/welcome_dialog.o \
	src/ui/custom_messagebox.o \
	src/ui/console_window.o \
	src/ui/pc_analytics_dialog.o \
	src/ui/log_viewer_dialog.o \
	src/ui/window_picker_dialog.o \
	src/ui/hide_window_dialog.o \
	src/ui/overlays_dock_panel.o \
	src/ui/overlay_editor_dialog.o \
	src/utils/log.o \
	src/utils/log_paths.o \
	src/utils/plugin_log.o \
	src/utils/pc_log.o \
	src/utils/system_integration.o \
	src/utils/crash_handler.o \
	src/utils/archive.o \
	src/core/input_overlay.o \
	src/core/input_overlay_registry.o \
	src/plugins/lua_engine.o \
	src/plugins/lua_api.o \
	src/core/display_info.o \
	src/core/profile_io.o \
	src/core/app_logic.o \
	src/core/capture_ctl.o \
	src/core/pipeline.o \
	src/core/overlay_render.o \
	src/core/webcam_enum.o \
	src/core/webcam_capture.o \
	ffmpeg/ffmpeg_runner.o \
	src/core/tools.o \
	src/utils/ui_utils.o \
	src/core/audio.o \
	src/core/dxgi_capture.o \
	src/core/wgc_capture.o \
	src/utils/stopwatch.o \
	src/core/settings.o \
	src/core/hotkey.o \
	src/core/mic_enum.o \
	src/core/app_audio_enum.o \
	src/ui/icons.o \
	src/core/encoder_helpers.o \
	src/core/settings_registry.o \
	src/utils/update.o

all: hr.exe

hr.exe: $(OBJS) resource.o
	$(CXX) $(OBJS) resource.o -o hr.exe $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

ffmpeg/%.o: ffmpeg/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

resource.o: resource.rc
	windres resource.rc -O coff -o resource.o

hom: FORCE
	$(CXX) $(HOM_CXXFLAGS) -o hom.exe tools/hom/hom.cpp $(HOM_LDLIBS)

FORCE:

clean:
	rm -f $(OBJS) resource.o hr.exe hom.exe

clwexe: # clear without exe
	rm -f $(OBJS) resource.o
.PHONY: all clean hom FORCE
