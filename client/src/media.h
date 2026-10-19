#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct AVFrame;

typedef struct video_dec video_dec;

/* need_keyframe is invoked from the decode thread when the stream has to restart from a keyframe. */
video_dec *video_dec_create(void (*need_keyframe)(void *ctx), void *ctx);
void video_dec_destroy(video_dec *v);
/* Queues an Annex-B access unit; safe to call from any thread. */
void video_dec_push(video_dec *v, const uint8_t *data, size_t len, bool keyframe);
/* Latest decoded frame (caller owns it, av_frame_free when done) or NULL. */
struct AVFrame *video_dec_take(video_dec *v);
void video_dec_stats(video_dec *v, int *decoded, int *dropped);

typedef struct audio_dec audio_dec;

/* out receives interleaved S16 PCM. */
audio_dec *audio_dec_create(int rate, int channels, void (*out)(void *ctx, const int16_t *pcm, int frames),
                            void *ctx);
void audio_dec_destroy(audio_dec *a);
void audio_dec_push(audio_dec *a, const uint8_t *data, size_t len);
