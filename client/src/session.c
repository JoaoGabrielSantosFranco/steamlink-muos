#include "session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pb.h"
#include "slcrypto.h"

enum {
    PT_UNCONNECTED = 0, PT_CONNECT = 1, PT_CONNECTED = 2, PT_UNRELIABLE = 3, PT_UNRELIABLE_FRAG = 4,
    PT_RELIABLE = 5, PT_RELIABLE_FRAG = 6, PT_ACK = 7, PT_NACK = 8, PT_DISCONNECT = 9,
};
enum { CH_DISCOVERY = 0, CH_CONTROL = 1, CH_STATS = 2, CH_MAX = 32 };
enum {
    MSG_AUTH_REQUEST = 1, MSG_AUTH_RESPONSE = 2, MSG_NEGOTIATION_INIT = 3, MSG_NEGOTIATION_SET_CONFIG = 4,
    MSG_NEGOTIATION_COMPLETE = 5, MSG_CLIENT_HANDSHAKE = 6, MSG_SERVER_HANDSHAKE = 7, MSG_KEEPALIVE = 9,
    MSG_START_AUDIO = 50, MSG_STOP_AUDIO = 51, MSG_START_VIDEO = 52, MSG_STOP_VIDEO = 53, MSG_QUIT_REQUEST = 83,
    MSG_SET_ACTIVITY = 98, MSG_SET_STREAMING_CLIENT_CONFIG = 99, MSG_REMOTE_HID = 106, MSG_STOP_REQUEST = 129,
};
enum { DATA_PACKET = 1, DATA_LOST = 2 };
enum {
    VF_NEED_START_SEQ = 0x01, VF_NEED_ESCAPE = 0x02, VF_SUBFRAME_ADVANCE = 0x04, VF_FRAME_FINISH = 0x08,
    VF_KEYFRAME = 0x10, VF_ENCRYPTED = 0x20,
};

#define CONNECT_MAGIC 0x3C8F3DC7u /* CRC32C("Connect") */
#define HEADER_SIZE 13
#define MAX_FRAGS 2048
#define MAX_MESSAGE (4u << 20) /* cap on any reassembled message or video frame */
#define RTX_SLOTS 128
#define WINDOW 256
#define HID_DEVICE_ID 1
#define HID_PATH "sdl://0"

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} buffer;

static void buf_reserve(buffer *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + extra) cap *= 2;
    b->buf = realloc(b->buf, cap);
    if (!b->buf) abort();
    b->cap = cap;
}

static void buf_append(buffer *b, const void *data, size_t len) {
    buf_reserve(b, len);
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

typedef struct {
    bool used;
    uint8_t type, channel;
    uint16_t pid;
    int tries;
    uint64_t sent_ms;
    size_t len;
    uint8_t payload[1400];
} rtx_entry;

typedef struct {
    bool valid;
    uint8_t type;
    uint16_t pid, frag;
    uint8_t *data;
    size_t len;
} window_slot;

typedef struct {
    bool init;
    uint16_t expected;
    window_slot slot[WINDOW];
} reliable_window;

typedef struct {
    bool active;
    uint16_t base;
    int total, got;
    size_t bytes;
    uint8_t **parts;
    size_t *lens;
} assembly;

struct sl_session {
    sl_config cfg;
    sl_callbacks cb;
    uint8_t key[32];
    size_t keylen;
    int sock;
    struct sockaddr_in peer;
    pthread_t thread;
    atomic_bool stop;
    atomic_int phase, activity;
    pthread_mutex_t lock; /* guards reason + gamepad */
    char reason[160];

    uint8_t my_id, peer_id;
    uint16_t next_pid[CH_MAX][2];
    uint64_t send_seq, recv_seq;
    rtx_entry rtx[RTX_SLOTS];
    reliable_window *windows[CH_MAX];
    assembly assemblies[CH_MAX];
    uint64_t last_recv_ms;

    int audio_ch, video_ch;
    bool remote_hid, input_enabled;

    /* video assembly */
    bool seq_init, waiting_key, frame_started, frame_key;
    uint16_t expected_seq, frame_id;
    uint64_t waiting_since, last_key_request;
    buffer frame;
    atomic_bool keyframe_wanted;

    /* HID */
    bool hid_open, hid_started;
    uint8_t hid_report_version;
    sl_gamepad pad;
    bool pad_dirty;
    uint8_t last_report[48];
    uint64_t last_report_ms;
};

static void set_reason(sl_session *s, const char *reason) {
    pthread_mutex_lock(&s->lock);
    if (!s->reason[0]) snprintf(s->reason, sizeof(s->reason), "%s", reason);
    pthread_mutex_unlock(&s->lock);
    LOG("session end: %s", reason);
}

/* ---------------------------------------------------------------- sending */

static void send_raw(sl_session *s, uint8_t type, uint8_t channel, uint16_t frag, uint16_t pid, uint8_t retries,
                     uint8_t dst, const uint8_t *payload, size_t len) {
    uint8_t pkt[1600];
    if (len > sizeof(pkt) - HEADER_SIZE - 4) return;
    pkt[0] = type | 0x80;
    pkt[1] = retries;
    pkt[2] = s->my_id;
    pkt[3] = dst;
    pkt[4] = channel;
    put_le16(pkt + 5, frag);
    put_le16(pkt + 7, pid);
    put_le32(pkt + 9, stream_timestamp());
    if (len) memcpy(pkt + HEADER_SIZE, payload, len);
    put_le32(pkt + HEADER_SIZE + len, crc32c(pkt, HEADER_SIZE + len));
    sendto(s->sock, pkt, HEADER_SIZE + len + 4, 0, (struct sockaddr *) &s->peer, sizeof(s->peer));
}

static void send_packet(sl_session *s, uint8_t type, uint8_t channel, const uint8_t *payload, size_t len) {
    bool reliable = type == PT_RELIABLE;
    uint16_t pid = s->next_pid[channel][reliable]++;
    send_raw(s, type, channel, 0, pid, 0, s->peer_id, payload, len);
    if (!reliable || len > sizeof(s->rtx[0].payload)) return;
    rtx_entry *slot = NULL;
    for (int i = 0; i < RTX_SLOTS && !slot; i++)
        if (!s->rtx[i].used) slot = &s->rtx[i];
    if (!slot) {
        slot = &s->rtx[0];
        for (int i = 1; i < RTX_SLOTS; i++)
            if (s->rtx[i].sent_ms < slot->sent_ms) slot = &s->rtx[i];
        LOG("warning: retransmit table full");
    }
    *slot = (rtx_entry) {.used = true, .type = type, .channel = channel, .pid = pid, .sent_ms = now_ms(), .len = len};
    memcpy(slot->payload, payload, len);
}

static void send_control(sl_session *s, int msg_type, const pbw *body) {
    uint8_t out[2200];
    out[0] = (uint8_t) msg_type;
    size_t n;
    bool plain = msg_type == MSG_AUTH_REQUEST || msg_type == MSG_AUTH_RESPONSE || msg_type == MSG_CLIENT_HANDSHAKE ||
                 msg_type == MSG_SERVER_HANDSHAKE;
    if (plain) {
        if (body->len > sizeof(out) - 1) return;
        memcpy(out + 1, body->buf, body->len);
        n = 1 + body->len;
    } else {
        n = 1 + sl_encrypt_control(s->key, s->keylen, s->send_seq++, body->buf, body->len, out + 1);
        if (n == 1) return;
    }
    if (n > 1400) {
        LOG("control message %d too large (%zu)", msg_type, n);
        return;
    }
    send_packet(s, PT_RELIABLE, CH_CONTROL, out, n);
}

static void retransmit(sl_session *s) {
    uint64_t now = now_ms();
    for (int i = 0; i < RTX_SLOTS; i++) {
        rtx_entry *e = &s->rtx[i];
        if (!e->used || now - e->sent_ms < 40) continue;
        /* Steam doesn't ACK every control message it processes (ihslib also stops after 20 tries); the 10 s
         * receive timeout is what ends a dead session. */
        if (++e->tries > 20) {
            e->used = false;
            continue;
        }
        send_raw(s, e->type, e->channel, 0, e->pid, (uint8_t) (e->tries > 255 ? 255 : e->tries), s->peer_id,
                 e->payload, e->len);
        e->sent_ms = now;
    }
}

static void request_keyframe(sl_session *s) {
    uint64_t now = now_ms();
    if (s->video_ch < 0 || now - s->last_key_request < 100) return;
    s->last_key_request = now;
    uint8_t msg = DATA_LOST;
    send_packet(s, PT_UNRELIABLE, (uint8_t) s->video_ch, &msg, 1);
    LOG("video: keyframe requested");
}

/* ---------------------------------------------------------------- HID */

static void hid_send(sl_session *s, const pbw *from_remote) {
    pbw wrap;
    pbw_init(&wrap);
    pb_msg(&wrap, 1, from_remote);
    send_control(s, MSG_REMOTE_HID, &wrap);
    pbw_free(&wrap);
}

static void hid_respond(sl_session *s, uint64_t request_id, int64_t result, const uint8_t *data, size_t len) {
    pbw resp, msg;
    pbw_init(&resp);
    pbw_init(&msg);
    pb_uint(&resp, 1, request_id);
    pb_int(&resp, 2, result);
    if (data) pb_bytes(&resp, 3, data, len);
    pb_msg(&msg, 2, &resp);
    hid_send(s, &msg);
    pbw_free(&resp);
    pbw_free(&msg);
}

static void hid_build_report(sl_session *s, uint8_t out[48]) {
    memset(out, 0, 48);
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < 6; i++) put_le16(out + 2 * i, (uint16_t) s->pad.axes[i]);
    put_le16(out + 16, s->pad.buttons);
    s->pad_dirty = false;
    pthread_mutex_unlock(&s->lock);
    out[27] = s->hid_report_version;
}

static void hid_send_report(sl_session *s, bool force) {
    if (!s->hid_started || !s->input_enabled) return;
    uint8_t report[48];
    hid_build_report(s, report);
    s->last_report_ms = now_ms();
    if (!force && memcmp(report, s->last_report, 48) == 0) return;
    memcpy(s->last_report, report, 48);
    pbw item, dev, reports, msg;
    pbw_init(&item);
    pbw_init(&dev);
    pbw_init(&reports);
    pbw_init(&msg);
    pb_bytes(&item, 1, report, 48);
    pb_uint(&dev, 1, HID_DEVICE_ID);
    pb_msg(&dev, 2, &item);
    pb_msg(&reports, 1, &dev);
    pb_msg(&msg, 3, &reports);
    hid_send(s, &msg);
    pbw_free(&item);
    pbw_free(&dev);
    pbw_free(&reports);
    pbw_free(&msg);
}

static void hid_announce(sl_session *s) {
    const uint32_t caps = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x4000 | 0x10000 |
                          0x20000 | 0x800000;
    pbw info, list, msg;
    pbw_init(&info);
    pbw_init(&list);
    pbw_init(&msg);
    pb_uint(&info, 1, 0); /* k_EDeviceLocationLocal */
    pb_str(&info, 2, HID_PATH);
    pb_uint(&info, 3, 0x045e); /* presented as an Xbox 360 pad through the SDL virtual gamepad protocol */
    pb_uint(&info, 4, 0x028e);
    pb_uint(&info, 6, 0x0114);
    pb_str(&info, 8, "Anbernic RG40XXH");
    pb_uint(&info, 9, 1);
    pb_uint(&info, 10, 5);
    pb_int(&info, 12, -203); /* k_eSteamOSTypeLinux */
    pb_bool(&info, 13, true);
    pb_uint(&info, 15, caps);
    pb_msg(&list, 1, &info);
    pb_msg(&msg, 1, &list);
    hid_send(s, &msg);
    pbw_free(&info);
    pbw_free(&list);
    pbw_free(&msg);
    LOG("hid: device list sent");
}

static void hid_on_message(sl_session *s, const uint8_t *data, size_t len) {
    pbr r = pbr_make(data, len);
    pbf f;
    uint64_t request_id = 0;
    int cmd = 0;
    const uint8_t *sub = NULL;
    size_t sublen = 0;
    while (pbr_next(&r, &f) == 1) {
        if (f.field == 1) request_id = f.v;
        else if (f.wire == 2) cmd = f.field, sub = f.data, sublen = f.len;
    }
    pbr sr = pbr_make(sub, sublen);
    switch (cmd) {
        case 2: { /* DeviceOpen */
            bool ok = false;
            while (pbr_next(&sr, &f) == 1) {
                if (f.field != 1 || f.wire != 2) continue;
                pbr ir = pbr_make(f.data, f.len);
                pbf g;
                while (pbr_next(&ir, &g) == 1)
                    if (g.field == 2 && g.len == strlen(HID_PATH) && !memcmp(g.data, HID_PATH, g.len)) ok = true;
            }
            s->hid_open = ok;
            hid_respond(s, request_id, ok ? HID_DEVICE_ID : -1, NULL, 0);
            LOG("hid: open -> %s", ok ? "ok" : "unknown path");
            break;
        }
        case 3: /* DeviceClose */
            s->hid_open = s->hid_started = false;
            LOG("hid: closed");
            break;
        case 4: { /* DeviceWrite */
            while (pbr_next(&sr, &f) == 1) {
                if (f.field != 2 || f.wire != 2 || f.len < 2) continue;
                if (f.data[0] == 0x09) s->hid_report_version = f.data[1];
                if (f.data[0] == 0x01 && f.len >= 9 && s->cb.rumble)
                    s->cb.rumble(s->cb.ctx, get_le16(f.data + 1), get_le16(f.data + 3), get_le32(f.data + 5));
            }
            break;
        }
        case 5: { /* DeviceRead: mirrors ihslib, which reports the length and no data */
            uint64_t length = 0;
            while (pbr_next(&sr, &f) == 1)
                if (f.field == 2) length = f.v;
            hid_respond(s, request_id, (int64_t) length, NULL, 0);
            break;
        }
        case 7: { /* DeviceGetFeatureReport */
            uint8_t number = 0;
            while (pbr_next(&sr, &f) == 1)
                if (f.field == 2 && f.wire == 2 && f.len) number = f.data[0];
            uint8_t out[21] = {number};
            size_t n = 2;
            if (number == 0x04) { /* caps: valid, xinput, controller type, player index, hid */
                out[1] = 1;
                out[2] = 0;
                put_le32(out + 3, 31); /* k_ControllerTypeXBox360Controller */
                put_le32(out + 7, 0);
                out[11] = 0;
                n = 21;
            } else if (number == 0x02) {
                out[1] = 5; /* SDL_JOYSTICK_POWER_WIRED */
            }
            hid_respond(s, request_id, 0, out, n);
            break;
        }
        case 8:
        case 9:
        case 10: { /* vendor / product / serial strings */
            uint8_t empty = 0;
            hid_respond(s, request_id, 0, &empty, 1);
            break;
        }
        case 11: /* DeviceStartInputReports */
            s->hid_started = true;
            LOG("hid: input reports started");
            hid_send_report(s, true);
            break;
        case 12: /* DeviceRequestFullReport */
            hid_send_report(s, true);
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------- control */

static void send_set_config(sl_session *s, bool reliable_data, bool remote_hid) {
    pbw cfg, client, caps, msg;
    pbw_init(&cfg);
    pbw_init(&client);
    pbw_init(&caps);
    pbw_init(&msg);
    pb_bool(&cfg, 1, reliable_data);
    pb_uint(&cfg, 2, SL_CODEC_OPUS);
    pb_uint(&cfg, 3, SL_CODEC_H264);
    pb_bool(&cfg, 5, remote_hid);
    pb_bool(&cfg, 6, false);
    pb_uint(&client, 2, (uint64_t) s->cfg.width);
    pb_uint(&client, 3, (uint64_t) s->cfg.height);
    pb_uint(&client, 4, (uint64_t) s->cfg.fps);
    pb_uint(&client, 5, 1);
    pb_int(&client, 6, s->cfg.bitrate_kbps > 0 ? s->cfg.bitrate_kbps : -1);
    pb_bool(&client, 7, true);
    pb_bool(&client, 11, true);
    pb_int(&client, 12, 2);
    pb_bool(&client, 13, false);
    pb_str(&caps, 1, "Anbernic RG40XXH (MuOS) native Steam Link");
    pb_int(&caps, 3, 20000);
    pb_uint(&caps, 14, SL_CODEC_OPUS);
    pb_uint(&caps, 15, SL_CODEC_H264);
    pb_msg(&msg, 1, &cfg);
    pb_msg(&msg, 2, &client);
    pb_msg(&msg, 3, &caps);
    send_control(s, MSG_NEGOTIATION_SET_CONFIG, &msg);
    pbw_free(&cfg);
    pbw_free(&client);
    pbw_free(&caps);
    pbw_free(&msg);
}

/* True if repeated varint field `field` (packed or not) contains `value`. */
static bool repeated_has(const uint8_t *data, size_t len, int field, uint64_t value) {
    pbr r = pbr_make(data, len);
    pbf f;
    while (pbr_next(&r, &f) == 1) {
        if (f.field != field) continue;
        if (f.wire == 0 && f.v == value) return true;
        if (f.wire != 2) continue;
        const uint8_t *p = f.data, *end = f.data + f.len;
        while (p < end) {
            uint64_t v = 0;
            int shift = 0;
            while (p < end) {
                uint8_t b = *p++;
                v |= (uint64_t) (b & 0x7f) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            if (v == value) return true;
        }
    }
    return false;
}

static void on_control(sl_session *s, const uint8_t *msg, size_t len) {
    if (len < 1) return;
    int type = msg[0];
    static __thread uint8_t plain[65536];
    const uint8_t *body = msg + 1;
    size_t bl = len - 1;
    bool encrypted = !(type == MSG_AUTH_REQUEST || type == MSG_AUTH_RESPONSE || type == MSG_CLIENT_HANDSHAKE ||
                       type == MSG_SERVER_HANDSHAKE);
    if (encrypted) {
        uint64_t seq;
        int n = sl_decrypt_control(s->key, s->keylen, body, bl, &seq, plain);
        if (n < 0) {
            LOG("control %d: decrypt failed", type);
            return;
        }
        if (seq < s->recv_seq) { /* replayed or stale message */
            LOG("control %d: dropping old sequence %llu", type, (unsigned long long) seq);
            return;
        }
        s->recv_seq = seq + 1;
        body = plain;
        bl = (size_t) n;
    }
    pbr r = pbr_make(body, bl);
    pbf f;
    switch (type) {
        case MSG_SERVER_HANDSHAKE: {
            LOG("<- ServerHandshake");
            /* Steam answers ClientHandshake with pings + ServerHandshake instead of an ACK. */
            for (int i = 0; i < RTX_SLOTS; i++)
                if (s->rtx[i].used && s->rtx[i].channel == CH_CONTROL && s->rtx[i].payload[0] == MSG_CLIENT_HANDSHAKE)
                    s->rtx[i].used = false;
            uint8_t token[32];
            sl_auth_token(s->key, s->keylen, token);
            pbw m;
            pbw_init(&m);
            pb_bytes(&m, 1, token, 32);
            pb_uint(&m, 2, 1); /* k_EStreamVersionCurrent */
            if (s->cfg.steamid) pb_uint(&m, 3, s->cfg.steamid);
            send_control(s, MSG_AUTH_REQUEST, &m);
            pbw_free(&m);
            break;
        }
        case MSG_AUTH_RESPONSE: {
            uint64_t result = 0;
            while (pbr_next(&r, &f) == 1)
                if (f.field == 1) result = f.v;
            LOG("<- AuthenticationResponse result=%llu", (unsigned long long) result);
            if (result != 0) {
                set_reason(s, "Autenticacao recusada pelo PC");
                atomic_store(&s->stop, true);
            }
            break;
        }
        case MSG_NEGOTIATION_INIT: {
            bool reliable = false, hid = false;
            while (pbr_next(&r, &f) == 1) {
                if (f.field == 1) reliable = f.v != 0;
                if (f.field == 4) hid = f.v != 0;
            }
            bool h264 = repeated_has(body, bl, 3, SL_CODEC_H264), opus = repeated_has(body, bl, 2, SL_CODEC_OPUS);
            LOG("<- NegotiationInit reliable=%d hid=%d h264=%d opus=%d", reliable, hid, h264, opus);
            if (!h264) {
                set_reason(s, "PC nao oferece H.264");
                atomic_store(&s->stop, true);
                break;
            }
            s->remote_hid = hid;
            send_set_config(s, reliable, hid);
            break;
        }
        case MSG_NEGOTIATION_SET_CONFIG: {
            LOG("<- NegotiationSetConfig");
            pbw m;
            pbw_init(&m);
            send_control(s, MSG_NEGOTIATION_COMPLETE, &m);
            pbw_free(&m);
            atomic_store(&s->phase, SL_PHASE_STREAMING);
            if (s->remote_hid) hid_announce(s);
            break;
        }
        case MSG_START_AUDIO: {
            int ch = -1, codec = 0, rate = 48000, chans = 2;
            while (pbr_next(&r, &f) == 1) {
                if (f.field == 2) ch = (int) f.v;
                if (f.field == 3) codec = (int) f.v;
                if (f.field == 5) rate = (int) f.v;
                if (f.field == 6) chans = (int) f.v;
            }
            LOG("<- StartAudioData channel=%d codec=%d %dHz x%d", ch, codec, rate, chans);
            if (ch > 0 && ch < CH_MAX) {
                s->audio_ch = ch;
                if (s->cb.audio_start) s->cb.audio_start(s->cb.ctx, codec, rate, chans);
            }
            break;
        }
        case MSG_STOP_AUDIO:
            s->audio_ch = -1;
            break;
        case MSG_START_VIDEO: {
            int ch = -1, codec = 0, w = 0, h = 0;
            while (pbr_next(&r, &f) == 1) {
                if (f.field == 1) ch = (int) f.v;
                if (f.field == 2) codec = (int) f.v;
                if (f.field == 4) w = (int) f.v;
                if (f.field == 5) h = (int) f.v;
            }
            LOG("<- StartVideoData channel=%d codec=%d %dx%d", ch, codec, w, h);
            if (ch > 0 && ch < CH_MAX) {
                s->video_ch = ch;
                s->seq_init = s->frame_started = false;
                if (s->cb.video_start) s->cb.video_start(s->cb.ctx, codec, w, h);
            }
            break;
        }
        case MSG_STOP_VIDEO:
            s->video_ch = -1;
            break;
        case MSG_SET_STREAMING_CLIENT_CONFIG:
            while (pbr_next(&r, &f) == 1) {
                if (f.field != 1 || f.wire != 2) continue;
                pbr cr = pbr_make(f.data, f.len);
                pbf g;
                while (pbr_next(&cr, &g) == 1)
                    if (g.field == 11) s->input_enabled = g.v != 0;
            }
            LOG("<- SetStreamingClientConfig input=%d", s->input_enabled);
            break;
        case MSG_REMOTE_HID:
            while (pbr_next(&r, &f) == 1)
                if (f.field == 1 && f.wire == 2) hid_on_message(s, f.data, f.len);
            break;
        case MSG_SET_ACTIVITY:
            while (pbr_next(&r, &f) == 1)
                if (f.field == 1) atomic_store(&s->activity, (int) f.v);
            LOG("<- SetActivity %d", atomic_load(&s->activity));
            break;
        case MSG_QUIT_REQUEST:
        case MSG_STOP_REQUEST:
            set_reason(s, "O PC encerrou o streaming");
            atomic_store(&s->stop, true);
            break;
        default: {
            static uint8_t seen[256];
            if (!seen[type]++) {
                char hex[100] = "";
                for (size_t i = 0; i < bl && i < 48; i++) {
                    char c = (char) body[i];
                    hex[i] = c >= 32 && c < 127 ? c : '.';
                    hex[i + 1] = 0;
                }
                char raw[100] = "";
                for (size_t i = 0; i < bl && i < 24; i++) snprintf(raw + 2 * i, 3, "%02x", body[i]);
                LOG("<- control message %d (%zu bytes) '%s' %s", type, bl, hex, raw);
            }
            break;
        }
    }
}

/* ---------------------------------------------------------------- media */

static void append_escaped(buffer *b, const uint8_t *data, size_t len, uint8_t flags) {
    if (!(flags & VF_NEED_ESCAPE)) {
        buf_append(b, data, len);
        return;
    }
    buf_reserve(b, len * 3 / 2 + 8);
    if (flags & VF_NEED_START_SEQ) {
        static const uint8_t sc[4] = {0, 0, 0, 1};
        buf_append(b, sc, 4);
    }
    uint8_t *out = b->buf + b->len;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (i >= 2 && data[i] <= 3 && out[n - 1] == 0 && out[n - 2] == 0) out[n++] = 3;
        out[n++] = data[i];
    }
    b->len += n;
}

static void on_video_data(sl_session *s, const uint8_t *msg, size_t len) {
    if (len < 20) return;
    uint16_t id = get_le16(msg + 1);
    uint16_t seq = get_le16(msg + 13);
    uint8_t flags = msg[15];
    const uint8_t *data = msg + 20;
    size_t dl = len - 20;
    uint64_t now = now_ms();

    if (flags & VF_KEYFRAME) {
        s->frame_started = false;
        s->waiting_key = false;
        s->expected_seq = seq;
    }
    if (!s->seq_init) {
        s->seq_init = true;
        s->expected_seq = seq;
        if (!(flags & VF_KEYFRAME)) {
            s->waiting_key = true;
            s->waiting_since = now;
            request_keyframe(s);
        }
    }
    if (s->waiting_key) {
        if (now - s->waiting_since >= 200) {
            request_keyframe(s);
            s->waiting_since = now;
        }
        s->expected_seq = seq + 1;
        return;
    }
    if (seq != s->expected_seq) {
        LOG("video: sequence %u, expected %u", seq, s->expected_seq);
        s->frame_started = false;
        s->waiting_key = true;
        s->waiting_since = now;
        s->expected_seq = seq + 1;
        request_keyframe(s);
        return;
    }
    s->expected_seq = seq + 1;

    static __thread uint8_t plain[1 << 20];
    if (flags & VF_ENCRYPTED) {
        int n = dl <= sizeof(plain) ? sl_decrypt_zero_iv(s->key, s->keylen, data, dl, plain) : -1;
        if (n < 0) {
            LOG("video: decrypt failed");
            s->frame_started = false;
            s->waiting_key = true;
            s->waiting_since = now;
            request_keyframe(s);
            return;
        }
        data = plain;
        dl = (size_t) n;
    }
    if (s->frame_started && id != s->frame_id) s->frame_started = false;
    if (!s->frame_started) {
        s->frame_started = true;
        s->frame_id = id;
        s->frame_key = false;
        s->frame.len = 0;
    }
    if (flags & VF_KEYFRAME) s->frame_key = true;
    if (s->frame.len + dl * 3 / 2 + 8 > MAX_MESSAGE) {
        s->frame_started = false;
        s->waiting_key = true;
        s->waiting_since = now;
        request_keyframe(s);
        return;
    }
    append_escaped(&s->frame, data, dl, flags);
    if (flags & VF_FRAME_FINISH) {
        if (s->cb.video_frame) s->cb.video_frame(s->cb.ctx, s->frame.buf, s->frame.len, s->frame_key);
        s->frame_started = false;
    }
}

static void on_data_message(sl_session *s, int channel, const uint8_t *msg, size_t len) {
    if (len < 13 || msg[0] != DATA_PACKET) return;
    if (channel == s->video_ch) on_video_data(s, msg, len);
    else if (channel == s->audio_ch && s->cb.audio_frame) s->cb.audio_frame(s->cb.ctx, msg + 13, len - 13);
}

static void deliver(sl_session *s, int channel, const uint8_t *msg, size_t len) {
    if (channel == CH_CONTROL) on_control(s, msg, len);
    else if (channel >= 3) on_data_message(s, channel, msg, len);
}

/* ---------------------------------------------------------------- receive paths */

static void assembly_reset(assembly *a) {
    if (a->parts) {
        for (int i = 0; i <= a->total; i++) free(a->parts[i]);
        free(a->parts);
        free(a->lens);
    }
    memset(a, 0, sizeof(*a));
}

/* Unreliable messages: first packet carries frag = continuation count, continuations follow with consecutive ids. */
static void on_unreliable(sl_session *s, uint8_t type, int channel, uint16_t frag, uint16_t pid,
                          const uint8_t *payload, size_t len) {
    assembly *a = &s->assemblies[channel];
    if (type == PT_UNRELIABLE || type == PT_RELIABLE) {
        if (a->active) assembly_reset(a);
        if (frag == 0) {
            deliver(s, channel, payload, len);
            return;
        }
        if (frag > MAX_FRAGS) return;
        a->active = true;
        a->base = pid;
        a->total = frag;
        a->parts = calloc((size_t) frag + 1, sizeof(uint8_t *));
        a->lens = calloc((size_t) frag + 1, sizeof(size_t));
        a->parts[0] = malloc(len ? len : 1);
        memcpy(a->parts[0], payload, len);
        a->lens[0] = len;
        a->bytes = len;
        a->got = 1;
        return;
    }
    if (!a->active) return;
    int idx = (uint16_t) (pid - a->base);
    if (idx < 1 || idx > a->total || a->parts[idx]) return;
    if (a->bytes + len > MAX_MESSAGE) {
        assembly_reset(a);
        return;
    }
    a->bytes += len;
    a->parts[idx] = malloc(len ? len : 1);
    memcpy(a->parts[idx], payload, len);
    a->lens[idx] = len;
    if (++a->got < a->total + 1) return;
    static __thread buffer whole;
    whole.len = 0;
    for (int i = 0; i <= a->total; i++) buf_append(&whole, a->parts[i], a->lens[i]);
    assembly_reset(a);
    deliver(s, channel, whole.buf, whole.len);
}

/* Reliable messages are delivered in packet-id order, reassembling fragments. */
static void on_reliable(sl_session *s, uint8_t type, int channel, uint16_t frag, uint16_t pid,
                        const uint8_t *payload, size_t len) {
    if (!s->windows[channel]) s->windows[channel] = calloc(1, sizeof(reliable_window));
    reliable_window *w = s->windows[channel];
    if (!w->init) {
        w->init = true;
        w->expected = pid;
    }
    int16_t d = (int16_t) (pid - w->expected);
    if (d < 0 || d >= WINDOW) return; /* duplicate or far ahead */
    window_slot *slot = &w->slot[pid % WINDOW];
    if (slot->valid) return;
    slot->valid = true;
    slot->type = type;
    slot->pid = pid;
    slot->frag = frag;
    slot->data = malloc(len ? len : 1);
    memcpy(slot->data, payload, len);
    slot->len = len;

    for (;;) {
        window_slot *head = &w->slot[w->expected % WINDOW];
        if (!head->valid) break;
        if (head->type != PT_RELIABLE) { /* stray continuation */
            free(head->data);
            head->valid = false;
            w->expected++;
            continue;
        }
        int n = head->frag;
        if (n >= WINDOW) {
            free(head->data);
            head->valid = false;
            w->expected++;
            continue;
        }
        bool complete = true;
        for (int i = 1; i <= n && complete; i++) {
            window_slot *c = &w->slot[(uint16_t) (w->expected + i) % WINDOW];
            complete = c->valid && c->pid == (uint16_t) (w->expected + i);
        }
        if (!complete) break;
        static __thread buffer whole;
        whole.len = 0;
        for (int i = 0; i <= n; i++) {
            window_slot *c = &w->slot[(uint16_t) (w->expected + i) % WINDOW];
            buf_append(&whole, c->data, c->len);
            free(c->data);
            c->valid = false;
        }
        w->expected = (uint16_t) (w->expected + n + 1);
        deliver(s, channel, whole.buf, whole.len);
    }
}

static void on_ping(sl_session *s, const uint8_t *payload, size_t len) {
    if (len < 5 || payload[0] != 1) return; /* k_EStreamDiscoveryPingRequest */
    uint32_t ml = get_le32(payload + 1);
    if (5 + (size_t) ml > len) return;
    uint64_t seq = 0, size_requested = 0;
    pbr r = pbr_make(payload + 5, ml);
    pbf f;
    while (pbr_next(&r, &f) == 1) {
        if (f.field == 1) seq = f.v;
        if (f.field == 2) size_requested = f.v;
    }
    pbw m;
    pbw_init(&m);
    pb_uint(&m, 1, seq);
    pb_uint(&m, 2, HEADER_SIZE + len);
    uint8_t out[1600];
    out[0] = 2; /* k_EStreamDiscoveryPingResponse */
    put_le32(out + 1, (uint32_t) m.len);
    memcpy(out + 5, m.buf, m.len);
    size_t n = 5 + m.len;
    pbw_free(&m);
    /* Pad so header + body reaches the requested size (the CRC comes on top). */
    if (size_requested > HEADER_SIZE && size_requested - HEADER_SIZE <= sizeof(out) - HEADER_SIZE - 4) {
        size_t target = (size_t) size_requested - HEADER_SIZE;
        if (n < target) {
            memset(out + n, 0xfe, target - n);
            n = target;
        }
    }
    send_raw(s, PT_UNCONNECTED, CH_DISCOVERY, 0, 0, 0, 0, out, n);
}

static void on_packet(sl_session *s, const uint8_t *pkt, size_t len) {
    if (len < HEADER_SIZE) return;
    uint8_t type = pkt[0] & 0x7f;
    if (pkt[0] & 0x80) {
        if (len < HEADER_SIZE + 4 || crc32c(pkt, len - 4) != get_le32(pkt + len - 4)) return;
        len -= 4;
    }
    uint8_t src = pkt[2], channel = pkt[4];
    uint16_t frag = get_le16(pkt + 5), pid = get_le16(pkt + 7);
    const uint8_t *payload = pkt + HEADER_SIZE;
    size_t pl = len - HEADER_SIZE;
    s->last_recv_ms = now_ms();

    if (type == PT_UNCONNECTED) {
        on_ping(s, payload, pl);
        return;
    }
    if (type == PT_CONNECTED) {
        if (atomic_load(&s->phase) == SL_PHASE_CONNECTING) {
            s->peer_id = src;
            atomic_store(&s->phase, SL_PHASE_HANDSHAKE);
            LOG("<- Connected peer_id=%d", src);
            pbw info, m;
            pbw_init(&info);
            pbw_init(&m);
            pb_msg(&m, 1, &info);
            send_control(s, MSG_CLIENT_HANDSHAKE, &m);
            pbw_free(&info);
            pbw_free(&m);
        }
        return;
    }
    if (type == PT_DISCONNECT) {
        set_reason(s, "O PC encerrou a conexao");
        atomic_store(&s->stop, true);
        return;
    }
    if (channel >= CH_MAX || src != s->peer_id) return;
    switch (type) {
        case PT_ACK:
        case PT_NACK:
            for (int i = 0; i < RTX_SLOTS; i++)
                if (s->rtx[i].used && s->rtx[i].channel == channel && s->rtx[i].pid == pid) s->rtx[i].used = false;
            break;
        case PT_RELIABLE:
        case PT_RELIABLE_FRAG: {
            uint8_t ack[4];
            put_le32(ack, stream_timestamp());
            send_raw(s, PT_ACK, channel, 0, pid, 0, s->peer_id, ack, 4);
            on_reliable(s, type, channel, frag, pid, payload, pl);
            break;
        }
        case PT_UNRELIABLE:
        case PT_UNRELIABLE_FRAG:
            on_unreliable(s, type, channel, frag, pid, payload, pl);
            break;
        default:
            break;
    }
}

/* ---------------------------------------------------------------- thread */

static void *net_thread(void *arg) {
    sl_session *s = arg;
    uint64_t next_connect = 0, next_keepalive = 0, next_rtx = 0, start = now_ms();
    int connect_tries = 0;
    s->last_recv_ms = now_ms();
    uint8_t *pkt = malloc(65536);
    while (!atomic_load(&s->stop)) {
        uint64_t now = now_ms();
        int phase = atomic_load(&s->phase);
        if (phase == SL_PHASE_CONNECTING && now >= next_connect) {
            if (++connect_tries > 20) {
                set_reason(s, "O PC nao respondeu (Connect)");
                break;
            }
            uint8_t magic[4];
            put_le32(magic, CONNECT_MAGIC);
            send_raw(s, PT_CONNECT, CH_DISCOVERY, 0, 0, 0, 0, magic, 4);
            LOG("-> Connect my_id=%d", s->my_id);
            next_connect = now + 500;
        }
        if (phase == SL_PHASE_STREAMING && now >= next_keepalive) {
            pbw m;
            pbw_init(&m);
            send_control(s, MSG_KEEPALIVE, &m);
            pbw_free(&m);
            next_keepalive = now + 1000;
        }
        if (now >= next_rtx) {
            retransmit(s);
            next_rtx = now + 10;
        }
        if (atomic_exchange(&s->keyframe_wanted, false)) request_keyframe(s);
        if (s->hid_started) {
            pthread_mutex_lock(&s->lock);
            bool dirty = s->pad_dirty;
            pthread_mutex_unlock(&s->lock);
            if ((dirty && now - s->last_report_ms >= 4) || now - s->last_report_ms >= 1000) hid_send_report(s, false);
        }
        if (phase != SL_PHASE_CONNECTING && now - s->last_recv_ms > 10000) {
            set_reason(s, "Conexao perdida (sem dados do PC por 10 s)");
            break;
        }
        if (phase == SL_PHASE_HANDSHAKE && now - start > 30000) {
            set_reason(s, "Tempo esgotado na negociacao com o PC");
            break;
        }
        struct pollfd p = {.fd = s->sock, .events = POLLIN};
        if (poll(&p, 1, 4) <= 0) continue;
        for (int burst = 0; burst < 256; burst++) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(s->sock, pkt, 65536, MSG_DONTWAIT, (struct sockaddr *) &from, &fl);
            if (n <= 0) break;
            if (from.sin_addr.s_addr != s->peer.sin_addr.s_addr || from.sin_port != s->peer.sin_port) continue;
            on_packet(s, pkt, (size_t) n);
        }
    }
    free(pkt);
    set_reason(s, "Streaming encerrado");
    if (atomic_load(&s->phase) != SL_PHASE_CONNECTING)
        for (int i = 0; i < 3; i++) send_raw(s, PT_DISCONNECT, CH_DISCOVERY, 0, 0, 0, s->peer_id, NULL, 0);
    atomic_store(&s->phase, SL_PHASE_ENDED);
    return NULL;
}

sl_session *session_start(const sl_config *cfg, const char *ip, int port, const uint8_t *key, size_t keylen,
                          const sl_callbacks *cb) {
    sl_session *s = calloc(1, sizeof(*s));
    s->cfg = *cfg;
    s->cb = *cb;
    memcpy(s->key, key, keylen);
    s->keylen = keylen;
    s->audio_ch = s->video_ch = -1;
    s->input_enabled = true;
    pthread_mutex_init(&s->lock, NULL);
    uint8_t id;
    do sl_random(&id, 1);
    while (id == 0);
    s->my_id = id;
    s->peer.sin_family = AF_INET;
    s->peer.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, ip, &s->peer.sin_addr);
    s->sock = socket(AF_INET, SOCK_DGRAM, 0);
    int rcvbuf = 4 << 20;
    setsockopt(s->sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct sockaddr_in any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(s->sock, (struct sockaddr *) &any, sizeof(any));
    atomic_store(&s->phase, SL_PHASE_CONNECTING);
    pthread_create(&s->thread, NULL, net_thread, s);
    return s;
}

void session_stop(sl_session *s) {
    if (!s) return;
    atomic_store(&s->stop, true);
    pthread_join(s->thread, NULL);
    close(s->sock);
    for (int i = 0; i < CH_MAX; i++) {
        assembly_reset(&s->assemblies[i]);
        if (s->windows[i])
            for (int j = 0; j < WINDOW; j++)
                if (s->windows[i]->slot[j].valid) free(s->windows[i]->slot[j].data);
        free(s->windows[i]);
    }
    free(s->frame.buf);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

int session_phase(sl_session *s) { return atomic_load(&s->phase); }

int session_activity(sl_session *s) { return atomic_load(&s->activity); }

void session_reason(sl_session *s, char *out, size_t len) {
    pthread_mutex_lock(&s->lock);
    snprintf(out, len, "%s", s->reason);
    pthread_mutex_unlock(&s->lock);
}

void session_set_gamepad(sl_session *s, const sl_gamepad *pad) {
    pthread_mutex_lock(&s->lock);
    if (memcmp(&s->pad, pad, sizeof(*pad)) != 0) {
        s->pad = *pad;
        s->pad_dirty = true;
    }
    pthread_mutex_unlock(&s->lock);
}

void session_request_keyframe(sl_session *s) { atomic_store(&s->keyframe_wanted, true); }
