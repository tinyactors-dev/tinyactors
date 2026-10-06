/**
 * @file invoke.h
 *
 * # Invocations
 *
 * The SCXML `<invoke>` machinery of a resident actor: starting an
 * invocation when its state is entered, cancelling it when the state is
 * exited, autoforwarding and `<finalize>`, the reports an invoker makes
 * about its service (an event, done, an error; ADR 0081), and the
 * `done.invoke.<id>` a child session sends its parent. This header holds
 * the invoker registry's entry, the private bookkeeping kept beside each
 * active invocation, and the calls the actor and session images make; the
 * calls the interpreter makes are declared in `interpreter.h`.
 *
 * ## Contract and invariants
 *
 * - An actor's active invocations live in its sparse state, as the public
 *   `ta_active_invoke` array with a `ta_invoke_meta` at the same index.
 * - The service behind an invocation is runtime-only. A session image
 *   records the logical invocation (ID, type, declaration), never the
 *   service; an invocation whose service did not survive the image is
 *   retired and reported once as
 *   `error.communication.invoke.<id>.unavailable` (ADR 0038), with no
 *   fault data and an envelope `invoke_id` of `TA_INVOKE_ID_NONE`.
 * - An invocation started while its actor replayed (ADR 0089) called no
 *   invoker: nothing is cancelled or forwarded to it.
 * - An ERROR report is an input (ADR 0099): `ta_actor_report_invoke()`
 *   admits it into the pool (`run.h`), and `ta_invoke_take_report` queues
 *   its event when `ta_run()` takes it. EVENT and DONE reports are
 *   admitted as `ta_actor_deliver()` admits an event.
 * - The names core generates for an invocation, `done.invoke.<id>` and
 *   `error.communication.invoke.<id>.<reason>`, take one segment per dotted
 *   part of the ID's text (ADR 0049: a segment never holds a dot), so a
 *   descriptor naming the first part of `s3.7` (`done.invoke.s3`,
 *   `error.communication.invoke.s3`) matches. A name that would be deeper
 *   than the system's `eventNameDepth` is not made: the call that needs it
 *   answers `TA_ERR_INVALID`.
 *
 * ## Memory
 *
 * Invocation records are owned by the actor's sparse state and freed with
 * it. Restoring an invocation may grow those arrays, and reporting a lost
 * one holds its event's name as text (ADR 0100) and may grow the actor's
 * internal queue; the retire pair separates that work
 * (`ta_invoke_prepare_retire`) from the work that cannot fail.
 * The registry entries are owned by the system.
 *
 * ## Failure
 *
 * The calls answer `ta_result`, `TA_ERR_NOMEM` when memory runs out; a
 * failed call changes nothing visible: the actor's invocations and queue
 * are as they were.
 *
 * ## Users
 *
 * The interpreter, the resident actor (publishing a replaying actor,
 * restoring one from a checkpoint, event origins), session images, and the
 * system's invoker registry. The SCXML error events it raises are described
 * in `docs/internal/errors.md`.
 */
#pragma once

#include "datamodel.h"
#include "event.h"
#include "../include/tinyactors.h"

typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;

/*
 * Private bookkeeping for one active invocation, kept parallel to the public
 * ta_active_invoke array of the actor's sparse state. `child` is set when the
 * service spawned an SCXML session with this invocation as its parent.
 */
typedef struct {
    ta_invoke_decl_id decl;
    ta_state_id state;
    ta_invoker invoker;
    ta_actor_id child;
    bool has_child;
    /* Started while its actor replayed (ADR 0089): no invoker was called, so
     * none is cancelled or forwarded to, and nothing runs behind it. */
    bool replayed;
    /* Between ta_invoke_prepare_retire and ta_invoke_retire_all: the name
     * of its `unavailable` error, held as text (ADR 0100). */
    ta_event_name_id retire_name;
} ta_invoke_meta;

/* The send ID of a child's queued done.invoke completion record. */
#define TA_SEND_ID_COMPLETION (UINT64_MAX - 1u)

/*
 * Retires an invocation whose service did not survive a session image, and
 * queues its error.communication.invoke.<id>.unavailable event.
 */
ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke);
/*
 * A replaying actor restored from a checkpoint (ADR 0091) takes back its
 * active invocations as replayed ones: active in core, with no invoker
 * called, so the services' later events still match them.
 */
typedef struct {
    ta_invoke_id id;
    ta_atom type;
    ta_invoke_decl_id decl;
} ta_invoke_restore_params;
ta_result ta_invoke_restore(ta_system *system, ta_actor *actor, const ta_invoke_restore_params *params);
/* Whether `invoke` is one of the actor's active invocations. */
bool ta_invoke_is_active(const ta_actor *actor, ta_invoke_id invoke);
/*
 * Publishing a replaying actor retires its active invocations (ADR 0089
 * section 6). The first makes room for their unavailable events, and
 * changes nothing visible when it fails; the second then queues them and
 * forgets the invocations, and cannot fail. It answers whether it queued one.
 */
ta_result ta_invoke_prepare_retire(ta_system *system, ta_actor *actor);
bool ta_invoke_retire_all(ta_system *system, ta_actor *actor);
/* Undoes ta_invoke_prepare_retire when what follows it fails: the names it
 * held go back. */
void ta_invoke_cancel_retire(ta_system *system, ta_actor *actor);

/* An admitted ERROR report as ta_run() takes it (run.h). */
typedef struct {
    /* The invocation it reports on. */
    ta_invoke_id invoke;
    /* error.communication.invoke.<id>.<reason>: a handle to the text the
     * report's envelope keeps (ADR 0100). */
    ta_event_name_id name;
    /* The report's data: moved into the event on TA_OK, kept otherwise. */
    ta_value *data;
    /* It retires the invocation. */
    bool ends;
    /* The report's trace parent, the cause the queued event is recorded
     * with; all zero for none. The macrostep reads it from the report's
     * envelope, which keeps it until the report completes. */
    const ta_trace_parent *trace;
} ta_invoke_report_take;
/*
 * Takes an admitted ERROR report for `actor`, between macrosteps (ADR 0081):
 * its event joins the internal queue, the invocation retires when the
 * report ends it, and a macrostep starts under its trace parent. May grow
 * the internal queue; never runs inside a macrostep. TA_ERR_NOT_FOUND when
 * the invocation ended since; TA_ERR_NOMEM when there is no room. A
 * failure changes nothing.
 */
ta_result ta_invoke_take_report(ta_system *system, ta_actor *actor, const ta_invoke_report_take *take);

/* The origin of an event from `invoke`: its ID and its invoker's registry index (ADR 0068). */
ta_origin ta_invoke_origin_of(const ta_system *system, const ta_active_invoke *invoke);
/* The origin of an event from the actor's active invocation `invoke`; false when there is none. */
bool ta_invoke_origin_for(const ta_system *system, const ta_actor *actor, ta_invoke_id invoke, ta_origin *out);
