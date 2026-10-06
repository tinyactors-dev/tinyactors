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
 * Mail one session sends another is an envelope too, which waits on the
 * sending macrostep's outgoing list until that macrostep ends, and, when
 * its target is not resident, on a waiting entry (`waiting.h`) until the
 * host answers a load request.
 *
 * ## Inputs
 *
 * An input is one envelope, from its admission until it completes, when
 * the macrostep it started ends. Its text is a binary **record** (the
 * event name's ID, the origin, whose references the envelope holds, the
 * input's time, the family of its trace parent's cause, and an operator
 * transition, owned, with the definition its states belong to); then, for
 * an event whose name core did not intern (ADR 0100: a host's event or
 * report, mail, a `<send>` to #_internal, an invocation's done or error
 * event), the **name** as text, with the record's name ID 0; and, when its macrostep runs under a trace ID, the **trace parent**
 * behind them as the text's last field (`name_area.h`), with
 * `TA_ENVELOPE_FLAG_TRACED` set. The field leaves the cause's family out;
 * the record keeps it, so the step key reads back whole. Its `source`
 * says what it is:
 *
 * - **An external event** (`TA_RECORD_INPUT_SOURCE_H2C`), admitted by
 *   `ta_actor_deliver()`. The envelope holds the event's data in `data`,
 *   its type, send ID and invocation in `type`, `send_id` and
 *   `invoke_instance`, the host's correlation in `source_seq`, and the
 *   text above.
 * - **Mail** (`TA_RECORD_INPUT_SOURCE_MAIL`): an event a session sent
 *   another, a `<send>`, a child's event to its parent or its
 *   `done.invoke`, an event autoforwarded to a child, or a send held for
 *   an unpublished session and released at its publish; and **a delayed
 *   send that fell due** (`TA_RECORD_INPUT_SOURCE_TIMER`). It holds what
 *   an external event holds, the sending session's ID in `from`, and
 *   correlation 0; see "Mail between resident sessions".
 * - **An invocation's error report** (`TA_RECORD_INPUT_SOURCE_INVOKE_REPORT`),
 *   admitted by `ta_actor_report_invoke()`: the invocation in
 *   `invoke_instance`, the report's data, and a record with the event name
 *   `error.communication.invoke.<id>.<reason>` (kept as text),
 *   whether it ends the invocation and the time, and the trace parent.
 * - **A wake** (`TA_RECORD_INPUT_SOURCE_PLATFORM`): core's own input for a
 *   session with a macrostep to run that no admitted input started, its
 *   bootstrap after a spawn, a prepare, a load or a reconstruction, or work
 *   queued outside any run (a delayed send's failure, invocations retired
 *   at a load or a publish). It has correlation 0, and text only when its
 *   macrostep runs under a trace ID, or when a reconstruction restores a
 *   macrostep that holds an external event: then the text holds them as an
 *   event's does, and the envelope holds the event's data. A session
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
 *   dropped instead, unreported; mail is **recycled** instead: it becomes
 *   error.communication for its sender (see "Mail that waits");
 * - an event or report for a session in a macrostep (waiting to begin,
 *   paused, or stopped by a fault) moves to that session's **deferred
 *   list**, in arrival order, and the walk goes on; so does mail for a
 *   session that is not published (prepared, or replaying), which returns
 *   to the run queue when it is published (`ta_run_session_published`),
 *   so a replay sees only its own inputs;
 * - otherwise it is **bound**: an event or mail becomes the session's
 *   external event, waiting (ADR 0069), a report queues its error event,
 *   and the wake the session names takes its pending macrostep; the
 *   envelope becomes `TA_ENVELOPE_TAKEN`, and the session names it as its
 *   taken `input`. With `report_taken`, its TAKEN outcome carries, for
 *   mail and a due send, the event retained for the host, which no host
 *   call admitted and so no host has seen.
 *
 * ## The taken envelope stays put
 *
 * An event whose name is kept as text names it by `TA_RUN_NAME_INPUT` and
 * its input's envelope index. Core reads the name into the run's name
 * buffer, one run of bytes, where it is needed (`ta_run_input_name`): the
 * system's resolver turns it into atoms for the matcher (`names.h`), and
 * `_event.name` and trace records read the text. The
 * buffer grows at a safe point (the refill before an input is taken) to
 * the longest such name any envelope was given; an input whose name does
 * not fit, because that growth failed, is not taken, and `ta_run` answers
 * TA_ERR_NOMEM.
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
 * ## Mail between sessions
 *
 * A `<send>` to another session, and an invocation's event or completion,
 * never leave core (ADR 0099): `ta_actor_mail_route()` finds the actor that
 * holds the target session, or the destination to wait for, and
 * `ta_run_post_mail` takes an envelope, writes the record and the sender's
 * trace parent, when traced, into the name area, and retains the event's
 * data and origin.
 *
 * - **Inside a macrostep** it goes on that macrostep's **outgoing list**
 *   (`outgoing`), in send order, where nothing can take it. When the
 *   macrostep ends (stable, finished, or with a fault that leaves nothing
 *   to continue), the whole list moves, in order, to the end of the run
 *   queue, each envelope with the next arrival number, or, for a target
 *   that is not resident, to its waiting entry. So a receiver never sees mail
 *   before its sender's macrostep ends, and one sender's mail to one
 *   receiver keeps its send order across macrosteps.
 * - **A macrostep that leaves `ta_run()` without ending** (a budget paused
 *   it, or a fault stopped it) **parks** its outgoing list with its taken
 *   input, the input's `owner_next` naming the list's first envelope and
 *   that envelope's `owner_next` its last, and takes it back when it runs
 *   again, in O(1). A macrostep that is abandoned, and a session destroyed
 *   while its macrostep waits, give their parked mail back unseen; a
 *   session with parked mail does not unload (`ta_run_has_unsent_mail`),
 *   since an image cannot carry it.
 * - **Outside a macrostep** (a delayed send that fell due, at
 *   `ta_system_dispatch_due()`; a held send released at a publish; a
 *   completion published again) it joins the run queue, or its waiting
 *   entry, at once.
 *
 * **Capacity.** Inside a macrostep there is no refill: mail takes only an
 * envelope, chunks and, for a target that is not resident, a waiting
 * entry that are free already. With none, the `<send>`
 * fails in the chart as `error.execution.capacity.pool` or `.names`, the
 * rest of its block does not run, so a fanout fails once, at the first
 * send that does not fit, and the sends before it stay on the outgoing
 * list. Outside a macrostep it refills first; a send that still finds no
 * room waits in the deadline index, submitted and due now, and is offered
 * again.
 *
 * ## Mail that waits
 *
 * Mail for a target that is not resident (a named address with no
 * resident actor, or a session with none) names its destination's
 * **waiting entry** (`waiting.h`) in `target`, with
 * `TA_ENVELOPE_FLAG_WAITING_TARGET`, and counts in the entry from the
 * moment it is posted. It joins the entry's list, in state
 * `TA_ENVELOPE_WAITING`, when it arrives: at its sender's macrostep end,
 * or at once outside a macrostep; so a macrostep that does not end asks
 * for no load. The entry is then owed a load request, which the host reads
 * with `ta_system_load_needed()` and answers:
 *
 * - `ta_system_load_settled()`: the waiting envelopes join the run queue,
 *   in the order they waited, for the actor that holds the destination
 *   now; mail still on its way follows them when it arrives;
 * - `ta_system_load_failed()`: each one is **recycled**: released and
 *   given back first, and its sender, when it is still resident, gets
 *   error.communication with the send's ID and the host's fault, so the
 *   error needs no room the pool may not have; mail still on its way fails
 *   the same way when it arrives.
 *
 * An external event the host posts to an address (`ta_actor_deliver()`
 * with no actor) waits the same way when no actor is resident there. When
 * its load fails it is not recycled: it carries `TA_ENVELOPE_FLAG_REFUSED`
 * and the fault's code in `due`, goes back to the run queue with no
 * target, and the next `ta_run()` reports it REFUSED with that code, under
 * its correlation.
 *
 * Mail whose target was resident when it was sent, but is gone or finished
 * when it is taken, is recycled the same way, with core's own fault.
 *
 * ## Withdrawal
 *
 * `ta_system_withdraw_input()` scans the pool for the first input admitted
 * with a correlation and not taken yet (ADR 0099 decision 11): runnable, by
 * arrival, or else waiting for its target's load. It unlinks it from the
 * run queue, a deferred list or a waiting entry's list; a waiting entry it
 * leaves empty stays until the host answers its request. The scan is
 * bounded by `poolEnvelopes` and allocates nothing.
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
 * - **Refills happen at safe points only:** at admission, at a wake's
 *   reservation and at mail outside any run, and in `ta_run()` before each
 *   input it takes or continues, never after the last one it runs. A
 *   refill keeps free envelopes, chunks and waiting entries, and room in
 *   the waiting entries' table. Inside a
 *   macrostep (a `<send>`, an invoker spawning a child) mail, an admission
 *   or a wake takes only envelopes, chunks and entries that are free
 *   already, and answers `TA_ERR_FULL` when there are none.
 * - Binding an input may allocate, between inputs only: room in the
 *   session's internal queue for a report's event, or, for an invoked
 *   session, the queue storage that keeps its last event. Taking an input
 *   for any other session allocates nothing once the pool is warm.
 * - `ta_run_session_gone`, `ta_run_input_dropped`, `ta_run_wake_place`,
 *   `ta_run_wake_cancel`, the readers of a taken input,
 *   `ta_actor_release()`, and the outgoing list's delivery, parking and
 *   giving back never allocate.
 * - Mail's envelope and text are the pool's and the name area's; the
 *   references it holds (data, origin) are released when it completes, is
 *   refused, given back with an abandoned macrostep, or discarded at
 *   teardown.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | admission, a wake, mail | O(1), plus a page when a refill adds one |
 * | a macrostep's end | O(its mail): each gets its arrival number |
 * | parking and taking back an outgoing list | O(1) |
 * | giving back parked mail | O(its mail) |
 * | taking an input | O(1); a filtered call walks the run queue to its session's first envelope |
 * | deferring, returning | O(1) at the tail; a walk to its place otherwise |
 * | `ta_system_withdraw_input()` | walks the pool, bounded by `poolEnvelopes`, and a deferred or waiting list to unlink |
 * | mail that arrives at its waiting entry | O(1) |
 * | `ta_system_load_needed()` | O(the waiting envelopes), to count them |
 * | `ta_system_load_settled()`, `ta_system_load_failed()` | O(the waiting envelopes) |
 * | `ta_run_session_gone` | O(its deferred envelopes), each returned in place |
 * | `ta_run_has_mail` | walks the run queue and the pool |
 * | `ta_run_has_unsent_mail` | O(1) |
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
 * - `ta_run_post_mail` answers `TA_ERR_FULL`, saying whether the pool or
 *   the name area had no room, and `TA_ERR_NOMEM` when only a failed
 *   refill left nothing free; it changes nothing.
 * - Mail whose session is gone or finished by the time it is reached is
 *   recycled into error.communication for its sender.
 * - A load answer for a request that was answered already, or never read,
 *   is `TA_ERR_STALE`, and changes nothing.
 *
 * ## Users
 *
 * The public `ta_run()`, `ta_actor_release()` and
 * `ta_system_withdraw_input()` live in this unit. `ta_actor_deliver()` and
 * `ta_actor_report_invoke()` admit through it; spawn, prepare, load,
 * reconstruction, publishing and a delayed send's failure reserve and
 * place wakes; the interpreter's `<send>`, an invocation's events and
 * completion, `ta_system_dispatch_due()` and a publish's held sends post
 * mail; publishing returns the mail that waited for it; destroying,
 * unloading and abandoning tell it a session's input or the session is
 * gone. `ta_system_load_needed()`, `ta_system_load_settled()` and
 * `ta_system_load_failed()` live here too. ADRs 0069, 0082, 0089, 0099,
 * 0102, 0103.
 */
#pragma once

#include "../include/tinyactors.h"
#include "envelope.h"
#include "generated/ring_records.h"
#include "list.h"
#include "name_area.h"
#include "waiting.h"

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
    /** Mail for targets that are not resident, by destination. */
    ta_waiting waiting;
    /** The taken input whose macrostep a budget paused in the slot, or 0. */
    uint32_t slot;
    /** The input whose macrostep runs inside `ta_run()`, or 0. */
    uint32_t current;
    /** The outgoing list of the macrostep that runs inside `ta_run()`: the
     * mail it sent to resident sessions, in send order, on no queue until
     * the macrostep ends. Empty whenever no macrostep runs. */
    ta_list outgoing;
    /** A macrostep runs inside `ta_run()`: no refill may allocate. */
    bool running;
    /** The input whose external event `event` holds, or 0. */
    uint32_t event_input;
    /** The external event of `event_input`, decoded from its envelope; its
     * references are the envelope's, borrowed. */
    ta_event event;
    /** Where an input's name kept as text is read to, as one run of bytes
     * (`ta_run_input_name`): `name_capacity` bytes, grown at a safe point
     * to `name_longest`, the longest such name any envelope was given. */
    char *name_buffer;
    uint32_t name_capacity;
    uint32_t name_longest;
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
    /** The resident session it is for, or null when it waits for `wait`. */
    struct ta_actor *actor;
    /** With no actor: the address it waits for (posted by address, with no
     * resident actor there). */
    const ta_waiting_key *wait;
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
    /** The event's name as text, which the envelope keeps, its `name` then
     * not read; empty for a name core holds by ID. */
    ta_str name_text;
} ta_run_event;

/**
 * Admits an external event for `input->actor` onto the run queue, or, with
 * no actor, onto the waiting list of `input->wait`'s destination, which is
 * then owed a load request. Refills first when no macrostep runs. Answers
 * `TA_OK`; `TA_ERR_FULL` at the admission watermark, with nothing free, or
 * with no waiting entry free; or `TA_ERR_NOMEM` when nothing was free
 * because the refill failed. A failure changes nothing and leaves the
 * operator transition the caller's. May run inside a macrostep, where it
 * never allocates.
 */
ta_result ta_run_admit_event(ta_system *system, const ta_run_event *input);

/** An invocation's error report to admit; see `ta_run_admit_report`. */
typedef struct {
    struct ta_actor *actor;
    /** The invocation it reports on. */
    ta_invoke_id invoke;
    /** error.communication.invoke.<id>.<reason> as text (ADR 0100),
     * borrowed for the call and copied into the envelope's text. */
    ta_str name_text;
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
    /** `event`'s name as text, written into the wake's text, when the event
     * comes from a session image, which keeps names as text; empty to use
     * `event->name`. */
    ta_str name_text;
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
 * The name ID of an input's event whose name core did not intern (ADR
 * 0100): this bit, or'ed with the input's envelope index. The name is the
 * text the envelope keeps; it lives as long as the input does, or, for an
 * invoked session's kept event, as long as that event is kept
 * (`TA_RUN_NAME_KEPT`), and no trie node has this bit. Core reads it with
 * `ta_run_input_name`; a host reads it with `ta_event_name_format` while
 * the event is valid (`tinyactors.h`), a TAKEN outcome carries it as text,
 * and a session image copies the text. Only a record that holds names by
 * ID, a delayed send, gives it an interned ID (`ta_event_name_store`).
 */
#define TA_RUN_NAME_INPUT TA_EVENT_NAME_TEXT

/**
 * With `TA_RUN_NAME_INPUT`, a kept event's name that core keeps as text in
 * the invoked session's own internal queue (`kept_text`), not in an
 * envelope: the handle is both bits and the actor's slot, below 2^30
 * (`TA_ACTOR_SLOTS_MAX`). It lives as long as the event is kept, holds
 * no pool envelope, and giving it up releases nothing (ADR 0100).
 */
#define TA_RUN_NAME_KEPT ((uint32_t)0x40000000)

/** Whether `name` names an input's name kept as text (`TA_RUN_NAME_INPUT`). */
static inline bool ta_run_name_is_input(ta_event_name_id name) {
    return (name & TA_RUN_NAME_INPUT) != 0u;
}

/**
 * The name input `name & ~TA_RUN_NAME_INPUT` keeps as text, read into the
 * run's name buffer: valid until the next call reads another name, or the
 * buffer grows. A kept event's name (`TA_RUN_NAME_KEPT`) is read in place
 * from the session's queue, valid while the event is kept. Empty when
 * there is none. Never allocates; may run
 * inside a macrostep.
 */
ta_str ta_run_input_name(const ta_system *system, ta_event_name_id name);

/**
 * Holds `name` as text in an envelope of its own (`TA_ENVELOPE_KEPT`), for
 * an event that outlives any input: an internal event whose name core
 * keeps only as text (ADR 0100), such as a `<send>` to #_internal or an
 * invocation's error, or a kept event a session image restores. `*out` is
 * the name's handle (`TA_RUN_NAME_INPUT` and the envelope), which the
 * event owns from here: `ta_run_release_name` gives the envelope back when
 * the event goes. Refills first when no macrostep runs, and grows the
 * run's name buffer to the name, so the macrostep that holds the event can
 * read it: inside a macrostep that growth is the one allocation it may
 * make. `TA_ERR_FULL`, or `TA_ERR_NOMEM` when only a failed refill left
 * too little or the buffer could not grow, changing nothing.
 */
ta_result ta_run_hold_name(ta_system *system, ta_str name, ta_event_name_id *out);

/**
 * `name` as an event that outlives its input holds it: a handle to text an
 * input keeps is copied into an envelope of its own (`ta_run_hold_name`);
 * an interned name is `name` itself. Answers as `ta_run_hold_name`.
 */
ta_result ta_run_hold_copy(ta_system *system, ta_event_name_id name, ta_event_name_id *out);

/** Gives back the envelope that holds `name` as text, when the event that
 * owns it goes: one `ta_run_hold_name` made. Nothing for an interned name
 * or a kept event's name (`TA_RUN_NAME_KEPT`). Never allocates. */
void ta_run_release_name(ta_system *system, ta_event_name_id name);

/** Mail for another session; see `ta_run_post_mail`. */
typedef struct {
    /** The actor that holds the session it is for (`ta_actor_mail_route`),
     * or null when it waits for the host to load `wait`'s destination. */
    struct ta_actor *target;
    /** With no target: the destination it waits for. */
    const ta_waiting_key *wait;
    /** The event as the target takes it, borrowed; the envelope retains its
     * data and its origin. */
    const ta_event *event;
    /** With the event's `name` 0: its name, kept as text (ADR 0100),
     * borrowed for the call and copied into the envelope's text. */
    ta_str name_text;
    /** The sending step's trace parent, copied into the envelope's text;
     * null, or no trace ID, for none. */
    const ta_trace_parent *trace;
    /** The sending session's ID, or 0 when it is gone. */
    uint64_t from;
    /** `TA_RECORD_INPUT_SOURCE_MAIL`, or `TA_RECORD_INPUT_SOURCE_TIMER` for
     * a delayed send that fell due. */
    uint8_t source;
} ta_run_mail;

/**
 * Puts mail for a resident session into an envelope of the pool. Inside a
 * macrostep it goes on that macrostep's outgoing list, which joins the run
 * queue only when the macrostep ends; outside one (a delayed send that fell
 * due, a held send released at a publish) it joins the run queue at once.
 * Outside a macrostep it refills first, a safe point; inside one it takes
 * only an envelope and chunks that are free already, and never allocates.
 * O(1) in the envelopes; the text costs one chunk per 60 bytes. Answers
 * `TA_OK`; or `TA_ERR_FULL` with `*short_of` saying what had no room, the
 * pool (`TA_RECORD_CAPACITY_RESOURCE_POOL`) or the name area
 * (`TA_RECORD_CAPACITY_RESOURCE_NAMES`); or `TA_ERR_NOMEM` the same way
 * when only a failed refill left nothing free. A failure changes nothing.
 */
ta_result ta_run_post_mail(ta_system *system, const ta_run_mail *mail, ta_record_capacity_resource *short_of);

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
 * Whether inputs admitted for `actor` wait to be taken: on the run queue,
 * on its deferred list, or sent to its runtime ID by a paused or stopped
 * macrostep and parked with that macrostep's input. Its own taken input,
 * queued again, and wakes do not count. Walks the run queue and the pool,
 * bounded by `poolEnvelopes`. Never allocates.
 */
bool ta_run_has_mail(const ta_system *system, const struct ta_actor *actor);

/**
 * Whether `actor`'s macrostep, waiting outside `ta_run()` (paused or
 * stopped), holds mail it sent, parked with its taken input until the
 * macrostep ends. An image does not carry it. O(1); never allocates.
 */
bool ta_run_has_unsent_mail(const ta_system *system, const struct ta_actor *actor);

/**
 * `actor` was published: the mail that waited for it on its deferred list
 * (run.h, "Taking an input") returns to the run queue, each envelope at its
 * arrival position. O(its deferred envelopes). Never allocates; cannot fail.
 */
void ta_run_session_published(ta_system *system, struct ta_actor *actor);

/**
 * `actor` is going (destroyed, unloaded, or a construction undone): its
 * taken input is given back, wherever it is, with the mail its macrostep
 * parked there, unseen, a wake it waits for is left bare on the run queue,
 * and its deferred envelopes return to the run queue, where they are
 * refused. O(its deferred envelopes and parked mail). Never allocates;
 * cannot fail.
 */
void ta_run_session_gone(ta_system *system, struct ta_actor *actor);

/**
 * `actor`'s macrostep ended outside `ta_run()` (an operator abandoned it):
 * its taken input, if any, is released and given back unreported, with
 * the mail the macrostep parked there, which nobody sees, a wake it waits
 * for is left bare on the run queue, and the session's first deferred
 * envelope returns to the run queue. O(its parked mail). Never allocates;
 * cannot fail.
 */
void ta_run_input_dropped(ta_system *system, struct ta_actor *actor);
