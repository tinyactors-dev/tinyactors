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

#include "base/output.h"

#define SERIALS_SHIFT 32
/* blocks the published effects still point into: each doubling of the
 * effect array or the pool while a batch is borrowed holds one */
#define HELD_BLOCKS_MAX 64
#define EFFECTS_PER_RUN 4096 /* effects per run before TA_RUN_EFFECTS_FULL */

/* The host's config, read from ta_host_create's CBOR (docs/host-config.md)
 * and its environment. Strings point into memory the host owns. */
typedef struct host_telemetry {
    ta_otlp_config otlp;               /* size 0: no encoder; else defaults filled in */
    ta_time flush_interval;            /* host clock; 0: only when the staging is full */
    const ta_otlp_attribute *resource; /* the host's copy (host_new) */
    uint32_t resource_count;
    uint32_t pad;
} host_telemetry;

typedef struct host_config {
    uint32_t datamodel_count;
    uint32_t io_processor_count;
    ta_string_view system_id;
    ta_system_config system; /* capacities of each new segment, defaults filled in
                                (the effective config, host.config); io_processors
                                and first_serial are set by the host layer */
    const ta_datamodel_binding *datamodels;
    const ta_io_processor_descriptor *io_processors;
    uint32_t migrate_quantum;   /* default 256 */
    uint32_t max_subscriptions; /* default 64 */
    ta_allocator allocator;
    host_telemetry telemetry;
} host_config;

/* The config of ta_host_create: `cbor` read into `config` (strings point
 * into `cbor`; resource attributes into `resource`, HOST_CONFIG_RESOURCE of
 * them). TA_ERROR_INVALID with the error reply written into `error` (when
 * set). In request.c, which reads requests the same way. */
#define HOST_CONFIG_RESOURCE 32u
#define HOST_CONFIG_IO_PROCESSORS 16u
typedef struct host_config_input {
    ta_bytes cbor;
    ta_buffer *error;
    ta_otlp_attribute *resource;              /* HOST_CONFIG_RESOURCE */
    ta_io_processor_descriptor *io_processors; /* HOST_CONFIG_IO_PROCESSORS */
} host_config_input;
ta_status host_config_read(const host_config_input *input, host_config *config);

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

/* target, event, sendid, aux, msgid, origintype, deduplication_key */
#define EFFECT_STRINGS 8
typedef struct effect_strings {
    size_t offset[EFFECT_STRINGS]; /* in the string pool (SIZE_MAX: empty) */
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
    host_config config;
    /* inside ta_system_run, or inside a datamodel the host calls outside a
     * run (host_guard), where a datamodel may call back: only the inputs
     * (spawn, send, send_many, terminate) and lookups are taken, nothing
     * grows, every other call is TA_ERROR_STATE (host.h, "During a run").
     * The host's own output may grow inside a guard it opened: the write
     * that calls the datamodel (hibernate, export, migration) took the
     * output before the guard, while this was 0 (host_output) */
    uint32_t running;
    uint32_t closing; /* inside ta_host_free: the inputs are refused as well */
    ta_trace_policy trace_policy; /* every segment's, and new ones' (config.system points here) */
    void *trace_rules;            /* its rules and their strings, the host's copy */
    size_t trace_rules_bytes;
    /* host_trace_set's options and the watches, from which the policy is made */
    host_rules trace_options; /* rule.detail: kinds */
    uint32_t trace_flags, trace_kinds, trace_parts, trace_whole;
    host_watch *watches;
    uint32_t watch_count, watch_capacity, watch_sequence;
    char *io_processor_text; /* the strings of io_processors[1..] */
    size_t io_processor_text_length;
    void *resource_block; /* config.telemetry.resource and its strings (host_new) */
    size_t resource_bytes;
    uint64_t *subscriptions; /* actor.subscribe: config.max_subscriptions sessions, allocated
                                with the first subscription (never during a run) */
    uint32_t subscription_count;
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
    char effect_traceparent[TA_TRACEPARENT_LENGTH]; /* ta_host_effect_text's TRACEPARENT */
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
    /* What the host writes for its caller (docs/adr/0010 in the repository
     * root, decision 2): a request's reply, an envelope, traces, logs,
     * metrics, a hibernation or export blob, each a view of this block until
     * the next call into the host. It grows through the host's allocator
     * (host_output) and keeps its largest size until ta_host_free. */
    ta_output output;
    /* scratch for blobs: an export moving between segments (it grows like
     * the output), and its translation */
    ta_output blob;
    unsigned char *blob2;
    size_t blob2_capacity;
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
    /* the encoder's operations (telemetry.c: the OTLP one, or the null one
     * when no encoder memory was given), the OTLP encoder and its block */
    const struct host_encoder *encoder;
    ta_otlp *otlp;
    void *otlp_memory;
    size_t otlp_bytes;
    ta_otlp_config otlp_config; /* its capacities, defaults filled in */
    uint32_t traces_pending, logs_pending; /* an effect is out, not taken yet */
    uint32_t staging_full; /* a read stopped at a full staging */
    uint32_t staged;       /* something is staged, since staged_since */
    uint32_t ending;       /* trace.end left spans open (staging full): each take ends more */
    ta_time staged_since;
    uint64_t wall_ns, wall_start_ns; /* the latest wall time, and the first (metrics) */
};

/* The guard (docs/adr/0010 in the repository root, decision 3): around
 * every call the host makes into a datamodel outside a run (directly, or
 * through a system call that reaches one), so that a datamodel's code that
 * calls back into the host follows the rules of a run and cannot change what
 * the host is using. host_guard returns what host_unguard restores (guards
 * nest: a guarded input reaches a datamodel again). */
static inline uint32_t host_guard(ta_host *h) {
    uint32_t was = h->running;
    h->running = 1;
    return was;
}
static inline void host_unguard(ta_host *h, uint32_t was) { h->running = was; }

/* host.c */
void *host_alloc(ta_host *h, size_t size);
/* A payload's retain and release, guarded (nothing for payload 0, or a
 * datamodel without them). */
void host_retain(ta_host *h, const ta_datamodel_binding *binding, ta_payload payload);
void host_release(ta_host *h, const ta_datamodel_binding *binding, ta_payload payload);
/* The host's output, emptied for the next one (its block kept). It grows
 * through the host's allocator as it is written, from host code or from the
 * system's one-pass writers; take it outside a run and before host_guard
 * (asserted), so it never grows during a run. */
ta_output *host_output(ta_host *h);
/* What was written there, as the caller's view; TA_ERROR_NO_MEMORY (and an
 * empty view) when the allocator refused room for it. */
ta_status host_output_view(ta_host *h, ta_bytes *out);
void host_free(ta_host *h, void *pointer, size_t size);
/* Room for `need` elements of `element` bytes in *array (capacity in
 * *capacity, new ones zeroed); 0 if the allocator refused (*array unchanged). */
int host_grow(ta_host *h, void **array, uint32_t *capacity, uint32_t need, size_t element);
/* Room for `count` more effects and `bytes` more pool bytes; 0 if refused. */
int host_effects_reserve(ta_host *h, uint32_t count, size_t bytes);
size_t host_pool_put(ta_host *h, ta_string_view s);
ta_host_effect *host_effect_new(ta_host *h, uint32_t kind, uint32_t segment);
/* The binding that runs `chart` of segment `segment` (its vtable's name is
 * the chart's datamodel attribute); NULL if the host has none by that name. */
const ta_datamodel_binding *host_chart_datamodel(const ta_host *h, uint32_t segment,
                                                 uint32_t chart);
void host_effect_strings_set(ta_host *h, ta_string_view t, ta_string_view event, ta_string_view sid,
                             ta_string_view aux);
ta_status host_send_to(ta_host *h, uint32_t si, uint32_t actor, const ta_event_in *event);
/* actor.subscribe / actor.unsubscribe (docs/requests.md): TA_ERROR_STALE for
 * a session that is not here (subscribe) or not subscribed (unsubscribe);
 * TA_ERROR_STATE when config.max_subscriptions are taken; TA_ERROR_NO_MEMORY
 * when the allocator refused the table. */
ta_status host_subscribe(ta_host *h, uint64_t session);
ta_status host_unsubscribe(ta_host *h, uint64_t session);
/* ta_host_locate, for callers that want either half (NULL: not wanted). */
ta_status host_locate(const ta_host *h, uint64_t session, uint32_t *segment, uint32_t *actor);
/* TA_ERROR_NO_MEMORY if it could be neither delivered nor kept; TA_OK otherwise
 * (also when the actor is gone: nobody to tell). */
ta_status host_communication_error(ta_host *h, uint32_t si, uint32_t actor, ta_string_view sendid);
uint32_t host_moved_get(const ta_host *h, uint64_t key);

/* router.c */
/* an SCXML send of segment si to another system (target not ours);
 * TA_ERROR_NO_MEMORY: nothing done, the effect stays for the next drain */
/* The rarely called host operations, behind ta_host_post (request.c).
 * Trace options: what every segment traces, and new segments start with
 * (docs/adr/0007). Rules and watches name the kinds they give (rule.detail:
 * TA_TRACE_* masks with TA_TRACE_VALUES, TA_NIL to keep the trace's own);
 * the host gives each different detail a class of the policy it sets in
 * every segment: class 0 the lifecycle kinds of `kinds`, class 1 `kinds`,
 * then one per detail the watches and rules ask for. A watch's rules come
 * before the options' rules, newer watches first. TA_ERROR_STATE when the
 * host's systems do not trace; TA_ERROR_NO_MEMORY for more than
 * HOST_TRACE_DETAILS different details, more rules than a system holds or
 * no memory; TA_ERROR_INVALID for a malformed rule. Nothing changes then. */
enum { HOST_TRACE_NO_RECORDS = 1u << 0 }; /* no records (traceparents still flow) */
#define HOST_TRACE_DETAILS (TA_TRACE_DETAILS - 2u)
typedef struct host_trace_options {
    uint32_t size;
    uint32_t flags;                      /* HOST_TRACE_* */
    uint32_t kinds;                      /* what a sampled trace writes */
    uint32_t sample_parts, sample_whole; /* as ta_trace_policy */
    uint32_t rule_count;
    const ta_trace_rule *rules; /* copied */
} host_trace_options;
ta_status host_trace_set(ta_host *h, const host_trace_options *options);
/* A watch: rules that hold until the host's time reaches `until`
 * (UINT64_MAX: until host_trace_unwatch); its end is a deadline of
 * ta_host_run. Errors as host_trace_set. */
typedef struct host_watch_rules {
    const ta_trace_rule *rules; /* copied */
    uint32_t rule_count;
    uint32_t pad;
    ta_time until;
} host_watch_rules;
ta_status host_trace_watch(ta_host *h, const host_watch_rules *watch, uint32_t *id);
ta_status host_trace_unwatch(ta_host *h, uint32_t id); /* TA_ERROR_STALE: it has ended */
void host_stats_get(const ta_host *h, ta_host_stats *out);
/* Per chart name, over every segment and retired version (ta_chart_stats);
 * fills up to `capacity` and returns how many names there are. */
uint32_t host_chart_stats(const ta_host *h, ta_chart_stats *out, uint32_t capacity);
/* Sessions host-wide from *cursor (0 to start); returns how many (0: done). */
uint32_t host_actors(const ta_host *h, uint64_t *cursor, ta_uint64_array out);
/* A session's state value (docs/requests.md, "state"). */
enum { HOST_STATE_VALUE_STATES = 1024 };
struct ta_output;
ta_status host_state_value(const ta_host *h, uint64_t session, struct ta_output *out);

/* telemetry.c: what the run loop asks of the encoder, chosen once at
 * ta_host_create (the OTLP encoder, or the null one: all of it does nothing) */
typedef struct host_encoder {
    void (*mark)(ta_host *h, uint64_t wall_ns); /* ta_host_advance_time */
    void (*drain)(ta_host *h);                  /* journals into the encoder */
    void (*effects)(ta_host *h);                /* a ready request becomes an effect */
    void (*note)(ta_host *h);                   /* after a run: records left count as staged */
    ta_time (*deadline)(const ta_host *h);      /* the flush interval's end (UINT64_MAX: none) */
    ta_status (*take)(ta_host *h, uint32_t kind, ta_bytes *out); /* ta_host_traces, _logs */
    ta_status (*metrics)(ta_host *h, ta_bytes *out);             /* ta_host_metrics */
    ta_status (*end)(ta_host *h); /* trace.end: end every open span, staged for a take */
    void (*definitions)(ta_host *h, uint32_t kind, uint32_t segment); /* definitions.c */
} host_encoder;
ta_status host_telemetry_init(ta_host *h);
void host_telemetry_free(ta_host *h);

/* definitions.c: the definition logs. LINKED: the charts of a segment just
 * added, and each version they retire; FREED: the charts of a segment going. */
enum { HOST_DEFINITION_LINKED = 0, HOST_DEFINITION_RETIRED = 1, HOST_DEFINITION_FREED = 2 };
void host_definitions_otlp(ta_host *h, uint32_t kind, uint32_t segment);

ta_status ta_router_outbound(ta_host *h, uint32_t si, const ta_effect *f);
void ta_router_time(ta_host *h);              /* expire queued envelopes */
ta_time ta_router_deadline(const ta_host *h); /* the next expiry (UINT64_MAX: none) */
void ta_router_free(ta_host *h);
size_t ta_router_snapshot(const ta_host *h, unsigned char *out, size_t capacity); /* dedup window */
ta_status ta_router_restore(ta_host *h, const unsigned char *p, size_t length);

#endif /* TINYACTORS_HOST_HOST_INTERNAL_H */
