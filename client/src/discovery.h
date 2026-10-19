#pragma once
#include <stddef.h>
#include <stdint.h>

#include "util.h"

typedef struct {
    char ip[64];
    char hostname[64];
} sl_host;

/* Broadcasts a discovery request on UDP 27036 and collects Status replies. Returns hosts found. */
int discover_hosts(uint64_t client_id, sl_host *hosts, int max, int timeout_ms);

/* LAN StreamingRequest incl. ProofRequest. Returns 0 and fills port/key on success, else -1 with err set. */
int request_stream(const sl_config *cfg, const char *ip, int *port, uint8_t *session_key, size_t *keylen,
                   char *err, size_t errlen);
