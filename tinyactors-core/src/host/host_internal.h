/* host_internal.h — ta_host internals shared by the host layer's files.
 *
 * Every block the host owns comes from its config's ta_allocator
 * (host_alloc) and goes back with its size (host_free): each array keeps
 * its capacity, each string its length. Whatever must not fail half done
 * reserves first (host_effects_reserve, moved_reserve, host_grow): after the
 * reservation the step cannot run out of memory. */
#ifndef TINYACTORS_HOST_HOST_INTERNAL_H
#define HOST_ORIGIN_SLOT TA_ADDRESS_SESSION_MAX
#define HOST_NAME_MAX    256u /* the longest actor name an address may name (decoded) */
#define TINYACTORS_HOST_HOST_INTERNAL_H

#include <tinyactors/address.h>
#include <tinyactors/arena.h>
#include <tinyactors/host.h>

#include "base/output.h"
#include "host/session_map.h"

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
    const ta_io_processor_entry *io_processors;
    uint32_t migrate_quantum;   /* default 256 */
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
    ta_io_processor_entry *io_processors; /* HOST_CONFIG_IO_PROCESSORS */
} host_config_input;
ta_status host_config_read(const host_config_input *input, host_config *config);

typedef struct host_segment {
    int live;
    int busy;
    /* the host's copy of the segment's image: one block from the host's
     * allocator, the ta_image then its bytes (image_copy), freed after the
     * segment's ta_system_deinit (retire, ta_host_free); NULL once freed.
     * A block of its own, since `segments` grows by copying */
    ta_image *image;
    size_t image_size;
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

typedef struct host_migration {
    uint32_t id;
    int active;
    char *name; /* name_length bytes, NULL when empty */
    uint32_t name_length;
    uint32_t flags;
    uint64_t session;
    ta_time deadline;
    int walking;
    uint32_t walk_segment, cursor;
    session_map members; /* the sessions it waits for: MIGRATION_ASLEEP or MIGRATION_RETRY (host.c) */
    uint64_t *retry;     /* the MIGRATION_RETRY members, tried round robin from retry_next */
    uint32_t retry_count, retry_capacity, retry_next;
    uint32_t migrated, failed;
    uint64_t *left; /* after it ended: stragglers (until the next run) */
    uint32_t left_count, left_capacity;
} host_migration;

/* target, event, sendid, aux, then the wire bytes */
#define EFFECT_STRINGS 5
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
    char base[TA_ADDRESS_SYSTEM_MAX + 16]; /* tinyactors://<system_id>/ */
    uint32_t base_length;
    ta_datamodel_binding datamodels[8];
    ta_io_processor_entry io_processors[HOST_CONFIG_IO_PROCESSORS]; /* the config's */
    uint32_t io_processor_count;
    host_segment *segments;
    uint32_t segment_count, segment_capacity, round_robin;
    chart_version *versions;
    uint32_t version_count, version_capacity;
    /* session -> segment, for sessions living outside their own range */
    session_map moved;
    ta_time now;
    /* effects of the current batch; effects and effect_strings have effect_capacity entries */
    ta_host_effect *effects;
    const ta_host_effect *published_effects; /* frozen until ack, including across growth */
    effect_strings *effect_strings;
    uint32_t effect_count, effect_capacity, effects_ready;
    /* ta_host_effect_text's TRACEPARENT: TA_TRACEPARENT_LENGTH bytes for each
     * effect of the published batch, at its index, valid until the ack. Grown
     * only by the first ask of a batch, before any view of it is handed out;
     * kept across batches, freed with the host. */
    char *traceparents;
    uint32_t traceparent_capacity; /* effects */
    /* ta_host_effect_text's ORIGIN, the same way: HOST_ORIGIN_SLOT bytes per
     * effect: the sender's address, written again (the same bytes) on each ask */
    char *origins;
    uint32_t origin_capacity; /* effects */
    char *pool;
    size_t pool_length, pool_capacity;
    /* A published batch (ta_host_effects, effects_ready > 0) is borrowed until
     * the next ack or free. Growing the effect array or the pool in the
     * meantime copies into new blocks and holds the old ones, unchanged,
     * until then (effects_release frees them). */
    void *held[HELD_BLOCKS_MAX];
    size_t held_size[HELD_BLOCKS_MAX];
    uint32_t held_count;
    /* migrations */
    host_migration *migrations;
    uint32_t migration_count, migration_capacity, migration_sequence;
    /* sweep cursor */
    uint32_t sweep_segment, sweep_cursor;
    /* What the host writes for its caller: an envelope, traces, logs,
     * metrics, a hibernation or export blob (and, as scratch, a request's
     * reply before it is copied into the pool). Each output is a block of its
     * own in `outputs`, so a view handed out (host_output_view) never moves
     * and lasts until the ack. `outputs_lent` is set by a view handed out: a
     * batch is open (ta_host_run refuses, the ack ends it); with none lent,
     * the next output starts the arena over. Released chunks are kept until
     * ta_host_free, so after warm-up no output reaches the allocator. */
    ta_output output;
    ta_growable_arena outputs;
    ta_allocator outputs_allocator; /* the arena's view, which `output` grows through */
    uint32_t outputs_lent;
    /* after ta_host_restore: the external invocations still to announce again (TA_EFFECT_INVOKE
       flagged TA_EFFECT_FLAG_RESTORED) at the next run, from segment announce_segment's request
       announce_from on; the datamodels' state is back by then */
    int announcing;
    uint32_t announce_segment, announce_from;
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
    uint32_t draining;         /* segment whose effects are being drained (TA_NIL: none) */
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
/* ta_host_locate, for callers that want either half (NULL: not wanted). */
ta_status host_locate(const ta_host *h, uint64_t session, uint32_t *segment, uint32_t *actor);
/* TA_ERROR_NO_MEMORY if it could be neither delivered nor kept; TA_OK otherwise
 * (also when the actor is gone: nobody to tell). */
ta_status host_communication_error(ta_host *h, uint32_t si, uint32_t actor, ta_string_view sendid);
uint32_t host_moved_get(const ta_host *h, uint64_t key);

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
/* Segment `segment`'s stats (its pools' capacities, what they hold, their
 * high-water marks): 0 if it is not live. */
int host_segment_pools(const ta_host *host, uint32_t segment, ta_system_stats *out);
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

int host_is_local(const ta_host *h, ta_string_view system);
uint64_t host_address_session(const ta_host *h, const ta_address *address);

#endif /* TINYACTORS_HOST_HOST_INTERNAL_H */
