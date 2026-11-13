# Settings reference

Open with **Settings > Preferences**. There are seven tabs. Changes are written to `homrec.hrc` next to `hr.exe` when you
click **Save**. The path of the file is shown in **Settings > Advanced > Settings Storage**.

<!-- IMG: hr-settings-tabs -->

## General

| Setting | What it does |
|---|---|
| Output folder | Where recordings are saved. Must be writable and have at least 200 MB free. |
| Quality | Overall quality choice for the recording. |
| Target FPS | Frame rate of the recording. |
| Resolution mode / Resolution | Record at native size (100%) or scale down. Lower resolution costs less CPU/GPU. |
| Countdown (3s) | Wait three seconds after pressing Start. |
| Timestamp / Cursor / Notification | Options for the timestamp, the mouse cursor in the picture, and the "recording saved" notice. |
| Language | Interface language. **Add language** loads a `.hrl` language file. |

## Video & Codec

| Setting | What it does |
|---|---|
| Video codec | `libx264`, `libx265` or a hardware encoder ([codecs.md](codecs.md)). |
| HW accel | `auto` tests NVENC, AMF and QSV; `none` turns the probe off. |
| Encoder preset | Speed/size trade-off (default `ultrafast`). |
| CRF (0-51, lower = better quality) | Quality target. Lower numbers give larger files (default 18). |
| Pixel format | Chroma format. Pick the default unless a player complains. |
| Output format (Container) | `mkv`, `mp4` or both ("Both" makes an `.mp4` copy after the `.mkv`). MKV survives a crash. |
| GPU colour conversion | Scale and convert on the GPU for hardware encoders. On by default. |
| Full-GPU capture via ffmpeg ddagrab | Experimental. Lowest CPU use; see the restrictions in [how-it-works.md](how-it-works.md#which-path-does-the-video-take). Unticks itself if a game changes the resolution. |
| Game-friendly priority | ffmpeg and capture threads take less CPU time from the game. Off by default. |
| Intel QSV low-power mode | `-low_power 1`. Ignored with custom ffmpeg arguments. Off by default. |
| Lower GPU priority of the recorder | Helps on shared integrated GPUs; can drop frames at 100% GPU load. Off by default. |

These switches are saved in `[video]` of the `.hrc` file and in presets. After a recording the tab shows frames
captured and dropped, and warns when the machine "struggled to keep up".

## Audio

Microphone device, Sample rate (Hz), AAC bitrate, Channels, **Also save audio as a separate MP3**, level-meter refresh
rate and mixer layout. **Sample rate, bitrate and channels apply for the session and travel with presets, but are not
yet written to the settings file.** Details: [audio.md](audio.md).

## Hotkeys

Start/Stop (`F9`), Pause (`F10`), Fullscreen for HomRec's own window (`F11`), Save Replay (`F8`). A binder below lets you
tie a key combo to any console command or `setting = value` line, for example `action "disable_preview"` -> `Ctrl+B`.

## Advanced

| Setting | What it does |
|---|---|
| Filename template (`{date}` / `{time}` / `{app}`) | Names the recording file. Default `HomRec_{date}_{time}`. |
| Auto-stop (min, 0 = off) | Stops the recording after N minutes. |
| Replay buffer (sec, 0 = off) + Enable Instant Replay | [instant-replay.md](instant-replay.md). |
| Settings Storage | Path of the `.hrc` file. `cfg\config.cfg`, if present, can layer overrides on top at every launch (`sethrc`, see [Commands.md](../commands/Commands.md)). |

## Security

| Setting | What it does |
|---|---|
| System logging | Writes `logs\pc.log`. |
| Plugin logging | Writes `logs\plugins.log`. |
| Window capture logging | Writes `logs\wgc.log`. |
| Live Preview | *Preview resolution* (Low 50% / Medium 75% / High 100%) and *Preview FPS*. |
| Reset | Puts **every field on every tab** back to defaults. Nothing is written until you click Save. |

## System

| Setting | What it does |
|---|---|
| Create a shortcut | Desktop shortcut. |
| Lock the panel layout | Freezes the Controls, Audio Mixer and Overlays panels. Also View > Lock layout, or `ui_locked = 1`. |
| Launch HomRec when Windows starts | Startup entry. |
| Enable tray icon (minimize to tray) | Keeps HomRec in the tray. |

Shortcut creation and the startup entry are applied as soon as you click Save.

## Settings that exist only as keys

These have no checkbox. Change them from the console (`Ctrl+Shift+T`) or by editing `homrec.hrc`.

| Section | Keys |
|---|---|
| `[recording_extra]` | `auto_pause_on_silence`, `silence_threshold_db`, `silence_duration_sec`, `scheduled_start_enabled`, `scheduled_start_time`, `post_record_hook_enabled`, `post_record_hook_type`, `post_record_hook_path`, `replay_in_ram`, `replay_ram_mb` |
| `[video]` | `ddagrab_pause`, `mf_encoder` (experimental in-process Media Foundation encoder) |

## Presets

**File > Set Preset...** manages named profiles in `presets\`. A preset may be a partial file (for example, only the
video block). **File > Export/Import .hrc** copies the whole settings file.

## What can go wrong

| Symptom | Cause | Fix |
|---|---|---|
| Settings do not stick | The program folder is not writable (for example `C:\Program Files`). | Move HomRec to a writable folder. |
| Sample rate / bitrate go back to defaults after restart | Not stored in `.hrc` yet (see Audio). | Set them each session, or in a preset. |
| Odd behaviour after editing `.hrc` | A typo in a key. | Rename `homrec.hrc` to let HomRec create a fresh one. |
| Everything reset | **Reset** in the Security tab, then Save. | Restore a backup of `homrec.hrc` or a preset. |
