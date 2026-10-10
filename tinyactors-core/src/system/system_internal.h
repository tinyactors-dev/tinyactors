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

#define TA_SYSTEM_MAGIC 0x53594154u /* "TAYS" */
/* The region's layout: 2 since the step counters and trace references
 * (docs/adr/0006 in the repository root); a snapshot of another is refused. */
#define TA_SYSTEM_VERSION      3u
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
        << 4,                    /* hibernated, TA_EFFECT_WAKE emitted, waiting for ta_actor_wake */
    ACTOR_FLAG_TOUCHED = 1u << 5 /* ran since the last ta_system_cold_actors sweep */
};
/* Hibernated actors (state TA_ACTOR_HIBERNATED) have no instance: `instance`
 * holds the chart index instead, `next` links the wake queue. */

typedef struct ta_actor {
    uint32_t next; /* run queue / workspace-wait queue link */
    uint8_t state; /* TA_ACTOR_* */
    uint8_t flags;
    uint16_t pad;
    uint32_t instance; /* slab offset of the instance */
    uint32_t mailbox_head, mailbox_tail, mailbox_length;
    uint32_t parent; /* actor handle (0 = none) */
    uint32_t parent_slot;
    uint32_t timers; /* head of this actor's pending delayed sends */
    uint32_t name;   /* string reference (the chart's name string unless the host named it) */
    uint64_t serial; /* _sessionid is decimal(serial), formatted on demand */
} ta_actor;
TA_STATIC_ASSERT(sizeof(ta_actor) == 48, actor_record_is_48_bytes);

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
    uint64_t payload;  /* invoke params awaiting a resolved chart / a free slot */
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
    uint32_t runnable_count; /* length of run_queue (stats are O(1)) */
    uint32_t hibernated_count, pending_wake_count;
    uint64_t stats_hibernations, stats_wakes;
    int64_t effects_relative, effect_views_relative, sessions_relative, chart_datamodels_relative,
        chart_contexts_relative, chart_stats_relative;
    uint32_t effects_head, effects_length, effects_capacity;
    uint32_t session_capacity, session_count;
    uint32_t chart_count, datamodel_count;
    uint32_t in_call; /* re-entrancy guard */
    uint32_t pad;
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
    ta_trace_policy policy; /* docs/adr/0007 */
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

/* system.c */
uint32_t ta_system_resolve_actor(const ta_system *s, uint32_t handle); /* index or TA_NIL */
/* Spawn an actor. A root's bootstrap runs under `traceparent` or in a new
 * trace; an invoked child's (parent != 0) under the parent's current step. */
uint32_t ta_system_spawn_actor(ta_system *s, uint32_t chart, ta_string_view name, ta_payload params,
                               uint32_t parent, uint32_t parent_slot, ta_string_view traceparent,
                               ta_status *status);
/* Free an actor and everything it holds. RECLAIM_ENDED: its session ended
 * here (finished, terminated, faulted): a `destroyed` record, counted as
 * ended. RECLAIM_LEFT: it lives on elsewhere (exported, or an adoption
 * undone): neither. */
enum { RECLAIM_ENDED = 0, RECLAIM_LEFT = 1 };
void ta_system_reclaim(ta_system *s, uint32_t index, uint32_t how);
void ta_system_make_runnable(ta_system *s, uint32_t index);
void ta_system_mailbox_push(ta_system *s, uint32_t index, uint32_t event);
void ta_system_wake_request(ta_system *s, uint32_t index); /* hibernated actor has work */
/* System variables for a session (sessionid, name, _ioprocessors). */
typedef struct ta_system_variables_buffer {
    char session_id[24], location[256];
    ta_io_processor_entry io_processors[17];
} ta_system_variables_buffer;
void ta_system_actor_variables(ta_system *s, const ta_actor *a, ta_system_variables_buffer *b,
                               ta_datamodel_system_variables *sv);
uint32_t ta_system_session_lookup(const ta_system *s, uint64_t serial);
void ta_system_session_insert(ta_system *s, uint32_t index);

/* trace.c (system): starting traces, lifecycle records */
/* What a host starts (an input, a spawn, a timer of an untraced send) runs
 * under `traceparent` when it parses, keeping its sampled flag, else in a new
 * trace sampled by the system's ratio. Returns a context (one reference), or
 * TA_NIL when unsampled or tracing is off; `*value` gets the trace either way
 * (no trace when tracing is off). */
uint32_t ta_system_start_trace(ta_system *s, ta_string_view traceparent, ta_trace_context *value);
/* The kernel's masks and the sampling threshold from system->policy. */
void ta_system_trace_policy_apply(ta_system *system);
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
uint32_t ta_system_invoke(ta_system *s, uint32_t parent, const ta_core_out *o);
void ta_system_uninvoke(ta_system *s, uint32_t parent, const ta_core_out *o);
void ta_system_autoforward(ta_system *s, uint32_t parent, const ta_core_out *o);
void ta_system_done(ta_system *s, uint32_t index, ta_payload done, uint32_t final_state);
/* Retry spawns parked by pool exhaustion; returns TA_POOL_* still exhausted. */
uint32_t ta_system_retry_pending(ta_system *s);

/* effects.c */
ta_effect_record *ta_system_effect_push(ta_system *s, uint32_t kind, uint32_t actor);
/* hibernate.c: move wake requests into TA_EFFECT_WAKE effects while there is room */
void ta_system_emit_wakes(ta_system *s);
int ta_system_effect_full(const ta_system *s);
void ta_system_effect_release(ta_system *s, ta_effect_record *f);

#endif /* TINYACTORS_SYSTEM_SYSTEM_INTERNAL_H */
