#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

uint64_t now_ms(void);
uint32_t stream_timestamp(void);
uint32_t crc32c(const uint8_t *data, size_t len);

void log_open(const char *path);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

static inline void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline void put_le64(uint8_t *p, uint64_t v) { put_le32(p, (uint32_t) v); put_le32(p + 4, (uint32_t) (v >> 32)); }
static inline uint16_t get_le16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }
static inline uint32_t get_le32(const uint8_t *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

typedef struct {
    uint64_t device_id;
    uint64_t steamid;
    uint8_t secret[64];
    size_t secret_len;
    char host[64];
    int width, height, fps, bitrate_kbps;
} sl_config;

/* Reads device.json written by tools/proto/pair.py, plus optional overrides. */
bool config_load(sl_config *cfg, const char *path);
