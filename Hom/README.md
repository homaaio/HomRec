# hom

`hom` is a tiny package manager for HomRec plugins - the same idea as
`apt`/`pacman`, just aimed at `.hrp` plugin packages instead of system
packages. It's a separate `hom.exe` you keep next to `hr.exe`, not part
of HomRec's own build.

## Build

Same toolchain as `hr.exe` (MinGW-w64), but `hom` doesn't need wxWidgets
or Lua - it's a single file with no dependencies beyond WinHTTP:

```bash
make hom          # from the repo root -- builds ./hom.exe
```

or directly:

```bash
g++ -O2 -std=c++17 -DUNICODE -D_UNICODE -o hom.exe tools/hom/hom.cpp -lwinhttp -lshlwapi
```

(no `-municode` - `hom.cpp`'s `main()` is a plain narrow `int main(argc, argv)`,
not `wWinMain`/`wmain`, so `-municode` would make the linker look for an
entry point that doesn't exist here - see the comment at the top of
`hom.cpp` and the `HOM_CXXFLAGS` comment in the top-level Makefile.)

## Usage

```
hom --version                          Show the hom version
hom update [-f|--force]                Update hom itself from the repo
                                          (-f: reinstall even if already latest)
hom ping                               Check connectivity to the plugin repo
hom upgrade [-y] [-f]                  Update every already-installed plugin
hom full-upgrade [-y] [-f]             Same as upgrade for now (see hom.cpp header --
                                          hom has no plugin dependencies to remove yet)
hom install <plugin-name> [-y] [-f]    Download and install a plugin
                                          (-y/-f: skip the disk-space prompt)
hom remove  <plugin-name> -r           Remove a plugin, keep its saved settings
                                          (-r is required to confirm)
hom purge   <plugin-name> -r           Remove a plugin and its saved settings
                                          (-r is required to confirm)
hom autoremove                         Clean up orphaned auto-installed plugins
                                          (currently always a no-op -- see hom.cpp header)
hom search <query>                     Search plugin names/descriptions
hom show <plugin-name>                 Show details about a plugin
hom list --installed                   List plugins installed here
hom list --upgradable                  List installed plugins with a newer version
```

```
> hom search overlay
input-overlay (1.0.0) - Keyboard / mouse / gamepad input overlay presets (WASD, QWERTY, mouse, gamepad).

> hom show input-overlay
Name:        input-overlay
Version:     1.0.0
Author:      HomRec
Description: Keyboard / mouse / gamepad input overlay presets (WASD, QWERTY, mouse, gamepad).
Package:     Hom/plugins/input-overlay.hrp
Size:        540.0 KB
Installed:   no

> hom install input-overlay
Fetching plugin 'input-overlay'...
Installed 'input-overlay' -> plugins\input-overlay.hrp (552854 bytes)
Restart HomRec (or reload plugins) to pick it up.

> hom list --installed
input-overlay (version unknown)

> hom list --upgradable
hom: no upgrades available.
hom: 1 plugin(s) skipped -- local version unknown (not yet loaded by HomRec since install;
run HomRec once, or 'hom upgrade', to find out).

> hom remove input-overlay
hom: remove needs -r to confirm deletion, e.g. 'hom remove input-overlay -r'

> hom remove input-overlay -r
Removed plugins\input-overlay.hrp
hom: plugin removed. Its saved settings (if any) were kept -- use 'hom purge input-overlay -r' to delete those too.

> hom purge input-overlay -r
Removed plugins\.installed\input-overlay

> hom update
Checking .../Hom/version.txt for updates...
hom is already up to date (0.4).
```

Every plugin name is rejected outright if it contains `..`, a path
separator, or a drive letter (`C:`) - install/remove/purge/show can never
resolve to anything outside `.\plugins\`, no matter what's typed.

Run `hom` from the same folder as `hr.exe` - it reads/writes `./plugins/`
relative to wherever you run it from, same as HomRec itself does. You can
also run it without leaving HomRec: the in-app developer console (see
`commands.md`) has a `hom` built-in that forwards to this same `hom.exe`
as a child process, with its working directory forced to HomRec's own
folder - so `hom install <name>` behaves identically whether you type it
in PowerShell/cmd or in the console. From inside the console, the
destructive subcommands (`hom update`, `hom remove`/`hom uninstall`,
`hom purge`, `hom autoremove`) additionally need an `inwid` prefix (e.g.
`inwid hom update`) - see `commands.md`'s Section 21. `install`,
`upgrade`, `full-upgrade`, `search`, `show`, and `list` don't need it.

## How it works

- **Plugins** live on this repo's `plugins-registry` branch, under
  `Hom/plugins/<name>.hrp` - a separate branch from the one you actually
  check out and build (`main`), so a normal `git clone`/`git pull` of the
  app doesn't also pull down every `.hrp` in the registry. `hom install
  <name>` just downloads that one file to `plugins/<name>.hrp`. It does
  *not* unzip anything itself - HomRec's own plugin loader
  (`lua_engine.cpp`'s `LoadPluginArchive()`) already knows how to extract
  and load a `.hrp` it finds in `plugins/`, so `hom`'s job stops at "the
  file is on disk."
- **`hom` self-updates** by comparing its compiled-in version against
  [`Hom/version.txt`](../../Hom/version.txt) *on `main`*, and if that's
  newer, downloading [`Hom/hom.exe`](../../Hom) and swapping it in for
  the running binary (rename-and-replace - this works even on the exe
  that's currently executing, the same way any self-updating Windows
  app does it).
- **`search`/`show`/`list --upgradable`** read `Hom/plugins/index.json`
  off the `plugins-registry` branch - the same file that used to be
  purely for humans browsing the repo. `install` itself still doesn't
  need it (it just requests `plugins/<name>.hrp` directly), so a plugin
  missing from `index.json` can still be installed by name - it just
  won't show up in `search`/`show`, and `list --upgradable` won't know a
  newer version exists for it.
- Publishing a new plugin or bumping a version is a commit to the
  `plugins-registry` branch, *not* `main` - `main`'s history stays clean
  of binary `.hrp` churn. See `k_plugins_path_prefix` in `hom.cpp` if
  you're forking this and want your own registry branch/repo.
- **A local plugin's "known version"** comes from whatever `plugin.json`
  HomRec has actually extracted for it - `plugins/.installed/<name>/`
  for a `.hrp`-based plugin, or `plugins/<name>/` directly for one shipped
  as a bare folder (see `lua_engine.cpp`'s `LoadPluginArchive()`). Right
  after `hom install`, if HomRec hasn't been run since, there's genuinely
  no local version yet - `list --upgradable` reports those separately as
  "can't tell" rather than guessing.
- Everything is plain HTTPS to `raw.githubusercontent.com` - no GitHub
  API, no auth, no server to run. Publishing an update is just a commit
  to `Hom/` (see [`Hom/README.md`](../../Hom/README.md)).

## Notes / things worth discussing

- Right now `hom update` always re-downloads the whole `hom.exe`. If you'd
  rather ship the plugin repo update mechanism differently (e.g. a
  git-based `hom update` that just does a shallow pull of `Hom/` for
  people who already have git, or signed releases instead of a bare
  `raw.githubusercontent.com` fetch), that's a quick change to
  `CmdUpdate()` - the plugin install/remove commands don't depend on
  which approach you pick.
- `full-upgrade` and `autoremove` are real commands (so scripts/cfg files
  that always run them don't need to special-case hom), but currently
  behave as "same as upgrade" and "nothing to do" respectively, since hom
  has no concept of one plugin depending on another. If plugin
  dependencies are ever introduced, those are the two places that would
  need real logic.
- `search` tries your query as an ECMAScript regex first (case-insensitive,
  matching `apt-cache search`'s behavior) and falls back to a plain
  substring match if it isn't valid regex syntax - so `hom search overlay`
  and `hom search over.ay` both work as expected.
