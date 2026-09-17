# icons/

This folder (together with `icons/files_icons/`) holds every icon asset
HomRec's build references by path - `resource.rc` (the app/tray icon baked
into `hr.exe`) and `installer/HomRec.iss` (the installer's own icon, plus
the optional per-file-type icons it can register).

## New in this pass

- `hrsetup.ico` / `hrsetup.png` - the installer's own icon
  (`installer/HomRec.iss`'s `SetupIconFile`). Deliberately a separate file
  from `main.ico` (hr.exe's icon) so `HomRec-Setup-*.exe` doesn't look like
  a second copy of the app itself in Explorer/Downloads/the taskbar while
  it's running. `hrsetup.png` isn't used by the build itself (Inno Setup
  only takes `.ico`) - it's there for anywhere else the setup icon shows up
  as a flat image (README, release notes, a download page).
- `files_icons/hrc.ico` - icon for `.hrc` (HomRec settings/export) files,
  used by the installer's optional "Associate .hrc and .hrp files with
  HomRec" task (`fileassoc` in `[Tasks]`) via a `DefaultIcon` registry
  value.
- `files_icons/hrp.ico` - same idea, for `.hrp` (plugin package) files.

**All four are placeholder art** (plain flat-color rounded squares with a
short label), the same way `tray.ico` started out - see the note in
`resource.rc`. Swap them for real art whenever it's ready; nothing else
needs to change on the code/installer side, since everything references
these by their fixed filename.
