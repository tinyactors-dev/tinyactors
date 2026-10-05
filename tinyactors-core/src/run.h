#pragma once

#include "types.h"
#include "envelope.h"
#include "ilist.h"
#include "pool.h"

typedef struct ta_actor ta_actor;

/*
 * Package run is how a system runs its inputs (ADR 0099 sections 4, 5, 7 and
 * 8, ADR 0103 for what it leaves to later): admission of inputs into the
 * envelope pool, the one running slot, and ta_run, which takes inputs in
 * arrival order and runs each to its end. The pool, the run queue and the
 * quotas are package envelope; this package is the policy over them.
 *
 * What it is for. A host used to ask a session, one event at a time, whether
 * it was free (TA_ERR_BUSY), and keep a queue per session for the answer
 * "no". Here core keeps the queue: ta_actor_deliver admits an event and never
 * runs anything, ta_run runs what was admitted. A session inside a macrostep
 * does not refuse mail; its mail waits. With one slot, the order is the order
 * of admission, which keeps every sender-to-receiver pair in order.
 *
 * What runs, and where it waits.
 *
 *   - An *input* is an admitted event: an envelope on the run queue, from
 *     ta_actor_deliver (the host's, or another session's mail through the
 *     router) or from an event report of ta_actor_report_invoke. It is
 *     named by a token ta_run reports it by. An operator's event with a
 *     transition (ta_actor_transition) is an input that skips the queue and
 *     fills the slot.
 *   - A *started macrostep* is a session that began a macrostep with no event
 *     to take: a bootstrap (ta_actor_spawn, ta_actor_prepare), a restored
 *     macrostep (ta_actor_load), an error raised for an idle session. It is
 *     not an envelope. A population of a million sessions spawned before the
 *     first ta_run would otherwise need a million envelopes, which is the
 *     pool's whole bound; the session's id waits in a cell of a small index
 *     pool instead (16 bytes each, in pages of TA_RUN_CELL_PAGE, so no block
 *     grows with the population; the pool is trimmed when it drains beyond
 *     TA_RUN_STARTED_KEEP cells). Started macrosteps run before envelopes:
 *     work already begun drains first, and a session's bootstrap always
 *     precedes the events for it, which can only be admitted after its spawn.
 *
 * Lifetimes.
 *
 *   - system: the ta_runner control block (embedded in the system), its
 *     started cells, the token counter. The cell pool grows only through
 *     ta_run_reserve_start, a safe point, and is never needed to grow
 *     inside a macrostep.
 *   - input: the slot, from take until the input's COMPLETED notice. An
 *     input that a budget pauses keeps the slot, so nothing else is taken
 *     before it ends. The slot's envelope lives as long.
 *   - call: the notice array the caller gives ta_run, and the events its
 *     notices borrow. An envelope whose event a notice lent goes on `lent`
 *     and is released at the start of the next ta_run (or at destroy).
 *
 * When it allocates. ta_actor_deliver and ta_actor_transition allocate only
 * through the envelope pool's refill, at their own call (the admission safe
 * point), and ta_actor_transition also allocates its jump. ta_run allocates
 * only before an input is taken, in the event block its macrostep needs, and
 * as a macrostep always has (the interpreter, ADR 0070); a failure
 * (TA_ERR_NOMEM) leaves the input where it stands. ta_run_reserve_start
 * allocates; ta_run_start, ta_run_actor_gone and ta_run_abandoned never do.
 *
 * Invariants.
 *   I1: at most one input is in the slot; `occupied` is false exactly when
 *       no session is mid-macrostep because of ta_run.
 *   I2: an envelope is on the run queue, in the slot, on `lent`, on
 *       `abandoned`, or released; never two of these. A started session id is in `started`
 *       at most once while `actor->started` is true.
 *   I3: for every session, `queued` is the number of envelopes on the run
 *       queue whose target it is. A session with `queued != 0` is busy for
 *       unload and export (its inputs would be refused if it went).
 *   I4: every notice ta_run writes is complete in the array; the count is
 *       set on every result.
 *
 * Failure. Admission changes nothing unless it answers TA_OK. ta_run answers
 * TA_ERR_NOMEM with the input in the slot (or at the head of the run queue),
 * and the next call continues it.
 *
 * Complexity. Admission, take and completion are O(1). A destroyed target's
 * envelopes are not searched for: they are refused when they come to the
 * head.
 */

/*
 * TA_RUN_CELL_PAGE is the number of started-session cells in a page of the
 * runner's pool: 16 KiB, whatever the population. TA_RUN_STARTED_KEEP is the
 * capacity, in cells, that a drain keeps; a larger pool is trimmed when the
 * queue empties.
 */
#define TA_RUN_CELL_PAGE 1024u
#define TA_RUN_STARTED_KEEP 4096u

/*
 * ta_run_cell holds one started session's id in the queue of started
 * sessions: `next` is the pool's free-list link while the cell is free and
 * the index of the next cell, 0 for none, while it is queued.
 */
typedef struct ta_run_cell {
    uint32_t next;
    uint32_t reserved;
    ta_actor_id id;
} ta_run_cell;

/*
 * ta_run_slot is the one running slot: the input a budget paused, or the next
 * to continue. ADR 0103 turns it into a table of slots. Lifetime: input.
 */
typedef struct ta_run_slot {
    ta_actor_id actor; /* the session that holds the slot */
    uint32_t envelope; /* the input's envelope, or 0 for a started macrostep */
    bool occupied;
} ta_run_slot;

/* ta_runner is the control block (lifetime: system). Its fields are for run.c and the tests. */
typedef struct ta_runner {
    ta_run_slot slot;
    ta_pool cells;           /* ta_run_cell: sessions that started a macrostep and are not running */
    uint32_t started_first;  /* the oldest queued cell, 0 when none */
    uint32_t started_last;   /* the newest queued cell, 0 when none */
    ta_ilist lent;         /* envelopes whose events the last call's notices lent */
    ta_ilist abandoned;    /* inputs ended by ta_actor_abandon or by their session's end; reported by the next call */
    uint64_t tokens;       /* the last token issued; 0 is "no token" */
} ta_runner;

/*
 * ta_runner_init prepares an empty runner over `allocator`. It allocates
 * nothing; it answers TA_ERR_INVALID or TA_ERR_NOMEM only if the cell pool's
 * sizes are wrong, which they are not, and leaves the runner zeroed then.
 */
ta_result ta_runner_init(ta_runner *runner, ta_allocator allocator);

/*
 * ta_runner_destroy frees the started cells. The envelopes the runner
 * names (the slot's, the lent ones) belong to the pool and are released with
 * it. Idempotent.
 */
void ta_runner_destroy(ta_runner *runner);

/*
 * ta_run_reserve_start makes room for one more started session. Call it at a
 * safe point before the step that will call ta_run_start, so that ta_run_start
 * cannot fail: it answers TA_ERR_NOMEM, with nothing changed, when the
 * allocator refuses. It allocates only while the pool needs a page.
 *
 * Known exception to "allocation only at safe points": ta_communication_error
 * (a failed delayed send, a router callback) can reach it from inside another
 * actor's macrostep. The path allocated before this unit too (the queue growth
 * of ta_raise_error); it goes with the other mid-macrostep allocations in
 * step 4.4, when the cell is reserved at ta_run's take.
 */
ta_result ta_run_reserve_start(ta_system *system);

/*
 * ta_run_start queues `actor`, which has just begun a macrostep with no event
 * to take, behind the sessions already started. It never allocates and never
 * fails, provided ta_run_reserve_start answered TA_OK since the last call.
 * It does nothing for a session that is queued already. O(1).
 */
void ta_run_start(ta_system *system, ta_actor *actor);

/*
 * ta_run_actor_gone tells the runner that `actor` is being freed: if it holds
 * the slot, the slot is freed at once and the input, if it has a token, is
 * reported as abandoned by the next ta_run. Its queued envelopes and its
 * started entry are not searched for; they are skipped when they come up.
 * Called from ta_actor_free. Never allocates. O(1).
 */
void ta_run_actor_gone(ta_system *system, const ta_actor *actor);

/*
 * ta_run_abandoned tells the runner that ta_actor_abandon ended the macrostep
 * of `actor`: as for ta_run_actor_gone, the slot is freed at once, so the next
 * input, an operator's included, can start before ta_run reports the end.
 * Never allocates. O(1).
 */
void ta_run_abandoned(ta_system *system, const ta_actor *actor);
