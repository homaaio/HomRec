# scripts/

- `tests/` - platform-independent doctest suites (pixel converters, audio mixer, replay ring, WAV, overlay canvas). Build recipes are in each file's header.
- `linux/` - Linux helpers for the raw port (`linux-raw` branch, see `docs/LINUX_PORT_PLAN.md`):
  - `install-deps-debian.sh` - toolchain + ffmpeg (`--future` adds the X11 / PipeWire / wx dev packages)
  - `test.sh` - builds and runs all unit tests with g++ and sanitizers, no CMake needed
  - `build.sh` - CMake build of `homrec` into `build-linux/` (`--test` also runs ctest)
- `create_linux_branch.sh` / `create_linux_branch.bat` - create the `linux-raw` branch and commit the Linux groundwork onto it (`--push` / `push` also pushes it).

There are still no Windows build scripts here: the Windows build is `make` / the root `CMakeLists.txt`, and packaging is `python tools/homrec_build.py`.
