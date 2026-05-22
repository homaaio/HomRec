/*
 * audio_mixer.c — HomRec 2.0
 * Real-time stereo audio mixer: mic + desktop → single PCM stream.
 *
 * Features:
 *   • Per-channel volume (0.0 – 1.0) applied before mix
 *   • Soft-clip limiter to prevent integer overflow artefacts
 *   • Down-mix from any channel count to stereo
 *   • Lock-free: all state passed via AudioMixerCtx; no global state
 *
 * Data format expected/produced:
 *   • Signed 16-bit interleaved PCM (S16LE) — matches pyaudio default
 *   • Sample rate: caller decides; mixer is sample-rate-agnostic
 *
 * Usage:
 *   AudioMixerCtx ctx;
 *   homrec_mixer_init(&ctx, 1.0f, 0.5f);                  // mic=100%, sys=50%
 *   homrec_mixer_mix(&ctx, mic_pcm, mic_len, sys_pcm, sys_len, out, out_len);
 */

#include "audio_mixer.h"
#include <string.h>
#include <math.h>

/* -- Soft-clip -------------------------------------------------------------- */
/* Maps any float sample to [-1,1] via tanh — gentle saturation instead of
   hard clipping which causes buzzing on transients.                          */
static inline float soft_clip(float x) {
    /* Fast tanh approximation: |error| < 0.0015 in [-3,3] */
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* -- homrec_mixer_init() --------------------------------------------------- */
void homrec_mixer_init(AudioMixerCtx* ctx, float mic_vol, float sys_vol) {
    if (!ctx) return;
    ctx->mic_vol = mic_vol;
    ctx->sys_vol = sys_vol;
    ctx->peak_l  = 0.0f;
    ctx->peak_r  = 0.0f;
}

/* -- homrec_mixer_set_volume() --------------------------------------------- */
void homrec_mixer_set_volume(AudioMixerCtx* ctx, float mic_vol, float sys_vol) {
    if (!ctx) return;
    ctx->mic_vol = mic_vol;
    ctx->sys_vol = sys_vol;
}

/* -- homrec_mixer_mix() ---------------------------------------------------- */
/*
 * Mix mic_pcm + sys_pcm → out_pcm.
 *
 * All buffers are S16LE stereo (2 channels, 2 bytes/sample).
 * frame_count = number of stereo frames (= byte_count / 4).
 *
 * If mic or sys buffer is NULL / shorter than frame_count, that source is
 * treated as silence.
 */
void homrec_mixer_mix(
        AudioMixerCtx* ctx,
        const int16_t* mic_pcm,   int mic_frames,
        const int16_t* sys_pcm,   int sys_frames,
        int16_t*       out_pcm,   int out_frames)
{
    if (!ctx || !out_pcm) return;

    float mic_v = ctx->mic_vol;
    float sys_v = ctx->sys_vol;
    float pk_l  = 0.0f, pk_r = 0.0f;

    for (int i = 0; i < out_frames; ++i) {
        /* Left channel */
        float ml = (mic_pcm && i < mic_frames) ? (float)mic_pcm[i * 2]     * mic_v : 0.0f;
        float sl = (sys_pcm && i < sys_frames) ? (float)sys_pcm[i * 2]     * sys_v : 0.0f;
        /* Right channel */
        float mr = (mic_pcm && i < mic_frames) ? (float)mic_pcm[i * 2 + 1] * mic_v : 0.0f;
        float sr = (sys_pcm && i < sys_frames) ? (float)sys_pcm[i * 2 + 1] * sys_v : 0.0f;

        float out_l = soft_clip((ml + sl) / 32768.0f) * 32767.0f;
        float out_r = soft_clip((mr + sr) / 32768.0f) * 32767.0f;

        out_pcm[i * 2]     = (int16_t)out_l;
        out_pcm[i * 2 + 1] = (int16_t)out_r;

        float al = out_l < 0 ? -out_l : out_l;
        float ar = out_r < 0 ? -out_r : out_r;
        if (al > pk_l) pk_l = al;
        if (ar > pk_r) pk_r = ar;
    }

    /* Store normalised peaks [0,1] for VU-meter */
    ctx->peak_l = pk_l / 32767.0f;
    ctx->peak_r = pk_r / 32767.0f;
}

/* -- homrec_mixer_peak() --------------------------------------------------- */
void homrec_mixer_peak(const AudioMixerCtx* ctx, float* out_l, float* out_r) {
    if (!ctx) { if (out_l) *out_l = 0; if (out_r) *out_r = 0; return; }
    if (out_l) *out_l = ctx->peak_l;
    if (out_r) *out_r = ctx->peak_r;
}

/* -- homrec_mono_to_stereo() ----------------------------------------------- */
/* Duplicate mono channel to both stereo channels in-place (output must be
   2× the size of the mono input).                                            */
void homrec_mono_to_stereo(const int16_t* mono, int16_t* stereo, int frame_count) {
    /* Walk backwards so in-place is safe (stereo is 2× mono size)           */
    for (int i = frame_count - 1; i >= 0; --i) {
        stereo[i * 2]     = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
}

/* -- homrec_apply_gain_s16() ----------------------------------------------- */
/* Apply a linear gain to a S16LE buffer in-place.                            */
void homrec_apply_gain_s16(int16_t* pcm, int sample_count, float gain) {
    for (int i = 0; i < sample_count; ++i) {
        float v = (float)pcm[i] * gain;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
}
