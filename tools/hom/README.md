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
hom install <plugin-name> [-y] [-f]    Download and install a plugin
                                          (-y/-f: skip the disk-space prompt)
hom remove  <plugin-name> -r           Remove an installed plugin
                                          (-r is required to confirm)
hom repo [list]                        Show download repositories ('*' = in use)
hom repo add <name> <url|owner/repo[@branch]>
                                       Remember a fork or a mirror (https only)
hom repo set <name>                    Download from that repo from now on
hom repo remove <name>                 Forget a repo
--progress=bar|lines|none              (any command) how downloads report progress
```

### Forks and mirrors (`hom repo`)

By default hom downloads from the official repo. To use a fork or a mirror:

```
hom repo add myfork someone/HomRec          # GitHub fork -> raw.githubusercontent.com/someone/HomRec/main/Hom/
hom repo add beta   someone/HomRec@dev      # same, another branch
hom repo add mirror https://example.org/homrec/Hom/   # any https folder with version.txt, hom.exe, plugins/
hom repo set myfork                         # use it
hom repo set official                       # go back
```

The list is stored in `hom_repos.cfg` next to `hom.exe` (plain text, hand-editable;
`official` is built in). The chosen repo is used for `install`, `upgrade`, `search`,
`show`, `list --upgradable`, `ping` and `update` (which downloads `hom.exe` from there).
Only `https://` is accepted, because hom downloads an executable from that place.
Whenever a non-official repo is in use, hom says so on the first line of its output.

### Download progress

`install`, `upgrade` and `update` show a bar with percent, sizes and an estimate:

```
  [#########-----------]  45%  1.5 / 3.4 MB  ~4s
```

In a real terminal hom redraws the line itself. HomRec's console starts hom with
`--progress=lines` (hom then prints `@progress <percent> <received> <total> <eta_sec>`
lines, which the console draws as one live bar). When the output is piped into
another program the default is `none`.

### If hom.exe was deleted

From HomRec's console just type any `hom` command: when `hom.exe` is missing,
HomRec downloads it again from the active repo (with a progress bar) and then runs
your command. `inwid hom restore` does the same on purpose (and replaces a broken
`hom.exe`). No need to dig it out of the portable archive.

```
> hom install input-overlay
Fetching plugin 'input-overlay'...
Installed 'input-overlay' -> plugins\input-overlay.hrp (552854 bytes)
Restart HomRec (or reload plugins) to pick it up.

> hom remove input-overlay
hom: remove needs -r to confirm deletion, e.g. 'hom remove input-overlay -r'

> hom remove input-overlay -r
Removed plugins\input-overlay.hrp

> hom update
Checking .../Hom/version.txt for updates...
hom is already up to date (1.0.0).
```

Every plugin name is rejected outright if it contains `..`, a path
separator, or a drive letter (`C:`) - install/remove can never resolve
to anything outside `.\plugins\`, no matter what's typed.

Run `hom` from the same folder as `hr.exe` - it reads/writes `./plugins/`
relative to wherever you run it from, same as HomRec itself does. You can
also run it without leaving HomRec: the in-app developer console (see
`commands.md`) has a `hom` built-in that forwards to this same `hom.exe`
as a child process, with its working directory forced to HomRec's own
folder - so `hom install <name>` behaves identically whether you type it
in PowerShell/cmd or in the console. From inside the console, `hom update`
and `hom remove`/`hom uninstall` additionally need an `inwid` prefix
(e.g. `inwid hom update`) - see `commands.md`'s Section 21.

## How it works

- **Plugins** live in this repo under [`Hom/plugins/<name>.hrp`](../../Hom).
  `hom install <name>` just downloads that one file to `plugins/<name>.hrp`.
  It does *not* unzip anything itself - HomRec's own plugin loader
  (`lua_engine.cpp`'s `LoadPluginArchive()`) already knows how to extract
  and load a `.hrp` it finds in `plugins/`, so `hom`'s job stops at "the
  file is on disk."
- **`hom` self-updates** by comparing its compiled-in version against
  [`Hom/version.txt`](../../Hom/version.txt), and if that's newer,
  downloading [`Hom/hom.exe`](../../Hom) and swapping it in for the
  running binary (rename-and-replace - this works even on the exe
  that's currently executing, the same way any self-updating Windows
  app does it).
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
- There's no uninstall confirmation prompt (unlike the console's
  `rm --window` etc., which usually asks first) - `hom remove` deletes
  immediately. Happy to add a `-q`/confirmation convention to match the
  rest of HomRec's command style if you'd prefer that.
