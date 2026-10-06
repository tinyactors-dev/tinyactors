/* host_internal.h — ta_host internals shared by host.c and router.c. */
#ifndef TINYACTORS_HOST_HOST_INTERNAL_H
#define TINYACTORS_HOST_HOST_INTERNAL_H

#include <tinyactors/address.h>
#include <tinyactors/host.h>

#include <stdlib.h>
#include <string.h>

#define SERIALS_SHIFT   32
#define EFFECTS_PER_RUN 4096 /* effects per run before TA_RUN_EFFECTS_FULL */

typedef struct host_segment {
    int live;
    int busy;
    const ta_image *image;
    ta_system *system;
    void *memory;
    ta_system_config config; /* current capacities */
    ta_time deadline;
} host_segment;

typedef struct chart_version { /* the current version of a chart name */
    char *name;
    uint32_t length, segment, chart;
} chart_version;

typedef struct migration_job {
    uint32_t id;
    int active;
    char *name;
    uint32_t name_length;
    uint32_t flags;
    uint64_t session;
    ta_time deadline;
    int walking;
    uint32_t walk_segment, cursor;
    uint64_t *retry;
    uint8_t *woken;
    uint32_t retry_count, retry_capacity;
    uint32_t migrated, failed;
    uint64_t *left; /* after it ended: stragglers (until the next run) */
    uint32_t left_count;
} migration_job;

typedef struct effect_strings {
    size_t offset[5]; /* target, event, sendid, aux, msgid in the string pool (SIZE_MAX: empty) */
} effect_strings;

struct ta_host {
    ta_host_config config;
    char base[TA_ADDRESS_SYSTEM_MAX + 16]; /* tinyactors://<system_id>/ */
    uint32_t base_length;
    ta_datamodel_binding datamodels[8];
    ta_io_processor_descriptor
        io_processors[17]; /* [0] the shared remote processor, then config.io_processors */
    uint32_t io_processor_count;
    host_segment *segments;
    uint32_t segment_count, segment_capacity, round_robin;
    chart_version *versions;
    uint32_t version_count, version_capacity;
    /* session -> segment, for sessions living outside their own range (open addressing) */
    uint64_t *moved_keys;
    uint32_t *moved_values, moved_capacity, moved_count;
    ta_time now;
    /* effects of the current batch */
    ta_host_effect *effects;
    effect_strings *effect_strings;
    uint32_t effect_count, effect_capacity, effects_ready;
    char *pool;
    size_t pool_length, pool_capacity;
    /* jobs */
    migration_job *jobs;
    uint32_t job_count, job_capacity, job_sequence;
    /* sweep cursor */
    uint32_t sweep_segment, sweep_cursor;
    /* scratch for blobs */
    unsigned char *blob, *blob2;
    size_t blob_capacity, blob2_capacity;
    /* error.communication for senders of the segment being drained (applied after its ack) */
    struct {
        uint32_t actor;
        size_t session_id; /* in the pool, (size_t)-1: none */
        uint32_t session_id_length;
    } *errors;
    uint32_t error_count, error_capacity;
    uint64_t routed, migrated, retired;
    struct ta_router *router;  /* other systems (router.c); NULL until a peer exists */
    uint64_t message_sequence; /* outbound message ids */
    uint32_t draining;         /* segment whose effects are being drained (TA_NIL: none) */
};

/* host.c */
void *grow(void *p, uint32_t *capacity, uint32_t need, size_t elem);
size_t pool_put(ta_host *h, ta_string_view s);
ta_host_effect *effect_new(ta_host *h, uint32_t kind, uint32_t segment);
void set_effect_strings(ta_host *h, ta_string_view t, ta_string_view event, ta_string_view sid,
                        ta_string_view aux);
ta_status send_to(ta_host *h, uint32_t si, uint32_t actor, const ta_event_in *event);
void communication_error(ta_host *h, uint32_t si, uint32_t actor, ta_string_view sendid);
uint32_t moved_get(const ta_host *h, uint64_t key);

/* router.c */
/* an SCXML send of segment si to another system (target not ours) */
void ta_router_outbound(ta_host *h, uint32_t si, const ta_effect *f);
void ta_router_time(ta_host *h);              /* expire queued envelopes */
ta_time ta_router_deadline(const ta_host *h); /* the next expiry (UINT64_MAX: none) */
void ta_router_free(ta_host *h);
size_t ta_router_snapshot(const ta_host *h, unsigned char *out, size_t capacity); /* dedup window */
int ta_router_restore(ta_host *h, const unsigned char *p, size_t length);

#endif /* TINYACTORS_HOST_HOST_INTERNAL_H */
