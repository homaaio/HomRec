# Instant Replay

Keep the last N seconds in memory and save them when something good happens, without recording all the time.

## How it works

1. A **second, separate ffmpeg encoder** runs in the background and writes short segments of the screen.
2. By default the segments are kept **in RAM** (the buffer is limited to `replay_ram_mb`, default **1024 MB**). Buffers
   longer than **300 seconds** stay on disk because RAM use grows with the length.
3. Press the **Save Replay** hotkey (default `F8`). HomRec stops adding to the buffer for a moment, cuts the last N
   seconds, mixes the microphone and desktop audio for that window, joins everything with ffmpeg and writes the file,
   then buffering resumes.
4. While a **normal recording** is running, Instant Replay is **paused** ("false while recording"); it resumes
   automatically after you stop. The *Enable* choice survives.

Extra audio sources from the Audio Mixer are **not** included in a replay.

## Set it up, step by step

1. **Settings > Advanced**.
2. Set **Replay buffer (sec, 0 = off)**, for example `30`. `0` turns the feature off.
3. Tick **Enable Instant Replay**.
4. Check the hotkey in **Settings > Hotkeys** (default `F8`).
5. Click OK. Instant Replay starts buffering right away.

<!-- IMG: hr-replay-settings -->

**Check:** wait longer than the buffer length, press `F8`, and open the Recordings folder: a new replay file should
be there, about as long as the buffer. The log shows lines starting with `Instant Replay:`.

## Memory and disk keys

In `homrec.hrc`, section `[recording_extra]`:

| Key | Meaning | Default |
|---|---|---|
| `replay_buffer_sec` | Buffer length in seconds | set in Settings > Advanced |
| `replay_in_ram` | Keep the buffer in RAM | on |
| `replay_ram_mb` | RAM limit for the buffer | 1024 |
| `instant_replay_enabled` | The Enable checkbox | off |

## What can fail

| Message / symptom | Meaning | Fix |
|---|---|---|
| "Failed to start the Instant Replay encoder." | The background ffmpeg could not start. | Check ffmpeg ([getting-started.md](getting-started.md#2-pick-a-download)) and the codec ([codecs.md](codecs.md)). |
| "Instant Replay isn't running." | The hotkey was pressed while it is off or paused for a recording. | Tick *Enable Instant Replay*; stop any manual recording first. |
| "Instant Replay buffer is empty (nothing captured yet)." | Pressed too early, or the screen produced no frames. | Wait a few seconds after enabling; check that the capture preview works. |
| "Couldn't write the replay buffer to the temp folder (disk full?)." | No room in the temp location. | Free disk space on the system drive. |
| "Couldn't save the replay (ffmpeg concat failed)." | Joining the segments failed. | Check the log around `Instant Replay:` and ffmpeg's exit code ([recording-troubleshooting.md](recording-troubleshooting.md)). |
| `Instant Replay: failed to resume buffering ...` in the log | Buffering could not restart after a save or a recording. | Toggle *Enable Instant Replay* off and on. |
| The replay is shorter than the buffer | The buffer had not filled yet. | Wait longer before saving. |
| High RAM use | The RAM buffer is working as designed. | Lower the buffer length or `replay_ram_mb`. |
