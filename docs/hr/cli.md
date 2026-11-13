# `homrec`: record from a terminal

`homrec.exe` is a small command-line tool that records with the same engine as the HomRec window. Use it for
scripts, remote sessions and low-overhead recording.

It is a separate program (no window, no Lua). Keep it **next to `hr.exe`**; it finds its config and logs from its own
location, not from the current folder.

## Install

1. Put `homrec.exe` in the HomRec folder (with the `.dll` files and `ffmpeg\`).
2. Optional: add that folder to your `PATH` so `homrec` works from any terminal. (Windows: Settings > System > About >
   Advanced system settings > Environment Variables > Path > Edit > New.) HomRec does not do this for you yet.
3. Open a new terminal and run `homrec --version`.

**Check:** it prints the HomRec version. If the terminal says `'homrec' is not recognized`, the folder is not on `PATH`:
run it with the full path, or add the folder.

## Commands

```
homrec                           In a console: menu (start / overlays / setup)
homrec --version                 Show the HomRec version
homrec setup                     First-run setup again
homrec start [config-path]       Record the screen; Ctrl+C stops and saves
homrec pause                     Pause / resume the running recording
homrec config path               Show the .hrc config homrec is bound to
homrec config change <path>      Bind homrec to another .hrc config
homrec overlay list              List the overlays of the active scene
homrec overlay add <type> ...    text | image | gif | webcam | input
homrec overlay set <row|id|name> key=value ...
homrec overlay show|hide|remove <row|id|name>
homrec overlay edit              Interactive overlay editor
homrec help | --help | -h
```

## Your first recording from the terminal

1. Run `homrec`. On the very first run (in a console, not in a script) it asks a few questions: *Use this config?*,
   the **folder for recordings**, the **format** (MP4 / MKV / MKV + MP4) and **frames per second** (15 / 30 / 60). Answer
   with Up/Down and Enter; Esc skips. Run `homrec setup` to repeat it.
2. Run `homrec start`. A small panel shows the config, output folder and overlays. The status line shows
   `REC 00:01:23 | 42.5 MB | 60 fps`.
3. Press **Ctrl+C** to stop. A bar animates while the file is saved. Closing the console window also stops and saves.
4. Open the recordings folder.

**Check:** the exit code is `0` after a normal save and `1` on any error. In PowerShell: `$LASTEXITCODE`.

<!-- IMG: cli-start-recording -->

### Pause

Run `homrec pause` in **another** terminal; run it again to resume. It only controls a recording started by
`homrec start`, not one started from the HomRec window. In full-GPU (ddagrab) mode, pause works only when `ddagrab_pause`
is on.

### Overlays

`homrec overlay ...` edits the overlays of the active scene (the `scenes\scene_*.hrc` files). **Close the HomRec window
first**: a running window keeps its own copy and would overwrite your changes. Examples:

```
homrec overlay add text "Hello" x=50 y=40 color=#FFAA00 opacity=80
homrec overlay add image C:\pics\logo.png w=300 h=120
homrec overlay set 1 x=100 y=20 opacity=50
homrec overlay hide logo
```

## Which config is used

By default `homrec.hrc` in the folder of `homrec.exe`. `homrec config change <path>` binds another file (saved in
`cfg\homrec_cli.cfg`). `homrec start <config-path>` uses a config for that run only. Only point it at configs you trust:
a config can contain custom ffmpeg arguments and a post-record hook.

## Limits

- Only one `homrec start` at a time.
- No live preview, no scheduled start, and **no Instant Replay** from the CLI.
- The countdown from the config is honoured.

## Logs

Everything goes to `logs\cli\` next to `homrec.exe`: `cli.log` (commands, setup answers, start/stop, exit code) and the
engine logs. They do not mix with the `logs\` folder of `hr.exe`.

## What can fail

| Symptom | Cause | Fix |
|---|---|---|
| `'homrec' is not recognized` | Not on `PATH`. | Add the folder or use the full path. |
| No setup questions appear | stdin/stdout is redirected (a script), or setup already done. | Run `homrec setup` in a real console. |
| `FFmpeg not found` style error | Same ffmpeg rules as the main program. | [getting-started.md](getting-started.md#2-pick-a-download) |
| `homrec pause` does nothing | No `homrec start` is running, or the recording was started from the window. | Start with `homrec start`. |
| Overlay changes lost | HomRec's window was open and wrote its copy back. | Close the window, then edit. |
| Garbled colours / no bar | The console has no ANSI support. | Use Windows Terminal, or accept plain output. |
| Exit code `1` after a recording | An error, or the encoder died and only part was saved. | Read `logs\cli\cli.log`. |
