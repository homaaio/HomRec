# Video codecs and hardware encoders (HomRec)

This document describes which video encoder HomRec uses, when it selects one
on its own, and what to do if a GPU encoder (NVENC, AMF, QSV) fails.

The relevant options are under **Settings > Video**: `Codec` and `HW accel`.

## What HomRec does on its own

| Situation | Behaviour | Modifies your `Codec` setting? |
|-----------|-----------|--------------------------------|
| First run only | HomRec inspects the installed video cards and proposes a default codec (a hardware encoder for the detected GPU, or `libx264` if there is none). | Only on that first run, as a default. |
| Every launch with `HW accel` = `auto` (the default) | HomRec attempts `h264_nvenc`, then `h264_amf`, then `h264_qsv` (up to 10 s each) using a very short test clip. If one succeeds **and `Codec` is set to `libx264`**, recordings are made with that GPU encoder. If none succeeds, recordings use `libx264`. | No. The swap happens only while recording. |
| Every launch with `Codec` set to a hardware encoder | HomRec initialises that encoder once (a 0.2 s check). If it fails, the cause is written to the log and the status bar displays a notice. | **No.** The encoder remains the one you selected. |
| While recording | If the hardware encoder stops working, the recording ends and the error is reported as usual (see `recording-troubleshooting.md`). There is no automatic restart with `libx264`. | No. |

Before 3.1 build 1, a hardware encoder that failed the startup check was
quietly swapped for `libx264` for the entire session, and a recording whose
encoder failed within the first 5 seconds was resumed in a new file using
`libx264`. Both automatic substitutions have been removed: if the encoder you
selected does not work, you are informed and nothing is changed without your
knowledge.

## Retaining full control

- Choose the encoder you want in `Codec` (for instance `h264_nvenc`). It is
  used exactly as selected.
- Set `HW accel` to `none` to disable the automatic GPU detection. Then
  `Codec` = `libx264` genuinely means software `libx264`.
- Keep `HW accel` on `auto` and `Codec` on `libx264` if you want HomRec to use
  the GPU when possible and the CPU otherwise.

## Log entries

All entries are written to `logs\homrec.log`.

| Entry | Meaning |
|-------|---------|
| `Startup: GPU encoder available (h264_nvenc) -- recordings will use it ...` | `auto` detected a working GPU encoder. |
| `Startup: no working GPU encoder found ... will fall back to software libx264` | `auto` detected none: recordings use `libx264`. |
| `Startup: hardware encoder available (h264_nvenc).` | The encoder you selected passed the check. |
| `Startup: h264_nvenc does not work on this system (...) -- the codec stays as chosen` | The check failed. The cause is shown in the brackets. Recording with it will fail in the same way until the cause is resolved. |
| `Recording: encoding with libx264 (software)` / `... with h264_nvenc (hardware)` | The encoder actually used for this recording. |
| `ffmpeg exit code -40 - ENOSYS: the hardware encoder is unavailable` | ffmpeg was unable to open the hardware encoder. See below. |

## When NVENC (or AMF, QSV) fails

The usual causes, most frequent first:

1. **The GPU driver is too old for this ffmpeg build.** NVENC requires a
   driver that supports the NVENC version the ffmpeg build was compiled
   against. HomRec's own message for exit code `-40` states that NVENC needs
   NVIDIA driver 570.0 or newer. Check your driver version (`nvidia-smi` in a
   console displays it) and update it from nvidia.com or GeForce Experience.
2. **This ffmpeg build lacks that encoder**, or it is built for a newer driver
   than you can install. Use a different ffmpeg build (a slightly older one
   often works with an older driver) and place it next to `hr.exe`, or specify
   its path in Settings.
3. **A laptop with two GPUs.** Ensure ffmpeg runs on the NVIDIA GPU rather
   than the integrated one.

To see the exact cause, run the following in a console from the folder
containing `ffmpeg.exe`:

    ffmpeg -hide_banner -f lavfi -i nullsrc=s=1280x720:d=0.1 -c:v h264_nvenc -f null -

A driver that is too old typically responds with a line stating that the
driver does not support the required NVENC API version, along with both
version numbers. For AMD use `h264_amf`, for Intel `h264_qsv` in the same
command.

Until the encoder works, select `libx264` in `Codec` yourself. It is slower
and consumes more CPU, but it always works.