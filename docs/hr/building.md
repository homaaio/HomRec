# Building HomRec from source

For contributors. Windows only. Normal users should download a release ([getting-started.md](getting-started.md)).

> **Not verified.** The build could not be run while writing this guide. The steps come from `build.cmd`,
> `tools/build.py`, `CMakeLists.txt` and the README. Package names below must be checked with `pacman -Ss` on your
> machine.

## What you need

| Tool | Why |
|---|---|
| MSYS2 in `C:\msys64` | `build.cmd` looks for `C:\msys64\usr\bin\bash.exe` and stops with an error if it is missing. |
| The **UCRT64** toolchain (g++, windres, mingw32-make) and CMake 3.15+ | HomRec's shipped wxWidgets and Lua DLLs are UCRT builds; the MINGW64 (MSVCRT) toolchain cannot be used. |
| wxWidgets 3.2 (MSW, Unicode, DLL) for UCRT64 | The UI. CMake expects headers in `<msys prefix>/include/wx-3.2`. |
| Lua 5.4 headers and import library | Plugin host. Defaults: `lua54\include` in the repo, `C:/lua54/lib`. |
| FFmpeg 6+ | Only at run time (and ddagrab needs 6+). |
| Python 3 | Optional: `tools/build.py`, `tools/homrec_build.py`. |
| Inno Setup 6 | Optional: the installer. |

Download pages for each tool:

<!-- LINK: msys2 -->
<!-- LINK: lua -->
<!-- LINK: wxwidgets -->
<!-- LINK: cmake -->
<!-- LINK: ffmpeg-download -->
<!-- LINK: python -->
<!-- LINK: inno-setup -->

## Build

Use any one of these from the repository root.

```
build.cmd
python tools\build.py            # menu: all / hr / homrec / tests / clean / nolto / package
python tools\build.py hr         # hr.exe only
```

`build.cmd` runs `cmake -S . -B build -G "MinGW Makefiles"` followed by `cmake --build build -j` inside the UCRT64
shell. Extra arguments go to the configure step, for example `build.cmd -DHR_LTO=OFF`.

If Lua is not in the default place:

```
build.cmd -DLUA_INCLUDE_DIR=D:/lua/include -DLUA_LIB_DIR=D:/lua/lib
```

**Check:** `hr.exe` and `homrec.exe` appear in the repository root (option `HR_EXE_IN_ROOT`, on by default). Logs are in
`logs\build\`: `cmake.log` (everything), `last-error.txt` (extracted errors after a failure), `last-build.json` (after a
success).

`hom.exe` is **not** built here; it moved to its own repository ([../hom/README.md](../hom/README.md)).

## What can fail

| Message | Cause | Fix |
|---|---|---|
| `[build] MSYS2 not found at C:\msys64` | MSYS2 is installed elsewhere or not at all. | Install it to `C:\msys64`, or edit `MSYS2_ROOT` in `build.cmd`. |
| `wxWidgets headers not found at ...` | wxWidgets for UCRT64 is not installed, or it is not the same toolchain as the compiler. | Install the UCRT64 wxWidgets 3.2 package; reconfigure with `--fresh`. |
| `WX_INCLUDE_DIR=... is outside the compiler's MSYS2 prefix` | Old cached `-DWX_*` values. | Delete `build\` and reconfigure without those values. |
| `fatal error: lua.h: No such file` / `cannot find -llua54` | Lua is not where CMake looks. | Pass `LUA_INCLUDE_DIR` and `LUA_LIB_DIR`. |
| `undefined reference to wWinMain` when linking `hr` | `-municode` was added to the **link** options of `hr`. In `CMakeLists.txt`, `hr` uses `-municode` only at compile time; `homrec` uses it at compile and link time. | Remove `-municode` from `hr`'s link options. |
| The program builds but starts crashing on another PC | Runtime DLLs of a different toolchain (MSVCRT vs UCRT) are next to `hr.exe`. | Use the UCRT64 DLLs matching the build; see [troubleshooting.md](troubleshooting.md#homrec-does-not-start-or-closes-immediately). |
| Your changes are not in the binary | A stale `build\` folder, or the edited files are not the ones in the tree you build. | `python tools\build.py clean`; check the file's modified time. |
| Link takes very long | Link-time optimisation is on. | `python tools\build.py nolto`. |

## Tests

```
python tools\build.py tests
```

Builds with `-DHOMREC_BUILD_TESTS=ON` and runs `ctest` (a small doctest suite for the pixel-conversion helpers).

## Release packages

`python tools\homrec_build.py` builds the archives into `dist\`; the version comes from `HR_APP_VERSION` in
`src\ui\version.h`. The **full** archive includes `ffmpeg.exe`; the **portable** one does not. The installer needs
Inno Setup 6 (`installer\`).
