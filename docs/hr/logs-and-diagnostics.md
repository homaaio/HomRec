# Logs and diagnostics

Where HomRec writes information, how to read it, and what to attach to a bug report.

## The log files

All logs are in `logs\` next to `hr.exe`.

| File | Contains | Always on? |
|---|---|---|
| `homrec.log` | The main log: startup, settings, recording steps, errors (`Recording: ...`, `Start failed: ...`, `Instant Replay: ...`). | Yes |
| `pc.log` | System information. | **Settings > Security > System logging** |
| `plugins.log` | Plugin events and Lua errors. | **Settings > Security > Plugin logging** |
| `wgc.log` | Window-capture details. | **Settings > Security > Window capture logging** |
| `ffmpeg_ddagrab.log` (+ a progress file) | ffmpeg output in full-GPU mode. | When ddagrab is used |
| `logs\cli\...` | Logs of the `homrec` terminal tool. | See [cli.md](cli.md) |

Crash reports go to `crashes\crash_<date>.dmp`.

The console (`Ctrl+Shift+T`) has a `log` command; for example `log clear --ffmpeg` clears the ddagrab log
([Commands.md](../commands/Commands.md)).

<!-- IMG: hr-logs-folder -->

The **View** menu has **Show Log** (a log viewer) and **PC Analytics**.

## Reading `homrec.log`

1. Open the file in a text editor and go to the **end**; the newest lines are last.
2. Search for `Failed`, `error`, `ffmpeg exit code`, `Recording overloaded`, `Start failed`.
3. Read the 10 lines **before** the first error: that is usually the cause.

Useful lines:

| Line (start) | Meaning |
|---|---|
| `Recording: encoding with ... (software)` / `(hardware)` | Which encoder was chosen. |
| `Recording: full-GPU capture + encode via ffmpeg ddagrab` | The experimental path is active. |
| `Recording overloaded` | The machine cannot keep up. |
| `ffmpeg exit code ...` | ffmpeg ended; see [recording-troubleshooting.md](recording-troubleshooting.md). |
| `Audio Mixer: ...` | Mixer source added, removed or failed. |
| `Capture source dialog: ...` | Window-matching events. |

## Self-check script

`tools\homrec_check.py` checks the PC and an installed copy: CPU/RAM, Windows version, GPU, disk space, ffmpeg,
settings, `cfg\` scripts, `plugins\`, logs. It needs Python 3.

```
python tools\homrec_check.py
python tools\homrec_check.py C:\path\to\hr.exe --launch
```

`--launch` also starts `hr.exe` and checks the log (a smoke test). Each line is `ok`, `warn` or `FAIL`. Its RAM and disk
thresholds (4 GB RAM minimum, 5 GB free disk) are stricter than the program's own limits.

## Preparing a bug report

Open an issue at <https://github.com/homaaio/HomRec/issues> and include:

1. HomRec version (**Help > About**) and whether it is the installer or a portable copy.
2. Windows version (`winver`) and GPU model and driver version.
3. What you did, what you expected, what happened.
4. `logs\homrec.log` (the whole file, or the last 200 lines).
5. For a crash: the newest file from `crashes\`.
6. For a recording problem: the codec, container and capture source you used.

**Before sharing**, open the log and remove anything private. Logs may contain file paths and window titles.

<!-- IMG: hr-about-dialog -->
