# homrec

`homrec` is the HomRec command-line interface. Like `hom.exe` it is a separate executable (no
wxWidgets, no Lua) that you keep next to `hr.exe`. `homrec start` records with the same engine as
`hr.exe` (`RecordingController`), so the wx-free engine objects are linked into `homrec.exe`.

## Build

```bash
cmake --build build --target homrec    # from the repo root -> ./homrec.exe
```

(`build` = the folder you configured with `cmake -S . -B build`). `webcam_enum.cpp` is part of the target for `overlay add webcam`. `-municode` is needed because `homrec.cpp` uses a
wide `wmain()`, so non-ASCII characters in paths survive. The engine sources are the wx-free part of the list `hr.exe` is built from.

## Usage

```
homrec                           In a console: menu (start / overlays / setup)
homrec --version                 Show the HomRec version
homrec setup                     First-run setup again (config, recordings folder, format, fps)
homrec start [config-path]       Record the screen; Ctrl+C stops and saves
homrec pause                     Pause / resume the running recording
homrec config path               Show the .hrc config homrec is bound to
homrec config change <path>      Bind homrec to another .hrc config
homrec overlay list              List the overlays of the active scene
homrec overlay add <type> ...    Add text | image | gif | webcam | input
homrec overlay set <row|id|name> key=value ...
homrec overlay show|hide|remove <row|id|name>
homrec overlay edit              Interactive overlay editor
homrec help | --help | -h        Usage (also works after any command, e.g. `homrec config -h`)
```

Run it from anywhere once the folder containing `homrec.exe` is in `PATH`. Nothing is looked up
relative to the current directory: the default config and the saved settings are found through
the location of `homrec.exe` itself.

### First run

The first time `homrec` is started in a console (any command except `--version`, `help`, `config`,
`pause`, `setup`) it asks a few basic questions, one after another, with Up/Down + Enter (Esc skips):

1. `Use this config?` - the bound `.hrc` (`Yes`, or `Enter a path`; a path that does not exist can be created).
2. The folder the recordings go to (keep the current one or type another; it is created if missing).
3. Video format (MP4 / MKV / MKV + MP4) and frames per second (15 / 30 / 60).

The answers are written into that `.hrc` (the same file HomRec reads) and, when you picked another config,
into the binding. The setup is offered only once - `setup_done=1` is stored in `cfg/homrec_cli.cfg`;
run `homrec setup` to go through it again. It is never started when stdin/stdout is redirected (scripts).

The texts are in Russian or English: `lang=ru|en` in `cfg/homrec_cli.cfg`, else the `language` of the bound
config, else the Windows UI language.

### Overlays

`homrec overlay ...` edits the overlays of the **active scene** - the same files the HomRec window uses
(`scenes/scene_*.hrc`), so a change shows up in HomRec after it is restarted, and `homrec start` records with them.
Close the HomRec window before editing from here: a running window keeps its own copy and would write it back.

```
homrec overlay list
homrec overlay add text "Hello" x=50 y=40 color=#FFAA00 opacity=80
homrec overlay add image C:\pics\logo.png w=300 h=120
homrec overlay add gif .\fire.gif
homrec overlay add webcam [index]              (first camera when no index is given)
homrec overlay add input layout.json sprites.png
homrec overlay set 1 x=100 y=20 opacity=50     (<row> | <id, e.g. ov_3> | <name>)
homrec overlay hide logo      /  show 2  /  remove ov_4
```

- Row 1 of `list` is drawn in front; new overlays are added on top (like in the window).
- `set` keys: `name x y w h opacity visible locked` for every type, `text color font` for text,
  `path` for image / gif, `webcam` (index) for webcam, `json png` for input overlays. A key that does not
  belong to the type, a bad number or a missing file is refused and nothing is changed.
- Relative paths are taken relative to the folder you ran the command from.
- `homrec overlay edit` (or *Overlays* in the menu) is an interactive list: Enter = change a property,
  `A` add, `D` delete, `V` show/hide, Esc back. Every change is saved at once.
- Window / region layers are made in the HomRec window; here they can be listed, moved, resized, hidden and removed.

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
- A small panel (config, output folder, overlays) is printed at start. While recording one status line
  (`REC 00:01:23 | 42.5 MB | 60 fps`) is checked twice a second and rewritten only when its text changed.
  While saving a bouncing bar is shown (about 10 frames a second): the engine reports no
  progress during finalizing, so it shows that homrec is working, not a percentage.
  The screen work of homrec is small next to capture and encoding; the changes were not measured.
- Colours and the bar need a console with ANSI support (Windows 10+); otherwise plain ASCII is used,
  and when output is redirected to a file there is no animation at all.
- Only one `homrec start` can record at a time. The 3-2-1 countdown of the config is honoured; the
  live preview is off (nothing to show it in), scheduled start and Instant Replay are not available
  from the CLI. A config chosen on the command line is loaded like the main window loads its own
  (including custom FFmpeg arguments and the post-record hook), so only point it at configs you trust.
- Exit code: 0 after a normal save, 1 on any error (also when the encoder died and only a part was saved).
- Files are written where the config says (`output_folder`; a relative one is relative to the folder
  of `homrec.exe`, the same as for the main window).
- The overlays of the active scene are applied, like in the main window.

### Logs

Everything homrec writes goes to `logs\cli\` next to `homrec.exe`: `cli.log` (what the CLI did: commands,
setup answers, start / stop / saved file, exit code) and the engine's own logs (`homrec.log`, `ffmpeg_*.log`, `wgc_*`, ...).
`--version`, `help` and `config path` are not logged. The folders `logs\` of hr.exe and `logs\cli\` do not mix.

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

Text pieces (status line, bars, menu rows, ru/en strings) are in `homrec_tui.h` and the overlay rules in
`homrec_overlay.h`; both are plain C++ and tested by `scripts/tests/test_homrec_cli.cpp` (`ctest`, or
`g++ -std=c++17 -I src -I tools/homrec -I scripts/tests scripts/tests/test_homrec_cli.cpp`).
