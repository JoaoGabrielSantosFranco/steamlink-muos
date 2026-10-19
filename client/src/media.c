#include "media.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>

#include "util.h"

#define QUEUE_LEN 16

typedef struct {
    uint8_t *data;
    size_t len;
    bool key;
} queued;

struct video_dec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame, *latest;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    queued q[QUEUE_LEN];
    int head, count;
    bool stop, wait_key;
    int decoded, dropped;
    void (*need_keyframe)(void *);
    void *cb_ctx;
};

static void *video_thread(void *arg) {
    video_dec *v = arg;
    for (;;) {
        pthread_mutex_lock(&v->lock);
        while (!v->count && !v->stop) pthread_cond_wait(&v->cond, &v->lock);
        if (v->stop) {
            pthread_mutex_unlock(&v->lock);
            break;
        }
        queued item = v->q[v->head];
        v->head = (v->head + 1) % QUEUE_LEN;
        v->count--;
        pthread_mutex_unlock(&v->lock);

        v->pkt->data = item.data;
        v->pkt->size = (int) item.len;
        if (item.key) v->pkt->flags |= AV_PKT_FLAG_KEY;
        else v->pkt->flags &= ~AV_PKT_FLAG_KEY;
        int r = avcodec_send_packet(v->ctx, v->pkt);
        free(item.data);
        if (r < 0 && r != AVERROR(EAGAIN)) {
            LOG("video: decode error %d, asking for keyframe", r);
            pthread_mutex_lock(&v->lock);
            v->wait_key = true;
            pthread_mutex_unlock(&v->lock);
            v->need_keyframe(v->cb_ctx);
            continue;
        }
        while (avcodec_receive_frame(v->ctx, v->frame) == 0) {
            pthread_mutex_lock(&v->lock);
            if (v->latest) v->dropped++;
            else v->latest = av_frame_alloc();
            av_frame_unref(v->latest);
            av_frame_move_ref(v->latest, v->frame);
            v->decoded++;
            pthread_mutex_unlock(&v->lock);
        }
    }
    return NULL;
}

video_dec *video_dec_create(void (*need_keyframe)(void *ctx), void *ctx) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return NULL;
    video_dec *v = calloc(1, sizeof(*v));
    v->need_keyframe = need_keyframe;
    v->cb_ctx = ctx;
    v->ctx = avcodec_alloc_context3(codec);
    v->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    v->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    v->ctx->thread_type = FF_THREAD_SLICE;
    v->ctx->thread_count = 4;
    if (avcodec_open2(v->ctx, codec, NULL) < 0) {
        avcodec_free_context(&v->ctx);
        free(v);
        return NULL;
    }
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    v->wait_key = true;
    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->cond, NULL);
    pthread_create(&v->thread, NULL, video_thread, v);
    return v;
}

void video_dec_destroy(video_dec *v) {
    if (!v) return;
    pthread_mutex_lock(&v->lock);
    v->stop = true;
    pthread_cond_signal(&v->cond);
    pthread_mutex_unlock(&v->lock);
    pthread_join(v->thread, NULL);
    for (int i = 0; i < v->count; i++) free(v->q[(v->head + i) % QUEUE_LEN].data);
    av_frame_free(&v->latest);
    av_frame_free(&v->frame);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->ctx);
    pthread_mutex_destroy(&v->lock);
    pthread_cond_destroy(&v->cond);
    free(v);
}

void video_dec_push(video_dec *v, const uint8_t *data, size_t len, bool keyframe) {
    if (!v || !len) return;
    bool ask = false;
    pthread_mutex_lock(&v->lock);
    if (keyframe) v->wait_key = false;
    if (v->wait_key) {
        pthread_mutex_unlock(&v->lock);
        return;
    }
    if (v->count == QUEUE_LEN) { /* decoder can't keep up: flush and restart from a keyframe */
        for (int i = 0; i < v->count; i++) free(v->q[(v->head + i) % QUEUE_LEN].data);
        v->dropped += v->count;
        v->count = 0;
        v->wait_key = true;
        ask = true;
    } else {
        queued *q = &v->q[(v->head + v->count) % QUEUE_LEN];
        q->data = malloc(len + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(q->data, data, len);
        memset(q->data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        q->len = len;
        q->key = keyframe;
        v->count++;
        pthread_cond_signal(&v->cond);
    }
    pthread_mutex_unlock(&v->lock);
    if (ask) {
        LOG("video: decoder queue overflow, asking for keyframe");
        v->need_keyframe(v->cb_ctx);
    }
}

AVFrame *video_dec_take(video_dec *v) {
    if (!v) return NULL;
    pthread_mutex_lock(&v->lock);
    AVFrame *f = v->latest;
    v->latest = NULL;
    pthread_mutex_unlock(&v->lock);
    return f;
}

void video_dec_stats(video_dec *v, int *decoded, int *dropped) {
    pthread_mutex_lock(&v->lock);
    *decoded = v->decoded;
    *dropped = v->dropped;
    pthread_mutex_unlock(&v->lock);
}

struct audio_dec {
    AVCodecContext *ctx;
    AVPacket *pkt;
    AVFrame *frame;
    int channels;
    int16_t *pcm;
    int pcm_cap;
    void (*out)(void *, const int16_t *, int);
    void *cb_ctx;
};

audio_dec *audio_dec_create(int rate, int channels, void (*out)(void *ctx, const int16_t *pcm, int frames),
                            void *ctx) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return NULL;
    audio_dec *a = calloc(1, sizeof(*a));
    a->ctx = avcodec_alloc_context3(codec);
    a->ctx->sample_rate = rate;
    av_channel_layout_default(&a->ctx->ch_layout, channels);
    if (avcodec_open2(a->ctx, codec, NULL) < 0) {
        avcodec_free_context(&a->ctx);
        free(a);
        return NULL;
    }
    a->channels = channels;
    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    a->out = out;
    a->cb_ctx = ctx;
    return a;
}

void audio_dec_destroy(audio_dec *a) {
    if (!a) return;
    av_frame_free(&a->frame);
    av_packet_free(&a->pkt);
    avcodec_free_context(&a->ctx);
    free(a->pcm);
    free(a);
}

static int16_t to_s16(float f) {
    f *= 32767.0f;
    return (int16_t) (f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : f);
}

void audio_dec_push(audio_dec *a, const uint8_t *data, size_t len) {
    if (!a || !len) return;
    uint8_t buf[4096 + AV_INPUT_BUFFER_PADDING_SIZE];
    if (len > 4096) return;
    memcpy(buf, data, len);
    memset(buf + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    a->pkt->data = buf;
    a->pkt->size = (int) len;
    if (avcodec_send_packet(a->ctx, a->pkt) < 0) return;
    while (avcodec_receive_frame(a->ctx, a->frame) == 0) {
        int n = a->frame->nb_samples, ch = a->frame->ch_layout.nb_channels;
        if (ch < 1) ch = a->channels;
        int outch = a->channels;
        if (n * outch > a->pcm_cap) {
            a->pcm_cap = n * outch;
            a->pcm = realloc(a->pcm, sizeof(int16_t) * (size_t) a->pcm_cap);
        }
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < outch; c++) {
                int sc = c < ch ? c : ch - 1;
                float sample = 0;
                switch (a->frame->format) {
                    case AV_SAMPLE_FMT_FLTP: sample = ((float *) a->frame->extended_data[sc])[i]; break;
                    case AV_SAMPLE_FMT_FLT: sample = ((float *) a->frame->data[0])[i * ch + sc]; break;
                    case AV_SAMPLE_FMT_S16: sample = ((int16_t *) a->frame->data[0])[i * ch + sc] / 32768.0f; break;
                    case AV_SAMPLE_FMT_S16P: sample = ((int16_t *) a->frame->extended_data[sc])[i] / 32768.0f; break;
                    default: break;
                }
                a->pcm[i * outch + c] = to_s16(sample);
            }
        }
        a->out(a->cb_ctx, a->pcm, n);
    }
}
