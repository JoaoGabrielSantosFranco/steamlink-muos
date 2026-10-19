/* Native Steam Link client for muOS handhelds (Anbernic RG40XXH). */
#include <SDL2/SDL.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/pixfmt.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "discovery.h"
#include "font.h"
#include "media.h"
#include "pairing.h"
#include "session.h"
#include "util.h"

typedef struct {
    sl_config cfg;
    bool headless;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    int tex_w, tex_h;
    SDL_AudioDeviceID audio;
    bool audio_playing;
    audio_dec *adec;
    pthread_mutex_t audio_lock;
    long audio_frames;
    video_dec *video;
    sl_session *session;
    pthread_mutex_t session_lock; /* the decode thread reads `session`; teardown clears it */
    SDL_GameController *pad;
    atomic_uint rumble_lo, rumble_hi, rumble_ms;
    atomic_bool rumble_pending;
    char dump_path[PATH_MAX];
    bool dumped;
} app_t;

/* ---------------------------------------------------------------- drawing */

static void draw_text(SDL_Renderer *r, int x, int y, int scale, const char *text) {
    SDL_Rect rects[512];
    int n = 0;
    for (const char *c = text; *c; c++, x += 6 * scale) {
        const uint8_t *g = font_glyph(*c);
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] >> row & 1) {
                    rects[n++] = (SDL_Rect) {x + col * scale, y + row * scale, scale, scale};
                    if (n == 512) {
                        SDL_RenderFillRects(r, rects, n);
                        n = 0;
                    }
                }
    }
    if (n) SDL_RenderFillRects(r, rects, n);
}

/* Word-wraps text into the screen width and draws it centred. */
static void draw_wrapped(SDL_Renderer *r, int w, int y, int scale, const char *text) {
    int max_chars = (w - 32) / (6 * scale);
    char line[128];
    const char *p = text;
    while (*p) {
        int len = (int) strlen(p), cut = len;
        if (len > max_chars) {
            cut = max_chars;
            while (cut > 0 && p[cut] != ' ') cut--;
            if (cut == 0) cut = max_chars;
        }
        snprintf(line, sizeof(line), "%.*s", cut, p);
        draw_text(r, (w - (int) strlen(line) * 6 * scale) / 2, y, scale, line);
        y += 10 * scale;
        p += cut;
        while (*p == ' ') p++;
    }
}

static void show_screen(app_t *a, const char *title, const char *msg, const char *hint) {
    if (a->headless) {
        LOG("[screen] %s | %s | %s", title, msg ? msg : "", hint ? hint : "");
        return;
    }
    int w, h;
    SDL_GetRendererOutputSize(a->ren, &w, &h);
    SDL_SetRenderDrawColor(a->ren, 23, 26, 33, 255);
    SDL_RenderClear(a->ren);
    SDL_SetRenderDrawColor(a->ren, 102, 192, 244, 255);
    draw_wrapped(a->ren, w, h / 6, 4, title);
    SDL_SetRenderDrawColor(a->ren, 220, 222, 223, 255);
    if (msg) draw_wrapped(a->ren, w, h / 6 + 70, 2, msg);
    SDL_SetRenderDrawColor(a->ren, 140, 150, 160, 255);
    if (hint) draw_wrapped(a->ren, w, h - 50, 2, hint);
    SDL_RenderPresent(a->ren);
}

/* ---------------------------------------------------------------- callbacks */

static void cb_need_keyframe(void *ctx) {
    app_t *a = ctx;
    pthread_mutex_lock(&a->session_lock);
    if (a->session) session_request_keyframe(a->session);
    pthread_mutex_unlock(&a->session_lock);
}

static void cb_video_start(void *ctx, int codec, int w, int h) {
    (void) ctx;
    LOG("video stream: codec %d %dx%d", codec, w, h);
}

static FILE *h264_out;

static void cb_video_frame(void *ctx, const uint8_t *data, size_t len, bool key) {
    app_t *a = ctx;
    if (h264_out) fwrite(data, 1, len, h264_out);
    video_dec_push(a->video, data, len, key);
}

static void on_pcm(void *ctx, const int16_t *pcm, int frames) {
    app_t *a = ctx;
    a->audio_frames += frames;
    if (!a->audio) return;
    const Uint32 bytes_per_frame = 4, queued = SDL_GetQueuedAudioSize(a->audio);
    if (queued > 48000 / 8 * bytes_per_frame) return; /* >125 ms behind: drop to stay low-latency */
    SDL_QueueAudio(a->audio, pcm, (Uint32) frames * bytes_per_frame);
    if (!a->audio_playing && SDL_GetQueuedAudioSize(a->audio) >= 48000 / 25 * bytes_per_frame) {
        SDL_PauseAudioDevice(a->audio, 0);
        a->audio_playing = true;
    }
}

static void cb_audio_start(void *ctx, int codec, int rate, int channels) {
    app_t *a = ctx;
    pthread_mutex_lock(&a->audio_lock);
    if (!a->adec && codec == SL_CODEC_OPUS) a->adec = audio_dec_create(rate, 2, on_pcm, a);
    pthread_mutex_unlock(&a->audio_lock);
    LOG("audio stream: codec %d %dHz x%d decoder=%s", codec, rate, channels, a->adec ? "ok" : "none");
}

static void cb_audio_frame(void *ctx, const uint8_t *data, size_t len) {
    app_t *a = ctx;
    pthread_mutex_lock(&a->audio_lock);
    audio_dec_push(a->adec, data, len);
    pthread_mutex_unlock(&a->audio_lock);
}

static void cb_rumble(void *ctx, uint16_t lo, uint16_t hi, uint32_t ms) {
    app_t *a = ctx;
    atomic_store(&a->rumble_lo, lo);
    atomic_store(&a->rumble_hi, hi);
    atomic_store(&a->rumble_ms, ms);
    atomic_store(&a->rumble_pending, true);
}

/* ---------------------------------------------------------------- input */

/* If muOS didn't export a mapping for the built-in pad, fall back to its stock "muOS-Keys" layout so the
 * buttons (and the exit chord) still work. */
static void ensure_mapping(int index) {
    if (SDL_IsGameController(index)) return;
    char guid[64], name[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    snprintf(name, sizeof(name), "%s", SDL_JoystickNameForIndex(index) ? SDL_JoystickNameForIndex(index) : "pad");
    for (char *c = name; *c; c++)
        if (*c == ',') *c = ' ';
    char map[512];
    snprintf(map, sizeof(map),
             "%s,%s,a:b3,b:b2,x:b5,y:b4,leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,righttrigger:b9,guide:b12,"
             "start:b11,back:b10,dpup:h0.1,dpleft:h0.8,dpright:h0.2,dpdown:h0.4,leftx:a0,lefty:a1,rightx:a2,"
             "righty:a3,leftstick:b13,rightstick:b14,",
             guid, name);
    LOG("joystick %d has no mapping, applying muOS-Keys layout: %d", index, SDL_GameControllerAddMapping(map));
}

static void try_open_pad(app_t *a, int index) {
    if (a->pad) return;
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    LOG("joystick %d: '%s' guid=%s gamecontroller=%d", index, SDL_JoystickNameForIndex(index), guid,
        SDL_IsGameController(index));
    ensure_mapping(index);
    if (SDL_IsGameController(index)) a->pad = SDL_GameControllerOpen(index);
    if (a->pad) LOG("using controller '%s'", SDL_GameControllerName(a->pad));
}

static void open_pads(app_t *a) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) try_open_pad(a, i);
}

/* Shared hot-plug handling for every event loop. */
static void handle_device_event(app_t *a, const SDL_Event *e) {
    if (e->type == SDL_JOYDEVICEADDED) try_open_pad(a, e->jdevice.which);
    if (e->type == SDL_CONTROLLERDEVICEREMOVED && a->pad &&
        e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(a->pad))) {
        SDL_GameControllerClose(a->pad);
        a->pad = NULL;
    }
}

/* Pumps events on the status screens; true if the user pressed B / SELECT / Esc or the system asked to quit. */
static bool poll_quit(app_t *a) {
    bool quit = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        handle_device_event(a, &e);
        if (e.type == SDL_QUIT) quit = true;
        if (e.type == SDL_CONTROLLERBUTTONDOWN &&
            (e.cbutton.button == SDL_CONTROLLER_BUTTON_B || e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK))
            quit = true;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = true;
    }
    return quit;
}

/* ---------------------------------------------------------------- background connect */

typedef struct {
    app_t *a;
    bool discover;
    volatile int cancel;
    atomic_bool done;
    sl_host hosts[8];
    int n;
    const char *ip;
    int port, rc;
    uint8_t key[32];
    size_t keylen;
    char reason[200];
} connect_job;

static void *connect_thread(void *arg) {
    connect_job *j = arg;
    if (j->discover)
        j->n = discover_hosts(j->a->cfg.device_id, j->hosts, 8, 2500, &j->cancel);
    else
        j->rc = request_stream(&j->a->cfg, j->ip, &j->port, j->key, &j->keylen, j->reason, sizeof(j->reason),
                               &j->cancel);
    atomic_store(&j->done, true);
    return NULL;
}

static void show_screen(app_t *a, const char *title, const char *msg, const char *hint);

/* Runs a blocking network step off the UI thread so B still works. False if the user asked to quit. */
static bool run_job(app_t *a, connect_job *j, const char *title, const char *msg) {
    pthread_t t;
    pthread_create(&t, NULL, connect_thread, j);
    bool quit = false;
    uint64_t next_draw = 0;
    while (!atomic_load(&j->done)) {
        if (!a->headless) {
            if (now_ms() >= next_draw) {
                show_screen(a, title, msg, "B: sair");
                next_draw = now_ms() + 250;
            }
            if (poll_quit(a)) {
                quit = true;
                j->cancel = 1;
            }
        }
        SDL_Delay(20);
    }
    pthread_join(t, NULL);
    return !quit;
}

static void read_pad(app_t *a, sl_gamepad *gp) {
    memset(gp, 0, sizeof(*gp));
    if (!a->pad) return;
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < 16; b++)
        if (SDL_GameControllerGetButton(a->pad, (SDL_GameControllerButton) b)) gp->buttons |= (uint16_t) (1u << b);
    for (int ax = 0; ax < 6; ax++) gp->axes[ax] = SDL_GameControllerGetAxis(a->pad, (SDL_GameControllerAxis) ax);
}

enum { CHOICE_QUIT, CHOICE_RETRY, CHOICE_PAIR };

/* A = retry, Y = pair again, B = quit. */
static int wait_choice(app_t *a) {
    if (a->headless) return CHOICE_QUIT;
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            handle_device_event(a, &e);
            if (e.type == SDL_QUIT) return CHOICE_QUIT;
            if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_A || e.cbutton.button == SDL_CONTROLLER_BUTTON_START)
                    return CHOICE_RETRY;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_Y) return CHOICE_PAIR;
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_B || e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
                    return CHOICE_QUIT;
            }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_RETURN) return CHOICE_RETRY;
                if (e.key.keysym.sym == SDLK_ESCAPE) return CHOICE_QUIT;
            }
        }
        SDL_Delay(16);
    }
}

/* ---------------------------------------------------------------- pairing */

static void show_pin(app_t *a, const char *pin) {
    if (a->headless) return;
    int w, h;
    SDL_GetRendererOutputSize(a->ren, &w, &h);
    SDL_SetRenderDrawColor(a->ren, 23, 26, 33, 255);
    SDL_RenderClear(a->ren);
    SDL_SetRenderDrawColor(a->ren, 102, 192, 244, 255);
    draw_wrapped(a->ren, w, h / 10, 3, "Parear com o PC");
    int scale = w / 60 > 4 ? w / 60 : 4;
    SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 255);
    draw_text(a->ren, (w - (int) strlen(pin) * 6 * scale) / 2, h / 10 + 50, scale, pin);
    SDL_SetRenderDrawColor(a->ren, 220, 222, 223, 255);
    draw_wrapped(a->ren, w, h / 10 + 70 + 8 * scale, 2,
                 "No PC: Steam > Configuracoes > Remote Play > Parear Steam Link, e digite este PIN.");
    SDL_SetRenderDrawColor(a->ren, 140, 150, 160, 255);
    draw_wrapped(a->ren, w, h - 40, 2, "B: cancelar");
    SDL_RenderPresent(a->ren);
}

/* Pairs this handheld through Steam and writes device.json; false if the user quit. */
static bool run_pairing(app_t *a, const char *config_path, const char *dir) {
    static char cafile[PATH_MAX + 16], name[64];
    snprintf(cafile, sizeof(cafile), "%s/cacert.pem", dir);
    const char *env = getenv("SL_DEVICE_NAME");
    if (env && *env) snprintf(name, sizeof(name), "%s", env);
    else if (gethostname(name, sizeof(name)) != 0 || !name[0]) snprintf(name, sizeof(name), "muOS");
    for (char *c = name; *c; c++)
        if (*c == '"' || *c == '\\' || (unsigned char) *c < 32) *c = '_';
    for (;;) {
        static pair_job job;
        memset(&job, 0, sizeof(job));
        job.config_path = config_path;
        job.cafile = cafile;
        job.device_name = name;
        pthread_t t;
        pthread_create(&t, NULL, pair_run, &job);
        bool logged_pin = false;
        while (atomic_load(&job.phase) != PAIR_DONE && atomic_load(&job.phase) != PAIR_FAILED) {
            if (atomic_load(&job.phase) == PAIR_SHOW_PIN) {
                if (!logged_pin) LOG("pairing PIN: %s (enter it on the PC)", job.pin);
                logged_pin = true;
                show_pin(a, job.pin);
            } else {
                show_screen(a, "Parear com o PC", job.message, "B: sair");
            }
            if (!a->headless && poll_quit(a)) job.cancel = 1;
            SDL_Delay(50);
        }
        pthread_join(t, NULL);
        if (atomic_load(&job.phase) == PAIR_DONE) {
            show_screen(a, "Pareado!", "Este aparelho agora esta autorizado no Steam do PC.", NULL);
            if (!a->headless) SDL_Delay(1500);
            return true;
        }
        if (job.cancel) return false;
        show_screen(a, "Pareamento falhou", job.message, "A: tentar de novo   B: sair");
        if (wait_choice(a) != CHOICE_RETRY) return false;
    }
}

/* ---------------------------------------------------------------- video output */

static void dump_frame(app_t *a, const AVFrame *f) {
    FILE *out = fopen(a->dump_path, "wb");
    if (!out) return;
    fprintf(out, "P5\n%d %d\n255\n", f->width, f->height);
    for (int y = 0; y < f->height; y++) fwrite(f->data[0] + y * f->linesize[0], 1, (size_t) f->width, out);
    fclose(out);
    a->dumped = true;
    LOG("dumped luma of first frame to %s", a->dump_path);
}

static void present_frame(app_t *a, AVFrame *f, bool show_hint, bool secure_desktop) {
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        static bool warned;
        if (!warned) LOG("unsupported pixel format %d", f->format);
        warned = true;
        return;
    }
    if (!a->tex || a->tex_w != f->width || a->tex_h != f->height) {
        if (a->tex) SDL_DestroyTexture(a->tex);
        a->tex = SDL_CreateTexture(a->ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, f->width, f->height);
        a->tex_w = f->width;
        a->tex_h = f->height;
        LOG("video texture %dx%d", f->width, f->height);
    }
    SDL_UpdateYUVTexture(a->tex, NULL, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2],
                         f->linesize[2]);
    int w, h;
    SDL_GetRendererOutputSize(a->ren, &w, &h);
    SDL_Rect dst;
    if ((long) w * f->height > (long) h * f->width) {
        dst.h = h;
        dst.w = (int) ((long) h * f->width / f->height);
    } else {
        dst.w = w;
        dst.h = (int) ((long) w * f->height / f->width);
    }
    dst.x = (w - dst.w) / 2;
    dst.y = (h - dst.h) / 2;
    SDL_SetRenderDrawColor(a->ren, 0, 0, 0, 255);
    SDL_RenderClear(a->ren);
    SDL_RenderCopy(a->ren, a->tex, NULL, &dst);
    if (show_hint) {
        SDL_SetRenderDrawColor(a->ren, 255, 255, 255, 255);
        draw_text(a->ren, 8, h - 20, 2, "SEGURE SELECT+START PARA SAIR");
    }
    if (secure_desktop) {
        SDL_SetRenderDrawColor(a->ren, 255, 210, 80, 255);
        draw_wrapped(a->ren, w, h / 2 - 20, 2, "O PC esta na tela de bloqueio/UAC do Windows, que o Steam nao transmite. "
                                               "Desbloqueie o PC.");
    }
    SDL_RenderPresent(a->ren);
}

/* ---------------------------------------------------------------- stream loop */

/* Returns true if the user asked to quit the app, false to go back to the retry screen. */
static bool run_stream(app_t *a, const char *ip, const char *name, int seconds, char *reason, size_t rlen) {
    char msg[200];
    snprintf(msg, sizeof(msg), "%s (%s). O Steam no PC pode pedir confirmacao.", name, ip);
    static connect_job job;
    memset(&job, 0, sizeof(job));
    job.a = a;
    job.ip = ip;
    if (!run_job(a, &job, "Conectando", msg)) return true;
    if (job.rc != 0) {
        snprintf(reason, rlen, "%s", job.reason);
        return false;
    }
    int port = job.port;
    size_t keylen = job.keylen;
    uint8_t key[32];
    memcpy(key, job.key, sizeof(key));

    a->video = video_dec_create(cb_need_keyframe, a);
    sl_callbacks cb = {
        .ctx = a, .video_start = cb_video_start, .video_frame = cb_video_frame, .audio_start = cb_audio_start,
        .audio_frame = cb_audio_frame, .rumble = cb_rumble,
    };
    sl_session *s = session_start(&a->cfg, ip, port, key, keylen, &cb);
    pthread_mutex_lock(&a->session_lock);
    a->session = s;
    pthread_mutex_unlock(&a->session_lock);
    show_screen(a, "Conectando", "Negociando streaming...", "B: sair");

    bool quit = false;
    uint64_t start = now_ms(), combo_since = 0, first_frame = 0, next_stats = now_ms() + 2000;
    while (session_phase(a->session) != SL_PHASE_ENDED) {
        if (seconds > 0 && now_ms() - start > (uint64_t) seconds * 1000) {
            quit = true;
            break;
        }
        if (!a->headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                handle_device_event(a, &e);
                if (e.type == SDL_QUIT) quit = true;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = true;
                /* Before any video shows, B leaves; once streaming, B belongs to the game. */
                if (!first_frame && e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_B)
                    quit = true;
            }
            if (quit) break;
            sl_gamepad gp;
            read_pad(a, &gp);
            uint16_t combo = 1u << SDL_CONTROLLER_BUTTON_BACK | 1u << SDL_CONTROLLER_BUTTON_START;
            if ((gp.buttons & combo) == combo) {
                if (!combo_since) combo_since = now_ms();
                if (now_ms() - combo_since > 1200) {
                    quit = true;
                    break;
                }
                gp.buttons &= (uint16_t) ~combo; /* don't leak the exit chord into the game */
            } else {
                combo_since = 0;
            }
            session_set_gamepad(a->session, &gp);
            if (atomic_exchange(&a->rumble_pending, false) && a->pad)
                SDL_GameControllerRumble(a->pad, (Uint16) atomic_load(&a->rumble_lo),
                                         (Uint16) atomic_load(&a->rumble_hi), atomic_load(&a->rumble_ms));
        }
        AVFrame *f = video_dec_take(a->video);
        if (f) {
            if (!first_frame) {
                first_frame = now_ms();
                LOG("first video frame %dx%d after %llu ms", f->width, f->height,
                    (unsigned long long) (first_frame - start));
            }
            if (a->dump_path[0] && !a->dumped && now_ms() - first_frame > 5000) dump_frame(a, f);
            if (!a->headless)
                present_frame(a, f, now_ms() - first_frame < 5000,
                              session_activity(a->session) == SL_ACTIVITY_SECURE_DESKTOP);
            av_frame_free(&f);
        } else {
            if (!first_frame && !a->headless && session_phase(a->session) == SL_PHASE_STREAMING)
                show_screen(a, "Conectando", "Aguardando video do PC...", "B: sair");
            SDL_Delay(2);
        }
        if (now_ms() >= next_stats) {
            int dec, drop;
            video_dec_stats(a->video, &dec, &drop);
            LOG("stats: frames decoded=%d dropped=%d audio_samples=%ld phase=%d", dec, drop, a->audio_frames,
                session_phase(a->session));
            next_stats = now_ms() + (a->headless ? 2000 : 10000);
        }
    }
    session_reason(a->session, reason, rlen);
    pthread_mutex_lock(&a->session_lock);
    sl_session *ending = a->session;
    a->session = NULL;
    pthread_mutex_unlock(&a->session_lock);
    session_stop(ending); /* joins the network thread, so no more frames reach the decoder */
    video_dec_destroy(a->video);
    a->video = NULL;
    pthread_mutex_lock(&a->audio_lock);
    audio_dec_destroy(a->adec);
    a->adec = NULL;
    pthread_mutex_unlock(&a->audio_lock);
    if (a->audio) {
        SDL_PauseAudioDevice(a->audio, 1);
        SDL_ClearQueuedAudio(a->audio);
        a->audio_playing = false;
    }
    return quit;
}

static void exe_dir(char *out, size_t len) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        snprintf(out, len, ".");
        return;
    }
    path[n] = 0;
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
    snprintf(out, len, "%s", path);
}

int main(int argc, char **argv) {
    static app_t app;
    app_t *a = &app;
    char dir[PATH_MAX], config_path[PATH_MAX], log_path[PATH_MAX + 32], host[64] = "";
    int seconds = 0;
    exe_dir(dir, sizeof(dir));
    snprintf(config_path, sizeof(config_path), "%s/device.json", dir);
    snprintf(log_path, sizeof(log_path), "%s/steamlink.log", dir);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) a->headless = true;
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) snprintf(host, sizeof(host), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(config_path, sizeof(config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) snprintf(log_path, sizeof(log_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) snprintf(a->dump_path, sizeof(a->dump_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--h264") && i + 1 < argc) h264_out = fopen(argv[++i], "wb");
    }
    log_open(log_path);
    av_log_set_level(AV_LOG_ERROR);
    pthread_mutex_init(&a->audio_lock, NULL);
    pthread_mutex_init(&a->session_lock, NULL);
    bool have_config = config_load(&a->cfg, config_path);
    if (host[0]) snprintf(a->cfg.host, sizeof(a->cfg.host), "%s", host);
    LOG("steamlink start: config=%s ok=%d host=%s %dx%d@%d", config_path, have_config,
        a->cfg.host[0] ? a->cfg.host : "(discover)", a->cfg.width, a->cfg.height, a->cfg.fps);

    if (!a->headless) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
            LOG("SDL_Init: %s", SDL_GetError());
            return 1;
        }
        SDL_DisplayMode mode = {0};
        SDL_GetCurrentDisplayMode(0, &mode);
        LOG("display %dx%d@%d driver=%s", mode.w, mode.h, mode.refresh_rate, SDL_GetCurrentVideoDriver());
        a->win = SDL_CreateWindow("Steam Link", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                  mode.w > 0 ? mode.w : 640, mode.h > 0 ? mode.h : 480, SDL_WINDOW_FULLSCREEN);
        if (!a->win) {
            LOG("SDL_CreateWindow: %s", SDL_GetError());
            return 1;
        }
        a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!a->ren) a->ren = SDL_CreateRenderer(a->win, -1, 0);
        if (!a->ren) {
            LOG("SDL_CreateRenderer: %s", SDL_GetError());
            return 1;
        }
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(a->ren, &ri);
        LOG("renderer: %s", ri.name);
        SDL_ShowCursor(SDL_DISABLE);
        SDL_AudioSpec want = {.freq = 48000, .format = AUDIO_S16SYS, .channels = 2, .samples = 512}, got;
        a->audio = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (!a->audio) LOG("audio: %s", SDL_GetError());
        open_pads(a);
    }

    bool paired = have_config;
    while (!paired) {
        if (!run_pairing(a, config_path, dir)) break;
        paired = config_load(&a->cfg, config_path);
        if (host[0]) snprintf(a->cfg.host, sizeof(a->cfg.host), "%s", host);
    }

    while (paired) {
        sl_host hosts[8];
        int n = 0;
        if (a->cfg.host[0]) {
            snprintf(hosts[0].ip, sizeof(hosts[0].ip), "%s", a->cfg.host);
            snprintf(hosts[0].hostname, sizeof(hosts[0].hostname), "%s", a->cfg.host);
            n = 1;
        } else {
            static connect_job job;
            memset(&job, 0, sizeof(job));
            job.a = a;
            job.discover = true;
            if (!run_job(a, &job, "Steam Link", "Procurando PC com Steam na rede...")) break;
            n = job.n;
            memcpy(hosts, job.hosts, sizeof(hosts));
        }
        char reason[200] = "";
        if (n == 0) {
            snprintf(reason, sizeof(reason), "Nenhum PC encontrado. Abra o Steam no PC, na mesma rede Wi-Fi.");
        } else if (run_stream(a, hosts[0].ip, hosts[0].hostname, seconds, reason, sizeof(reason))) {
            break;
        }
        LOG("back to menu: %s", reason);
        show_screen(a, "Steam Link", reason[0] ? reason : "Streaming encerrado",
                    "A: tentar de novo   Y: parear de novo   B: sair");
        int choice = wait_choice(a);
        if (choice == CHOICE_QUIT) break;
        if (choice == CHOICE_PAIR) {
            if (!run_pairing(a, config_path, dir)) break;
            paired = config_load(&a->cfg, config_path);
            if (host[0]) snprintf(a->cfg.host, sizeof(a->cfg.host), "%s", host);
        }
    }
    if (a->pad) SDL_GameControllerClose(a->pad);
    if (a->audio) SDL_CloseAudioDevice(a->audio);
    if (a->tex) SDL_DestroyTexture(a->tex);
    if (a->ren) SDL_DestroyRenderer(a->ren);
    if (a->win) SDL_DestroyWindow(a->win);
    if (!a->headless) SDL_Quit();
    LOG("exit");
    return 0;
}
