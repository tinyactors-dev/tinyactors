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
 * its queues (scheduler.h). Adding work asks for a turn through the pump
 * driver's `wake` (tah_system_set_driver), which the Node-API layer installs
 * (pump.c) and native tests may set to anything; a flag coalesces the
 * requests until the turn runs.
 *
 * Routing (ADR 0006): the directory is installed on core when the system is
 * created, and each turn answers the load requests core owes for mail to
 * targets that are not resident (its loads, loads.h). Close uninstalls the directory,
 * gives back its address references and the mailboxes' events, and only
 * then destroys core, because core frees its address records at the end of
 * ta_system_destroy().
 *
 * Time (ADR 0007 point 3): core's clock is logical milliseconds that only
 * the host moves. With the manual clock only tah_system_advance_to() moves
 * it. With the realtime clock it follows a monotonic clock read natively,
 * relative to system creation: every turn starts by setting it, and so does
 * the host timer when it fires. After each turn the system asks the host for
 * one timer at core's next deadline (the wake its last ta_run() or
 * ta_system_set_time() reported), through `arm`, but only when that
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
#include "loads.h"
#include "offloads.h"
#include "params.h"
#include "scheduler.h"
#include "tinyactors.h"
#include "tinyactors_otlp.h"
#include "platform.h"

typedef struct tah_system tah_system;

/*
 * Asks the host to run tah_system_turn() soon, never inside the caller.
 * False: no turn is coming (the host could not schedule one), and a later
 * request asks again.
 */
typedef bool (*tah_wake_fn)(tah_system *system, void *context);
/*
 * Asks the host for one timer that calls tah_system_timer_fired() after
 * `delay` milliseconds, replacing any timer it armed before. It must not keep
 * the process alive (ADR 0007 point 3). False: no timer is armed, and the
 * next turn arms again.
 */
typedef bool (*tah_arm_fn)(tah_system *system, ta_time delay, void *context);

/* The binding's pump driver (ADR 0007): how the system asks for turns and
 * timers. `arm` may be NULL when the binding only uses the manual clock. */
typedef struct {
    tah_wake_fn wake;
    tah_arm_fn arm;
    void *context;
} tah_pump_driver;

/* A monotonic clock in nanoseconds; only differences between readings matter. */
typedef uint64_t (*tah_now_fn)(void *context);

typedef enum { TAH_CLOCK_REALTIME = 0, TAH_CLOCK_MANUAL } tah_clock_kind;

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

/*
 * What the system asks the binding's language layer (tah_system_set_callbacks).
 * Each may be NULL; all get `context`. None is called after
 * tah_system_close() returns.
 */
typedef struct {
    void *context;
    /*
     * The in-system SCXML invoker (adapter_invoker.c) asks which registered
     * definition an invoke starts: the child nested when the parent was
     * defined, the one its content or src names, or chart text it compiles
     * (napi ADR 0024). TA_OP_ERROR with `fault` saying why, TA_OP_NOMEM.
     * NULL: the invoker refuses every invoke.
     */
    ta_op_status (*child_definition)(void *context, const tah_child_params *params);
    /*
     * The data of the error.communication a chart gets for mail to a target
     * that does not exist (napi ADR 0022): napi makes { reason, target }.
     * False leaves the event without data.
     */
    bool (*unreachable_data)(void *context, const tah_unreachable *unreachable, ta_value *out);
    /*
     * The data of the error a quarantined child's parent gets (napi ADR
     * 0025): napi makes { code, message }, as the fault hook sees them.
     * False: no memory, so the report waits for a later turn. NULL: the
     * event has no data.
     */
    bool (*quarantine_data)(void *context, const tah_quarantine_report *report, ta_value *out);
    /* The data of an invocation's `failed` report: { code, message } (napi
     * ADR 0032 section 3); NULL leaves it without data. */
    bool (*fault_data)(void *context, int32_t code, ta_str message, ta_value *out);
    /*
     * Called by close after core is destroyed and before the leak count:
     * the binding frees what core released into it (napi's value-box free
     * list, ADR 0011).
     */
    void (*after_core)(void *context);
} tah_system_callbacks;

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
    /* The kernel's parameters (params.h, napi ADR 0035); null for their defaults. */
    const tah_params *hosting;
    /* Set to the core parameter a TA_ERR_INVALID refers to; may be null. */
    ta_param *rejected;
    /*
     * The binding may install a trace encoder (tah_system_set_otlp).
     * The kernel reads core's tel ring only between ta_run() calls, and core sizes
     * the ring at creation, so the kernel then sizes it for one call's
     * records, unless the binding set telBytes (core ADR 0098). False keeps
     * core's ring as the parameters say.
     */
    bool traced;
} tah_system_create_params;
ta_result tah_system_create(const tah_system_create_params *params, tah_system **out);
void tah_system_close(tah_system *system);
bool tah_system_closed(const tah_system *system);

/*
 * The system's parts, for the binding to call their functions with. Each
 * lives as long as the system; core, and what close destroys, only until
 * tah_system_close().
 */
ta_system *tah_system_core(tah_system *system);

/* The kernel ID of an event name given as text, which the kernel keeps
 * (events.h): TA_ERR_INVALID for malformed text, TA_ERR_NOMEM when its table
 * cannot grow. */
ta_result tah_event_name_intern(tah_system *system, ta_str text, ta_event_name_id *out);

/* Like ta_event_name_format(), for a kernel name ID or a name core gave. */
ta_result tah_event_name_format(const tah_system *system, const ta_event_name_format_params *params);
tah_allocator *tah_system_allocator(tah_system *system);
tah_scratch *tah_system_scratch(tah_system *system);
tah_sched *tah_system_sched(tah_system *system);
tah_directory *tah_system_directory(tah_system *system);
tah_loads *tah_system_loads(tah_system *system);
tah_offloads *tah_system_offloads(tah_system *system);
tah_effects *tah_system_effects(tah_system *system);
/* The installed trace encoder (tah_system_set_otlp), or NULL. */
tao_encoder *tah_system_otlp(tah_system *system);
tah_trace_watch *tah_system_trace_watch(tah_system *system);
tah_definitions *tah_system_definitions(tah_system *system);
tah_host_metrics *tah_system_metrics(tah_system *system);

/*
 * Around every core call the binding makes (ADR 0013 point 4): a binding
 * that finds tah_system_inside_core() true refuses to call core again
 * ("reentrant"), since the callbacks core runs may run the binding's code.
 */
bool tah_system_inside_core(const tah_system *system);
void tah_system_enter_core(tah_system *system);
/* Leaving core also reads what it wrote to tel (tah_system_drain_telemetry). */
void tah_system_leave_core(tah_system *system);

/*
 * Tracing is on while a trace encoder (tinyactors_otlp.h, napi ADR 0016) is
 * installed: the binding creates one with tao_create() and installs it
 * here, and the kernel owns it from then on. Installing another, or NULL,
 * destroys the one before; close destroys the last. <log> records then go
 * to it as well, the kernel has it read core's tel ring
 * (tah_system_drain_telemetry), and a turn ends early once its staging is
 * full (tao_staging_full). The binding sets the kind masks it asks core
 * for with ta_system_set_trace_masks.
 */
bool tah_system_tracing(const tah_system *system);
void tah_system_set_otlp(tah_system *system, tao_encoder *encoder);
/* <log> records go to the binding's log sink as effects (setLogSink); call
 * tah_system_refresh_log_sink() after. */
void tah_system_set_log_to_js(tah_system *system, bool log_to_js);

/* Installs the binding's callbacks (tah_system_callbacks), replacing any
 * before; NULL removes them. */
void tah_system_set_callbacks(tah_system *system, const tah_system_callbacks *callbacks);
/*
 * Installs the binding's pump driver; NULL detaches it: the system asks for
 * no turn or timer after, and a timer it armed counts as gone. A binding
 * detaches before it closes the system from where its callbacks cannot run.
 */
void tah_system_set_driver(tah_system *system, const tah_pump_driver *driver);
/* Whether a turn was asked for and has not run yet. */
bool tah_system_turn_pending(const tah_system *system);
/* The clock tah_system_set_clock() chose (realtime unless it was called). */
tah_clock_kind tah_system_clock_kind(const tah_system *system);
size_t tah_system_leaked_bytes(const tah_system *system);
void tah_system_free(tah_system *system);

/*
 * Accepts a message, admits it into core's pool, and asks for a turn (ADR
 * 0005, core ADR 0099). A message posted by address goes to the actor
 * resident there, as one posted to its ID; with none, core keeps it until
 * the address's load is answered, and a failed load makes it a dead letter
 * with the load's reason. On TA_OK the system owns the event's references
 * and the message's waiter. TA_ERR_FULL: core's pool is at its admission
 * watermark and the message may not be held (tah_message.may_overflow);
 * TA_ERR_NOMEM: nothing accepted.
 */
ta_result tah_system_post(tah_system *system, const tah_message *message);

/*
 * Queues an effect (effects.h), which hands over its references, and asks
 * for a turn. False when it was dropped (closing, or no memory).
 */
bool tah_system_effect(tah_system *system, const tah_effect *effect);

/*
 * Reads every record core wrote to its tel ring (core ADR 0098) into the
 * trace encoder, releasing each, and asks for a turn when a span ended. The
 * kernel calls it whenever core returns to it (tah_system_leave_core, the
 * turn, the calls it makes itself), and before it writes a <log> record, so
 * records and logs keep core's order; a binding calls it before it takes
 * the encoder's requests and before it forgets a definition. Does nothing
 * unless the encoder is on: the ring then stays for whoever else reads it.
 */
void tah_system_drain_telemetry(tah_system *system);
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
 * Cancels the send() waiter at `index` (ADR 0005 point 11) with
 * tah_sched_cancel(): a message core has not taken is withdrawn, also one
 * waiting in core for its address's load (napi ADR 0020). True when the
 * waiter was cancelled; the caller settles and gives it back.
 */
bool tah_system_cancel(tah_system *system, uint32_t index);
/*
 * Finishes a load JavaScript ran (loads.h, tah_loads_finish()), and answers
 * the load request core owes for it, when it has one: the mail
 * core kept for the address goes to `actor` when the load succeeded, and
 * otherwise becomes error.communication for each sender. TA_ERR_STALE: the
 * token names no load in flight; TA_ERR_NOT_FOUND: `actor` is not the
 * resident actor the load needs; TA_ERR_NOMEM as tah_loads_finish().
 */
typedef struct {
    uint32_t index;
    uint32_t generation;
    tah_load_outcome outcome;
    ta_actor_id actor;
} tah_system_load_finish;
ta_result tah_system_finish_load(tah_system *system, const tah_system_load_finish *finish);
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

/* An operator withdraws `actor`'s pending delayed send (core ADR 0084); its address's wake is reported again. */
ta_result tah_system_withdraw_send(tah_system *system, ta_actor_id actor, ta_send_id send);
/* An operator abandons `actor`'s macrostep (core ADR 0082), then tah_hook_actor_abandoned(). */
ta_result tah_system_abandon(tah_system *system, ta_actor_id actor);
