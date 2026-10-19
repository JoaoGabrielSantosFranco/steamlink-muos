#pragma once
/* Minimal verified TLS client, HTTPS GET and WebSocket (binary frames) on top of OpenSSL. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct tls_conn tls_conn;

/* cafile may be NULL; the system bundle is tried as well. */
tls_conn *tls_connect(const char *host, int port, const char *cafile, char *err, size_t errlen);
void tls_close(tls_conn *c);

/* Returns a malloc'd, NUL-terminated body or NULL. */
char *https_get(const char *url, const char *cafile, size_t *len, char *err, size_t errlen);

typedef struct ws_conn ws_conn;

ws_conn *ws_connect(const char *host, int port, const char *path, const char *cafile, char *err, size_t errlen);
int ws_send(ws_conn *ws, const void *data, size_t len);
/* Waits up to timeout_ms for a message: >0 = length (*out malloc'd), 0 = timeout, -1 = closed or error. */
int ws_recv(ws_conn *ws, uint8_t **out, int timeout_ms);
void ws_close(ws_conn *ws);
