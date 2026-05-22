/*
 * audio_mixer.h — HomRec 2.0
 * Real-time stereo audio mixer: mic + desktop → single PCM stream.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    float mic_vol;   /* [0.0, 1.0] */
    float sys_vol;   /* [0.0, 1.0] */
    float peak_l;    /* last frame peak, normalised [0,1], left  */
    float peak_r;    /* last frame peak, normalised [0,1], right */
} AudioMixerCtx;

void homrec_mixer_init(AudioMixerCtx* ctx, float mic_vol, float sys_vol);
void homrec_mixer_set_volume(AudioMixerCtx* ctx, float mic_vol, float sys_vol);

void homrec_mixer_mix(
    AudioMixerCtx* ctx,
    const int16_t* mic_pcm,  int mic_frames,
    const int16_t* sys_pcm,  int sys_frames,
    int16_t*       out_pcm,  int out_frames);

void homrec_mixer_peak(const AudioMixerCtx* ctx, float* out_l, float* out_r);
void homrec_mono_to_stereo(const int16_t* mono, int16_t* stereo, int frame_count);
void homrec_apply_gain_s16(int16_t* pcm, int sample_count, float gain);

#ifdef __cplusplus
}
#endif
