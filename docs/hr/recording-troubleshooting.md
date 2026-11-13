# Recording troubleshooting (HomRec)

This document describes the error messages HomRec writes to `logs\homrec.log`
during recording, and what to do about each one.

## How to read the log

A recording error looks like this:

    [ERROR] Recording: the ffmpeg encoder died mid-recording - ffmpeg exit code -40

The important part is the code `-40`. It is the exit code of the ffmpeg process,
and it tells you the cause.

To see the latest recording events:

    findstr /i "Recording ffmpeg ERROR WARN" logs\homrec.log

## ffmpeg exit codes

On Windows, HomRec and ffmpeg report exit codes of two kinds.

### 1. Negative codes (POSIX errno, ffmpeg `AVERROR`)

| Code | Name   | Meaning                                              | What to do |
|------|--------|------------------------------------------------------|------------|
| -13  | EACCES | No access to the output file or folder              | Choose a folder in your own profile (for example `C:\Users\<name>\Videos`). Folders of other users and system folders do not work. Check that the path exists. |
| -2   | ENOENT | File or folder not found                             | Create the output folder or choose an existing one. Check that `ffmpeg.exe` is next to `hr.exe` or in `ffmpeg\`. |
| -22  | EINVAL | Invalid argument (often the codec or one of its options) | Switch the codec to `libx264` and clear `Custom ffmpeg args`. |
| -28  | ENOSPC | The disk is full                                     | Free up space or choose a different drive. |
| -32  | EPIPE  | Broken pipe: ffmpeg stopped reading frames          | ffmpeg usually failed earlier. Look at the line before it in the log. |
| -40  | ENOSYS | Function not implemented: ffmpeg could not open the hardware encoder | See "Hardware encoders" below. This happens with NVENC, QSV and AMF alike. |

### 2. Windows NTSTATUS codes (shown as negative numbers)

| Code (log)  | Hex        | Meaning                                    | What to do |
|-------------|------------|--------------------------------------------|------------|
| -1073741819 | 0xC0000005 | Access violation (invalid memory access)    | Update the GPU driver first. If that does not help, switch to `libx264` and send the log with your report. |
| -1073741515 | 0xC0000135 | A required DLL was not found               | A DLL next to `ffmpeg.exe` is missing. Use a complete ffmpeg build with its `bin` folder. |
| -1073741795 | 0xC000001D | Illegal instruction                        | The CPU does not support the instruction set of this ffmpeg build. Use another build or `libx264`. |
| -1073740791 | 0xC0000409 | Stack buffer overrun (fast-fail)           | Send the log and the ffmpeg version with your report. |

## Hardware encoders (nvenc / qsv / amf)

The encoder is chosen in **Settings > Video & Codec > Video codec** and stored in `homrec.hrc`
(`video_codec`, `hw_accel`, `qsv_low_power`).

Matching GPU and encoder:

| GPU                    | Encoder        |
|------------------------|----------------|
| NVIDIA                 | `h264_nvenc`   |
| AMD                    | `h264_amf`     |
| Intel (integrated)     | `h264_qsv`     |

Important: `h264_qsv` works only on Intel. An encoder for the wrong vendor fails
with `-40`. A hardware encoder can also fail with `-40` on a correct vendor when
the driver is too old for the ffmpeg build (see the NVENC section below).

### NVENC (NVIDIA) fails with -40

ffmpeg returns `ENOSYS` when the NVIDIA driver does not support the NVENC API
version that the ffmpeg build expects. The fix is to update the driver.

1. Check the driver version:

       nvidia-smi --query-gpu=name,driver_version --format=csv

2. Update the driver from NVIDIA (GeForce Experience or nvidia.com/drivers) and reboot.
3. Test NVENC by hand from the folder with `hr.exe`:

       ffmpeg\ffmpeg.exe -hide_banner -f lavfi -i testsrc=duration=3:size=1920x1080:rate=30 -c:v h264_nvenc -y test_nvenc.mp4

   The command prints the reason on failure, for example `driver does not support the required nvenc API version`.
   If it exits without error and creates `test_nvenc.mp4`, NVENC works.

If the driver cannot be updated, use `libx264` (software encoder, works everywhere).

### Choosing the encoder

1. Open **Settings > Video & Codec** and pick the encoder for your GPU.
2. If it fails, pick `libx264`. It runs on the CPU and always works.
3. Turn off `qsv_low_power` if you use QSV.

## Checking ffmpeg

    ffmpeg\ffmpeg.exe -version

- It must be a 64-bit build of version 6 or newer (`ddagrab` needs 6+).
- The `configuration` line must contain `--enable-libx264`, and for hardware encoding
  the matching option: `--enable-nvenc`, `--enable-libvpl` (QSV) or `--enable-amf`.

## Log messages worth understanding

| Log message | Meaning |
|-------------|---------|
| `writing a frame to the encoder's stdin pipe failed` | ffmpeg has already died. The cause is in the `ffmpeg exit code` line. |
| `the audio did not reach ffmpeg completely` | A consequence of the ffmpeg crash. The end of the sound may be missing. |
| `Startup: no working GPU encoder found` | The hardware encoder probe failed. Recording will use `libx264`. |
| `full-GPU (ddagrab) capture is switched off` | ddagrab mode was turned off for the rest of the session after a failure. |

## If nothing helps

Attach to your report:
- the last 200 lines of `logs\homrec.log` and `logs\pc.log`;
- the output of `ffmpeg -version`;
- the output of `nvidia-smi` (or `dxdiag`, section "Display", for other GPUs);
- the error code from the tables above and the time of the recording.