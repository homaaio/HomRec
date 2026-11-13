# Troubleshooting by symptom

Find your symptom, follow the steps **in order**, and stop when it works. Not solved? Collect the files listed in
[logs-and-diagnostics.md](logs-and-diagnostics.md#preparing-a-bug-report) and open an issue.

## Step 0: before anything else

1. Note the exact message (screenshot it).
2. Open `logs\homrec.log` and read the last 30 lines.
3. Check the HomRec version (**Help > About**) and Windows version (`winver`).
4. Try the same thing with default settings: rename `homrec.hrc` to `homrec.hrc.bak` and restart.

## Index

- [HomRec does not start or closes immediately](#homrec-does-not-start-or-closes-immediately)
- [Start does nothing or shows an error](#start-does-nothing-or-shows-an-error)
- [The recording is black, frozen or shows the wrong thing](#the-recording-is-black-frozen-or-shows-the-wrong-thing)
- [There is no sound, or the wrong sound](#there-is-no-sound-or-the-wrong-sound)
- [The recording stutters or the game loses FPS](#the-recording-stutters-or-the-game-loses-fps)
- [The file is missing, damaged, huge or tiny](#the-file-is-missing-damaged-huge-or-tiny)
- [Hotkeys do not work](#hotkeys-do-not-work)
- [Window and overlay problems](#window-and-overlay-problems)
- [Plugins, console and hom](#plugins-console-and-hom)
- [Updates and installation](#updates-and-installation)
- [Update the graphics driver](#update-the-graphics-driver)

## HomRec does not start or closes immediately

| Check | What it means | What to do |
|---|---|---|
| Is it already running? | Only one copy runs; a second launch exits at once. | Look in the system tray. |
| SmartScreen or antivirus message | New unsigned program; some products flag it falsely. | **More info > Run anyway**; add an exclusion for the folder. |
| "...dll was not found" | A runtime DLL next to `hr.exe` is missing. | Re-extract the **whole** archive. Do not copy `hr.exe` alone. |
| It starts and crashes straight away | A crash. | Open `crashes\` for a `.dmp` and `logs\homrec.log` for the last lines. Attach both to a bug report. |
| Built from source, runs only on the build PC | Wrong runtime DLLs (MSVCRT vs UCRT) or a missing DLL on the other PC. | Ship the UCRT64 DLLs that match your toolchain ([building.md](building.md)). |
| Settings might be corrupt | A bad `homrec.hrc`. | Rename it; restart. |
| It is in `C:\Program Files` | The folder is not writable, so settings and logs cannot be saved. | Move it to a writable folder. |

Run `python tools\homrec_check.py --launch` for an automatic check ([logs-and-diagnostics.md](logs-and-diagnostics.md#self-check-script)).

## Start does nothing or shows an error

| Message | Cause | Fix |
|---|---|---|
| "FFmpeg not found." | No `ffmpeg.exe` in a known place. The **portable** (non-"full") archive does not include it. | [getting-started.md](getting-started.md#2-pick-a-download) |
| "Output folder doesn't exist and couldn't be created." | Bad path or no rights. | Pick a folder in your profile. |
| "Not enough free disk space ... need at least 200 MB." | Disk almost full. | Free space or change drive. |
| "Already recording." / "The previous recording is still being saved" | State not finished. | Wait until the status is idle. |
| "The window you selected ... is not on any screen" | The window is closed or minimised. | [capture-sources.md](capture-sources.md) |
| "Failed to start the ffmpeg process." | ffmpeg cannot run (blocked by antivirus, damaged file). | [recording-troubleshooting.md](recording-troubleshooting.md) |
| "Failed to start the capture pipeline." | The capture engine could not start (remote session, graphics driver). | See the next section. |
| The hotkey does nothing | See [Hotkeys](#hotkeys-do-not-work). | |

## The recording is black, frozen or shows the wrong thing

1. **Is the preview also black?** If yes, it is a capture problem, not an encoder problem.
2. Remote Desktop or a virtual display? Capture fails there. Record on the physical session.
3. Wrong monitor? Double-click the first row of the Layers panel and choose the monitor again.
4. Laptop with two GPUs: set HomRec to use the other GPU in Windows Settings > System > Display > Graphics. Also
   [update the graphics driver](#update-the-graphics-driver).
5. Window source? Use the **Test: find it now** button; read [capture-sources.md](capture-sources.md#recording-one-window).
6. Program running as administrator? Start HomRec as administrator too.
7. Protected (DRM) video is black in every recorder.
8. Picture froze after a game changed resolution: stop and start again.
9. Still wrong: turn on **Window capture logging** and read `logs\wgc.log`.

Nothing here is specific to a driver version that was tested for this guide. Mention your GPU and driver in a report.

## There is no sound, or the wrong sound

Follow [audio.md](audio.md). The three quickest checks:

1. Do the level bars in the Audio Mixer move?
2. Is the strip muted?
3. Is **Settings > Audio > Microphone** the device you speak into? Use **Test**.

## The recording stutters or the game loses FPS

First find out which side is slow:

- In the log, **`Recording overloaded`** or a status warning that the machine "struggled to keep up" means HomRec
  dropped frames.
- If the **game** loses FPS but the recording is fine, the game is short of CPU or GPU time.

Then try, **one at a time**, testing after each:

1. Use a hardware encoder (`HW accel` = `auto` or NVENC/AMF/QSV) so the CPU is free ([codecs.md](codecs.md)).
2. Lower the **Target FPS** or the **Resolution**.
3. With a software encoder, choose a faster preset (`ultrafast` is the default).
4. Keep **GPU colour conversion** on.
5. **Game-friendly priority** (Video & Codec tab): lowers HomRec's thread priority so the game keeps its CPU time.
6. On Intel integrated graphics: **Intel QSV low-power mode** and **Lower GPU priority of the recorder**. If the log
   says `Recording overloaded`, turn the second one off.
7. Lower the preview cost: **Settings > Security > Live Preview** has *Preview resolution* (Low 50% / Medium 75% /
   High 100%) and *Preview FPS*.
8. Record to a fast drive, not the one the game is installed on.

Do not trust a setting just because it sounds fast. Measure: record 60 seconds, then compare the log's frame counts
and the game's FPS before and after.

## The file is missing, damaged, huge or tiny

| Symptom | Cause | Fix |
|---|---|---|
| Cannot find the file | The *Output folder* is somewhere else. | **File > Open Recordings Folder**. |
| `.mp4` will not open after a crash | MP4 is only complete when it is finished. | Record as **MKV** (survives crashes) or *Both*. Try `ffmpeg -i in.mp4 -c copy out.mkv`; it may not recover. |
| File is much smaller than the time you recorded | The encoder stopped early. | Read the log for `ffmpeg exit code`. [recording-troubleshooting.md](recording-troubleshooting.md) |
| File is huge | CRF too low, or a hardware encoder at high quality. | Raise **CRF** (for example 23), or lower resolution/FPS. |
| Video has no sound, a `.wav` sits next to it | The audio merge failed. | [how-it-works.md](how-it-works.md#what-can-go-wrong-at-stop) |
| Recording never ends | Stop was pressed while saving. | Wait for the status to become idle. |
| Long sessions (hours) | A single huge file is one point of failure. | Record in MKV, set **Auto-stop** (Settings > Advanced), and check the result of a short test first. |

## Hotkeys do not work

1. Defaults: `F9` start/stop, `F10` pause, `F11` fullscreen of HomRec's window, `F8` save replay.
2. Another program may use the same key: change it in **Settings > Hotkeys**.
3. Keyboards where the F-keys are media keys need `Fn`.
4. A hotkey that cannot be parsed is rejected; use a form like `Ctrl+Shift+R`.
5. HomRec must be running (tray is fine). A recording started by `homrec start` is controlled from its terminal.

## Window and overlay problems

See [capture-sources.md](capture-sources.md) and [overlays-and-scenes.md](overlays-and-scenes.md). The most common:

| Symptom | Fix |
|---|---|
| Panels disappeared or are stuck | **View > Reset layout**, and untick **Lock layout** (Settings > System). |
| Overlay shows in preview, not in the file | Untick the full-GPU ddagrab option. |
| Window recorded as a screen crop although "window itself" is set | **Try the native Windows capture again...** in the Capture source dialog. |

## Plugins, console and hom

| Symptom | Fix |
|---|---|
| A plugin does nothing | Turn on plugin logging and read `logs\plugins.log` ([plugins.md](plugins.md)). |
| `blocked - needs the "inwid" ...` | Add the `inwid` prefix ([Commands.md](../commands/Commands.md#the-inwid-confirmation-prefix)). |
| `hom` is not found | [../hom/troubleshooting.md](../hom/troubleshooting.md) |

## Updates and installation

| Symptom | Fix |
|---|---|
| "No installer was found in that release" | The release has no Setup `.exe`; download from the release page. |
| A portable copy cannot update itself | Replace the files by hand and keep `homrec.hrc`, `presets\`, `scenes\`, `plugins\`, `cfg\`. |
| The update downloads but does not install | Close HomRec from the tray and run the downloaded Setup manually. |
| After an update settings look reset | A new version may add keys; restore from your backup of `homrec.hrc`. |

## Update the graphics driver

A hardware encoder (NVENC, AMF, Quick Sync) and screen capture both depend on the graphics driver. If a hardware
encoder fails, or the capture is black on a PC that should work, install the newest driver for your card from the
maker:

1. Find your card: `Win+R`, type `dxdiag`, open the **Display** tab.
2. Download the driver from the maker's site and install it:
   <!-- LINK: driver-nvidia -->
   <!-- LINK: driver-amd -->
   <!-- LINK: driver-intel -->
3. Restart the PC, then start HomRec again.

Still stuck? [logs-and-diagnostics.md](logs-and-diagnostics.md#preparing-a-bug-report).
