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
g++ -O2 -std=c++17 -municode -DUNICODE -D_UNICODE -o hom.exe tools/hom/hom.cpp -lwinhttp -lshlwapi
```

## Usage

```
hom --version              Show the hom version
hom update                 Update hom itself from the repo
hom install <plugin-name>  Download and install a plugin
hom remove  <plugin-name>  Remove an installed plugin
```

```
> hom install input-overlay
Fetching plugin 'input-overlay'...
Installed 'input-overlay' -> plugins\input-overlay.hrp (552854 bytes)
Restart HomRec (or reload plugins) to pick it up.

> hom remove input-overlay
Removed plugins\input-overlay.hrp

> hom update
Checking .../Hom/version.txt for updates...
hom is already up to date (1.0.0).
```

Run `hom` from the same folder as `hr.exe` - it reads/writes `./plugins/`
relative to wherever you run it from, same as HomRec itself does. You can
also run it without leaving HomRec: the in-app developer console (see
`commands.md`) has a `hom` built-in that forwards to this same `hom.exe`
as a child process, with its working directory forced to HomRec's own
folder - so `hom install <name>` behaves identically whether you type it
in PowerShell/cmd or in the console.

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
