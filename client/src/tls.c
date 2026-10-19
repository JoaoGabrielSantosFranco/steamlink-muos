#define _GNU_SOURCE /* memmem */
#include "tls.h"

#include <ctype.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "slcrypto.h"
#include "util.h"

struct tls_conn {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
};

static const char *SYSTEM_CA_FILES[] = {
    "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem",
    "/etc/ssl/ca-bundle.pem", NULL,
};

static int tcp_connect(const char *host, int port, char *err, size_t errlen) {
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) {
        snprintf(err, errlen, "Sem internet? Nao achei %s (%s)", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {.tv_sec = 15};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) snprintf(err, errlen, "Nao consegui conectar a %s:%d", host, port);
    return fd;
}

tls_conn *tls_connect(const char *host, int port, const char *cafile, char *err, size_t errlen) {
    int fd = tcp_connect(host, port, err, errlen);
    if (fd < 0) return NULL;
    tls_conn *c = calloc(1, sizeof(*c));
    c->fd = fd;
    c->ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
    bool loaded = cafile && access(cafile, R_OK) == 0 && SSL_CTX_load_verify_locations(c->ctx, cafile, NULL) == 1;
    for (int i = 0; SYSTEM_CA_FILES[i]; i++)
        if (access(SYSTEM_CA_FILES[i], R_OK) == 0 && SSL_CTX_load_verify_locations(c->ctx, SYSTEM_CA_FILES[i], NULL))
            loaded = true;
    SSL_CTX_set_default_verify_paths(c->ctx);
    /* Handhelds without an RTC often boot in 1970: only then skip validity dates (chain + hostname still
     * checked). With a sane clock certificates are fully validated. */
    if (time(NULL) < 1767225600) { /* 2026-01-01 */
        LOG("tls: system clock looks unset, not checking certificate dates");
        X509_VERIFY_PARAM_set_flags(SSL_CTX_get0_param(c->ctx), X509_V_FLAG_NO_CHECK_TIME);
    }
    c->ssl = SSL_new(c->ctx);
    SSL_set_fd(c->ssl, fd);
    SSL_set_tlsext_host_name(c->ssl, host);
    SSL_set1_host(c->ssl, host);
    if (SSL_connect(c->ssl) != 1) {
        long v = SSL_get_verify_result(c->ssl);
        if (v != X509_V_OK)
            snprintf(err, errlen, "Certificado de %s rejeitado: %s%s", host, X509_verify_cert_error_string(v),
                     loaded ? "" : " (sem lista de certificados)");
        else
            snprintf(err, errlen, "Falha TLS com %s", host);
        tls_close(c);
        return NULL;
    }
    return c;
}

void tls_close(tls_conn *c) {
    if (!c) return;
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

static int tls_write_all(tls_conn *c, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        int n = SSL_write(c->ssl, p, (int) len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

static int tls_read_exact(tls_conn *c, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        int n = SSL_read(c->ssl, p, (int) len);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

/* Reads HTTP response headers into buf (NUL-terminated). Returns 0 on success. */
static int read_headers(tls_conn *c, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        if (tls_read_exact(c, buf + n, 1)) return -1;
        n++;
        if (n >= 4 && !memcmp(buf + n - 4, "\r\n\r\n", 4)) {
            buf[n] = 0;
            return 0;
        }
    }
    return -1;
}

static bool header_has(const char *headers, const char *name, const char *value) {
    size_t nl = strlen(name);
    for (const char *line = headers; line && *line; line = strstr(line, "\r\n") ? strstr(line, "\r\n") + 2 : NULL) {
        if (strncasecmp(line, name, nl) || line[nl] != ':') continue;
        const char *end = strstr(line, "\r\n");
        size_t ll = end ? (size_t) (end - line) : strlen(line);
        for (const char *p = line + nl + 1; p + strlen(value) <= line + ll; p++)
            if (!strncasecmp(p, value, strlen(value))) return true;
    }
    return false;
}

static bool split_url(const char *url, char *host, size_t hl, int *port, char *path, size_t pl) {
    if (strncmp(url, "https://", 8)) return false;
    const char *h = url + 8, *slash = strchr(h, '/');
    size_t len = slash ? (size_t) (slash - h) : strlen(h);
    if (len == 0 || len >= hl) return false;
    memcpy(host, h, len);
    host[len] = 0;
    *port = 443;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
    }
    snprintf(path, pl, "%s", slash ? slash : "/");
    return true;
}

char *https_get(const char *url, const char *cafile, size_t *out_len, char *err, size_t errlen) {
    char host[256], path[1024], req[1600], headers[8192];
    int port;
    if (!split_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        snprintf(err, errlen, "URL invalida");
        return NULL;
    }
    tls_conn *c = tls_connect(host, port, cafile, err, errlen);
    if (!c) return NULL;
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: steamlink-muos\r\nAccept: */*\r\nConnection: close\r\n\r\n",
             path, host);
    char *body = NULL;
    size_t len = 0, cap = 0;
    if (tls_write_all(c, req, strlen(req)) || read_headers(c, headers, sizeof(headers)) ||
        strncmp(headers, "HTTP/1.1 200", 12)) {
        snprintf(err, errlen, "Resposta inesperada de %s", host);
        tls_close(c);
        return NULL;
    }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 16384;
            body = realloc(body, cap);
        }
        int n = SSL_read(c->ssl, body + len, 4096);
        if (n <= 0) break;
        len += (size_t) n;
    }
    tls_close(c);
    if (!body) body = calloc(1, 1);
    if (header_has(headers, "Transfer-Encoding", "chunked")) { /* decode in place */
        size_t r = 0, w = 0;
        while (r < len) {
            size_t sz = strtoul(body + r, NULL, 16);
            char *eol = memmem(body + r, len - r, "\r\n", 2);
            if (!eol || sz == 0) break;
            r = (size_t) (eol - body) + 2;
            if (r > len) break;
            if (sz > len - r) sz = len - r;
            memmove(body + w, body + r, sz);
            w += sz;
            r += sz + 2;
        }
        len = w;
    }
    body[len] = 0;
    if (out_len) *out_len = len;
    return body;
}

struct ws_conn {
    tls_conn *tls;
};

ws_conn *ws_connect(const char *host, int port, const char *path, const char *cafile, char *err, size_t errlen) {
    tls_conn *c = tls_connect(host, port, cafile, err, errlen);
    if (!c) return NULL;
    uint8_t nonce[16];
    char key[32], req[1024], headers[4096];
    sl_random(nonce, sizeof(nonce));
    EVP_EncodeBlock((unsigned char *) key, nonce, sizeof(nonce));
    char hosthdr[300];
    if (port == 443) snprintf(hosthdr, sizeof(hosthdr), "%s", host);
    else snprintf(hosthdr, sizeof(hosthdr), "%s:%d", host, port);
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\nUser-Agent: steamlink-muos\r\n\r\n",
             path, hosthdr, key);
    if (tls_write_all(c, req, strlen(req)) || read_headers(c, headers, sizeof(headers)) ||
        !strstr(headers, " 101 ")) {
        snprintf(err, errlen, "Servidor da Steam recusou o WebSocket");
        tls_close(c);
        return NULL;
    }
    ws_conn *ws = calloc(1, sizeof(*ws));
    ws->tls = c;
    return ws;
}

static int ws_send_frame(ws_conn *ws, uint8_t opcode, const void *data, size_t len) {
    uint8_t hdr[14];
    size_t hl = 0;
    hdr[hl++] = 0x80 | opcode;
    if (len < 126) {
        hdr[hl++] = 0x80 | (uint8_t) len;
    } else if (len < 65536) {
        hdr[hl++] = 0x80 | 126;
        hdr[hl++] = (uint8_t) (len >> 8);
        hdr[hl++] = (uint8_t) len;
    } else {
        hdr[hl++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[hl++] = (uint8_t) ((uint64_t) len >> (8 * i));
    }
    uint8_t mask[4];
    sl_random(mask, 4);
    memcpy(hdr + hl, mask, 4);
    hl += 4;
    uint8_t *frame = malloc(hl + len);
    memcpy(frame, hdr, hl);
    for (size_t i = 0; i < len; i++) frame[hl + i] = ((const uint8_t *) data)[i] ^ mask[i & 3];
    int rc = tls_write_all(ws->tls, frame, hl + len);
    free(frame);
    return rc;
}

int ws_send(ws_conn *ws, const void *data, size_t len) { return ws_send_frame(ws, 2, data, len); }

int ws_recv(ws_conn *ws, uint8_t **out, int timeout_ms) {
    *out = NULL;
    if (!SSL_pending(ws->tls->ssl)) {
        struct pollfd p = {.fd = ws->tls->fd, .events = POLLIN};
        int r = poll(&p, 1, timeout_ms);
        if (r == 0) return 0;
        if (r < 0) return -1;
    }
    uint8_t *msg = NULL;
    size_t msglen = 0;
    for (;;) { /* a frame has started arriving: read it (and any continuations) completely */
        uint8_t h[2];
        if (tls_read_exact(ws->tls, h, 2)) break;
        uint8_t opcode = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (len == 126 || len == 127) {
            uint8_t ext[8];
            int n = len == 126 ? 2 : 8;
            if (tls_read_exact(ws->tls, ext, (size_t) n)) break;
            len = 0;
            for (int i = 0; i < n; i++) len = len << 8 | ext[i];
        }
        uint8_t mask[4] = {0};
        if ((h[1] & 0x80) && tls_read_exact(ws->tls, mask, 4)) break;
        if (len > (16u << 20) || msglen + len > (16u << 20)) break;
        uint8_t *payload = malloc(len ? len : 1);
        if (len && tls_read_exact(ws->tls, payload, len)) {
            free(payload);
            break;
        }
        if (h[1] & 0x80)
            for (uint64_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
        if (opcode == 8) { /* close */
            free(payload);
            break;
        }
        if (opcode == 9) { /* ping */
            ws_send_frame(ws, 10, payload, len);
            free(payload);
            continue;
        }
        if (opcode == 10) {
            free(payload);
            continue;
        }
        msg = realloc(msg, msglen + len + 1);
        memcpy(msg + msglen, payload, len);
        msglen += len;
        free(payload);
        if (h[0] & 0x80) {
            *out = msg;
            return msglen ? (int) msglen : 0;
        }
    }
    free(msg);
    return -1;
}

void ws_close(ws_conn *ws) {
    if (!ws) return;
    ws_send_frame(ws, 8, NULL, 0);
    tls_close(ws->tls);
    free(ws);
}
