/* host_internal.h — ta_host internals shared by host.c and router.c.
 *
 * Every block the host owns comes from its config's ta_allocator
 * (host_alloc) and goes back with its size (host_free): each array keeps
 * its capacity, each string its length. Whatever must not fail half done
 * reserves first (host_effects_reserve, moved_reserve, host_grow): after the
 * reservation the step cannot run out of memory. */
#ifndef TINYACTORS_HOST_HOST_INTERNAL_H
#define TINYACTORS_HOST_HOST_INTERNAL_H

#include <tinyactors/address.h>
#include <tinyactors/host.h>

#define SERIALS_SHIFT 32
/* blocks the published effects still point into: each doubling of the
 * effect array or the pool while a batch is borrowed holds one */
#define HELD_BLOCKS_MAX 64
#define EFFECTS_PER_RUN 4096 /* effects per run before TA_RUN_EFFECTS_FULL */

typedef struct host_segment {
    int live;
    int busy;
    const ta_image *image;
    ta_system *system;
    void *memory;
    size_t memory_size;
    ta_system_config config; /* current capacities */
    ta_time deadline;
    int retiring; /* retired once its trace records were read, or a run later */
} host_segment;

typedef struct chart_version { /* the current version of a chart name */
    ta_string_view name;       /* in the image of `segment` (live while it is current) */
    uint32_t segment, chart;
} chart_version;

/* The per-chart counts of segments that were retired, by chart name, so a
 * host-wide count never goes back when a version retires. */
typedef struct retired_chart {
    char *name; /* name_length bytes (at least 1 allocated) */
    uint32_t name_length, pad;
    ta_chart_stats totals;
} retired_chart;

typedef struct migration_job {
    uint32_t id;
    int active;
    char *name; /* name_length bytes, NULL when empty */
    uint32_t name_length;
    uint32_t flags;
    uint64_t session;
    ta_time deadline;
    int walking;
    uint32_t walk_segment, cursor;
    uint64_t *retry; /* retry and woken: retry_capacity entries each */
    uint8_t *woken;
    uint32_t retry_count, retry_capacity;
    uint32_t migrated, failed;
    uint64_t *left; /* after it ended: stragglers (until the next run) */
    uint32_t left_count, left_capacity;
} migration_job;

typedef struct effect_strings {
    size_t offset[5]; /* target, event, sendid, aux, msgid in the string pool (SIZE_MAX: empty) */
} effect_strings;

/* error.communication for a sender, not delivered yet: kept while its
 * segment's effects are drained, and after that while its segment cannot
 * take it (no memory). The sendid is in the string pool, which keeps it when
 * effects_release compacts the pool. */
typedef struct pending_error {
    uint32_t segment, actor;
    size_t sendid; /* in the pool, (size_t)-1: none */
    uint32_t sendid_length;
} pending_error;

typedef struct host_rules { /* trace rules and their strings, the host's copy */
    ta_trace_rule *rules;
    uint32_t count;
    size_t bytes;
} host_rules;

typedef struct host_watch {
    uint32_t id;
    ta_time until;
    host_rules rules;
} host_watch;

struct ta_host {
    ta_host_config config;
    /* inside ta_system_run, where a datamodel may call back: only the inputs
     * (spawn, send, send_many, terminate) and lookups are taken, nothing
     * grows, every other call is TA_ERROR_STATE (host.h, "During a run") */
    uint32_t running;
    ta_trace_policy trace_policy; /* every segment's, and new ones' (config.system points here) */
    void *trace_rules;            /* its rules and their strings, the host's copy */
    size_t trace_rules_bytes;
    /* ta_host_trace_set's options and the watches, from which the policy is made */
    host_rules trace_options; /* rule.detail: kinds */
    uint32_t trace_flags, trace_kinds, trace_parts, trace_whole;
    host_watch *watches;
    uint32_t watch_count, watch_capacity, watch_sequence;
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
    /* session -> segment, for sessions living outside their own range (open
     * addressing; a removed entry keeps its key with value TA_NIL) */
    uint64_t *moved_keys;
    uint32_t *moved_values, moved_capacity, moved_count, moved_used;
    ta_time now;
    /* effects of the current batch; effects and effect_strings have effect_capacity entries */
    ta_host_effect *effects;
    effect_strings *effect_strings;
    uint32_t effect_count, effect_capacity, effects_ready;
    char *pool;
    size_t pool_length, pool_capacity;
    /* A published batch (ta_host_effects, effects_ready > 0) is borrowed until
     * the next ack, run or free. Growing the effect array or the pool in the
     * meantime copies into new blocks and holds the old ones, unchanged,
     * until then (effects_release frees them). */
    void *held[HELD_BLOCKS_MAX];
    size_t held_size[HELD_BLOCKS_MAX];
    uint32_t held_count;
    /* jobs */
    migration_job *jobs;
    uint32_t job_count, job_capacity, job_sequence;
    /* sweep cursor */
    uint32_t sweep_segment, sweep_cursor;
    /* scratch for blobs */
    unsigned char *blob, *blob2;
    size_t blob_capacity, blob2_capacity;
    pending_error *errors;
    uint32_t error_count, error_capacity;
    uint64_t routed, migrated, retired;
    retired_chart *retired_charts;
    uint32_t retired_chart_count, retired_chart_capacity;
    uint64_t fuel_clock;       /* the segments' common fuel clock (docs/adr/0006, section 9) */
    struct ta_router *router;  /* other systems (router.c); NULL until a peer exists */
    uint64_t message_sequence; /* outbound message ids */
    uint32_t draining;         /* segment whose effects are being drained (TA_NIL: none) */
    uint32_t expiry_behind;    /* an expired envelope could not be reported yet */
};

/* host.c */
void *host_alloc(ta_host *h, size_t size);
void host_free(ta_host *h, void *pointer, size_t size);
/* Room for `need` elements of `element` bytes in *array (capacity in
 * *capacity, new ones zeroed); 0 if the allocator refused (*array unchanged). */
int host_grow(ta_host *h, void **array, uint32_t *capacity, uint32_t need, size_t element);
/* Room for `count` more effects and `bytes` more pool bytes; 0 if refused. */
int host_effects_reserve(ta_host *h, uint32_t count, size_t bytes);
size_t host_pool_put(ta_host *h, ta_string_view s);
ta_host_effect *host_effect_new(ta_host *h, uint32_t kind, uint32_t segment);
void host_effect_strings_set(ta_host *h, ta_string_view t, ta_string_view event, ta_string_view sid,
                             ta_string_view aux);
ta_status host_send_to(ta_host *h, uint32_t si, uint32_t actor, const ta_event_in *event);
/* ta_host_locate, for callers that want either half (NULL: not wanted). */
ta_status host_locate(const ta_host *h, uint64_t session, uint32_t *segment, uint32_t *actor);
/* TA_ERROR_NO_MEMORY if it could be neither delivered nor kept; TA_OK otherwise
 * (also when the actor is gone: nobody to tell). */
ta_status host_communication_error(ta_host *h, uint32_t si, uint32_t actor, ta_string_view sendid);
uint32_t host_moved_get(const ta_host *h, uint64_t key);

/* router.c */
/* an SCXML send of segment si to another system (target not ours);
 * TA_ERROR_NO_MEMORY: nothing done, the effect stays for the next drain */
ta_status ta_router_outbound(ta_host *h, uint32_t si, const ta_effect *f);
void ta_router_time(ta_host *h);              /* expire queued envelopes */
ta_time ta_router_deadline(const ta_host *h); /* the next expiry (UINT64_MAX: none) */
void ta_router_free(ta_host *h);
size_t ta_router_snapshot(const ta_host *h, unsigned char *out, size_t capacity); /* dedup window */
ta_status ta_router_restore(ta_host *h, const unsigned char *p, size_t length);

#endif /* TINYACTORS_HOST_HOST_INTERNAL_H */
