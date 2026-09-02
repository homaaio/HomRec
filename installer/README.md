# Building the Windows installer

`HomRec.iss` is an [Inno Setup](https://jrsoftware.org/isinfo.php) script.
Install Inno Setup 6 (Windows only - it doesn't cross-compile), then either:

- Run `python tools/homrec_build.py` from the repo root and say yes when it
  asks about building the installer (it finds `ISCC.exe`, reads the version
  from `src/ui/version.h`, and drops the result in `dist\`), **or**
- Build it yourself:
  ```
  make
  make hom
  iscc /DMyAppVersion=2.0.2 installer\HomRec.iss
  ```
  matching `/DMyAppVersion` to `HR_APP_VERSION` in `src/ui/version.h`.

Either way, `hr.exe` (and `hom.exe`, and any runtime `.dll`s next to it,
and `ffmpeg.exe` if you want it bundled) need to already be built and
sitting in the repo root first - the script only packages what it finds,
it doesn't compile anything.

Output: `dist\HomRec-Setup-<version>.exe`.

## What it does

- Installs per-user (`{userpf}`, i.e. `%LOCALAPPDATA%\Programs\HomRec`) with
  no admin/UAC prompt, since HomRec keeps its settings/logs next to its own
  exe (see `hrc_config.h`/`hr_log_paths.cpp`) and needs to be able to write
  there without elevation.
- Ships a real Windows uninstaller (Add/Remove Programs + Start Menu
  shortcut), which asks once whether to also delete `homrec.hrc`/`logs\`
  (recordings are never touched, wherever the output folder points).
- Offers the same two toggles the app's own Settings > System tab has
  (desktop shortcut, launch at Windows startup) as install-time Tasks -
  this is the *only* place those are offered before first launch now; the
  first-run Welcome wizard no longer has its own copy of this step.

## Auto-update

There's no separate updater binary. The flow is:

1. `hr.exe`'s **Help > Check for Updates** (`src/hr_update.cpp`) asks
   GitHub's API for `homaaio/HomREC`'s latest release and compares its tag
   against `HR_APP_VERSION`.
2. If newer, it looks at that release's assets for one whose filename ends
   in `.exe` - i.e. the Setup exe this script produces - downloads it to
   `%TEMP%`, and launches it with
   `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS`.
3. `/CLOSEAPPLICATIONS` works because of `AppMutex` in `HomRec.iss`
   (`HomRec_SingleInstance_150`, matching `HR_SINGLE_INSTANCE_MUTEX_NAME`
   in `version.h`) - Inno uses it to find and close the running HomRec
   before overwriting its files, same as it would for a normal manual
   reinstall over a running copy.
4. Because `AppId` never changes between versions, the new installer
   recognizes the existing install and upgrades it in place rather than
   creating a second copy.

**For this to actually offer anything, the release needs the Setup exe
attached as an asset** - `tools/homrec_build.py` prints a reminder of this
after building one. A release with only the source zip/tarball (GitHub
auto-generates those, they don't count) won't trigger an update prompt for
existing installs, since `FindInstallerAssetUrl()` in `hr_update.cpp` finds
nothing ending in `.exe` to offer.

## Changing the installer

- **New bundled file/folder:** add a line under `[Files]`.
- **New install-time toggle:** add it to `[Tasks]`, then reference that
  task's name from the `[Icons]`/`[Registry]`/`[Run]` entry it should
  control via `Tasks: yourtaskname`.
- **AppId:** never change this once released - it's what ties an upgrade
  installer to the existing install instead of creating a second one.
