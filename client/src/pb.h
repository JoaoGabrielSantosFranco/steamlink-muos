#pragma once
/* Just enough protobuf wire format to talk to Steam, field numbers are taken from re/protos. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} pbw;

void pbw_init(pbw *w);
void pbw_free(pbw *w);
void pbw_raw(pbw *w, const void *data, size_t len);
void pb_uint(pbw *w, int field, uint64_t v);
void pb_int(pbw *w, int field, int64_t v);
void pb_bool(pbw *w, int field, bool v);
void pb_bytes(pbw *w, int field, const void *data, size_t len);
void pb_str(pbw *w, int field, const char *s);
void pb_msg(pbw *w, int field, const pbw *sub);

typedef struct {
    const uint8_t *p, *end;
} pbr;

typedef struct {
    int field, wire;
    uint64_t v;
    const uint8_t *data;
    size_t len;
} pbf;

static inline pbr pbr_make(const uint8_t *data, size_t len) { return (pbr) {data, data + len}; }
/* 1 = field read, 0 = end of message, -1 = malformed. */
int pbr_next(pbr *r, pbf *f);
