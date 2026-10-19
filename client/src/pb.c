#include "pb.h"

#include <stdlib.h>
#include <string.h>

void pbw_init(pbw *w) { memset(w, 0, sizeof(*w)); }

void pbw_free(pbw *w) {
    free(w->buf);
    memset(w, 0, sizeof(*w));
}

void pbw_raw(pbw *w, const void *data, size_t len) {
    if (w->len + len > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 128;
        while (cap < w->len + len) cap *= 2;
        w->buf = realloc(w->buf, cap);
        if (!w->buf) abort();
        w->cap = cap;
    }
    if (len) memcpy(w->buf + w->len, data, len);
    w->len += len;
}

static void varint(pbw *w, uint64_t v) {
    uint8_t tmp[10];
    int n = 0;
    while (v > 0x7f) {
        tmp[n++] = (uint8_t) (v & 0x7f) | 0x80;
        v >>= 7;
    }
    tmp[n++] = (uint8_t) v;
    pbw_raw(w, tmp, n);
}

static void tag(pbw *w, int field, int wire) { varint(w, (uint64_t) field << 3 | wire); }

void pb_uint(pbw *w, int field, uint64_t v) {
    tag(w, field, 0);
    varint(w, v);
}

void pb_int(pbw *w, int field, int64_t v) { pb_uint(w, field, (uint64_t) v); }

void pb_bool(pbw *w, int field, bool v) { pb_uint(w, field, v ? 1 : 0); }

void pb_bytes(pbw *w, int field, const void *data, size_t len) {
    tag(w, field, 2);
    varint(w, len);
    pbw_raw(w, data, len);
}

void pb_str(pbw *w, int field, const char *s) { pb_bytes(w, field, s, strlen(s)); }

void pb_msg(pbw *w, int field, const pbw *sub) { pb_bytes(w, field, sub->buf, sub->len); }

static int read_varint(pbr *r, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return -1;
        uint8_t b = *r->p++;
        v |= (uint64_t) (b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

int pbr_next(pbr *r, pbf *f) {
    if (r->p >= r->end) return 0;
    uint64_t key;
    if (read_varint(r, &key)) return -1;
    f->field = (int) (key >> 3);
    f->wire = (int) (key & 7);
    f->data = NULL;
    f->len = 0;
    f->v = 0;
    switch (f->wire) {
        case 0:
            return read_varint(r, &f->v) ? -1 : 1;
        case 1:
            if (r->end - r->p < 8) return -1;
            for (int i = 7; i >= 0; i--) f->v = f->v << 8 | r->p[i];
            r->p += 8;
            return 1;
        case 2: {
            uint64_t len;
            if (read_varint(r, &len) || len > (uint64_t) (r->end - r->p)) return -1;
            f->data = r->p;
            f->len = (size_t) len;
            r->p += len;
            return 1;
        }
        case 5:
            if (r->end - r->p < 4) return -1;
            f->v = (uint32_t) r->p[0] | (uint32_t) r->p[1] << 8 | (uint32_t) r->p[2] << 16 | (uint32_t) r->p[3] << 24;
            r->p += 4;
            return 1;
        default:
            return -1;
    }
}
