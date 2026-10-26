// mf_encoder.h - in-process hardware H.264 encoder (Windows Media Foundation Sink Writer).
//
// WHAT: replaces "pipeline -> pipe -> ffmpeg.exe -> NVENC/QSV/AMF -> .mp4" with
//       "pipeline -> MF Sink Writer -> hardware encoder MFT -> .mp4" inside the HomRec process:
//       no ffmpeg child process, no pipe, no 3 MB/frame copy into the pipe, and - when the capture
//       thread hands over the D3D11 NV12 texture (hr_mf_write_texture) - no GPU->CPU->GPU round trip.
//
// Without hr_mf_audio_configure() the output is a video-only .mp4 and the audio is merged afterwards by the ffmpeg
// merge step (hr_merge_av_ex). With it (item 10) the sink writer also gets an AAC audio stream that is fed from the
// live audio mixer, so the finished .mp4 already has its sound and nothing is merged after Stop.
//
// Threading: configure/start/end/wait/destroy come from the UI / finalize thread; the write calls
// come from the pipeline's single writer thread. Every handle is validated against an internal
// registry, so a stale handle is a harmless no-op instead of a crash.
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void  *hr_mf_create(void);
void   hr_mf_destroy(void *h);                       // finalizes a still-open file first

// out_path_utf8 must end in .mp4. kbps = target bitrate (peak = 2x). fps = constant output frame rate.
int    hr_mf_configure(void *h, const char *out_path_utf8, int w, int h_px, int fps, int kbps);

// Creates the sink writer + hardware encoder and starts writing. d3d11_device (ID3D11Device*, may be
// NULL) enables GPU-texture input when want_textures != 0. Returns 1 on success, 0 on failure
// (reason: hr_mf_last_error). Fails - on purpose - when only a SOFTWARE H.264 encoder is available.
int    hr_mf_start(void *h, void *d3d11_device, int want_textures);
int    hr_mf_textures_enabled(const void *h);        // 1 if the writer was built with a D3D manager

// ---- audio (item 10) ------------------------------------------------------------------------------
// Call BEFORE hr_mf_start(). Adds an AAC stream (kbps is rounded to 96/128/160/192) fed with 16-bit stereo PCM at
// rate_hz (44100 or 48000 only - what the Windows AAC encoder accepts). Returns 1 if accepted for the attempt;
// whether the writer really got the stream is hr_mf_audio_enabled() AFTER hr_mf_start() (0 = no AAC encoder, e.g.
// Windows N without the Media Feature Pack -> the caller falls back to the WAV + merge path; the video is unaffected).
int    hr_mf_audio_configure(void *h, int rate_hz, int kbps);
int    hr_mf_audio_enabled(const void *h);
// Audio sink callback (same shape as HrAudioSinkFn in audio.cpp; `h` is the encoder handle): `frames` stereo
// 16-bit frames, consecutive, the first one at the video's time zero. Called from the audio mixer thread. Audio
// that arrives before the first picture is held until the picture fixes the video's time zero.
void   hr_mf_write_audio(void *h, const int16_t *stereo, size_t frames);

// ---- writer thread ------------------------------------------------------------------------------
// `slot` = position on the constant-fps timeline (same numbering the pipeline's timestamped pipe uses).
// Slots skipped since the previous picture are filled by repeating the previous picture, so the file
// is constant frame rate. Return 1 = written, 0 = this picture was not written.
int    hr_mf_write_sysmem(void *h, const uint8_t *nv12, int w, int h_px, int64_t slot);
// tex = ID3D11Texture2D* (NV12, same device as hr_mf_start's). Returns 0 if the texture path does not
// work with this encoder (the caller then switches to hr_mf_write_sysmem for the rest of the recording).
int    hr_mf_write_texture(void *h, void *tex, int64_t slot);
// Repeats the last picture so that the timeline reaches `slot` (inclusive). Cheap; called while idle.
int    hr_mf_hold_until(void *h, int64_t slot);
// End of input: holds the last picture until end_slot-1, then finalizes (moov atom) in the background.
int    hr_mf_end(void *h, int64_t end_slot);

// ---- controller ---------------------------------------------------------------------------------
// Blocks until the file is finalized. 0 = ok, -1 = timeout, -2 = failed.
int    hr_mf_wait(void *h, int timeout_ms);
int    hr_mf_failed(const void *h);                  // 1 once the encoder stopped accepting frames
int    hr_mf_last_error(const void *h, char *buf, int buf_len);
double hr_mf_output_size_mb(const void *h);
long long hr_mf_frames_written(const void *h);       // samples submitted, including repeats

#ifdef __cplusplus
}
#endif
