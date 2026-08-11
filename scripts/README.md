# scripts/

Release-packaging scripts. These build the app and assemble a
distributable release **folder** (you archive it yourself into
`.zip`/`.7z`/whatever you prefer - see the note in each script for why
that last step is manual instead of automated).

- **`release-windows.sh`** - actually works today. Run it from the
  "MSYS2 MinGW64" terminal (same requirement as the existing
  `build.sh` at the project root, which this calls). Produces
  `dist/HomRec-<version>-win64/` with `hr.exe`, `hom.exe`, the default
  plugins, and the license/readme. Does **not** bundle `ffmpeg.exe`
  automatically (redistribution-license reasons - see the comment in
  the script), but can if you point it at one.

- **`release-linux.sh`** - a placeholder, on purpose. There's no Linux
  build of HomRec to package yet (the whole codebase is currently
  Windows-only). Running it explains what has to exist first rather
  than silently doing nothing and calling it a success. See the
  comment at the top of the file for the actual checklist.

## `packaging/`

Distro/format-specific packaging metadata (`.deb` control files,
`.desktop` entries, etc.) - kept separate from `src/platform/linux/`,
because *which distro/package format* (`.deb` vs Arch's `PKGBUILD` vs
Flatpak) and *which display-server backend* (X11 vs Wayland, in
`src/platform/linux/`) are independent questions. A single Linux build
could ship as multiple package formats from the same source tree.
