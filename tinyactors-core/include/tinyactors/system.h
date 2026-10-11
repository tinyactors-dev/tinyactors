/* tinyactors/system.h — the public embedding ABI: a cooperative SCXML actor-system VM.
 *
 * Commands (host -> system) are synchronous calls. Results come back as a
 * tagged ta_run outcome plus a batch of ta_effect records. No callbacks other
 * than the datamodel vtable. See docs/architecture.md §11.
 *
 * View lifetime rule: ta_string_view/payload views pointing into the system are valid
 * until the next mutating call on that system (send*, run, resume,
 * advance_time, effects_ack, spawn, terminate). Host strings passed in are
 * copied/interned before return; payload handles passed in are moved into the
 * system when the call succeeds. When it fails (any status) the caller still
 * owns them: on TA_ERROR_NO_MEMORY grow the system and retry with the same handle,
 * otherwise release it.
 */
#ifndef TINYACTORS_SYSTEM_H
#define TINYACTORS_SYSTEM_H

#include <tinyactors/datamodel.h>
#include <tinyactors/image.h>
#include <tinyactors/stats.h>
#include <tinyactors/trace.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_system ta_system;

/* Host-provided (external) I/O processor types, advertised in _ioprocessors. */
typedef struct ta_io_processor_descriptor {
    ta_string_view type;     /* e.g. "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor" */
    ta_string_view location; /* base location; system appends nothing */
    uint32_t flags;          /* TA_IO_PROCESSOR_* */
    uint32_t pad;
} ta_io_processor_descriptor;
/* TA_IO_PROCESSOR_SYNC: ta_system_run returns TA_RUN_EFFECTS right after a send to
 * this processor, before the sender continues, so the host can deliver the
 * outcome (e.g. error.communication) as part of the sender's macrostep. */
/* TA_IO_PROCESSOR_SCXML_REMOTE: carries SCXML-to-SCXML events to sessions of
 * *other* systems (another ta_system in this process, another process, ...).
 * `location` is this system's base URI, e.g. "tinyactors://a/". Sessions then
 * advertise "<location>#_scxml_<id>" as their SCXML processor location;
 * SCXML sends to "<location>#_scxml_<id>" are delivered locally, and other
 * non-"#" SCXML targets become TA_EFFECT_SEND effects for this processor (target =
 * the URI). The host delivers them to the other system with origintype set
 * to the SCXML processor and origin = "<sender base>#_scxml_<sender id>". */
/* TA_IO_PROCESSOR_SCXML_SHARED (with TA_IO_PROCESSOR_SCXML_REMOTE): the base URI is
 * shared by several systems (e.g. the versions of a hot-reloaded system,
 * see ta_system_adopt), each creating sessions from its own range
 * (ta_system_config.first_serial). A send to "<location>#_scxml_<id>" whose
 * session is not here is then not error.communication but a TA_EFFECT_SEND for
 * this processor (target = the full URI): the host knows which system has
 * that session now, or reports failure with TA_EVENT_IN_ERROR_COMMUNICATION. */
enum {
    TA_IO_PROCESSOR_SYNC = 1u << 0,
    TA_IO_PROCESSOR_SCXML_REMOTE = 1u << 1,
    TA_IO_PROCESSOR_SCXML_SHARED = 1u << 2
};

/* Hard per-system limits (handles are 24-bit indices; actor handles lend one
 * bit to external invoke tokens). More actors = more systems (cross-system
 * SCXML addressing, TA_IO_PROCESSOR_SCXML_REMOTE). */
#define TA_SYSTEM_MAX_ACTORS 8388606u  /* 2^23 - 2 */
#define TA_SYSTEM_MAX_POOL   16777214u /* events, timers, workspaces: 2^24 - 2 */

typedef struct ta_trace_policy ta_trace_policy;

typedef struct ta_system_config {
    uint32_t size;
    uint32_t max_actors;     /* clamped to TA_SYSTEM_MAX_ACTORS; other pools to 2^24 - 2 */
    uint32_t max_events;     /* shared event pool (all mailboxes/internal queues/timers) */
    uint32_t max_timers;     /* pending delayed sends */
    uint32_t workspaces;     /* pooled microstep workspaces (default 4) */
    uint32_t max_requests;   /* outstanding host requests */
    uint32_t outbox;         /* effect ring capacity */
    uint32_t quantum;        /* per-actor fuel per turn */
    uint32_t instance_bytes; /* slab for chart-dependent instances; 0 = derive from max_actors */
    uint32_t string_bytes;   /* slab for dynamic strings */
    uint32_t io_processor_count;
    uint32_t pad;
    uint64_t ticks_per_second;
    const ta_io_processor_descriptor *io_processors;
    uint64_t first_serial; /* session ids start after this (default 0: 1, 2, ...) */
    /* Tracing (docs/adr/0006 in the repository root): on by default, at full
     * detail. TA_TRACING_DISABLED carves nothing for it. */
    uint32_t tracing;        /* TA_TRACING_ENABLED (0) / TA_TRACING_DISABLED */
    uint32_t trace_bytes;    /* the trace journal; 0 = 512 per event, at least 64 KiB */
    uint32_t trace_contexts; /* the pool of trace contexts; 0 = max_events */
    uint32_t trace_rules;    /* the policy's rules a system holds; 0 = 16 (at most 255) */
    /* what is sampled and which kinds each detail class writes; copied at
     * init (NULL: ta_trace_policy_default) */
    const ta_trace_policy *trace_policy;
    uint64_t trace_seed; /* the trace ID generator's seed: the same seed, the same IDs */
} ta_system_config;
enum { TA_TRACING_ENABLED = 0, TA_TRACING_DISABLED = 1 };
/* The default and the smallest journal (TA_TRACE_*_BYTES), and its default
 * share per event of max_events. */
#define TA_TRACE_BYTES_MIN       65536u
#define TA_TRACE_BYTES_PER_EVENT 512u

/* ---- lifetime: one caller-owned memory block ---------------------------- */
/* A system to create: the block it lives in, the image it runs, its
 * capacities and the datamodels its charts use (borrowed for as long as
 * the system lives). */
typedef struct ta_system_setup {
    ta_memory memory; /* ta_system_layout bytes (ta_runtime_system_new: unused) */
    const ta_image *image;
    const ta_system_config *config; /* ta_system_restore: host pointers only, may be NULL */
    const ta_datamodel_binding *datamodels;
    uint32_t datamodel_count;
    uint32_t pad;
} ta_system_setup;
TA_API ta_status ta_system_layout(const ta_image *image, const ta_system_config *config,
                                  size_t *bytes);
TA_API ta_status ta_system_init(const ta_system_setup *setup, ta_system **out);
/* Move into a bigger block with bigger capacities (memcpy + fix-up); on
 * TA_OK *system is the system in its new block. */
TA_API ta_status ta_system_relocate(ta_system **system, ta_memory memory,
                                    const ta_system_config *config);
TA_API void ta_system_deinit(ta_system *system);

/* ---- actors ------------------------------------------------------------- */
TA_API ta_status ta_system_boot(ta_system *system); /* spawn image-defined actors */
/* A root actor to spawn. Its bootstrap runs under `traceparent` (W3C text)
 * when it parses, else in a new trace. */
typedef struct ta_spawn {
    uint32_t segment; /* ta_host_spawn: the segment (ta_host_chart); ta_system_spawn: unused */
    uint32_t chart;
    ta_string_view name;
    ta_payload params; /* moved into the system on success */
    ta_string_view traceparent;
} ta_spawn;
TA_API ta_status ta_system_spawn(ta_system *system, const ta_spawn *spawn, uint32_t *actor);
TA_API ta_status ta_system_terminate(ta_system *system, uint32_t actor);
TA_API uint32_t ta_system_actor_by_name(const ta_system *system, ta_string_view name);
TA_API uint32_t ta_system_actor_by_session(const ta_system *system,
                                           uint64_t sessionid); /* 0 if none */
/* Enumerate actors, incrementally: writes up to capacity handles of live actors
 * from *cursor on (start with 0) and advances it; returns how many (0 = no
 * more). A call scans slots until it found capacity actors (one byte per free
 * slot); a whole walk is O(slots in use so far). Take chunks (e.g. 256) and
 * resume later to keep the host's loop responsive. */
TA_API uint32_t ta_system_actors(const ta_system *system, uint32_t *cursor, ta_uint32_array out);

/* ---- inbound ------------------------------------------------------------ */
enum {
    TA_EVENT_IN_FROM_INVOKE = 1u << 0, /* invoke_token identifies an external invoke */
    TA_EVENT_IN_INVOKE_DONE = 1u << 1, /* generate done.invoke.<id> for invoke_token */
    TA_EVENT_IN_ERROR_COMMUNICATION = 1u
        << 2 /* failure of an earlier TA_EFFECT_SEND: error.communication is
   placed in the target's internal queue (sendid copied) */
};
typedef struct ta_event_in {
    uint32_t size;
    uint32_t target; /* actor handle */
    uint32_t flags;
    uint32_t token;   /* invoke token / send token, depending on flags */
    uint64_t session; /* ta_host_send, _send_many: the target session (target is ignored) */
    ta_string_view name;
    ta_string_view sendid;
    ta_string_view origin;
    ta_string_view origintype;
    ta_payload data;    /* moved into the system */
    ta_string_view raw; /* optional raw message (_event.raw) */
    /* W3C traceparent: the macrostep this event starts runs under it (keeping
     * its sampled flag); without one, it starts a new trace */
    ta_string_view traceparent;
    /* with a traceparent from another system of this process: the sender's
     * context flags above the W3C byte (its detail class: ta_effect.trace.flags) */
    uint32_t trace_flags;
    uint32_t pad;
} ta_event_in;

TA_API ta_status ta_system_send(ta_system *system, const ta_event_in *event);
/* Sends in order and stops at the first failure: returns the length of the
 * accepted prefix. events[returned] failed (its payload stays the caller's: on
 * TA_ERROR_NO_MEMORY grow and continue from it, else release it and skip it). */
TA_API uint32_t ta_system_send_many(ta_system *system, const ta_event_in *events, uint32_t n);
/* The answer to a TA_EFFECT_REQUEST. */
typedef struct ta_resume {
    uint32_t segment; /* ta_host_resume: the segment of the effect; ta_system_resume: unused */
    uint32_t request; /* the effect's token */
    ta_status status;
    uint32_t pad;
    ta_payload result; /* moved into the system on success */
} ta_resume;
TA_API ta_status ta_system_resume(ta_system *system, const ta_resume *resume);

/* ---- execution ---------------------------------------------------------- */
enum {
    TA_RUN_IDLE = 0,         /* nothing runnable; wait for input or next_deadline */
    TA_RUN_YIELDED = 1,      /* budget exhausted; actors still runnable */
    TA_RUN_EFFECTS_FULL = 2, /* outbox full; drain effects and run again */
    TA_RUN_EXHAUSTED = 3,    /* a pool is exhausted (detail = TA_POOL_*); relocate/grow */
    TA_RUN_FAULT = 4,
    TA_RUN_EFFECTS = 5 /* a TA_IO_PROCESSOR_SYNC send needs handling; drain effects, run again */
};
/* ta_run.detail for TA_RUN_EXHAUSTED: which capacity to grow */
enum {
    TA_POOL_NONE = 0,
    TA_POOL_EVENTS = 1,
    TA_POOL_SLAB = 2, /* instances + dynamic strings */
    TA_POOL_TIMERS = 3,
    TA_POOL_ACTORS = 4,
    TA_POOL_REQUESTS = 5
};
typedef struct ta_run {
    uint32_t status;
    uint32_t effect_count;
    uint32_t used;
    uint32_t detail;
    ta_time next_deadline; /* UINT64_MAX if none */
} ta_run;

TA_API ta_run ta_system_run(ta_system *system, uint32_t budget);
TA_API void ta_system_advance_time(ta_system *system, ta_time now);
TA_API ta_time ta_system_next_deadline(const ta_system *system);

/* ---- outbound effects (batched, borrowed) ------------------------------- */
enum {
    TA_EFFECT_SEND = 1,     /* external send (fire-and-forget) */
    TA_EFFECT_REQUEST = 2,  /* needs ta_system_resume (ta_resume.request = token) */
    TA_EFFECT_INVOKE = 3,   /* start external invoke (token = invoke token) */
    TA_EFFECT_UNINVOKE = 4, /* cancel external invoke */
    TA_EFFECT_LOG = 5,
    TA_EFFECT_DONE = 6, /* top-level actor reached a final state; payload = donedata */
    TA_EFFECT_ERROR = 7,
    TA_EFFECT_WAKE = 8 /* a hibernated actor has work: load its blob, call ta_actor_wake() */
};
enum { TA_EFFECT_FLAG_NEEDS_RESPONSE = 1u << 0 };
/* Field use per kind (unused fields are empty / 0):
 *   SEND      target, event (name), sendid, payload, io_processor; token = external
 *             invoke token when sending to an external invoke ("#_<id>")
 *   REQUEST   aux = unresolved invoke src; answer with
 *             ta_system_resume with {.request = token, .status = TA_OK,
 *             .result = chart_index + 1} or an error
 *   INVOKE    event = type, target = src, sendid = invokeid, payload = params;
 *             report back with ta_event_in{.token, TA_EVENT_IN_FROM_INVOKE /
 *             TA_EVENT_IN_INVOKE_DONE}
 *   UNINVOKE  token
 *   LOG       aux = label, event = text
 *   DONE      target = actor name, payload = donedata, aux = id of the top-level
 *             <final> that ended the session (root actors only)
 *   ERROR     flags = fault code (the actor was reclaimed)
 *   WAKE      actor = the hibernated actor (one effect per hibernation, however
 *             many events arrive meanwhile; they wait in its mailbox) */
typedef struct ta_effect {
    uint32_t size;
    uint32_t kind;
    uint32_t actor;
    uint32_t token;
    uint32_t io_processor;
    uint32_t flags;
    ta_string_view target;
    ta_string_view event;
    ta_string_view sendid;
    ta_string_view auxiliary; /* log label, request detail, invoke src, ... */
    ta_payload payload;       /* borrowed: call the datamodel's retain to keep it */
    ta_time delay_hint;       /* external delayed sends handled by host, if allowed */
    uint64_t session;         /* the actor's session id (it may be gone already: DONE, ERROR) */
    uint32_t datamodel; /* index of the datamodel binding that owns `payload`, TA_NIL if none */
    uint32_t pad;
    /* SEND, INVOKE, LOG, DONE: the step's trace context (its span is
     * `trace.parent`), so an I/O processor writes a traceparent without
     * calling core (ta_traceparent_format); no trace if untraced */
    ta_trace_context trace;
} ta_effect;

TA_API uint32_t ta_system_effects(ta_system *system, const ta_effect **out);
TA_API void ta_system_effects_ack(ta_system *system, uint32_t n);

/* ---- inspection / snapshots --------------------------------------------- */
enum {
    TA_ACTOR_FREE = 0,
    TA_ACTOR_IDLE = 1,
    TA_ACTOR_RUNNABLE = 2,
    TA_ACTOR_WAIT_WORKSPACE = 3,
    TA_ACTOR_WAIT_EXTERNAL = 4,
    TA_ACTOR_DONE = 5,
    TA_ACTOR_HIBERNATED = 6 /* unloaded by the host; see ta_actor_hibernate */
};
typedef struct ta_actor_info {
    uint32_t size;
    uint32_t state;
    uint32_t chart;
    uint32_t parent;
    uint32_t in_macrostep;
    uint32_t mailbox_length;
    uint32_t pending_request;
    uint32_t current_event_name; /* name id or TA_NIL */
    uint64_t serial;             /* _sessionid */
    uint64_t datamodel_instance; /* its datamodel instance handle (0 while hibernated), for
                             datamodel-side inspection such as ta_datamodel_ecmascript_payload_eval */
    uint32_t invocations; /* running invocations (SCXML sessions it invoked, external ones) */
    uint32_t pad;
} ta_actor_info;

/* Set out->size first (sizeof(ta_actor_info)); exactly that much is written. */
TA_API ta_status ta_actor_info_get(const ta_system *system, uint32_t actor, ta_actor_info *out);
/* The actor's active states (up to states.capacity); returns how many it has. */
TA_API uint32_t ta_actor_config(const ta_system *system, uint32_t actor, ta_uint32_array states);
/* Pool occupancy and monotonic counters (tinyactors/stats.h), O(1). Set
 * `size` first. */
TA_API void ta_system_stats_get(const ta_system *system, ta_system_stats *out);
/* What the actors of `chart` did (tinyactors/stats.h), O(1). Set `size`
 * first. TA_ERROR_INVALID for a chart the image does not have. */
TA_API ta_status ta_system_chart_stats_get(const ta_system *system, uint32_t chart,
                                           ta_chart_stats *out);

/* ---- tracing (docs/adr/0006 in the repository root) ----------------------
 * The system writes trace records (tinyactors/trace.h) into a journal in its
 * block; the host drains them as it drains effects. ta_system_trace returns
 * the bytes of the oldest records that lie in one piece (0 when there are
 * none) and fills `view`; the view is valid until the next mutating call.
 * ta_system_trace_ack releases `bytes` from its start (whole records), and
 * what they hold; call ta_system_trace again for the rest.
 *
 * Records are stamped with the fuel clock (ta_system_stats.fuel_clock), a
 * counter of the fuel the system used. A host notes its wall clock and the
 * fuel clock before and after each run, and an encoder (tinyactors/otlp.h)
 * maps fuel to wall time within the run that holds it.
 * ta_system_trace_clock moves the clock forward to `at_least` (never back):
 * a host that runs several systems keeps them on one clock that way. */
TA_API uint32_t ta_system_trace(ta_system *system, ta_trace_view *view);
TA_API void ta_system_trace_ack(ta_system *system, uint32_t bytes);
TA_API void ta_system_trace_clock(ta_system *system, uint64_t at_least);
/* The tracing policy (docs/adr/0007): the share of the traces the system
 * starts that are sampled, parts / whole (TraceIDRatioBased on the trace ID;
 * whole 0 is 1 / 1; a traceparent keeps its caller's decision), and the
 * kinds each detail class writes (TA_TRACE_DETAIL in trace.h). A sampled
 * trace the system starts or takes in is TA_TRACE_DETAIL_SAMPLED, an
 * unsampled one TA_TRACE_DETAIL_UNSAMPLED. */
/* Rules decide again where something new starts (docs/adr/0007): when a
 * trace starts (a host input, a spawn, a timer of an untraced send) and
 * when an actor takes an external event. At each point the first rule that
 * matches decides; what it decides travels with the context to everything
 * the step causes. Empty views and zeros match anything. */
enum { TA_TRACE_AT_START = 1u, TA_TRACE_AT_TAKE = 2u };
enum { TA_TRACE_KEEP = 0, TA_TRACE_SAMPLE = 1, TA_TRACE_DROP = 2, TA_TRACE_RATIO = 3 };
enum {
    TA_TRACE_FROM_HOST = 1u << 0,     /* an input from the host */
    TA_TRACE_FROM_ACTOR = 1u << 1,    /* an event an actor sent */
    TA_TRACE_FROM_TIMER = 1u << 2,    /* a delayed send of an untraced step */
    TA_TRACE_FROM_SPAWN = 1u << 3,    /* a root actor's bootstrap */
    TA_TRACE_FROM_SAMPLED = 1u << 4,  /* the trace so far is sampled */
    TA_TRACE_FROM_UNSAMPLED = 1u << 5 /* ... is not (or there is none) */
};
#define TA_TRACE_NAME_RULES 16u /* rules with an actor name a policy may hold */
typedef struct ta_trace_rule {
    uint32_t size;
    uint32_t at;          /* TA_TRACE_AT_*: where it is asked (0: both) */
    ta_string_view chart; /* a chart's name; a system without it never matches */
    ta_string_view actor; /* an actor's name; a trailing '*' makes it a prefix */
    ta_string_view event; /* an event descriptor, SCXML's prefix rule ("error", "webhook.*") */
    uint64_t session;
    uint32_t from;   /* TA_TRACE_FROM_*: any of them (0: anything) */
    uint32_t sample; /* TA_TRACE_KEEP, _SAMPLE, _DROP, _RATIO (parts / whole of the trace ID) */
    uint32_t sample_parts, sample_whole;
    uint32_t detail;            /* the class a sampled trace gets; TA_NIL: keep its own */
    uint32_t per_second, burst; /* matches at most this often (logical time); 0: no limit */
} ta_trace_rule;

struct ta_trace_policy {
    uint32_t size;
    uint32_t sample_parts, sample_whole;
    uint32_t kinds[TA_TRACE_DETAILS]; /* TA_TRACE_* masks, TA_TRACE_VALUES included */
    uint32_t rule_count;              /* at most ta_system_config.trace_rules */
    const ta_trace_rule *rules;       /* copied by ta_system_trace_set */
};
/* Every trace sampled; unsampled: TA_TRACE_LIFECYCLE (lifecycle records are
 * written whatever the sampling); sampled and watched: TA_TRACE_FULL. */
TA_API void ta_trace_policy_default(ta_trace_policy *policy);
/* Replace the policy between runs; open spans are not affected.
 * TA_ERROR_STATE when tracing is off; TA_ERROR_NO_MEMORY for more rules
 * than trace_rules, or no room for their strings (nothing changes);
 * TA_ERROR_INVALID for a malformed rule or more than TA_TRACE_NAME_RULES
 * with an actor name. */
TA_API ta_status ta_system_trace_set(ta_system *system, const ta_trace_policy *policy);

/* ---- hibernation (host-driven: the system never does I/O) ----------------
 * An idle actor can be unloaded from RAM: ta_actor_hibernate() writes a blob
 * (kernel instance + datamodel state, see ta_datamodel_vtable.serialize) into the
 * host's buffer and frees the instance and its datamodel state. The actor
 * record stays (48 B): its handle, session address (#_scxml_<id>), name,
 * parent link, pending delayed sends and mailbox keep working. When anything
 * needs the actor — an event (from an actor, a timer, the host), a cancel or
 * terminate — the event waits in its mailbox and ta_system_run emits one
 * TA_EFFECT_WAKE; the host loads the blob (asynchronously, as it likes) and
 * calls ta_actor_wake(). SCXML semantics are unchanged: hibernation happens
 * only at a stable point, and _event, the configuration and the data come
 * back as they were.
 *
 * ta_actor_hibernate: TA_ERROR_STATE unless the actor is idle at a stable
 * point with empty queues, no invoked children and no pending request;
 * TA_ERROR_UNSUPPORTED if its datamodel cannot serialize it; TA_ERROR_NO_MEMORY if
 * `out` is too small (ta_buffer: nothing changes).
 * ta_actor_wake: TA_ERROR_INVALID if the blob does not belong to this actor
 * (session id, chart, image); TA_ERROR_NO_MEMORY if the system needs to grow first
 * (ta_runtime_system_grow, then retry); TA_ERROR_STATE if it is not hibernated.
 * A woken actor can be hibernated again; old blobs are then stale (the host
 * keeps the latest).
 * ta_system_cold_actors: a CLOCK sweep for the host's eviction policy. Scans
 * up to sweep->max_scan actor slots from sweep->cursor (wrapping, and moves
 * it), clears each actor's "ran since the last sweep" bit and reports actors
 * that did not run since the previous sweep and could be hibernated now.
 * Returns the count. */
typedef struct ta_sweep {
    uint32_t cursor; /* where the sweep goes on (start at 0) */
    uint32_t max_scan;
} ta_sweep;
TA_API ta_status ta_actor_hibernate(ta_system *system, uint32_t actor, ta_buffer *out);
TA_API ta_status ta_actor_wake(ta_system *system, uint32_t actor, ta_bytes blob);
TA_API uint32_t ta_system_cold_actors(ta_system *system, ta_sweep *sweep, ta_uint32_array out);

/* ---- moving and upgrading actors (hot reload; the host does the I/O) -----
 * ta_actor_export: like ta_actor_hibernate, but the actor leaves the system:
 * the blob also carries its pending delayed sends (due times are absolute
 * ticks of the shared clock) and its name, and the actor record is freed
 * (no done event, no effects). Root actors only (no parent). An actor with
 * running SCXML invocations leaves together with its invoked sessions (and
 * theirs): the blob holds the whole session tree — each session's state,
 * delayed sends (including sends to invoked children, #_<invokeid>), name and
 * session id, and the parent/child links. Payloads of delayed sends are
 * written as values (ta_datamodel_vtable.payload_serialize), so any datamodel host
 * can adopt the blob; with datamodels that lack the hook they stay handles
 * and only the writing process can. What a session's durable state is and
 * which path keeps which part of it: docs/durability.md. Every session of the tree must be
 * idle at a stable point with an empty mailbox and no pending request, and
 * awake (not hibernated); invocations the host carries out (external
 * invokes) cannot move: TA_ERROR_STATE. TA_ERROR_UNSUPPORTED if a datamodel
 * cannot serialize a session; TA_ERROR_NO_MEMORY if `out` is too small (ta_buffer;
 * nothing changes).
 *
 * ta_blob_migrate: translate an exported blob of chart `from` (of image
 * `from_image`) to chart `to_chart` of `to_image`. Pure: no system involved.
 * States are matched by id; the new configuration is completed to a legal
 * one (missing ancestors added, compound states without an active child
 * enter their initial states, parallel states all regions, extra children
 * dropped). Recorded history is kept where all its states still exist under
 * the same parent; late-binding data flags follow their states. <data> of the
 * new chart whose id the old chart did not declare is listed for
 * initialization on adoption. `out` must be 4-byte aligned. TA_ERROR_NO_MEMORY if
 * it is too small (ta_buffer), TA_ERROR_INVALID if the blob does
 * not belong to from_image, TA_ERROR_UNSUPPORTED for a session tree (an actor
 * with running invocations is not translated; it can be moved unchanged, and
 * migrated once its invocations have finished).
 *
 * ta_system_adopt: create an actor from an exported (possibly migrated)
 * blob, with the blob's session id — so its address is unchanged; a session
 * tree comes back whole (invoked sessions with their ids, linked to their
 * parents' invocations, done.invoke and #_<invokeid> working as before).
 * Fails with TA_ERROR_STATE if one of its sessions exists here, TA_ERROR_INVALID if the blob is for
 * another image, TA_ERROR_NO_MEMORY if the system must grow first. Delayed sends
 * are re-armed. For a migrated blob the datamodel's migrate hook runs, the
 * new <data> is initialized, and the platform event error.platform.upgrade
 * (SCXML 3.12.2 reserves error.platform for platform events) is placed in the
 * internal queue; its data are the previous <datamodel> bindings. The actor
 * becomes runnable to process it before any mailbox event.
 *
 * ta_blob_rebase: move an exported (possibly migrated) blob's delayed sends
 * from one clock to another, in place: each due time d becomes d - from + to
 * (one due before `from` is due at `to`). Due times are absolute ticks of the
 * clock the systems share; hosts whose clocks are not shared pass the
 * exporter's clock at export and the adopter's now, so every delayed send
 * keeps the time it had left. TA_ERROR_INVALID if the blob is malformed
 * (nothing changes then). */
TA_API ta_status ta_actor_export(ta_system *system, uint32_t actor, ta_buffer *out);
typedef struct ta_blob_migration {
    const ta_image *from_image, *to_image;
    uint32_t to_chart;
    uint32_t pad;
    ta_bytes blob; /* exported from a system running from_image */
} ta_blob_migration;
TA_API ta_status ta_blob_migrate(const ta_blob_migration *migration, ta_buffer *out);
TA_API ta_status ta_system_adopt(ta_system *system, ta_bytes blob, uint32_t *actor);
TA_API ta_status ta_blob_rebase(ta_memory blob, ta_time from, ta_time to);

/* Sessions created from now on (spawns, invocations) get ids after `serial`.
 * For hosts that keep sessions outside the system and allocate their ids
 * themselves (the PostgreSQL binding draws invoked children's ids from a
 * sequence). IDs only grow: TA_ERROR_INVALID if `serial` is below the last id
 * handed out. */
TA_API ta_status ta_system_set_next_serial(ta_system *system, uint64_t serial);

TA_API ta_status ta_system_snapshot(const ta_system *system, ta_buffer *out);
/* Deterministic: only real state is written — dead pool/slab/heap/outbox
 * storage is zero, never host-heap residue, and host pointers (the datamodel
 * contexts) are dropped; the restore re-attaches them. */
/* setup->config (may be NULL) re-supplies host pointers such as
 * io_processors; capacities come from the snapshot. Datamodel instance
 * handles stored in the snapshot must be valid for setup->datamodels (the
 * host restores its datamodel state). */
TA_API ta_status ta_system_restore(const ta_system_setup *setup, ta_bytes snapshot,
                                   ta_system **out);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_SYSTEM_H */
