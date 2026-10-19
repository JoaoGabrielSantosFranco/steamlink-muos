#include "discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pb.h"
#include "slcrypto.h"

#define LAN_PORT 27036
static const uint8_t SIGNATURE[8] = {0xff, 0xff, 0xff, 0xff, 0x21, 0x4c, 0x5f, 0xa0};

enum {
    MSG_DISCOVERY = 0, MSG_STATUS = 1, MSG_STREAMING_REQUEST = 5, MSG_STREAMING_RESPONSE = 6,
    MSG_PROOF_REQUEST = 7, MSG_PROOF_RESPONSE = 8, MSG_STREAMING_PROGRESS = 13,
};

static int open_socket(bool bind_lan_port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    a.sin_port = htons(bind_lan_port ? LAN_PORT : 0);
    if (bind(s, (struct sockaddr *) &a, sizeof(a)) < 0) {
        a.sin_port = 0;
        if (bind(s, (struct sockaddr *) &a, sizeof(a)) < 0) {
            close(s);
            return -1;
        }
    }
    return s;
}

size_t lan_pack(const pbw *hdr, const pbw *body, uint8_t *pkt, size_t cap) {
    size_t n = 0;
    if (8 + 4 + hdr->len + 4 + body->len > cap) return 0;
    memcpy(pkt, SIGNATURE, 8);
    n = 8;
    put_le32(pkt + n, (uint32_t) hdr->len);
    n += 4;
    memcpy(pkt + n, hdr->buf, hdr->len);
    n += hdr->len;
    put_le32(pkt + n, (uint32_t) body->len);
    n += 4;
    memcpy(pkt + n, body->buf, body->len);
    n += body->len;
    return n;
}

static void send_framed(int s, const struct sockaddr_in *to, const pbw *hdr, const pbw *body) {
    uint8_t pkt[2048];
    size_t n = lan_pack(hdr, body, pkt, sizeof(pkt));
    if (n) sendto(s, pkt, n, 0, (const struct sockaddr *) to, sizeof(*to));
}

#define parse_framed lan_unpack
int lan_unpack(const uint8_t *pkt, size_t len, const uint8_t **body, size_t *bodylen) {
    if (len < 16 || memcmp(pkt, SIGNATURE, 8) != 0) return -1;
    uint32_t hl = get_le32(pkt + 8);
    if (12 + (size_t) hl + 4 > len) return -1;
    int type = 0;
    pbr r = pbr_make(pkt + 12, hl);
    pbf f;
    while (pbr_next(&r, &f) == 1)
        if (f.field == 2) type = (int) f.v;
    uint32_t bl = get_le32(pkt + 12 + hl);
    if (16 + (size_t) hl + bl > len) return -1;
    *body = pkt + 16 + hl;
    *bodylen = bl;
    return type;
}

static void broadcast_targets(int s, const pbw *hdr, const pbw *body) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT)};
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    send_framed(s, &to, hdr, body);
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_BROADCAST) ||
            (i->ifa_flags & IFF_LOOPBACK) || !i->ifa_broadaddr)
            continue;
        to.sin_addr = ((struct sockaddr_in *) i->ifa_broadaddr)->sin_addr;
        send_framed(s, &to, hdr, body);
    }
    freeifaddrs(ifs);
}

int discover_hosts(uint64_t client_id, sl_host *hosts, int max, int timeout_ms, const volatile int *cancel) {
    int s = open_socket(false);
    if (s < 0) return 0;
    pbw hdr, body;
    pbw_init(&hdr);
    pbw_init(&body);
    pb_uint(&hdr, 1, client_id);
    pb_uint(&hdr, 2, MSG_DISCOVERY);
    pb_uint(&body, 1, 1);
    int found = 0;
    uint64_t end = now_ms() + (uint64_t) timeout_ms, next_send = 0;
    while (now_ms() < end && found < max && !(cancel && *cancel)) {
        if (now_ms() >= next_send) {
            broadcast_targets(s, &hdr, &body);
            next_send = now_ms() + 1000;
        }
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) continue;
        uint8_t buf[4096];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        const uint8_t *b;
        size_t bl;
        if (n <= 0 || parse_framed(buf, (size_t) n, &b, &bl) != MSG_STATUS) continue;
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        bool dup = false;
        for (int i = 0; i < found; i++) dup |= strcmp(hosts[i].ip, ip) == 0;
        if (dup) continue;
        sl_host *h = &hosts[found++];
        snprintf(h->ip, sizeof(h->ip), "%s", ip);
        snprintf(h->hostname, sizeof(h->hostname), "%s", ip);
        pbr r = pbr_make(b, bl);
        pbf f;
        while (pbr_next(&r, &f) == 1)
            if (f.field == 4 && f.wire == 2)
                snprintf(h->hostname, sizeof(h->hostname), "%.*s", (int) f.len, (const char *) f.data);
        LOG("discovery: found %s (%s)", h->hostname, h->ip);
    }
    pbw_free(&hdr);
    pbw_free(&body);
    close(s);
    return found;
}

static void make_header(const sl_config *cfg, int msg_type, pbw *hdr) {
    uint8_t id[8], token[64];
    put_le64(id, cfg->device_id);
    size_t tl = sl_symmetric_encrypt(cfg->secret, cfg->secret_len, id, 8, token);
    pbw_init(hdr);
    pb_uint(hdr, 1, cfg->device_id);
    pb_uint(hdr, 2, (uint64_t) msg_type);
    pb_bytes(hdr, 5, token, tl);
}

static const char *streaming_result_text(int r) {
    switch (r) {
        case 1: return "PC recusou: dispositivo nao autorizado (pareie de novo)";
        case 2: return "PC com a tela bloqueada";
        case 3: return "PC falhou ao iniciar o streaming";
        case 4: return "PC ocupado (outro streaming ativo?)";
        case 5: return "Streaming ja em andamento";
        case 6: return "Cancelado no PC";
        case 7: return "Drivers de streaming nao instalados no PC";
        case 8: return "Remote Play desativado no PC";
        case 9: return "PC esta transmitindo (broadcast ativo)";
        case 10: return "SteamVR ativo no PC";
        case 11: return "PC pede PIN";
        case 12: return "Transporte indisponivel";
        case 13: return "PC invisivel";
        default: return "PC recusou o streaming";
    }
}

int request_stream(const sl_config *cfg, const char *ip, int *port, uint8_t *session_key, size_t *keylen,
                   char *err, size_t errlen, const volatile int *cancel) {
    int s = open_socket(true);
    if (s < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT)};
    if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) {
        snprintf(err, errlen, "IP invalido: %s", ip);
        close(s);
        return -1;
    }
    uint32_t request_id;
    sl_random(&request_id, sizeof(request_id));
    request_id = (request_id & 0x7fffffff) | 1;

    uint8_t id[8], token[64];
    put_le64(id, cfg->device_id);
    size_t tl = sl_symmetric_encrypt(cfg->secret, cfg->secret_len, id, 8, token);
    pbw req, hdr;
    pbw_init(&req);
    pb_uint(&req, 1, request_id);
    pb_int(&req, 2, cfg->width);
    pb_int(&req, 3, cfg->height);
    pb_int(&req, 4, 2);
    pb_str(&req, 5, "1.4.2.321");
    pb_bool(&req, 6, true);
    pb_bytes(&req, 7, token, tl);
    pb_bool(&req, 9, true);
    pb_bool(&req, 10, true);
    pb_bool(&req, 11, true);
    pb_uint(&req, 14, 1); /* k_EStreamTransportUDP */
    pb_uint(&req, 14, 5); /* k_EStreamTransportUDP_SNS */
    pb_uint(&req, 16, 1); /* form factor phone, as the working Python probe */
    pb_int(&req, 17, 1);
    pb_uint(&req, 20, 2); /* Big Picture */
    pb_int(&req, 21, cfg->fps);
    pb_int(&req, 22, 1);
    make_header(cfg, MSG_STREAMING_REQUEST, &hdr);
    send_framed(s, &to, &hdr, &req);
    pbw_free(&hdr);
    LOG("-> StreamingRequest %u to %s", request_id, ip);

    int ret = -1;
    snprintf(err, errlen, "PC nao respondeu ao pedido de streaming");
    uint64_t end = now_ms() + 30000, resend = now_ms() + 5000;
    while (now_ms() < end) {
        if (cancel && *cancel) {
            snprintf(err, errlen, "Cancelado");
            break;
        }
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 200) <= 0) {
            if (now_ms() > resend) {
                make_header(cfg, MSG_STREAMING_REQUEST, &hdr);
                send_framed(s, &to, &hdr, &req);
                pbw_free(&hdr);
                resend = now_ms() + 5000;
            }
            continue;
        }
        uint8_t buf[4096];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        const uint8_t *b;
        size_t bl;
        int type = n > 0 ? parse_framed(buf, (size_t) n, &b, &bl) : -1;
        if (type < 0 || from.sin_addr.s_addr != to.sin_addr.s_addr || from.sin_port != htons(LAN_PORT)) continue;
        pbr r = pbr_make(b, bl);
        pbf f;
        if (type == MSG_PROOF_REQUEST) {
            const uint8_t *challenge = NULL;
            size_t cl = 0;
            uint64_t rid = 0;
            while (pbr_next(&r, &f) == 1) {
                if (f.field == 1) challenge = f.data, cl = f.len;
                if (f.field == 2) rid = f.v;
            }
            if (!challenge || cl > 256 || rid != request_id) continue;
            uint8_t proof[512];
            size_t pl = sl_symmetric_encrypt(cfg->secret, cfg->secret_len, challenge, cl, proof);
            pbw resp;
            pbw_init(&resp);
            pb_bytes(&resp, 1, proof, pl);
            pb_uint(&resp, 2, rid);
            make_header(cfg, MSG_PROOF_RESPONSE, &hdr);
            send_framed(s, &to, &hdr, &resp);
            pbw_free(&hdr);
            pbw_free(&resp);
            LOG("<- ProofRequest -> ProofResponse");
            resend = end;
        } else if (type == MSG_STREAMING_PROGRESS) {
            LOG("<- StreamingProgress");
            resend = end;
        } else if (type == MSG_STREAMING_RESPONSE) {
            int result = -1;
            uint64_t rid = 0;
            const uint8_t *ek = NULL;
            size_t el = 0;
            *port = 0;
            while (pbr_next(&r, &f) == 1) {
                if (f.field == 1) rid = f.v;
                if (f.field == 2) result = (int) f.v;
                if (f.field == 3) *port = (int) f.v;
                if (f.field == 4) ek = f.data, el = f.len;
            }
            if (rid != request_id) continue;
            LOG("<- StreamingResponse result=%d port=%d", result, *port);
            if (result != 0) {
                snprintf(err, errlen, "%s (codigo %d)", streaming_result_text(result), result);
                break;
            }
            uint8_t key[256];
            int kl = ek && el <= sizeof(key) ? sl_symmetric_decrypt(cfg->secret, cfg->secret_len, ek, el, key) : -1;
            if (kl != 16 && kl != 32) {
                snprintf(err, errlen, "Falha ao decifrar a chave de sessao");
                break;
            }
            memcpy(session_key, key, (size_t) kl);
            *keylen = (size_t) kl;
            ret = 0;
            break;
        }
    }
    pbw_free(&req);
    close(s);
    return ret;
}
