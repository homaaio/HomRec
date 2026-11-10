# Video codecs and hardware encoders (HomRec)

This document explains what a video codec is and why HomRec requires one, how to select a codec for your computer, which encoder HomRec uses and when it picks one by itself, and what to do when something goes wrong (including GPU encoder failures: NVENC, AMF, QSV).

The relevant options are on **Settings > Video & Codec**: `Video codec` (referred to as `Codec` below) and `HW accel`. The picture quality is adjusted with the quality slider on the **General** tab.

## What a codec is and why it is needed

A screen recording is a huge stream of pictures: an uncompressed 1920x1080 video at 60 frames per second is hundreds of megabytes every second. Nobody can store or upload that. A **codec** is the method that compresses the video into a file of a sensible size, and decompresses it again when you play it.

- The part that compresses (while recording) is the **encoder**. HomRec's `Codec` setting determines the encoder.
- The part that decompresses (while playing or editing) is the **decoder**. It resides in your player, browser, TV or video editor.

There are two kinds of encoder:

| Kind | Runs on | Names in HomRec | Advantages | Drawbacks |
|------|---------|-----------------|------------|-----------|
| Software | The CPU | `libx264`, `libx265`, `mpeg4` | Works on any PC. | Loads the CPU. In a game on a weak CPU this can cause stutter or dropped frames. |
| Hardware | A dedicated encoding block on the GPU | `*_nvenc` (NVIDIA), `*_qsv` (Intel), `*_amf` (AMD) | Very little CPU load, so games are affected less. | Needs the matching GPU and a recent enough driver. At the same file size the picture may be a little less efficient than a slow software preset. |

How to read a name: the first part is the video standard (`h264` or `hevc`, that is H.265), the second part is who does the work (`nvenc` = NVIDIA, `qsv` = Intel, `amf` = AMD). `libx264` and `libx265` are the software implementations of H.264 and H.265.

## Codecs available in HomRec

| Codec | Standard | Runs on | Notes |
|-------|----------|---------|-------|
| `libx264` | H.264 | CPU | The default. Plays everywhere. Works on any PC. |
| `libx265` | H.265 (HEVC) | CPU | Smaller files than H.264 at similar quality, but needs a lot more CPU time and not every player or editor can open it. |
| `h264_nvenc` | H.264 | NVIDIA GPU | The usual choice on NVIDIA cards. |
| `hevc_nvenc` | H.265 | NVIDIA GPU | Needs a newer NVIDIA card than H.264 does. |
| `h264_qsv` | H.264 | Intel GPU | Intel only (integrated graphics or Intel Arc). |
| `hevc_qsv` | H.265 | Intel GPU | Needs a newer Intel generation than H.264 does. |
| `h264_amf` | H.264 | AMD GPU | AMD Radeon only. |
| `hevc_amf` | H.265 | AMD GPU | Needs a newer Radeon than H.264 does. |
| `mpeg4` | MPEG-4 Part 2 | CPU | An old codec. Files are much larger than H.264 for the same picture. Use it only if you must play the video on a very old device. |

The `Codec` box can also be typed into, but HomRec builds its encoder settings only for the codecs listed above. Other names are not supported.

A hardware encoder must match your GPU vendor. An encoder for another vendor fails to start (see "Common problems" below).

## How to choose a codec for yourself

1. **Find out what GPU you have.** Open Task Manager > Performance > GPU (or run `dxdiag` and look at the Display tab).
2. **Pick by the situation:**

   | Your situation | Choose |
   |----------------|--------|
   | You just want it to work, or you are not sure. | `libx264` and leave `HW accel` on `auto`. HomRec then uses your GPU encoder if one works, and the CPU otherwise (see below). |
   | You record games, or the CPU is weak or busy. | The hardware H.264 encoder for your GPU: `h264_nvenc` (NVIDIA), `h264_amf` (AMD) or `h264_qsv` (Intel). |
   | You want smaller files and every device you use can play H.265. | `hevc_*` for your GPU, or `libx265` if you have a strong CPU and do not record games at the same time. |
   | You will upload or send the video to other people, or edit it in a program you have not checked. | H.264 (`libx264` or `h264_*`), pixel format `yuv420p`, MP4. This is the combination that opens almost everywhere. |
   | The PC may crash, freeze or lose power during long recordings. | Any codec, with the output format set to MKV (see below). |

3. **Check that it works.** On the Video & Codec tab press **Test Recording (5s)**. HomRec records five seconds with the selected codec and reports the average FPS and the number of dropped frames. If it warns that the machine struggled to keep up, choose a hardware encoder, a faster preset, or a lower resolution or FPS, and test again.

## Settings that go together with the codec

| Setting | Where | What it does |
|---------|-------|--------------|
| Quality slider (0-100) | General | Higher means a better picture and a larger file. For `libx264`, `libx265`, NVENC and AMF it is converted to a quantizer value (100 is the best quality, 0 the lowest). For QSV it is converted to a target bitrate that also depends on resolution and FPS. |
| Encoder preset | Video & Codec | Used by `libx264` and `libx265` only. `ultrafast` uses the least CPU and makes the biggest files; slower presets (`medium`, `slow`, ...) make smaller files and use more CPU. Hardware encoders ignore it. |
| Pixel format | Video & Codec | Leave it at `yuv420p`. `yuv444p` and the 10-bit formats (`yuv420p10le`, `p010le`) are not played by many players, browsers and TVs. |
| Output format | Video & Codec | MP4 plays everywhere, but an MP4 left unfinished by a crash or power loss may be unusable. MKV survives a crash. "Both" records MKV and then also saves an MP4 copy. |
| Custom FFmpeg args | Video & Codec | Extra ffmpeg arguments for experts. Leave empty unless you know what you need; arguments written for one codec can break another. |
| GPU colour conversion | Video & Codec | On by default. With a hardware encoder the picture is converted on the GPU, which lowers CPU use. |
| Game-friendly priority, Intel QSV low-power mode, Lower GPU priority | Video & Codec | Options for recording games on weak PCs. Each one is off by default. Read the tooltip of each: for example, lowering the GPU priority can make the recording drop frames when the GPU is 100% busy. |

## What HomRec does by itself

| Situation | Behaviour | Modifies your `Codec` setting? |
|-----------|-----------|--------------------------------|
| First run only | HomRec inspects the installed video cards and proposes a default codec (a hardware encoder for the detected GPU, or `libx264` if there is none). | Only on that first run, as a default. |
| Every launch with `HW accel` = `auto` (the default) | HomRec attempts `h264_nvenc`, then `h264_amf`, then `h264_qsv` (up to 10 s each) using a very short test clip. If one succeeds **and `Codec` is set to `libx264`**, recordings are made with that GPU encoder. If none succeeds, recordings use `libx264`. | No. The swap happens only while recording. |
| Every launch with `Codec` set to a hardware encoder | HomRec initialises that encoder once (a 0.2 s check). If it fails, the cause is written to the log and the status bar displays a notice. | **No.** The encoder remains the one you selected. |
| While recording | If the hardware encoder stops working, the recording ends and the error is reported as usual (see `recording-troubleshooting.md`). There is no automatic restart with `libx264`. | No. |

Before 3.1 build 1, a hardware encoder that failed the startup check was quietly swapped for `libx264` for the entire session, and a recording whose encoder failed within the first 5 seconds was resumed in a new file using `libx264`. Both automatic substitutions have been removed: if the encoder you selected does not work, you are informed and nothing is changed without your knowledge.

## Retaining full control

- Choose the encoder you want in `Codec` (for instance `h264_nvenc`). It is used exactly as selected.
- Set `HW accel` to `none` to disable the automatic GPU detection. Then `Codec` = `libx264` genuinely means software `libx264`.
- Keep `HW accel` on `auto` and `Codec` on `libx264` if you want HomRec to use the GPU when possible and the CPU otherwise.

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

## Common problems

| Problem | Likely cause | What to do |
|---------|--------------|------------|
| Recording fails with `ffmpeg exit code -40` (`ENOSYS`). | The hardware encoder could not be opened: the driver is too old, the encoder is for a GPU you do not have (for example `h264_qsv` on an AMD or NVIDIA PC), or the ffmpeg build lacks it. | Make sure the encoder matches your GPU vendor, then follow "When NVENC (or AMF, QSV) fails" below. Until it works, select `libx264`. |
| A notice about the encoder appears in the status bar at startup. | The startup check of the encoder you selected failed. HomRec does not change your setting. | Read the cause in `logs\homrec.log` (`Startup: ... does not work on this system (...)`), fix it, or select another codec yourself. |
| Recording fails with `-22` (`EINVAL`). | The codec does not accept some argument, often one from `Custom FFmpeg args` that was written for another codec. | Clear `Custom FFmpeg args`. If it still fails, select `libx264`. |
| The video does not play, or the picture is black, in a browser, a TV, a phone or an editor. | The player cannot decode the format: usually H.265 (`libx265`, `hevc_*`), or a pixel format other than `yuv420p`. | Record again with an H.264 codec, pixel format `yuv420p`, MP4. For an existing H.265 file, try a different player (for example VLC) or convert it. |
| The game stutters or loses FPS while recording. | A software encoder is taking CPU time from the game, or the GPU is overloaded. | Use the hardware encoder of your GPU. Lower the recording resolution or FPS. Use a faster preset (`ultrafast`) if you stay on `libx264`. Try **Game-friendly priority**. Check with **Test Recording (5s)**. |
| The log says `Recording overloaded`, or frames are dropped. | The encoder cannot keep up with the resolution and FPS. | Same as above. If **Lower GPU priority** is on, turn it off: it can cause drops when the GPU is fully busy. |
| The files are too large. | A high quality setting, a fast preset, or an inefficient codec. | Lower the quality slider a little. With `libx264` or `libx265`, choose a slower preset (needs more CPU). Or switch to an H.265 codec if all your players support it. |
| The hardware encoder works in one program but not in HomRec. | A laptop with two GPUs where ffmpeg runs on the integrated GPU, or a different ffmpeg build. | See cause 3 and cause 2 below. Check `ffmpeg\ffmpeg.exe -version`. |
| The recording is not recoverable after a crash or power loss. | The file was written as MP4, which can be unusable if it was never finished. | Set the output format to MKV (or "Both") in Settings > Video & Codec. |

If your problem is not here, see `recording-troubleshooting.md` for the list of ffmpeg exit codes, and attach `logs\homrec.log` to your report.

## When NVENC (or AMF, QSV) fails

The usual causes, most frequent first:

1. **The GPU driver is too old for this ffmpeg build.** NVENC requires a driver that supports the NVENC version the ffmpeg build was compiled against. HomRec's own message for exit code `-40` states that NVENC needs NVIDIA driver 570.0 or newer. Check your driver version (`nvidia-smi` in a console displays it) and update it from nvidia.com or GeForce Experience.
2. **This ffmpeg build lacks that encoder**, or it is built for a newer driver than you can install. Use a different ffmpeg build (a slightly older one often works with an older driver) and place it next to `hr.exe`, or specify its path in Settings.
3. **A laptop with two GPUs.** Ensure ffmpeg runs on the NVIDIA GPU rather than the integrated one.

To see the exact cause, run the following in a console from the folder containing `ffmpeg.exe`:

    ffmpeg -hide_banner -f lavfi -i nullsrc=s=1280x720:d=0.1 -c:v h264_nvenc -f null -

A driver that is too old typically responds with a line stating that the driver does not support the required NVENC API version, along with both version numbers. For AMD use `h264_amf`, for Intel `h264_qsv` in the same command.

Until the encoder works, select `libx264` in `Codec` yourself. It is slower and consumes more CPU, but it always works.