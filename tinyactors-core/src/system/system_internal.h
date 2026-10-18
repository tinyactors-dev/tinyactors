/* system_internal.h — actor-system internals. Everything here lives inside the
 * single caller-provided region and refers to other region objects by index
 * or self-relative offset only. Host pointers (image view, datamodel
 * bindings, io_processor descriptors) are re-bound on relocate/restore. */
#ifndef TINYACTORS_SYSTEM_SYSTEM_INTERNAL_H
#define TINYACTORS_SYSTEM_SYSTEM_INTERNAL_H

#include "base/arena.h"
#include "core/core_internal.h"
#include "base/heap.h"
#include "base/index_queue.h"
#include "base/output.h"

#define TA_SYSTEM_MAGIC 0x53594154u /* "TAYS" */
/* The region's layout: 2 since the step counters and trace references
 * (docs/adr/0006 in the repository root), 3 with the charts' counts, 4 since
 * an external invocation's request keeps its type and params, 5 with the queue
 * of parked spawns, 6 with the index of actor names, 7 with each actor's count
 * of requests, 8 with the wake queue linked both ways; a snapshot of another
 * is refused. */
#define TA_SYSTEM_VERSION      8u
#define TA_MAX_DATAMODELS      8u
#define TA_EXTERNAL_INVOKE_BIT (1u << 23) /* in a child handle: external invoke token */
/* slab byte budgets stay below 4 GiB so page arithmetic cannot overflow u32 */
#define TA_SLAB_MAX_BYTES 0xFFF00000u
TA_STATIC_ASSERT(TA_SYSTEM_MAX_ACTORS == TA_EXTERNAL_INVOKE_BIT - 2u, max_actors_matches_handles);
TA_STATIC_ASSERT(TA_SYSTEM_MAX_POOL == TA_POOL_MAX_CAPACITY, max_pool_matches_handles);

enum {
    ACTOR_FLAG_CANCEL = 1u << 0,
    ACTOR_FLAG_ORPHAN = 1u << 1,
    ACTOR_FLAG_DONE_REPORTED = 1u << 2,
    ACTOR_FLAG_IN_WAKE_QUEUE = 1u
        << 3, /* hibernated, in the wake queue (TA_EFFECT_WAKE not yet emitted) */
    ACTOR_FLAG_WAKE_SENT = 1u
        << 4, /* hibernated, TA_EFFECT_WAKE emitted, waiting for ta_actor_wake */
    ACTOR_FLAG_TOUCHED = 1u << 5,    /* ran since the last ta_system_cold_actors sweep */
    ACTOR_FLAG_TRACE_NAMES = 1u << 6, /* trace_names says which name rules match it (policy.c) */
    ACTOR_FLAG_WATCHED = 1u << 7 /* ta_actor_watch: TA_EFFECT_CHANGED at its next stable point */
};
/* Hibernated actors (state TA_ACTOR_HIBERNATED) have no instance: `instance`
 * holds the chart index instead, `next` links the wake queue. */

typedef struct ta_actor {
    uint32_t next; /* run queue / workspace-wait queue link */
    uint8_t state; /* TA_ACTOR_* */
    uint8_t flags;
    uint16_t trace_names; /* bit i: the policy's i-th rule with an actor name matches its name */
    uint32_t instance;    /* slab offset of the instance */
    uint32_t mailbox_head, mailbox_tail, mailbox_length;
    uint32_t parent; /* actor handle (0 = none) */
    uint32_t parent_slot;
    uint32_t timers; /* head of this actor's pending delayed sends */
    uint32_t name;   /* string reference (the chart's name string unless the host named it) */
    uint64_t serial; /* _sessionid is decimal(serial), formatted on demand */
} ta_actor;
TA_STATIC_ASSERT(sizeof(ta_actor) == 48, actor_record_is_48_bytes);

/* What an actor is linked into besides its record, in an array beside the
 * actor pool (actor_links, indexed like it): the ring of the live actors of
 * its name, in the order they came (the index of names holds the oldest),
 * and how many requests it made that are live, or, in the wake queue, the
 * actor before it there (the queue's forward link is the record's `next`). */
typedef struct ta_actor_links {
    uint32_t name_prev, name_next; /* actor indices; an actor alone with its name links itself */
    union {
        uint32_t requests;  /* live ta_requests whose `actor` is this one (ta_system_request_*) */
        uint32_t wake_prev; /* in the wake queue (hibernated: no request is live, so `requests`
                               is 0, and it is 0 again once the actor leaves the queue) */
    };
} ta_actor_links;

typedef struct ta_timer {
    uint32_t owner_next;
    uint32_t owner; /* actor handle */
    uint32_t event;
    uint32_t route, arg, target; /* target string reference (owned) */
} ta_timer;

enum { REQUEST_EXTERNAL_INVOKE = 1, REQUEST_RESOLVE_SOURCE = 2, REQUEST_PENDING_SPAWN = 3 };
typedef struct ta_request {
    uint32_t kind;
    uint32_t actor; /* parent actor handle */
    uint32_t slot;
    uint32_t invokeid; /* string reference (owned) */
    uint32_t source;   /* string reference (owned); REQUEST_PENDING_SPAWN: chart index */
    uint32_t pad;      /* datamodel index owning `payload` */
    uint64_t payload;  /* invoke params awaiting a resolved chart / a free slot; an external
                          invocation's params, retained while it runs (a restore announces it
                          again with them) */
    union {
        uint32_t type; /* REQUEST_EXTERNAL_INVOKE: the service's type, string reference (owned) */
        uint32_t next; /* REQUEST_PENDING_SPAWN: the link of the system's pending_spawns */
    };
    uint32_t ended;    /* REQUEST_EXTERNAL_INVOKE: the service finished (its INVOKE_DONE was accepted,
                          the done.invoke not taken yet) or failed (ERROR_COMMUNICATION from it): not
                          announced after a restore, and its FROM_INVOKE / INVOKE_DONE refused */
} ta_request;

/* Internal effect record; ta_effect views are materialized on demand. */
typedef struct ta_effect_record {
    uint32_t kind, actor, token, io_processor, flags;
    uint32_t target, event_name, sendid, auxiliary; /* srefs (owned) */
    uint32_t event;     /* event pool index to release on ack, or TA_NIL */
    uint32_t datamodel; /* datamodel owning payload (0xFF none) */
    uint32_t pad;
    uint64_t payload; /* released on ack if `owned` */
    uint64_t delay;
    uint64_t serial;        /* the actor's session id at push time */
    ta_trace_context trace; /* SEND, INVOKE, LOG, DONE: the step's (docs/adr/0006) */
} ta_effect_record;

struct ta_system {
    uint32_t magic, version;
    uint64_t region_size;
    ta_system_config config;
    ta_core core;
    ta_pool actors, events, timers, workspaces, requests;
    ta_slab slab;
    ta_heap heap;
    ta_index_queue run_queue, workspace_queue, wake_queue;
    /* the REQUEST_PENDING_SPAWNs, in the order they were parked (request.next):
     * a run retries these, never walks the requests, which hold every running
     * external invocation too */
    ta_index_queue pending_spawns;
    uint32_t runnable_count; /* length of run_queue (stats are O(1)) */
    uint32_t hibernated_count, pending_wake_count;
    uint64_t stats_hibernations, stats_wakes;
    int64_t effects_relative, effect_views_relative, sessions_relative, chart_datamodels_relative,
        chart_contexts_relative, chart_stats_relative;
    /* the index of names: open addressing (linear probing, deletion by
     * backward shift) of name_capacity slots, each TA_NIL or the oldest live
     * actor of a name, the others in its ring (actor_links) */
    int64_t actor_links_relative, names_relative;
    uint32_t name_capacity, name_count; /* name_count: distinct names indexed */
    uint32_t effects_head, effects_length, effects_capacity;
    uint32_t session_capacity, session_count;
    uint32_t chart_count, datamodel_count;
    uint32_t in_call; /* re-entrancy guard */
    /* the done.invoke event made for a session that ends now (core's
     * done_event hook), for the ta_system_done of the same step: its index
     * + 1, 0 between steps */
    uint32_t done_event;
    uint64_t now, sequence, next_serial;
    uint64_t stats_delivered, stats_fuel, stats_turns, stats_timers, stats_spawned,
        stats_traces_started, stats_traces_sampled; /* ta_system_stats */
    /* tracing (docs/adr/0006): the journal and the context pool (carved only
     * when tracing is on), the generator, the sampling threshold and the fuel
     * clock records are stamped with */
    ta_journal journal;
    ta_pool contexts;
    ta_trace_generator generator;
    uint64_t trace_threshold;
    ta_trace_policy policy; /* docs/adr/0007; its rules are compiled (rules_relative) */
    int64_t rules_relative, chart_points_relative;
    uint32_t rule_count, rule_points; /* rule_points: TA_TRACE_AT_* any rule is asked at */
    uint64_t fuel_clock;
    ta_datamodel_binding datamodels[TA_MAX_DATAMODELS + 1];
};

#define SYSTEM_POINTER(sys, rel, type) ((type *)ta_pointer_add((sys), (sys)->rel))

TA_INLINE ta_actor *ta_actor_at(const ta_system *system, uint32_t index) {
    return (ta_actor *)ta_pool_at(&system->actors, index);
}
TA_INLINE ta_instance *ta_actor_instance(const ta_system *system, const ta_actor *actor) {
    return (ta_instance *)ta_slab_pointer(&system->slab, actor->instance);
}
TA_INLINE uint32_t ta_actor_chart(const ta_system *system, const ta_actor *actor) {
    return actor->state == TA_ACTOR_HIBERNATED ? actor->instance
                                               : ta_actor_instance(system, actor)->chart;
}

/* route.c: core's done_event hook (core_internal.h) */
int ta_system_done_event(void *system, uint32_t actor);

/* system.c */
uint32_t ta_system_resolve_actor(const ta_system *s, uint32_t handle); /* index or TA_NIL */
/* Spawn an actor. A root's bootstrap runs under `traceparent` or in a new
 * trace; an invoked child's (parent != 0) under the parent's current step. */
uint32_t ta_system_spawn_actor(ta_system *s, uint32_t chart, ta_string_view name, ta_payload params,
                               uint32_t parent, uint32_t parent_slot, ta_string_view traceparent,
                               ta_status *status);
/* `in` with every default filled in and every count clamped (system.c): the
 * config a system runs with. The host fills its copy of the system config
 * with it (view NULL: instance_bytes stays as given, 0 derives it from each
 * segment's image), so a host and a system created alone share the defaults. */
void ta_system_config_normalize(const ta_image_view *view, const ta_system_config *in,
                                ta_system_config *config);
/* Free an actor and everything it holds. RECLAIM_ENDED: its session ended
 * here (finished, terminated, faulted): a `destroyed` record, counted as
 * ended. RECLAIM_LEFT: it lives on elsewhere (exported, or an adoption
 * undone): neither. */
enum { RECLAIM_ENDED = 0, RECLAIM_LEFT = 1 };
/* ta_actor_hibernate and ta_actor_export into an output that may grow
 * (base/output.h), written in one pass: each datamodel output (serialize,
 * payload_serialize) is asked for once and copied before the datamodel is
 * called again (docs/adr/0010 in the repository root, decision 1), so a
 * value is never serialized twice to size a buffer. TA_ERROR_NO_MEMORY when
 * the output did not fit (its length is what it needs; nothing changed).
 *
 * The one way system code reaches an allocator after ta_system_init: when
 * `out` has one (the host's output or its blob scratch), writing grows it
 * through that allocator. Only outside a run: TA_ERROR_STATE inside this
 * system's run (in_call), and the host takes its outputs only outside a run,
 * before it opens its guard (host.c, host_output). The blocks are the
 * host's, never region memory: the region is unchanged until the write has
 * fit, and then only the actor leaves it. */
ta_status ta_actor_write_hibernation(ta_system *s, uint32_t actor, ta_output *out);
ta_status ta_actor_write_export(ta_system *s, uint32_t actor, ta_output *out);
void ta_system_reclaim(ta_system *s, uint32_t index, uint32_t how);
void ta_system_make_runnable(ta_system *s, uint32_t index);
void ta_system_mailbox_push(ta_system *s, uint32_t index, uint32_t event);
void ta_system_wake_request(ta_system *s, uint32_t index); /* hibernated actor has work */
/* Take actor `index` (ACTOR_FLAG_IN_WAKE_QUEUE) out of the wake queue: O(1). */
void ta_system_wake_unlink(ta_system *s, uint32_t index);
/* System variables for a session (sessionid, name, _ioprocessors). */
typedef struct ta_system_variables_buffer {
    char session_id[24], location[256];
    ta_io_processor_entry io_processors[17];
} ta_system_variables_buffer;
void ta_system_actor_variables(ta_system *s, const ta_actor *a, ta_system_variables_buffer *b,
                               ta_datamodel_system_variables *sv);
uint32_t ta_system_session_lookup(const ta_system *s, uint64_t serial);
void ta_system_session_insert(ta_system *s, uint32_t index);
/* The index of names: an actor enters it when it is created (spawned or
 * adopted) and leaves it when it is reclaimed; hibernation keeps it there. */
void ta_system_name_insert(ta_system *s, uint32_t index);

/* trace.c (system): starting traces, lifecycle records */
/* What a host starts (an input, a spawn, a timer of an untraced send) runs
 * under `traceparent` when it parses, keeping its sampled flag, else in a new
 * trace sampled by the system's ratio. Returns a context (one reference), or
 * TA_NIL when unsampled or tracing is off; `*value` gets the trace either way
 * (no trace when tracing is off). */
/* The kernel's masks and the sampling threshold from system->policy. */
void ta_system_trace_policy_apply(ta_system *system);

/* ---- the policy's rules (policy.c, docs/adr/0007) ---- */
/* A rule as a system holds it: names resolved to indices, strings in the slab. */
typedef struct ta_trace_rule_compiled {
    uint8_t at, sample, detail, name_bit; /* detail 0xFF: keep; name_bit 0xFF: no actor name */
    uint32_t from;
    uint32_t chart;                  /* TA_NIL: any; TA_NIL - 1: a chart this image lacks */
    uint32_t event_first, event_end; /* the event trie's interval; TA_NIL: any */
    uint32_t event_text;             /* a descriptor not in the trie (slab string), TA_NIL */
    uint32_t actor, actor_prefix;    /* the actor's name (slab string), TA_NIL; a prefix? */
    uint32_t per_second, burst;
    uint64_t session;
    uint64_t threshold;
    uint64_t tokens, refilled; /* the rate limit: ticks of credit, and when last refilled */
} ta_trace_rule_compiled;
/* What a decision point knows. */
typedef struct ta_trace_ask {
    uint32_t at, from;
    uint32_t actor; /* actor index, TA_NIL */
    uint32_t chart; /* TA_NIL without an actor */
    uint32_t event; /* event pool index, TA_NIL */
} ta_trace_ask;
/* Apply the first matching rule at ask->at to *flags (a context's: W3C and
 * the class) for `trace`; 0 when none matched (nothing changed). */
int ta_system_trace_rules(ta_system *system, const ta_trace_ask *ask, ta_trace_id trace,
                          uint32_t *flags);
/* Compile policy->rules into the system (the old ones' strings let go). */
ta_status ta_system_trace_rules_set(ta_system *system, const ta_trace_policy *policy);
/* An actor took event `event`: the take rules decide again (the event's context
 * replaced when the decision changes). */
void ta_system_trace_take(ta_system *system, uint32_t index, uint32_t event);
/* Start a trace for what `ask` describes (TA_TRACE_AT_START): under
 * `traceparent` (with the sender's local `trace_flags`) or a new trace ID,
 * sampled as the policy and its rules say. The context, TA_NIL when
 * unsampled; *value its value. */
uint32_t ta_system_start_trace(ta_system *s, ta_string_view traceparent, uint32_t trace_flags,
                               const ta_trace_ask *ask, ta_trace_context *value);

/* A lifecycle record (TA_TRACE_SPAWNED, ...) of actor `index`, its header
 * filled: under `context` (a value; a new trace ID if it has none), keyed
 * (session, macrostep, step). NULL when the kind is off or the journal full. */
ta_trace_record *ta_system_trace_lifecycle(ta_system *s, uint32_t index, uint32_t kind,
                                           uint32_t size, const ta_trace_context *context,
                                           uint64_t macrostep, uint32_t step);

/* route.c */
/* Route a kernel send: returns 0 on success, else a TA_POOL_* code (exhausted). */
uint32_t ta_system_route_send(ta_system *s, uint32_t sender, const ta_core_out *o);
void ta_system_deliver(ta_system *s, uint32_t sender, uint32_t event, uint32_t route, uint32_t arg,
                       uint32_t target);
void ta_system_cancel(ta_system *s, uint32_t sender, uint32_t sendid);
void ta_system_cancel_timers(ta_system *s, uint32_t index);
void ta_system_fire_timers(ta_system *s);
/* TA_POOL_NONE, or the pool a parked spawn waits for (the host grows it) */
uint32_t ta_system_invoke(ta_system *s, uint32_t parent, const ta_core_out *o);
/* The default string slab: a base and a share per actor (a host that grows
 * the actors grows the strings by their share) */
#define TA_STRING_BYTES_BASE      16384u
#define TA_STRING_BYTES_PER_ACTOR 16u
/* The pool an allocation found full since the last call or the last
 * relocation (a TA_POOL_*; TA_POOL_NONE if none did), and the marks cleared:
 * what the host grows for a TA_ERROR_NO_MEMORY or a TA_RUN_EXHAUSTED. */
uint32_t ta_system_exhausted(ta_system *s);
void ta_system_uninvoke(ta_system *s, uint32_t parent, const ta_core_out *o);
/* An external invocation ends: its request and what it holds (invoke id, type, src, params). */
void ta_system_external_free(ta_system *s, uint32_t ri);
/* The first external invocation at request index `from` or past it, as a TA_EFFECT_INVOKE view
 * (flagged TA_EFFECT_FLAG_RESTORED; strings valid until the system's next call); its index, or
 * TA_NIL when there is none. A restored host's first run announces each one again with it. */
uint32_t ta_system_external_invocation(ta_system *s, uint32_t from, ta_effect *out);
void ta_system_autoforward(ta_system *s, uint32_t parent, const ta_core_out *o);
void ta_system_done(ta_system *s, uint32_t index, ta_payload done, uint32_t final_state);
/* A request of actor `index` (its parent): allocated with its `actor` set,
 * counted in the actor's links, so a stable point is checked in O(1)
 * (hibernate.c); TA_NIL when the pool is full. Freed the same way. */
uint32_t ta_system_request_alloc(ta_system *s, uint32_t index);
void ta_system_request_free(ta_system *s, uint32_t ri);
/* the live requests actor `index` made */
uint32_t ta_system_actor_requests(const ta_system *s, uint32_t index);
ta_actor_links *ta_system_actor_links(const ta_system *s);
/* Retry spawns parked by pool exhaustion; returns TA_POOL_* still exhausted. */
uint32_t ta_system_retry_pending(ta_system *s);

/* effects.c */
ta_effect_record *ta_system_effect_push(ta_system *s, uint32_t kind, uint32_t actor);
/* hibernate.c: move wake requests into TA_EFFECT_WAKE effects while there is room */
void ta_system_emit_wakes(ta_system *s);
int ta_system_effect_full(const ta_system *s);
void ta_system_effect_release(ta_system *s, ta_effect_record *f);
void ta_system_effects_release(ta_system *system, uint32_t n, int resume);
void ta_system_sender_resume(ta_system *system, uint32_t actor, uint64_t serial);
int ta_system_senders_waiting(const ta_system *system);
/* How many actors wait for a workspace (WAIT_WORKSPACE): their queue's length. */
uint32_t ta_system_workspace_waiters(const ta_system *system);

#endif /* TINYACTORS_SYSTEM_SYSTEM_INTERNAL_H */
