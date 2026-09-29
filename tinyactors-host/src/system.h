#pragma once
/*
 * The native system (ADR 0012, ADR 0014): one ta_system and everything the
 * addon keeps for it. This module has no Node-API in it, so native tests can
 * create, close and free systems directly; napi_system.{c,h} wraps it as the
 * JavaScript System class.
 *
 * Lifecycle:
 * - tah_system_create() allocates the record from the platform's allocator
 *   (it holds the counting allocator, so it cannot count itself), initializes the
 *   allocator and the scratch arenas, and creates the ta_system with the
 *   allocator's core lane as core's allocator.
 * - tah_system_close() is idempotent and authoritative: it destroys the
 *   ta_system and the arenas. After it, the allocator's bytes in use are
 *   zero unless something leaked, and tah_system_leaked_bytes() says how
 *   many.
 * - tah_system_free() closes the system if needed and frees the record.
 *
 * Scheduling (ADRs 0005, 0007): the system owns the scheduling table and
 * its queues (scheduler.h). Adding work asks for a turn through `wake`,
 * which the Node-API layer sets (pump.c) and native tests may set to anything;
 * a flag coalesces the requests until the turn runs.
 *
 * Routing (ADR 0006): the directory and the router are installed on core
 * when the system is created. Close uninstalls both, gives back the
 * directory's address references and the mailboxes' events, and only then
 * destroys core, because core frees its address records at the end of
 * ta_system_destroy().
 *
 * Time (ADR 0007 point 3): core's clock is logical milliseconds that only
 * the host moves. With the manual clock only tah_system_advance_to() moves
 * it. With the realtime clock it follows a monotonic clock read natively,
 * relative to system creation: every turn starts by setting it, and so does
 * the host timer when it fires. After each turn the system asks the host for
 * one timer at ta_system_next_deadline(), through `arm`, but only when that
 * deadline is earlier than the one already armed; a later deadline lets the
 * armed timer fire early and arm again. Due sends are dispatched at the end
 * of a turn, at most `due` of them.
 *
 * Services (ROADMAP step 13, ADR 0001 rule 3): the log sink, the I/O
 * processors and the invokers core calls are adapters (the adapters in adapters/) that
 * queue effects (effects.h) instead of running JavaScript inside core. The
 * queue counts as work, so a system with effects waiting is not quiet, and
 * close releases what they hold while core is still alive. The in-system
 * SCXML invoker is registered at creation; it spawns its child in the same
 * system and tells the scheduler about it.
 *
 * Later steps add more plumbing (values); each one is released by
 * tah_system_close() in the order ADR 0012 gives.
 */
#include <stdbool.h>
#include <stddef.h>

#include "definitions.h"
#include "allocator.h"
#include "directory.h"
#include "effects.h"
#include "inputs.h"
#include "loads.h"
#include "offloads.h"
#include "adapter_trace.h"
#include "otlp.h"
#include "params.h"
#include "scheduler.h"
#include "tinyactors.h"
#include "tinyactors_host.h"

typedef struct tah_system tah_system;

/* Asks the host to run tah_system_turn() soon, never inside the caller. */
typedef void (*tah_wake_fn)(tah_system *system, void *context);
/*
 * Asks the host for one timer that calls tah_system_timer_fired() after
 * `delay` milliseconds, replacing any timer it armed before. It must not keep
 * the process alive (ADR 0007 point 3).
 */
typedef void (*tah_arm_fn)(tah_system *system, ta_time delay, void *context);
/* A monotonic clock in nanoseconds; only differences between readings matter. */
typedef uint64_t (*tah_now_fn)(void *context);

typedef enum { TAH_CLOCK_REALTIME = 0, TAH_CLOCK_MANUAL } tah_clock_kind;

typedef struct {
    tah_clock_kind kind;
    ta_time initial;       /* core's time at creation */
    uint64_t origin;       /* realtime: `now` at creation, in nanoseconds */
    tah_now_fn now;        /* realtime: the monotonic clock (a test may replace it) */
    void *now_context;
    tah_arm_fn arm;        /* realtime: the host timer (pump.c) */
    bool armed;            /* a host timer is pending */
    ta_time armed_deadline;
} tah_clock;

typedef struct tah_io_processor tah_io_processor;

/* What the in-system SCXML invoker asks: which definition this invoke starts. */
typedef struct {
    const ta_invoke_request *request;
    ta_definition_id *out;
    /* The child's data-model binding: the invoke's <param> and namelist
     * values the child declares as top-level <data> (SCXML 6.4), or empty.
     * The invoker releases it after the spawn. */
    ta_value *binding;
    ta_fault *fault;
    /* Set when the definition was compiled for this invoke: the hook took a
     * hold, which the invoker gives back after the spawn, so the definition
     * lives as long as its children (core ADR 0077). */
    bool *release;
} tah_child_params;

/*
 * Mail a chart sent to a target that does not exist when it is sent (napi
 * ADR 0022): the sender gets error.communication instead of a dead letter.
 */
typedef struct {
    tah_dead_reason reason; /* TAH_DEAD_NOT_FOUND, TAH_DEAD_DONE or TAH_DEAD_UNLOADED */
    const ta_actor_ref *destination;
} tah_unreachable;

/*
 * A quarantined invoked child's report to its parent (napi ADR 0025):
 * error.communication.invoke.<id>.quarantined, kept until it is queued in
 * the parent's mailbox (napi ADR 0032 section 3), so running out of memory
 * or a full mailbox delays it but never loses it.
 */
typedef struct {
    ta_actor_id parent;
    ta_invoke_id invoke_id;
    /* The child's step when it faulted (core ADR 0067). */
    ta_trace_parent trace;
    /* The fault, as the fault hook sees it: core's result and the call. */
    ta_result code;
    const char *operation;
} tah_quarantine_report;

struct tah_system {
    /* What the binding gives the kernel from the machine (tinyactors_host.h). */
    tah_platform platform;
    tah_allocator allocator;
    tah_scratch scratch;
    ta_system *core;
    tah_sched sched;
    tah_directory directory; /* installed on core at creation (ADR 0006) */
    /* Loads of unloaded actors, and their wake-ups (loads.h, napi ADR 0020). */
    tah_loads loads;
    /* Actors offloaded to bytes while unloaded (core ADR 0074). */
    tah_offloads offloads;
    tah_turn_budgets budgets;
    uint32_t due_budget;   /* due sends dispatched in one turn, at least 1 */
    tah_clock clock;
    tah_wake_fn wake;
    void *wake_context;
    bool turn_pending; /* wake was called and the turn has not run yet */
    bool closed;
    /*
     * Set around every core call (ADR 0013 point 4). A generated entry point
     * that finds it set throws "reentrant": data-model, I/O and invoker
     * callbacks may run JavaScript, but that JavaScript cannot call core.
     */
    bool inside_core;
    /* Bytes still in use when close finished; nonzero means a leak. */
    size_t leaked_bytes;
    /*
     * Called by close after core is destroyed and before the leak count:
     * the Node-API layer frees what core released into it (the value-box
     * free list, ADR 0011).
     */
    void (*after_core)(void *context);
    void *after_core_context;
    /* Effects waiting for JavaScript (effects.h), in the turn arena. */
    tah_effects effects;
    /* The trace encoder (otlp.h), while a sink is installed (napi ADR 0016):
     * core's records become OTLP spans as they are written, and the pump
     * hands the spans that ended to JavaScript as bytes, once per turn. */
    tah_otlp otlp;
    bool tracing;
    /* <log> records go to JavaScript's log sink (setLogSink). */
    bool log_to_js;
    /* What the sink asks core for, and what is watched (napi ADR 0017). */
    tah_trace_masks trace_masks;
    tah_trace_watch trace_watch;
    /* The definition lifecycle as core tells it (core ADR 0079): counts and origins. */
    tah_definitions definitions;
    /* The last service handle a JavaScript invoker's start was given. */
    uint64_t next_service;
    /*
     * The in-system SCXML invoker (adapters/invoker.c) asks the Node-API
     * layer which registered definition an invoke starts: the child nested
     * when the parent was defined, the one its content or src names, or
     * chart text it compiles (napi ADR 0024, napi_children.h). TA_OP_ERROR with `fault` saying why, TA_OP_NOMEM.
     */
    ta_op_status (*child_definition)(void *context, const tah_child_params *params);
    void *child_context;
    /*
     * The data of the error.communication a chart gets for mail to a target
     * that does not exist (napi ADR 0022): the Node-API layer makes
     * { reason, target }. False leaves the event without data.
     */
    bool (*unreachable_data)(void *context, const tah_unreachable *unreachable, ta_value *out);
    void *unreachable_context;
    /*
     * The data of the error a quarantined child's parent gets (napi ADR
     * 0025): the Node-API layer makes { code, message }, as the fault hook
     * sees them. False: no memory, so the report waits for a later turn.
     * Without the callback the event has no data.
     */
    bool (*quarantine_data)(void *context, const tah_quarantine_report *report, ta_value *out);
    /* The data of an invocation's `failed` report: { code, message } (napi
     * ADR 0032 section 3); NULL leaves it without data. */
    bool (*fault_data)(void *context, int32_t code, ta_str message, ta_value *out);
    void *fault_context;
    void *quarantine_context;
    /* Reports to parents that core has not taken yet (tah_quarantine_report). */
    tah_vec quarantine_reports;
    /* One record per registered I/O processor type (adapters/io.h, core ADR 0068). */
    tah_vec io_processors; /* tah_io_processor * */
    /* Turn duration and timer lateness (host_metrics.h, napi ADR 0017 section 7). */
    tah_host_metrics metrics;
    /* Set while core dispatches due delayed sends: what the router is handed
     * then is a timer firing, and its lateness is recorded. */
    bool dispatching_due;
    /* The input log (inputs.h, napi ADR 0032 section 6). */
    tah_inputs inputs;
    /* napi's parameters, fixed at creation (napi ADR 0035). */
    tah_params params;
};

/* TA_ERR_NOMEM when the record or core cannot be allocated. */
/* `incarnation` is core's (core ADR 0088): distinct from earlier systems and processes. */
/*
 * What a system is created with: core's incarnation (core ADR 0088) and
 * the core parameters napi sets at creation (core ADR 0092).
 */
typedef struct {
    /* The binding's platform: memory, clocks and randomness (tinyactors_host.h). */
    tah_platform platform;
    uint64_t incarnation;
    const ta_param_value *params;
    uint32_t param_count;
    /* napi's own parameters (napi ADR 0035); null for their defaults. */
    const tah_params *napi;
    /* Set to the core parameter a TA_ERR_INVALID refers to; may be null. */
    ta_param *rejected;
} tah_system_create_params;
ta_result tah_system_create(const tah_system_create_params *params, tah_system **out);
void tah_system_close(tah_system *system);
bool tah_system_closed(const tah_system *system);
size_t tah_system_leaked_bytes(const tah_system *system);
void tah_system_free(tah_system *system);

/*
 * Accepts a message and asks for a turn (ADR 0005). On TA_OK the system owns
 * the event's references and the message's waiter. TA_ERR_BUSY: the queue is
 * full (max_queued or max_retained); TA_ERR_NOMEM: nothing accepted.
 */
ta_result tah_system_post(tah_system *system, const tah_message *message);

/*
 * Queues an effect (effects.h), which hands over its references, and asks
 * for a turn. False when it was dropped (closing, or no memory).
 */
bool tah_system_effect(tah_system *system, const tah_effect *effect);

/* Encodes a trace record (otlp.h) and asks for a turn. */
void tah_system_trace(tah_system *system, const ta_trace_record *record);
/* Wall-clock time in nanoseconds since the Unix epoch, from the platform. */
uint64_t tah_system_wall_ns(const tah_system *system);
/* The platform's monotonic clock, in nanoseconds. */
uint64_t tah_system_monotonic_ns(const tah_system *system);
/* How far behind the earliest pending delayed send is, in seconds: 0 when
 * nothing is overdue. A value that keeps growing means timers are not
 * being served. */
double tah_system_overdue_seconds(const tah_system *system);
/* A turn that began at `started_ns` (tah_system_monotonic_ns) ended: records how
 * long it took, with the turn's sampled macrostep as its exemplar. */
void tah_system_record_turn(tah_system *system, uint64_t started_ns);
/* A delayed send due at `due` fired now: records how late (host_metrics.h). */
void tah_system_record_lateness(tah_system *system, ta_time due, const ta_trace_parent *trace);
/*
 * A send core dispatches outside its sender's run (a timer) failed: core
 * raises error.communication in the sender and starts a macrostep for it,
 * which the scheduler cannot see (napi ADR 0022). This makes the sender
 * runnable, and asks for a turn. Nothing happens inside a run, or for a
 * sender the table does not know.
 */
void tah_system_wake_sender(tah_system *system, const ta_actor_ref *sender);
/* Installs core's log sink when JavaScript or the trace encoder wants
 * <log> records (napi ADR 0017 section 7), and the null sink otherwise. */
ta_result tah_system_refresh_log_sink(tah_system *system);
/* 64 random bits from the platform, to seed core's trace-ID generator once
 * per sink (core ADR 0073: core never reads the OS). */
uint64_t tah_system_random_seed(tah_system *system);
/*
 * Gives `context` a new trace from core's generator (core ADR 0073) when
 * tracing is on and it has none, so a delivery or spawn that brings no
 * traceparent starts a trace (napi ADR 0016 point 4). Core's sampler sets
 * the sampled flag. `detail` is kept.
 */
void tah_system_start_trace(tah_system *system, ta_trace_context *context);
/* Asks for a turn unless one is pending or the system is quiet or closed. */
void tah_system_want_turn(tah_system *system);
/*
 * Runs one turn (ADR 0007 point 1). The turn's records stay in
 * system->sched until tah_system_end_turn(), which resets the turn arena and
 * asks for the next turn when work remains.
 */
void tah_system_turn(tah_system *system);
bool tah_system_dispatch_due(tah_system *system);
/*
 * Cancels the send() waiter at `index` (ADR 0005 point 11). A message still
 * waiting for a load is withdrawn from the load's mailbox (napi ADR 0020),
 * as one in an actor's mailbox is; otherwise tah_sched_cancel(). True when
 * the waiter was cancelled; the caller settles and gives it back.
 */
bool tah_system_cancel(tah_system *system, uint32_t index);
void tah_system_end_turn(tah_system *system);
/*
 * No runnable or ready actors, no dead letters to report, no effects or load
 * requests waiting for JavaScript, and nothing due at core's current time
 * (ADR 0007 point 4). A load JavaScript is running does not count: flush()
 * waits for it separately (pump.c).
 */
bool tah_system_quiet(const tah_system *system);

/*
 * Chooses the clock before anything runs: `kind`, and core's time at
 * creation. TA_ERR_INVALID when the system has run or is closed.
 */
ta_result tah_system_set_clock(tah_system *system, tah_clock_kind kind, ta_time initial);
/*
 * Realtime: moves core's time to the monotonic clock, unless a core call is
 * running. Manual: nothing.
 */
void tah_system_sync_time(tah_system *system);
/*
 * Manual clock: moves core's time to `time` and asks for a turn when
 * something is due. TA_ERR_INVALID when the clock is realtime or `time` is
 * earlier than core's time; TA_ERR_BUSY inside a core call.
 */
ta_result tah_system_advance_to(tah_system *system, ta_time time);
/* A deadline may have moved earlier (a wake-up was added, napi ADR 0020):
 * ask for a turn if something is due, and arm the host timer for the rest. */
void tah_system_arm_timer(tah_system *system);
/* The host timer fired: move time, ask for a turn, and arm for what is left. */
void tah_system_timer_fired(tah_system *system);

