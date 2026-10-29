# homrec

`homrec` is the HomRec command-line interface. Like `hom.exe` it is a separate executable (no
wxWidgets, no Lua) that you keep next to `hr.exe`. `homrec start` records with the same engine as
`hr.exe` (`RecordingController`), so the wx-free engine objects are linked into `homrec.exe`.

## Build

```bash
make homrec        # from the repo root -> ./homrec.exe
```

(or the `homrec` target of the CMake build). `-municode` is needed because `homrec.cpp` uses a
wide `wmain()`, so non-ASCII characters in paths survive. The engine objects are the same `.o`
files `hr.exe` is built from, so after `make` they are not compiled twice.

## Usage

```
homrec --version                 Show the HomRec version
homrec start [config-path]       Record the screen; Ctrl+C stops and saves
homrec pause                     Pause / resume the running recording
homrec config path               Show the .hrc config homrec is bound to
homrec config change <path>      Bind homrec to another .hrc config
homrec help | --help | -h        Usage (also works after any command, e.g. `homrec config -h`)
```

Run it from anywhere once the folder containing `homrec.exe` is in `PATH`. Nothing is looked up
relative to the current directory: the default config and the saved settings are found through
the location of `homrec.exe` itself.

### Recording

- `homrec start` records with the **bound** config (see below - `homrec.hrc` next to `homrec.exe`
  unless you changed it). If that file has a custom settings path set in HomRec (Settings >
  Advanced), it is followed exactly like `hr.exe` does at startup.
- `homrec start <config-path>` records with that `.hrc` for this run only; the binding stays as it is.
- **Ctrl+C** stops and saves. It never kills the process mid-save (pressing it again while saving
  does nothing); closing the console window also stops and saves, with a few seconds to finish.
- **`homrec pause`**, run in another terminal, pauses the recording; run it again to resume. It
  talks to the running `homrec start` through named Windows objects, so it does nothing for a
  recording started from the HomRec window. For ddagrab (full-GPU) captures pause only works when
  `ddagrab_pause` is on; otherwise it reports that the recording did not react.
- While recording one status line is redrawn about 4 times a second (`* REC 00:01:23  42.5 MB  60 fps`).
  While saving a bouncing bar is shown (same look as `tools/homrec_build.py`): the engine reports no
  progress during finalizing, so it shows that homrec is working, not a percentage.
- Colours and the bar need a console with ANSI support (Windows 10+); otherwise plain ASCII is used,
  and when output is redirected to a file there is no animation at all.
- Only one `homrec start` can record at a time. The 3-2-1 countdown of the config is honoured; the
  live preview is off (nothing to show it in), scheduled start and Instant Replay are not available
  from the CLI. A config chosen on the command line is loaded like the main window loads its own
  (including custom FFmpeg arguments and the post-record hook), so only point it at configs you trust.
- Exit code: 0 after a normal save, 1 on any error (also when the encoder died and only a part was saved).
- Files are written where the config says (`output_folder`; a relative one is relative to the folder
  of `homrec.exe`, the same as for the main window), logs go to `logs\` next to it.

### Which config is "bound"

- Default: `homrec.hrc` in the folder of `homrec.exe`.
- `homrec config change <path>` stores the **absolute** path (a relative argument is resolved
  against where you ran the command) in `cfg/homrec_cli.cfg` next to `homrec.exe`:

  ```
  config=D:\HomRec\configs\main.hrc
  ```
- The file must exist and have the `.hrc` extension; its contents are not validated.
- `homrec config path` prints just the path on stdout (a note on stderr if the file doesn't exist
  yet), so `$(homrec config path)` works in scripts.

## Adding a command

1. Write `int CmdXxx(const Args &args)` in `homrec.cpp` (return 0 on success, `Fail("...")` on error).
2. Add one `Leaf(...)` (or a `Group(...)` of leaves) line to `BuildCommands()`.

Help text, "unknown command" errors and `-h/--help` come from that table - see `homrec_cli.h`.
