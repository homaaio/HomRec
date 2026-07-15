# Makefile — HomRec native C++/C build
#
# Lives at the HomRec-main project root, builds hr.exe from src/ directly.
# This REPLACES the PyInstaller step (which packaged homrec.py into
# hr.exe) — there is no Python involved in this build at all anymore.
#
# Usage:
#   On Windows with MinGW-w64 g++ on PATH:      make
#   Cross-compiling from Linux/macOS:            make CXX=x86_64-w64-mingw32-g++ WINDRES=x86_64-w64-mingw32-windres

CXX      ?= g++
CC       ?= gcc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -municode -D_WIN32_WINNT=0x0601 -Isrc
# Static-link the MinGW runtime (same idea as build_native.py's
# BASE_LDFLAGS) so hr.exe doesn't need libgcc_s_seh-1.dll/libstdc++-6.dll/
# libwinpthread-1.dll present on the target machine.
LDFLAGS  ?= -municode -mwindows -static-libgcc -static-libstdc++ -Wl,-Bstatic,-lpthread,-Bdynamic
LDLIBS   := -lcomctl32 -lgdi32 -lshell32 -luser32 -lpsapi -lwininet \
            -ld3d11 -ldxgi -lpdh -lwinmm -lole32 -luuid -llua

# Lua 5.4 is NOT vendored in this repo — see README_PHASE9.md for how
# to get it (vcpkg or the amalgamation from lua.org). If it's somewhere
# non-standard: make LUA_CFLAGS="-Ic:/lua54/include" LUA_LDFLAGS="-Lc:/lua54/lib"
LUA_CFLAGS ?=
LUA_LDFLAGS ?=
CXXFLAGS += $(LUA_CFLAGS)
LDFLAGS += $(LUA_LDFLAGS)

SRCS := \
    src/win_main.cpp \
    src/ui/main_window.cpp \
    src/ui/theme.cpp \
    src/ui/language.cpp \
    src/ui/recording_controller.cpp \
    src/ui/audio_panel.cpp \
    src/ui/settings_dialog.cpp \
    src/ui/advanced_settings_dialog.cpp \
    src/ui/overlay_manager.cpp \
    src/ui/welcome_dialog.cpp \
    src/ui/custom_messagebox.cpp \
    src/ui/console_window.cpp \
    src/ui/pc_analytics_dialog.cpp \
    src/ui/log_viewer_dialog.cpp \
    src/ui/window_picker_dialog.cpp \
    src/ui/overlays_dock_panel.cpp \
    src/plugins/lua_engine.cpp \
    src/plugins/lua_api.cpp \
    src/hr_display_info.cpp \
    src/hr_profile_io.cpp \
    src/hr_app_logic.cpp \
    src/hr_capture_ctl.cpp \
    src/hr_pipeline.cpp \
    src/hr_ffmpeg_runner.cpp \
    src/hr_tools.cpp \
    src/hr_ui_utils.cpp \
    src/hr_audio.cpp \
    src/hr_dxgi_capture.cpp \
    src/hr_stopwatch.cpp \
    src/hr_settings.cpp \
    src/hr_hotkey.cpp

C_SRCS := \
    src/hr_encoder_helpers.c

OBJS := $(SRCS:.cpp=.o) $(C_SRCS:.c=.o)
RES  := resource.o
TARGET := hr.exe

WINDRES ?= windres

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(RES)
	$(CXX) $(OBJS) $(RES) -o $@ $(LDFLAGS) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.c
	$(CC) -O2 -Wall -c $< -o $@

resource.o: resource.rc
	$(WINDRES) resource.rc -O coff -o resource.o

clean:
	rm -f $(OBJS) $(RES) $(TARGET)
