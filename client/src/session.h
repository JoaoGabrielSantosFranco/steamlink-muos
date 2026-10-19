#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "util.h"

typedef struct sl_session sl_session;

enum { SL_CODEC_OPUS = 3, SL_CODEC_H264 = 4 };

/* Called from the network thread. */
typedef struct {
    void *ctx;
    void (*video_start)(void *ctx, int codec, int width, int height);
    void (*video_frame)(void *ctx, const uint8_t *data, size_t len, bool keyframe);
    void (*audio_start)(void *ctx, int codec, int rate, int channels);
    void (*audio_frame)(void *ctx, const uint8_t *data, size_t len);
    void (*rumble)(void *ctx, uint16_t low, uint16_t high, uint32_t ms);
} sl_callbacks;

/* Gamepad state in SDL_GameController order: axes LX, LY, RX, RY, LT, RT; buttons bit = SDL button index. */
typedef struct {
    int16_t axes[6];
    uint16_t buttons;
} sl_gamepad;

enum { SL_PHASE_CONNECTING, SL_PHASE_HANDSHAKE, SL_PHASE_STREAMING, SL_PHASE_ENDED };

sl_session *session_start(const sl_config *cfg, const char *ip, int port, const uint8_t *key, size_t keylen,
                          const sl_callbacks *cb);
/* Sends Disconnect, joins the network thread and frees everything. */
void session_stop(sl_session *s);
int session_phase(sl_session *s);
/* Last EStreamActivity reported by the PC (4 = secure desktop: lock screen / UAC, which Steam can't capture). */
enum { SL_ACTIVITY_SECURE_DESKTOP = 4 };
int session_activity(sl_session *s);
/* Human readable reason once the phase is SL_PHASE_ENDED. */
void session_reason(sl_session *s, char *out, size_t len);
void session_set_gamepad(sl_session *s, const sl_gamepad *pad);
void session_request_keyframe(sl_session *s);
