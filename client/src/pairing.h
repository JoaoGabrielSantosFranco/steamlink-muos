#pragma once
/* On-device PIN pairing through Valve's Remote Client service (docs/PROTOCOLO.md section 4). */
#include <stdatomic.h>

enum { PAIR_STARTING, PAIR_SHOW_PIN, PAIR_DONE, PAIR_FAILED };

typedef struct {
    const char *config_path; /* device.json to write on success */
    const char *cafile;      /* bundled CA certificates (optional) */
    const char *device_name; /* shown in Steam's paired device list */
    volatile int cancel;
    atomic_int phase;
    char pin[8];
    char message[200];
} pair_job;

/* Thread entry point: runs the whole pairing; check job->phase for progress. */
void *pair_run(void *job);
