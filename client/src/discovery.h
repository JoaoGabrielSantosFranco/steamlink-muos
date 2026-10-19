#pragma once
#include <stddef.h>
#include <stdint.h>

#include "pb.h"
#include "util.h"

/* LAN framing on UDP 27036: signature, u32 header length, header, u32 body length, body. */
size_t lan_pack(const pbw *hdr, const pbw *body, uint8_t *pkt, size_t cap);
/* Returns the header's msg_type and points body at the body, or -1 if the packet isn't framed. */
int lan_unpack(const uint8_t *pkt, size_t len, const uint8_t **body, size_t *bodylen);

typedef struct {
    char ip[64];
    char hostname[64];
} sl_host;

/* Both calls return early once *cancel becomes non-zero (cancel may be NULL). */

/* Broadcasts a discovery request on UDP 27036 and collects Status replies. Returns hosts found. */
int discover_hosts(uint64_t client_id, sl_host *hosts, int max, int timeout_ms, const volatile int *cancel);

/* LAN StreamingRequest incl. ProofRequest. Returns 0 and fills port/key on success, else -1 with err set. */
int request_stream(const sl_config *cfg, const char *ip, int *port, uint8_t *session_key, size_t *keylen,
                   char *err, size_t errlen, const volatile int *cancel);
