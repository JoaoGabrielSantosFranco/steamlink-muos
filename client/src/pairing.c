#include "pairing.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "discovery.h"
#include "pb.h"
#include "slcrypto.h"
#include "tls.h"
#include "util.h"

#define CM_LIST_URL "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets"
#define LAN_PORT 27036
#define PIN_TIMEOUT_MS (10 * 60 * 1000)

enum { MSG_DISCOVERY = 0, MSG_AUTH_RESPONSE = 4, MSG_AUTH_CONFIRMED = 14 };

/* Valve's public universe keys used to encrypt the key escrow ticket (re/keys). */
static const char KEY_PUBLIC[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIDANBgkqhkiG9w0BAQEFAAOCAQ0AMIIBCAKCAQEAx7iQLqQkm17hJPpGiOpb/QJHtQpi\n"
    "Kk2pm1zomiuqs3S4bwOroZ+mX9yGys5GpE61q0IaadEDeMO4ZyPfwjlb6kqznauRJ2Hj40Or\n"
    "mCSwRW5X2rre/aTRwcerKF5kfbEYi8z47+WKdZXM3TGEXAz3W4q0snB9IY721GoRXQhfzHVK\n"
    "dcUN2NAYk2z16WQsZkVjfsO1CUgRuc7tsX0jR4HOVEXC3bOKeHyVhRIWfXovPzR5rCchqt2Y\n"
    "EZ7oMvMhxinGVyQ0okL1r/UTB87aCCMu9WRg1PGRyPSISj4qFDYXEAxDz1WtH8fHBSB9hoaX\n"
    "zJsj3u1GB2MvfqVNSYmDPckg5wIBEQ==\n"
    "-----END PUBLIC KEY-----\n";
static const char KEY_BETA[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIDANBgkqhkiG9w0BAQEFAAOCAQ0AMIIBCAKCAQEAttWMEpWwStit8v+kEh1cUr4Tt6w+\n"
    "K+k4RA2bUBC5vrSrTMfLZQSst/mzKPz4MZEcft5p1oJwSTs/a1kps79LFuHUf8wxXVTq6m2i\n"
    "Orc10yVWnGjclSG4bemuTdORmkJxe5rkRPJKUZXKhrQNgGOKAKgLoXSAgvqRbSjpSZpmH1In\n"
    "qhDPDYXTlZ6UQUrsLpXwQ06WwMNyDi0/ODr5AdwP8vyK+g3mgj8cd8rUpRyXzvwNvL8rNTFx\n"
    "iC5vxCnicWyOmFTWebmslJZkpUDrGp6dkpxRVt+KgM34Onw7flcvmsdyVvn+/MDtelRRIajp\n"
    "rPYpIQIN1sUCbOw+WZbHFgG3CwIBEQ==\n"
    "-----END PUBLIC KEY-----\n";
static const char KEY_OTHER[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBIDANBgkqhkiG9w0BAQEFAAOCAQ0AMIIBCAKCAQEAwbALFqA/rm6FZ+9/X+RVGZ58WXUp\n"
    "wX/5L9gAdd2ykVIXw2cI23hmlLlQj2FjzWDc9sGvGkO1OAFu+/0MQ01mP4YtSt7ArFfe5p3D\n"
    "tm6xw/Icckq+31dkYHUJOJ/gDGTfV6UiDZ+cpd2WnxEubRuoqSFlSyUCFcZqaBFaX6zWLxZZ\n"
    "TXngRMGlQ6xW6L/d5d9b6bAVj0ug4UYXwpvwaqvnrzjK7mw2XU+sVhg/T42SBb6ijGEws0E8\n"
    "cauut3cH6czi/mcH6eUnWwc5/GNfadDE2n2h2y1jLW2dBuovZT5RY6gcBbaXCF2KtkbJ99CX\n"
    "KKSM3E/CumvChfK/vYJMKAtM9wIBEQ==\n"
    "-----END PUBLIC KEY-----\n";

static void fail(pair_job *j, const char *msg) {
    snprintf(j->message, sizeof(j->message), "%s", msg);
    LOG("pairing failed: %s", msg);
    atomic_store(&j->phase, PAIR_FAILED);
}

/* Picks the index-th "endpoint" whose object says "type":"websockets". */
static bool pick_endpoint(const char *json, int index, char *host, size_t hl, int *port) {
    const char *p = json;
    int found = 0;
    while ((p = strstr(p, "\"endpoint\""))) {
        const char *q = strchr(p + 10, '"');
        const char *end = q ? strchr(q + 1, '"') : NULL;
        const char *obj_end = strchr(p, '}');
        if (!q || !end || !obj_end) break;
        const char *type = strstr(p, "\"type\"");
        const char *wsword = type ? strstr(type, "websockets") : NULL;
        bool ws = type && type < obj_end && wsword && wsword < obj_end;
        if (ws && found++ == index) {
            size_t len = (size_t) (end - q - 1);
            if (len >= hl) return false;
            memcpy(host, q + 1, len);
            host[len] = 0;
            *port = 443;
            char *colon = strrchr(host, ':');
            if (colon) {
                *colon = 0;
                *port = atoi(colon + 1);
            }
            return true;
        }
        p = end;
    }
    return false;
}

static int send_client_message(ws_conn *ws, int field, const pbw *sub) {
    pbw outer;
    pbw_init(&outer);
    pb_msg(&outer, field, sub);
    int rc = ws_send(ws, outer.buf, outer.len);
    pbw_free(&outer);
    return rc;
}

/* Waits for a ServerMessage carrying `field`; copies that submessage into out. */
static bool expect(pair_job *j, ws_conn *ws, int field, uint8_t **out, size_t *outlen) {
    uint64_t end = now_ms() + 30000;
    while (now_ms() < end && !j->cancel) {
        uint8_t *msg;
        int n = ws_recv(ws, &msg, 200);
        if (n < 0) return false;
        if (n == 0) continue;
        pbr r = pbr_make(msg, (size_t) n);
        pbf f;
        bool ok = false;
        int result = 1;
        while (pbr_next(&r, &f) == 1) {
            if (f.field == 1 && f.wire == 0) result = (int) f.v;
            if (f.field == field && f.wire == 2) {
                *out = malloc(f.len ? f.len : 1);
                memcpy(*out, f.data, f.len);
                *outlen = f.len;
                ok = true;
            }
        }
        free(msg);
        if (ok) return true;
        if (result != 1) {
            LOG("pairing: server result %d", result);
            return false;
        }
    }
    return false;
}

static bool save_config(pair_job *j, uint64_t device_id, const uint8_t *secret, uint64_t steamid) {
    char tmp[4200], hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", secret[i]);
    snprintf(tmp, sizeof(tmp), "%s.tmp", j->config_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    char when[32];
    time_t t = time(NULL);
    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime(&t));
    fprintf(f,
            "{\n  \"device_id\": %llu,\n  \"secret\": \"%s\",\n  \"device_name\": \"%s\",\n  \"steamid\": %llu,\n"
            "  \"paired_at\": \"%s\"\n}\n",
            (unsigned long long) device_id, hex, j->device_name, (unsigned long long) steamid, when);
    fclose(f);
    chmod(tmp, 0600);
    return rename(tmp, j->config_path) == 0;
}

static const char *auth_result_text(int r) {
    switch (r) {
        case 1: return "O PC recusou o pareamento";
        case 2: return "O Steam do PC nao esta logado";
        case 3: return "O PC esta offline";
        case 4: return "O PC esta ocupado";
        case 6: return "Tempo esgotado: o PIN nao foi digitado a tempo";
        case 8: return "Pareamento cancelado no PC";
        default: return "O pareamento falhou";
    }
}

typedef struct {
    pair_job *job;
    ws_conn *ws;
    int udp;
    uint64_t session_id, device_id;
    uint8_t priv[32], secret[32];
    bool done;
} pairing;

/* Handles a framed LAN packet that arrived over UDP (from != NULL) or relayed over the WebSocket. */
static void on_lan_packet(pairing *p, const uint8_t *pkt, size_t len, const struct sockaddr_in *from) {
    const uint8_t *body;
    size_t bl;
    if (lan_unpack(pkt, len, &body, &bl) != MSG_AUTH_RESPONSE) return;
    int result = -1;
    uint64_t steamid = 0;
    const uint8_t *auth_key = NULL, *token = NULL;
    size_t akl = 0, tl = 0;
    pbr r = pbr_make(body, bl);
    pbf f;
    while (pbr_next(&r, &f) == 1) {
        if (f.field == 1) result = (int) f.v;
        if (f.field == 2) steamid = f.v;
        if (f.field == 3 && f.wire == 2) auth_key = f.data, akl = f.len;
        if (f.field == 4 && f.wire == 2) token = f.data, tl = f.len;
    }
    LOG("pairing: AuthorizationResponse result=%d auth_key=%zu", result, akl);
    if (result != 0) {
        fail(p->job, auth_result_text(result));
        p->done = true;
        return;
    }
    uint8_t secret[32];
    memcpy(secret, p->secret, 32);
    if (akl == 32) { /* PC's X25519 key masked with SHA256(PIN); prove it via the device token */
        uint8_t mask[32], pc_pub[32], shared[32], plain[64], id[8];
        sl_sha256(p->job->pin, strlen(p->job->pin), mask);
        for (int i = 0; i < 32; i++) pc_pub[i] = auth_key[i] ^ mask[i];
        put_le64(id, p->device_id);
        int n = token && tl <= sizeof(plain) && sl_x25519_shared_sha256(p->priv, pc_pub, shared)
                    ? sl_symmetric_decrypt(shared, 32, token, tl, plain)
                    : -1;
        if (n != 8 || memcmp(plain, id, 8) != 0) {
            fail(p->job, "A confirmacao do PC nao bateu (PIN errado?)");
            p->done = true;
            return;
        }
        memcpy(secret, shared, 32);
    }
    if (!save_config(p->job, p->device_id, secret, steamid)) {
        fail(p->job, "Nao consegui gravar o device.json na pasta do app");
        p->done = true;
        return;
    }
    /* Tell the PC we stored the key, over whichever path its answer came in. */
    uint8_t id[8], tok[64], pkt_out[512];
    put_le64(id, p->device_id);
    size_t toklen = sl_symmetric_encrypt(secret, 32, id, 8, tok);
    pbw hdr, conf;
    pbw_init(&hdr);
    pbw_init(&conf);
    pb_uint(&hdr, 1, p->device_id);
    pb_uint(&hdr, 2, MSG_AUTH_CONFIRMED);
    pb_bytes(&hdr, 5, tok, toklen);
    pb_uint(&conf, 1, 0);
    size_t n = lan_pack(&hdr, &conf, pkt_out, sizeof(pkt_out));
    if (from) {
        sendto(p->udp, pkt_out, n, 0, (const struct sockaddr *) from, sizeof(*from));
    } else {
        pbw note;
        pbw_init(&note);
        pb_fixed64(&note, 1, p->session_id);
        pb_fixed64(&note, 2, steamid);
        pb_bytes(&note, 4, pkt_out, n);
        send_client_message(p->ws, 7, &note);
        pbw_free(&note);
    }
    pbw_free(&hdr);
    pbw_free(&conf);
    OPENSSL_cleanse(secret, sizeof(secret));
    LOG("pairing: paired with steamid %llu", (unsigned long long) steamid);
    atomic_store(&p->job->phase, PAIR_DONE);
    p->done = true;
}

static int open_lan_socket(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *) &a, sizeof(a)) != 0) {
        a.sin_port = 0;
        bind(s, (struct sockaddr *) &a, sizeof(a));
    }
    return s;
}

static void send_discovery(pairing *p, uint32_t seq) {
    pbw hdr, body;
    pbw_init(&hdr);
    pbw_init(&body);
    pb_uint(&hdr, 1, p->device_id);
    pb_uint(&hdr, 2, MSG_DISCOVERY);
    pb_uint(&body, 1, seq);
    uint8_t pkt[256];
    size_t n = lan_pack(&hdr, &body, pkt, sizeof(pkt));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT),
                             .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
    sendto(p->udp, pkt, n, 0, (struct sockaddr *) &to, sizeof(to));
    pbw_free(&hdr);
    pbw_free(&body);
}

void *pair_run(void *arg) {
    pair_job *j = arg;
    pairing p = {.job = j, .udp = -1};
    char err[200] = "";
    atomic_store(&j->phase, PAIR_STARTING);
    snprintf(j->message, sizeof(j->message), "Conectando aos servidores da Steam...");

    char *list = https_get(CM_LIST_URL, j->cafile, NULL, err, sizeof(err));
    if (!list) {
        fail(j, err[0] ? err : "Sem acesso a internet");
        return NULL;
    }
    char host[256];
    int port;
    for (int i = 0; i < 5 && !p.ws && !j->cancel && pick_endpoint(list, i, host, sizeof(host), &port); i++) {
        LOG("pairing: trying wss://%s:%d/remoteplay/", host, port);
        p.ws = ws_connect(host, port, "/remoteplay/", j->cafile, err, sizeof(err));
    }
    free(list);
    if (!p.ws) {
        fail(j, err[0] ? err : "Nenhum servidor da Steam respondeu");
        return NULL;
    }

    uint8_t *sub = NULL;
    size_t sl = 0;
    pbr r;
    pbf f;
    pbw m, ticket, req;
    pbw_init(&m);
    send_client_message(p.ws, 1, &m); /* create_session_request {} */
    int euniverse = 1;
    if (!expect(j, p.ws, 2, &sub, &sl)) {
        fail(j, "A Steam nao abriu a sessao de pareamento");
        goto out;
    }
    r = pbr_make(sub, sl);
    while (pbr_next(&r, &f) == 1) {
        if (f.field == 1) p.session_id = f.v;
        if (f.field == 2 && f.v) euniverse = (int) f.v;
    }
    free(sub);

    pb_fixed64(&m, 1, p.session_id);
    send_client_message(p.ws, 2, &m); /* start_pairing_request */
    pbw_free(&m);
    if (!expect(j, p.ws, 3, &sub, &sl)) {
        fail(j, "A Steam nao gerou o PIN");
        goto out;
    }
    uint64_t pin = 0;
    r = pbr_make(sub, sl);
    while (pbr_next(&r, &f) == 1)
        if (f.field == 1) pin = f.v;
    free(sub);
    snprintf(j->pin, sizeof(j->pin), "%04u", (unsigned) pin);

    /* Identity + key escrow ticket, built like the official client (tools/proto/pair.py). */
    do sl_random(&p.device_id, sizeof(p.device_id));
    while (!p.device_id);
    p.secret[0] = 1;
    sl_random(p.secret + 1, 31);
    uint8_t pub[32], mask[32], id[8], token[64], ticket_enc[1024];
    if (!sl_x25519_keypair(p.priv, pub)) {
        fail(j, "Falha ao gerar chaves");
        goto out;
    }
    pbw_init(&ticket);
    pb_bytes(&ticket, 1, j->pin, strlen(j->pin));
    pb_uint(&ticket, 2, p.device_id);
    pb_bytes(&ticket, 3, p.secret, 32);
    pb_uint(&ticket, 5, 0);
    pb_str(&ticket, 6, j->device_name);
    pb_str(&ticket, 7, j->device_name);
    pb_str(&ticket, 8, "");
    pb_uint(&ticket, 9, 0);
    const char *key = euniverse == 1 ? KEY_PUBLIC : euniverse == 2 ? KEY_BETA : KEY_OTHER;
    int el = sl_rsa_oaep_encrypt(key, ticket.buf, ticket.len, ticket_enc, sizeof(ticket_enc));
    pbw_free(&ticket);
    if (el < 0) {
        fail(j, "Falha ao cifrar o pedido de pareamento");
        goto out;
    }
    put_le64(id, p.device_id);
    size_t tl = sl_symmetric_encrypt(p.secret, 32, id, 8, token);
    sl_sha256(j->pin, strlen(j->pin), mask);
    for (int i = 0; i < 32; i++) mask[i] ^= pub[i];
    pbw_init(&req);
    pb_bytes(&req, 1, token, tl);
    pb_str(&req, 2, j->device_name);
    pb_bytes(&req, 3, ticket_enc, (size_t) el);
    pb_bytes(&req, 4, mask, 32);
    pb_uint(&req, 5, 1);
    pbw_init(&m);
    pb_fixed64(&m, 1, p.session_id);
    pb_fixed64(&m, 2, p.device_id);
    pb_msg(&m, 3, &req);
    send_client_message(p.ws, 3, &m); /* set_pairing_info_request */
    pbw_free(&m);
    pbw_free(&req);
    LOG("pairing: PIN shown, waiting for the PC");
    atomic_store(&j->phase, PAIR_SHOW_PIN);

    /* The PC may answer over the WebSocket (relayed) or straight over the LAN. */
    p.udp = open_lan_socket();
    uint64_t deadline = now_ms() + PIN_TIMEOUT_MS, next_discovery = 0;
    uint32_t seq = 0;
    while (!p.done && !j->cancel && now_ms() < deadline) {
        if (now_ms() >= next_discovery) {
            send_discovery(&p, ++seq);
            next_discovery = now_ms() + 2000;
        }
        uint8_t *msg;
        int n = ws_recv(p.ws, &msg, 100);
        if (n < 0) {
            fail(j, "A conexao com a Steam caiu");
            break;
        }
        if (n > 0) {
            pbr mr = pbr_make(msg, (size_t) n);
            while (pbr_next(&mr, &f) == 1 && !p.done) {
                if (f.field != 6 || f.wire != 2) continue; /* reply_packet { repeated bytes payload = 1 } */
                pbr rr = pbr_make(f.data, f.len);
                pbf g;
                while (pbr_next(&rr, &g) == 1 && !p.done)
                    if (g.field == 1 && g.wire == 2) on_lan_packet(&p, g.data, g.len, NULL);
            }
            free(msg);
        }
        struct pollfd pf = {.fd = p.udp, .events = POLLIN};
        while (!p.done && poll(&pf, 1, 0) > 0) {
            uint8_t buf[2048];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t got = recvfrom(p.udp, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
            if (got <= 0) break;
            on_lan_packet(&p, buf, (size_t) got, &from);
        }
    }
    if (!p.done && !j->cancel && atomic_load(&j->phase) != PAIR_FAILED)
        fail(j, "Tempo esgotado: o PIN nao foi digitado no PC");
out:
    if (j->cancel && atomic_load(&j->phase) != PAIR_DONE) fail(j, "Pareamento cancelado");
    OPENSSL_cleanse(p.priv, sizeof(p.priv));
    OPENSSL_cleanse(p.secret, sizeof(p.secret));
    if (p.udp >= 0) close(p.udp);
    ws_close(p.ws);
    return NULL;
}
