/**
 * @file run.h
 *
 * # The run: admission, the run queue and one macrostep slot
 *
 * `ta_run_state` is what a system needs to take inputs and run them: its
 * pool of pending inputs (`envelope.h`), the name area that holds their
 * text (`name_area.h`), and its one macrostep slot (ADR 0099, ADR 0103).
 * Everything that gives a session something to run admits it here as an
 * envelope on the run queue, and `ta_run()` takes the run queue's
 * envelopes in arrival order and runs each one's macrostep to its end.
 *
 * ## Inputs
 *
 * An input is one envelope, from its admission until the macrostep it
 * started ends. Its `source` says what it is:
 *
 * - **An external event** (`TA_RECORD_INPUT_SOURCE_H2C`), admitted by
 *   `ta_actor_deliver()`. The envelope holds the event's data in `data`,
 *   its type, send ID and invocation in `type`, `send_id` and
 *   `invoke_instance`, the host's correlation in `source_seq`, and one
 *   binary field of text: the event name's ID, the origin (whose
 *   references the envelope holds), the input's time, the trace parent
 *   when there is one, and an operator transition (owned) with the
 *   definition its states belong to.
 * - **An invocation's error report** (`TA_RECORD_INPUT_SOURCE_INVOKE_REPORT`),
 *   admitted by `ta_actor_report_invoke()`: the invocation in
 *   `invoke_instance`, the report's data, and a field with the event name
 *   `error.communication.invoke.<id>.<reason>` (interned when admitted),
 *   whether it ends the invocation, the time and the trace parent.
 * - **A wake** (`TA_RECORD_INPUT_SOURCE_PLATFORM`): core's own input for a
 *   session with a macrostep to run that no admitted input started, its
 *   bootstrap after a spawn, a prepare, a load or a reconstruction, or work
 *   queued outside any run (a delayed send's failure, invocations retired
 *   at a load or a publish). It has no text and correlation 0. A session
 *   may have more than one; the ones its macrostep does not need are
 *   dropped when they are reached. The bootstrap of an invoked child,
 *   which a macrostep spawns, takes the arrival number of that
 *   macrostep's input, so the child starts before the mail sent during
 *   that macrostep is taken, as it would had the invocation started at
 *   once.
 *
 * ## Taking an input
 *
 * `ta_run()` takes the head of the run queue, or, with a session filter,
 * the first envelope for that session (the debugging exception). Then:
 *
 * - an envelope whose session is gone is refused (`TA_ERR_NOT_FOUND`), and
 *   one whose session finished is refused (`TA_ERR_DONE`); a wake is
 *   dropped instead, unreported;
 * - an event or report for a session in a macrostep (waiting to begin,
 *   paused, or stopped by a fault) moves to that session's **deferred
 *   list**, in arrival order, and the walk goes on;
 * - otherwise it is **bound**: an event moves into the session's event
 *   block as its external event, waiting (ADR 0069), a report queues its
 *   error event, a wake takes the session's pending macrostep; the
 *   envelope becomes `TA_ENVELOPE_TAKEN`, its text and data go, and the
 *   session's event block names it as `input`.
 *
 * Its macrostep then runs with what is left of the call's budgets. When
 * it ends, the input's outcome is reported, the envelope is given back,
 * and the session's first deferred envelope returns to the run queue at
 * its arrival position (all of them when the session finished, so that
 * each is refused). When a budget runs out inside it, the input stays in
 * the slot, and the next call continues it before it takes anything else.
 * When a fault stops it, the input stays with its session, which holds no
 * slot: it is **stopped** until `ta_actor_release()` queues the input
 * again, or it is abandoned or destroyed. A FAULTED outcome always means
 * that: a fault that comes as the macrostep ends leaves nothing to
 * continue, and is reported as the input's end (COMPLETED or FINISHED,
 * with the fault's code).
 *
 * With one slot, nothing is deferred in the ordinary run: an input is taken
 * only when no other macrostep runs, and each one ends before the next is
 * taken. Deferral serves a session whose macrostep waits for a wake behind
 * the input, one stopped by a fault, and one a filtered call paused.
 *
 * ## Invariants
 *
 * - A session has at most one taken input, the one its event block
 *   names; its envelope is in the slot, on no list while stopped or
 *   running, or on the run queue when released or paused by a filtered
 *   call.
 * - Every envelope on a deferred list arrived before every envelope for
 *   the same session still on the run queue, and a deferred list is in
 *   arrival order, so one sender's inputs to one session run in the order
 *   they were admitted.
 * - A session in a macrostep has a taken input or a wake on the run queue,
 *   so an envelope deferred for it always returns.
 *
 * ## Memory
 *
 * The system owns the state: the pool and the name area allocate pages
 * from the system's allocator, on demand, up to `poolEnvelopes` and
 * `nameAreaBytes`, and keep them until the system goes. An envelope holds
 * references (its data, an event's origin) and an operator transition,
 * which the run releases when the input is bound, refused, withdrawn or
 * discarded at teardown.
 *
 * - **Refills happen at safe points only:** at admission and at a wake's
 *   reservation outside any run, and in `ta_run()` before each input it
 *   takes or continues, never after the last one it runs. Inside
 *   a macrostep (a router admitting mail, an invoker spawning a child) an
 *   admission or a wake takes only envelopes and chunks that are free
 *   already, and answers `TA_ERR_FULL` when there are none.
 * - Binding an input may allocate the session's event block, or room in
 *   its internal queue for a report's event, between inputs.
 * - `ta_run_session_gone`, `ta_run_input_dropped`, `ta_run_wake_place`,
 *   `ta_run_wake_cancel` and `ta_actor_release()` never allocate.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | admission, a wake | O(1), plus a page when a refill adds one |
 * | taking an input | O(1); a filtered call walks the run queue to its session's first envelope |
 * | deferring, returning | O(1) at the tail; a walk to its place otherwise |
 * | `ta_system_withdraw_input()` | walks the pool, bounded by `poolEnvelopes` |
 * | `ta_run_session_gone` | O(its deferred envelopes), each returned in place |
 * | `ta_run_has_mail` | walks the run queue |
 *
 * ## Failure
 *
 * - Admission answers `TA_ERR_FULL` at the admission watermark of the pool
 *   or the name area, or when nothing is free inside a macrostep, and
 *   changes nothing. It answers `TA_ERR_NOMEM` instead when nothing was
 *   free only because the refill before it could not allocate (ADR 0102):
 *   the limits would have let the input in.
 * - A wake's reservation answers `TA_ERR_FULL` and `TA_ERR_NOMEM` the same
 *   way. A refill that fails leaves the envelopes that are free, and an
 *   admission or a wake that finds one free succeeds.
 * - `ta_run()` answers `TA_ERR_NOMEM` when it cannot bind the next input
 *   (its event block, or a report's queue room): that input goes back to
 *   the head of the run queue.
 *
 * ## Users
 *
 * The public `ta_run()`, `ta_actor_release()` and
 * `ta_system_withdraw_input()` live in this unit. `ta_actor_deliver()` and
 * `ta_actor_report_invoke()` admit through it; spawn, prepare, load,
 * reconstruction, publishing and a delayed send's failure reserve and
 * place wakes; destroying, unloading and abandoning tell it a session's
 * input or the session is gone. ADRs 0069, 0082, 0089, 0099, 0103.
 */
#pragma once

#include "../include/tinyactors.h"
#include "envelope.h"
#include "name_area.h"

struct ta_actor;
struct ta_operator_jump;

/** Free envelopes, and free name-area chunks, a refill keeps: one page each. */
#define TA_RUN_RESERVE 64u

/** A system's pool, name area and slot; see the file comment. */
typedef struct {
    /** The pending inputs and the run queue. */
    ta_envelope_pool pool;
    /** The inputs' text. */
    ta_name_area names;
    /** The taken input whose macrostep a budget paused in the slot, or 0. */
    uint32_t slot;
    /** The input whose macrostep runs inside `ta_run()`, or 0. */
    uint32_t current;
    /** A macrostep runs inside `ta_run()`: no refill may allocate. */
    bool running;
} ta_run_state;

/**
 * Sets up the system's empty pool and name area from its parameters.
 * Allocates nothing. Answers what their init answers for parameters the
 * system's bounds already checked, so `TA_OK`.
 */
ta_result ta_run_init(ta_system *system);

/**
 * Releases what every envelope still holds (data, origins, operator
 * transitions, text) and frees the pool and the name area. Runs at
 * teardown, after every actor is gone; only frees.
 */
void ta_run_destroy(ta_system *system);

/** An external event to admit; see `ta_run_admit_event`. */
typedef struct {
    /** The resident session it is for. */
    struct ta_actor *actor;
    /** The event, borrowed; the envelope retains its data and origin. */
    const ta_event *event;
    /** What its macrostep runs under, copied; all zero for none. */
    const ta_trace_parent *trace;
    /** An operator transition, or null; taken over on `TA_OK`. */
    struct ta_operator_jump *jump;
    /** The input's time, checked by the caller. */
    ta_time time;
    /** The host's name for the input. */
    uint64_t correlation;
} ta_run_event;

/**
 * Admits an external event for `input->actor` onto the run queue. Refills
 * first when no macrostep runs. Answers `TA_OK`; `TA_ERR_FULL` at the
 * admission watermark or with nothing free; or `TA_ERR_NOMEM` when nothing
 * was free because the refill failed. A failure changes nothing and leaves
 * the operator transition the caller's. May run inside a macrostep, where
 * it never allocates.
 */
ta_result ta_run_admit_event(ta_system *system, const ta_run_event *input);

/** An invocation's error report to admit; see `ta_run_admit_report`. */
typedef struct {
    struct ta_actor *actor;
    /** The invocation it reports on. */
    ta_invoke_id invoke;
    /** error.communication.invoke.<id>.<reason>, interned by the caller. */
    ta_event_name_id name;
    /** The event's data, borrowed and retained, or null. */
    const ta_value *data;
    /** It retires the invocation. */
    bool ends;
    const ta_trace_parent *trace;
    ta_time time;
    uint64_t correlation;
} ta_run_report;

/**
 * Admits an error report as `ta_run_admit_event` admits an event, with the
 * same answers. Never allocates inside a macrostep.
 */
ta_result ta_run_admit_report(ta_system *system, const ta_run_report *report);

/**
 * Reserves an envelope for a wake, before the caller gives a session a
 * macrostep to run, so placing it cannot fail. `admission` holds it to the
 * admission watermark: a spawn's or a load's bootstrap is admitted; core's
 * own wakes are not. Refills first when no macrostep runs. Answers
 * `TA_OK` with the envelope in `*out`, `TA_ERR_FULL`, or `TA_ERR_NOMEM`
 * when nothing was free because the refill failed.
 */
ta_result ta_run_wake_reserve(ta_system *system, bool admission, uint32_t *out);

/**
 * Queues the reserved wake for `actor` when it has a macrostep to run, and
 * gives it back otherwise. Outside a run it joins the tail of the run
 * queue. Inside one, where a macrostep spawns an invoked child, it counts
 * as having arrived with the input of that macrostep: the child's bootstrap
 * runs before the mail that arrived during it, and the children of one
 * macrostep start in the order it spawned them. Never allocates; cannot
 * fail.
 */
void ta_run_wake_place(ta_system *system, struct ta_actor *actor, uint32_t wake);

/** Gives back a reserved wake the caller no longer needs. Never allocates. */
void ta_run_wake_cancel(ta_system *system, uint32_t wake);

/**
 * Whether inputs admitted for `actor` wait to be taken: on the run queue or
 * on its deferred list. Its own taken input, queued again, and wakes do not
 * count. Walks the run queue, bounded by `poolEnvelopes`. Never allocates.
 */
bool ta_run_has_mail(const ta_system *system, const struct ta_actor *actor);

/**
 * `actor` is going (destroyed, unloaded, or a construction undone): its
 * taken input is given back, wherever it is, and its deferred envelopes
 * return to the run queue, where they are refused. Never allocates; cannot
 * fail.
 */
void ta_run_session_gone(ta_system *system, struct ta_actor *actor);

/**
 * `actor`'s macrostep ended outside `ta_run()` (an operator abandoned it):
 * `input`, the input it had taken (read before the macrostep ended, which
 * may give back the event block that named it), or 0, is given back
 * unreported, and the session's first deferred envelope returns to the run
 * queue. Never allocates; cannot fail.
 */
void ta_run_input_dropped(ta_system *system, struct ta_actor *actor, uint32_t input);
