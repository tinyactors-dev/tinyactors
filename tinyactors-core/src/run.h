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
 * An input is one envelope, from its admission until it completes, when
 * the macrostep it started ends. Its `source` says what it is:
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
 *   at a load or a publish). It has correlation 0, and text only when its
 *   macrostep runs under a trace parent, or when a reconstruction restores
 *   a macrostep that holds an external event: then the field holds them as
 *   an event's does, and the envelope holds the event's data. A session
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
 * - otherwise it is **bound**: an event becomes the session's external
 *   event, waiting (ADR 0069), a report queues its error event, and the
 *   wake the session names takes its pending macrostep; the envelope
 *   becomes `TA_ENVELOPE_TAKEN`, and the session names it as its taken
 *   `input`.
 *
 * ## The taken envelope stays put
 *
 * A taken input keeps its text and its references until it completes:
 * nothing is copied out of it. While its macrostep runs, `_event`,
 * inspection and trace records read the external event from the envelope
 * (`ta_run_input_event`), along with its operator transition
 * (`ta_run_input_jump`), the trace parent the macrostep runs under
 * (`ta_run_session_trace`), and the macrostep's logical time, which is
 * written into the envelope's `due` when the input is taken
 * (`ta_run_macrostep_time`). The session holds only the index. Reading the
 * event decodes it into one `ta_event` the run state keeps, for the input
 * read last; a pointer to it is valid until another input's event is read
 * or the input completes, never across a refill, which happens only
 * between inputs.
 *
 * Its macrostep then runs with what is left of the call's budgets. When
 * it ends, the input's outcome is reported, the envelope is released and
 * given back,
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
 * - A session has at most one taken input, the one it names as `input`;
 *   its envelope is in the slot, on no list while stopped or running, or
 *   on the run queue when released or paused by a filtered call. Before
 *   its macrostep begins, a session in a macrostep names the first wake
 *   placed for it as its `input`, marked waiting, so what the macrostep
 *   runs under is found without a walk; that wake is the one taken, and
 *   any other wake for the session is dropped when it is reached. When
 *   the session goes or its macrostep is abandoned before that wake is
 *   taken, what the wake holds (a restored event's data, origin and
 *   operator transition, its text) is released at once, in O(1), and the
 *   bare wake stays queued until it is reached and dropped.
 * - A taken envelope holds its text and references until its input
 *   completes, is abandoned, or its session goes; then they are released
 *   and the envelope is given back.
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
 * which the run releases when the input completes, is refused, withdrawn,
 * abandoned, or discarded at teardown.
 *
 * - **Refills happen at safe points only:** at admission and at a wake's
 *   reservation outside any run, and in `ta_run()` before each input it
 *   takes or continues, never after the last one it runs. Inside
 *   a macrostep (a router admitting mail, an invoker spawning a child) an
 *   admission or a wake takes only envelopes and chunks that are free
 *   already, and answers `TA_ERR_FULL` when there are none.
 * - Binding an input may allocate, between inputs only: room in the
 *   session's internal queue for a report's event, or, for an invoked
 *   session, the queue storage that keeps its last event. Taking an input
 *   for any other session allocates nothing once the pool is warm.
 * - `ta_run_session_gone`, `ta_run_input_dropped`, `ta_run_wake_place`,
 *   `ta_run_wake_cancel`, the readers of a taken input and
 *   `ta_actor_release()` never allocate.
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
 * | `ta_run_input_event`, `ta_run_input_jump`, `ta_run_session_trace`, `ta_run_macrostep_time` | O(1): one field of text |
 *
 * ## Failure
 *
 * - Admission answers `TA_ERR_FULL` at the admission watermark of the pool
 *   or the name area, or when nothing is free inside a macrostep, and
 *   changes nothing. It answers `TA_ERR_NOMEM` instead when nothing was
 *   free only because the refill before it could not allocate (ADR 0102):
 *   the limits would have let the input in.
 * - A wake's reservation answers `TA_ERR_FULL` and `TA_ERR_NOMEM` the same
 *   way, for its envelope and, when it carries a trace parent or an event,
 *   its text. A refill that fails leaves the envelopes that are free, and
 *   an admission or a wake that finds one free succeeds.
 * - `ta_run()` answers `TA_ERR_NOMEM` when it cannot bind the next input
 *   (a report's queue room, or an invoked session's queue storage): that
 *   input goes back to the head of the run queue.
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
    /** The input whose external event `event` holds, or 0. */
    uint32_t event_input;
    /** The external event of `event_input`, decoded from its envelope; its
     * references are the envelope's, borrowed. */
    ta_event event;
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

/** A wake to reserve; see `ta_run_wake_reserve`. */
typedef struct {
    /** Hold it to the admission watermark: a spawn's or a load's bootstrap
     * is admitted; core's own wakes are not. */
    bool admission;
    /** What the macrostep it starts runs under, copied; null, or no trace
     * ID, for none. */
    const ta_trace_parent *trace;
    /** The external event of a macrostep a reconstruction restores, or
     * null. Its references are taken over on `TA_OK`. */
    const ta_event *event;
    /** The operator transition waiting with `event`, or null; taken over on
     * `TA_OK`. */
    struct ta_operator_jump *jump;
} ta_run_wake;

/**
 * Reserves an envelope for a wake, before the caller gives a session a
 * macrostep to run, so placing it cannot fail, and writes its text when it
 * carries a trace parent or an event. Refills first when no macrostep
 * runs. Answers `TA_OK` with the envelope in `*out`, `TA_ERR_FULL`, or
 * `TA_ERR_NOMEM` when nothing was free because the refill failed. A
 * failure changes nothing and leaves the event and the operator transition
 * the caller's.
 */
ta_result ta_run_wake_reserve(ta_system *system, const ta_run_wake *wake, uint32_t *out);

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

/** Gives back a reserved wake the caller no longer needs, with what its
 * text holds. Never allocates. */
void ta_run_wake_cancel(ta_system *system, uint32_t wake);

/**
 * The external event of taken input `input`, decoded from its envelope
 * into the run state, or null for 0. Its references are the envelope's,
 * borrowed: the pointer is valid until another input's event is read or
 * `input` completes. O(1); never allocates; may run inside a macrostep.
 */
const ta_event *ta_run_input_event(ta_system *system, uint32_t input);

/**
 * The external event of taken input `input`, by value, its references
 * borrowed from the envelope as `ta_run_input_event` says; all zero with
 * no origin for 0. O(1); never allocates.
 */
ta_event ta_run_input_event_of(const ta_system *system, uint32_t input);

/**
 * The operator transition that came with taken input `input`, owned by its
 * envelope until it completes, or null. O(1); never allocates.
 */
const struct ta_operator_jump *ta_run_input_jump(const ta_system *system, uint32_t input);

/**
 * The trace parent `actor`'s macrostep runs under, all zero for none: what
 * its input holds, taken, or the wake queued to start it. O(1); never
 * allocates.
 */
ta_trace_parent ta_run_session_trace(const ta_system *system, const struct ta_actor *actor);

/**
 * The logical time of `actor`'s macrostep (ADR 0089 section 9): the time
 * written into its taken input when it was taken, so it does not move while
 * the macrostep spans several `ta_run()` calls; the actor's time when no
 * macrostep has taken an input. O(1); never allocates.
 */
ta_time ta_run_macrostep_time(const ta_system *system, const struct ta_actor *actor);

/**
 * Whether inputs admitted for `actor` wait to be taken: on the run queue or
 * on its deferred list. Its own taken input, queued again, and wakes do not
 * count. Walks the run queue, bounded by `poolEnvelopes`. Never allocates.
 */
bool ta_run_has_mail(const ta_system *system, const struct ta_actor *actor);

/**
 * `actor` is going (destroyed, unloaded, or a construction undone): its
 * taken input is given back, wherever it is, a wake it waits for is left
 * bare on the run queue, and its deferred envelopes return to the run
 * queue, where they are refused. Never allocates; cannot fail.
 */
void ta_run_session_gone(ta_system *system, struct ta_actor *actor);

/**
 * `actor`'s macrostep ended outside `ta_run()` (an operator abandoned it):
 * its taken input, if any, is released and given back unreported, a wake
 * it waits for is left bare on the run queue, and the session's first
 * deferred envelope returns to the run queue. Never allocates; cannot
 * fail.
 */
void ta_run_input_dropped(ta_system *system, struct ta_actor *actor);
