# Makefile - HomRec native C++/C build
#
# Lives at the HomRec-main project root, builds hr.exe from src/ directly.
# This REPLACES the PyInstaller step (which packaged homrec.py into
# hr.exe) - there is no Python involved in this build at all anymore.
#
# The UI shell (src/ui/main_frame.*) is wxWidgets - a real widget toolkit
# (comparable to Tkinter's Frame/Label/Button model), replacing the earlier
# hand-rolled GDI version that couldn't get font sizing/DPI or the live
# preview right. wxWidgets must be installed to build:
#
#   MSYS2 (native Windows build):
#     pacman -S mingw-w64-x86_64-wxwidgets3.2-msw   (or the ucrt64 variant)
#     make                                            # wx-config is on PATH
#
#   Cross-compiling from Linux/macOS with a MinGW-w64 wxWidgets build:
#     make CXX=x86_64-w64-mingw32-g++ WINDRES=x86_64-w64-mingw32-windres \
#          WX_CONFIG=x86_64-w64-mingw32-wx-config
#
#   No wx-config available (manually-built/vendored wxWidgets): skip
#   WX_CONFIG entirely and set WX_CFLAGS/WX_LIBS yourself, e.g.:
#     make WX_CONFIG= WX_CFLAGS="-Ic:/wx/include -Ic:/wx/lib/mswu" \
#          WX_LIBS="-Lc:/wx/lib -lwxmsw32u_core -lwxbase32u ..."
#   (exact library list depends on your wx build - check its
#   build/msw/*.txt or the wx-config output from a matching build if you
#   have one elsewhere, and copy the --libs line.)

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0601 -Isrc
# Static-link the MinGW runtime (same idea as build_native.py's
# BASE_LDFLAGS) so hr.exe doesn't need libgcc_s_seh-1.dll/libstdc++-6.dll/
# libwinpthread-1.dll present on the target machine.
# NOTE: no -municode here (unlike CXXFLAGS) - wxIMPLEMENT_APP (win_main.cpp)
# always generates an ANSI WinMain() internally (it converts to wide args
# itself via GetCommandLineW()), never a wWinMain(). -municode forces MinGW's
# CRT startup stub to look for wWinMain specifically, which doesn't exist,
# and the link fails with "undefined reference to `wWinMain'". UNICODE/
# _UNICODE (still in CXXFLAGS) are what actually matter for wx/Win32 to use
# the wide API - they're a compile-time macro, unrelated to this.
LDFLAGS  ?= -mwindows -static-libgcc -static-libstdc++ -Wl,-Bstatic,-lpthread,-Bdynamic
LDLIBS   := -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -luser32 -lpsapi -lwininet \
            -ld3d11 -ldxgi -lpdh -lwinmm -lole32 -luuid -llua -ldwmapi -luxtheme -lwindowscodecs \
            -lstrmiids -loleaut32

# wxWidgets - used only by src/win_main.cpp and src/ui/main_frame.cpp.
# Everything else (RecordingController, AudioPanel/OverlaysDockPanel's
# native controls, the various dialogs, the engine .cpp files) is plain
# Win32/C++ and doesn't need these flags, but adding them project-wide is
# harmless and keeps this Makefile simple.
WX_CONFIG ?= wx-config
WX_CFLAGS ?= $(shell $(WX_CONFIG) --cxxflags 2>/dev/null)
WX_LIBS   ?= $(shell $(WX_CONFIG) --libs std,adv 2>/dev/null)
CXXFLAGS  += $(WX_CFLAGS)
LDLIBS    += $(WX_LIBS)

# Lua 5.4 is NOT vendored in this repo - see README_PHASE9.md for how
# to get it (vcpkg or the amalgamation from lua.org). If it's somewhere
# non-standard: make LUA_CFLAGS="-Ic:/lua54/include" LUA_LDFLAGS="-Lc:/lua54/lib"
LUA_CFLAGS ?=
LUA_LDFLAGS ?=
CXXFLAGS += $(LUA_CFLAGS)
LDFLAGS += $(LUA_LDFLAGS)

SRCS := \
    src/win_main.cpp \
    src/ui/main_frame.cpp \
    src/ui/themed_widgets.cpp \
    src/ui/theme.cpp \
    src/ui/language.cpp \
    src/ui/recording_controller.cpp \
    src/ui/audio_panel.cpp \
    src/ui/settings_dialog.cpp \
    src/ui/hrc_config.cpp \
    src/ui/win32_theme.cpp \
    src/ui/overlay_add_dialogs.cpp \
    src/ui/welcome_dialog.cpp \
    src/ui/custom_messagebox.cpp \
    src/ui/console_window.cpp \
    src/ui/pc_analytics_dialog.cpp \
    src/ui/log_viewer_dialog.cpp \
    src/ui/window_picker_dialog.cpp \
    src/ui/overlays_dock_panel.cpp \
    src/hr_log.cpp \
    src/hr_archive.cpp \
    src/hr_input_overlay.cpp \
    src/hr_input_overlay_registry.cpp \
    src/plugins/lua_engine.cpp \
    src/plugins/lua_api.cpp \
    src/hr_display_info.cpp \
    src/hr_profile_io.cpp \
    src/hr_app_logic.cpp \
    src/hr_capture_ctl.cpp \
    src/hr_pipeline.cpp \
    src/hr_overlay_render.cpp \
    src/hr_webcam_enum.cpp \
    src/hr_ffmpeg_runner.cpp \
    src/hr_tools.cpp \
    src/hr_ui_utils.cpp \
    src/hr_audio.cpp \
    src/hr_dxgi_capture.cpp \
    src/hr_stopwatch.cpp \
    src/hr_settings.cpp \
    src/hr_hotkey.cpp \
    src/hr_mic_enum.cpp

C_SRCS := \
    src/hr_encoder_helpers.c

OBJS := $(SRCS:.cpp=.o) $(C_SRCS:.c=.o)
RES  := resource.o
TARGET := hr.exe

WINDRES ?= windres

.PHONY: all clean


# BUGFIX: `hom.exe` used to only get built by an explicit `make hom` - a
# plain `make`/`make all` never touched it. That's how a stale hom.exe
# (built before the -static-libgcc/-static-libstdc++ fix below) could
# keep sitting next to a freshly-rebuilt hr.exe forever: nothing ever
# prompted a rebuild of it. `hom --version` erroring with "the .exe
# exists but won't run" is exactly the missing-runtime-DLL symptom that
# fix addresses - now a plain `make` always rebuilds both together so
# they can't drift out of sync again.
all: $(TARGET) hom.exe

$(TARGET): $(OBJS) $(RES)
	$(CXX) $(OBJS) $(RES) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) -O2 -Wall -c $< -o $@

resource.o: resource.rc
	$(WINDRES) resource.rc -O coff -o resource.o

clean:
	rm -f $(OBJS) $(RES) $(TARGET) hom.exe

# hom - the plugin package manager (see tools/hom/). Standalone binary,
# doesn't touch anything above (no wx, no Lua, no src/ objects), so it
# gets its own tiny build rule instead of joining $(SRCS)/$(OBJS).
#
# NOTE: no -municode here, unlike hr.exe's CXXFLAGS. hom.cpp defines a
# plain narrow `int main(int argc, char **argv)` (see tools/hom/hom.cpp),
# not `wWinMain`/`wmain`. -municode makes MinGW's CRT startup stub look
# for a wide entry point instead, and since hom.cpp doesn't define one the
# link fails with "undefined reference to `wWinMain'". UNICODE/_UNICODE
# are kept since they only affect which Win32 API variant gets selected at
# compile time (unrelated to the CRT startup/entry-point symbol), and
# hom.cpp still converts to/from wide strings itself (see Widen()) where it
# needs to call wide Win32 APIs.
# BUGFIX: hom.exe was linked without -static-libgcc/-static-libstdc++
# (unlike hr.exe just above, see the BASE_LDFLAGS comment for why those
# matter) -- so it silently depended on libstdc++-6.dll, libgcc_s_seh-1.dll,
# and libwinpthread-1.dll being reachable at runtime. On the machine that
# built it (with MinGW installed) that's invisible; on any other machine
# -- exactly "the .exe exists but running it errors out" -- Windows can't
# find those DLLs and refuses to start the process at all. hom --version
# never gets a chance to run; the loader fails before main() either way.
HOM_CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE
HOM_LDFLAGS  := -static-libgcc -static-libstdc++ -Wl,-Bstatic,-lpthread,-Bdynamic
HOM_LDLIBS   := -lwinhttp -lshlwapi

.PHONY: hom
hom: hom.exe

hom.exe: tools/hom/hom.cpp
	$(CXX) $(HOM_CXXFLAGS) -o $@ $< $(HOM_LDFLAGS) $(HOM_LDLIBS)

clean-hom:
	rm -f hom.exe
