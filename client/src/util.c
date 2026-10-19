#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 16.16 fixed-point seconds of the monotonic clock, truncated to 32 bits. */
uint32_t stream_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) (((uint64_t) ts.tv_sec << 16) + ((uint64_t) ts.tv_nsec * 65536 / 1000000000));
}

static uint32_t crc_table[256];

uint32_t crc32c(const uint8_t *data, size_t len) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
            crc_table[i] = c;
        }
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static FILE *log_file;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t log_start;

void log_open(const char *path) {
    log_start = now_ms();
    if (path) log_file = fopen(path, "w");
}

void logf_(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&log_lock);
    double t = (double) (now_ms() - log_start) / 1000.0;
    fprintf(stderr, "%8.3f %s\n", t, line);
    if (log_file) {
        fprintf(log_file, "%8.3f %s\n", t, line);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_lock);
}

/* Minimal lookup of "key": value in a flat JSON object; good enough for device.json. */
static bool json_find(const char *json, const char *key, char *out, size_t outlen) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (isspace((unsigned char) *p)) p++;
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && n + 1 < outlen) out[n++] = *p++;
    } else {
        while (*p && (isalnum((unsigned char) *p) || *p == '-' || *p == '.') && n + 1 < outlen) out[n++] = *p++;
    }
    out[n] = 0;
    return n > 0 && strcmp(out, "null") != 0;
}

bool config_load(sl_config *cfg, const char *path) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->width = 640;
    cfg->height = 480;
    cfg->fps = 60;
    cfg->bitrate_kbps = 10000;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char v[256];
    if (!json_find(buf, "device_id", v, sizeof(v))) return false;
    cfg->device_id = strtoull(v, NULL, 10);
    if (json_find(buf, "steamid", v, sizeof(v))) cfg->steamid = strtoull(v, NULL, 10);
    if (!json_find(buf, "secret", v, sizeof(v))) return false;
    size_t hl = strlen(v);
    if (hl % 2 || hl / 2 > sizeof(cfg->secret)) return false;
    for (size_t i = 0; i < hl / 2; i++) {
        unsigned int b;
        if (sscanf(v + 2 * i, "%2x", &b) != 1) return false;
        cfg->secret[i] = (uint8_t) b;
    }
    cfg->secret_len = hl / 2;
    if (json_find(buf, "host", v, sizeof(v))) snprintf(cfg->host, sizeof(cfg->host), "%s", v);
    if (json_find(buf, "width", v, sizeof(v))) cfg->width = atoi(v);
    if (json_find(buf, "height", v, sizeof(v))) cfg->height = atoi(v);
    if (json_find(buf, "fps", v, sizeof(v))) cfg->fps = atoi(v);
    if (json_find(buf, "bitrate_kbps", v, sizeof(v))) cfg->bitrate_kbps = atoi(v);
    return true;
}
