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
 *     ta_actor_deliver (the host's), from the mail another session's
 *     macrostep sent (package mail: it joins the queue when that macrostep
 *     ends), or from a report of ta_actor_report_invoke (an event or done
 *     report is a host input; an error report is an input of source ERROR,
 *     applied when it is taken). A host input
 *     is named by a token ta_run reports it by; mail has none. A host input
 *     admitted while a macrostep executes (a data-model callback posting to
 *     another session) is paid from that macrostep's reserve and waits on its
 *     outgoing list, so it is ordered with the mail the macrostep sends and
 *     dropped with it if the macrostep is abandoned. An operator's
 *     event with a transition (ta_actor_transition) is an input that skips
 *     the queue and fills the slot.
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
 *     started cells, the token sequence and the host-input gauge
 *     (ta_counters.host_inputs, in the system's counters). The cell pool grows only through
 *     ta_run_reserve_start, a safe point, and is never needed to grow
 *     inside a macrostep.
 *   - input: the slot, from take until the input's COMPLETED notice. An
 *     input that a budget pauses keeps the slot, so nothing else is taken
 *     before it ends. The slot's envelope lives as long, and so do the
 *     macrostep reserve (a claim on the envelope quota) and the outgoing list
 *     of the mail the input sends (package mail), which the slot holds: the
 *     reserve is taken when the input is taken and given back when it ends,
 *     the list is spliced onto the run queue when it ends, or freed unseen
 *     when it is abandoned.
 *   - token: from ta_actor_deliver (or ta_actor_transition) until the input's
 *     terminal notice, or until ta_input_withdraw. It is
 *     `sequence << TA_INPUT_INDEX_BITS | envelope index`, so a withdrawal finds
 *     its envelope with no search, and the sequence makes a token that names a
 *     reused envelope fail the check instead of reaching the new input.
 *   - call: the notice array the caller gives ta_run, and the events its
 *     notices borrow. An envelope whose event a notice lent goes on `lent`
 *     and is released at the start of the next ta_run (or at destroy).
 *
 * When it allocates. ta_actor_deliver and ta_actor_transition allocate only
 * through the envelope pool's refill, at their own call (the admission safe
 * point; inside a macrostep they allocate nothing), and ta_actor_transition
 * also allocates its jump. An error report interns its event name at its
 * call (invoke.c explains the one exception that makes). ta_input_withdraw
 * and ta_actor_pending never allocate. ta_run allocates
 * before an input is taken: the pool refill that backs the macrostep reserve
 * (so a send never fails because the pool had not grown yet) and the event
 * block its macrostep needs; and as a macrostep always has (the interpreter,
 * ADR 0070). A failure (TA_ERR_NOMEM) leaves the input where it stands and
 * changes nothing. ta_run_reserve_start
 * allocates; ta_run_start, ta_run_actor_gone and ta_run_abandoned never do.
 *
 * Invariants.
 *   I1: at most one input is in the slot; `occupied` is false exactly when
 *       no session is mid-macrostep because of ta_run.
 *   I2: an envelope is on the run queue, in the slot, on `lent`, on
 *       `abandoned`, or released; never two of these. A started session id is in `started`
 *       at most once while `actor->started` is true.
 *   I3: for every session, `queued` is the number of envelopes addressed to
 *       it that wait: on the run queue, or on the outgoing list of the input
 *       in the slot (package mail). A session with `queued != 0` is busy for
 *       unload and export (its inputs would be refused if it went); the mail
 *       its own paused macrostep sent itself is the one exception.
 *   I4: every notice ta_run writes is complete in the array; the count is
 *       set on every result.
 *   I5: an envelope is on `loads` exactly when it is the first of a waiting
 *       list whose cue is pending; it is on `refused` only between
 *       ta_address_load_failed and the call that writes its notice. Both lists
 *       count as pending work for ta_run_pending.
 *
 *   I6: ta_counters.host_inputs is the number of HOST and TRANSITION
 *       envelopes whose end is not decided: not yet completed, refused,
 *       abandoned or withdrawn. It goes down at the moment the end is
 *       decided (ta_run_input_decided), which is before the notice is
 *       written, so a host that counts from it never counts an input twice.
 *   I7: a withdrawn envelope (TA_ENVELOPE_WITHDRAWN) is on the run queue, an
 *       outgoing list or a waiting list until the list reaches it; it holds
 *       no place in any session's `queued` and is not in host_inputs. It is
 *       released unseen, with no notice, wherever it is next met: ta_run's
 *       take, a waiting list that moves or fails, a discarded macrostep.
 *
 * Failure. Admission changes nothing unless it answers TA_OK. ta_run answers
 * TA_ERR_NOMEM with the input in the slot (or at the head of the run queue),
 * and the next call continues it.
 *
 * Waiting for an address. An input for an address that has no resident
 * session (ta_actor_deliver with `address`, or a mail envelope whose receiver
 * was unloaded before it was taken) joins that address's waiting list
 * (package envelope); the first one raises a cue, which ta_run writes as a
 * LOAD_NEEDED notice. The list leaves in one piece: to the run queue when the
 * address becomes resident (package mail, ta_mail_address_resident), or
 * recycled when the host says the load failed (ta_address_load_failed).
 * Waiting inputs hold an envelope each and count against the pool's bound, but
 * not against any session's `queued`.
 *
 * Complexity. Admission, take and completion are O(1). A destroyed target's
 * envelopes are not searched for: they are refused when they come to the
 * head. Removing a pending cue is O(cues pending), which are the lists that
 * began since the last call.
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
    /*
     * The macrostep reserve (ADR 0099 section 3): the envelopes this input
     * may take for the mail it sends, reserved when the input is taken and
     * given back when it ends. Lifetime: input.
     */
    ta_quota_claim claim;
    /*
     * The mail this macrostep has sent, in send order, not yet visible to
     * any receiver (ADR 0099 section 4). Spliced onto the run queue when
     * the macrostep ends, freed unseen when it is abandoned. Lifetime:
     * input. See mail.h.
     */
    ta_ilist outgoing;
    /*
     * The mail this macrostep has sent to destinations that are not resident,
     * in send order: each waits for an address (package mail). Placed one by
     * one when the macrostep ends, freed unseen when it is abandoned.
     * Lifetime: input.
     */
    ta_ilist outgoing_waiting;
} ta_run_slot;

/* ta_runner is the control block (lifetime: system). Its fields are for run.c and the tests. */
typedef struct ta_runner {
    ta_run_slot slot;
    ta_pool cells;           /* ta_run_cell: sessions that started a macrostep and are not running */
    uint32_t started_first;  /* the oldest queued cell, 0 when none */
    uint32_t started_last;   /* the newest queued cell, 0 when none */
    ta_ilist lent;         /* envelopes whose events the last call's notices lent */
    ta_ilist abandoned;    /* inputs ended by ta_actor_abandon or by their session's end; reported by the next call */
    /*
     * Cues the next call reports first (ADR 0099 sections 6 and 7). `loads`
     * holds the first envelope of every waiting list whose LOAD_NEEDED notice
     * has not been written yet, linked through ta_envelope.owner_next;
     * `refused` holds the host inputs that ta_address_load_failed discarded,
     * linked through ta_envelope.next, with the reason in ta_envelope.flags.
     * Both are written by ta_run before it takes anything, as far as the
     * caller's array has room, and both are empty when the call is idle.
     * Lifetime: system for the heads, envelope for membership.
     */
    ta_ilist loads;
    ta_ilist refused;
    uint64_t tokens;       /* the last token issued; 0 is "no token" */
    /*
     * True only while ta_run is inside the interpreter for the slot's input:
     * a macrostep is executing, not merely holding the slot. A budget-paused
     * macrostep keeps the slot between calls, so `slot.occupied` cannot say
     * this. Lifetime: one ta_interpreter_run_input call. See ta_run_executing.
     */
    bool executing;
} ta_runner;

/*
 * A token is `sequence << TA_INPUT_INDEX_BITS | envelope index`. The index
 * takes 25 bits because the envelope pool is bounded by 2^24 (index 2^24 is
 * the last) and an index is never 0; the sequence has the other 39. Hosts
 * treat a token as opaque; core reads the index back in ta_input_withdraw.
 */
#define TA_INPUT_INDEX_BITS 25u
#define TA_INPUT_INDEX_MASK ((UINT64_C(1) << TA_INPUT_INDEX_BITS) - 1u)
#define TA_INPUT_SEQUENCE_MASK ((UINT64_C(1) << (64u - TA_INPUT_INDEX_BITS)) - 1u)

/*
 * ta_run_input_decided counts a host input (HOST or TRANSITION envelope) as
 * no longer pending: ta_counters.host_inputs goes down. Call it exactly once
 * per such envelope, where its end becomes decided (completed, refused,
 * abandoned, withdrawn); other sources do nothing. Never allocates. O(1).
 */
void ta_run_input_decided(ta_system *system, const ta_envelope *envelope);

/*
 * ta_run_report is an invocation error report to admit (ta_run_admit_report).
 * Lifetime: call; the event is retained by the envelope, not moved.
 */
typedef struct ta_run_report {
    ta_event event;       /* error.communication.invoke.<id>.<reason>, with the report's data */
    ta_trace_parent trace;
    ta_time time;         /* the report's logical time (ADR 0094) */
    ta_invoke_id invoke;  /* the invocation it is about */
    bool ends;            /* retire the invocation when the report is taken */
} ta_run_report;

/*
 * ta_run_admit_report queues an invocation error report for `actor` as an
 * input of source ERROR (ta_actor_report_invoke with TA_INVOKE_REPORT_ERROR),
 * behind everything admitted before it. Like ta_actor_deliver it never runs
 * anything; inside a macrostep it pays from the running input's reserve and
 * waits on the slot's outgoing list. It carries no token: the host does not
 * ask to hear how it ended. The invocation is looked up again when the report
 * is taken (ta_invoke_report_take), so it may end meanwhile.
 * TA_ERR_FULL: the watermark, the session's 65,535 inputs, or a spent
 * reserve inside a macrostep. TA_ERR_NOMEM: the pool could not grow. Nothing
 * is retained on any other result than TA_OK. Allocates only in the pool's
 * refill, at the admission safe point.
 */
ta_result ta_run_admit_report(ta_system *system, ta_actor *actor, const ta_run_report *report);

/*
 * ta_run_executing reports whether a macrostep is executing now, inside
 * ta_run. Everything a send does while it is true is paid from the slot's
 * claim and waits on the slot's outgoing lists, whichever session sends:
 * an invoked child cancelled by its parent's macrostep runs its <onexit>
 * inside that macrostep, so its mail belongs to the parent's input (dropped
 * if the parent's macrostep is abandoned, after the parent's earlier sends).
 * While it is false the caller is at a safe point (a host call, a due
 * dispatch, a publish) and posts from the pool after ta_mail_prepare. O(1).
 */
bool ta_run_executing(const ta_system *system);

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
 * (a failed delayed send, a host I/O processor) can reach it from inside another
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
 * ta_run_unloaded tells the runner that `actor` is leaving residency (ta_actor_unload):
 * as for ta_run_actor_gone, the slot is freed if the session holds it, but
 * the mail its macrostep had sent is made visible instead of freed, because
 * an unload moves the session out and abandons nothing (ADR 0014). Never
 * allocates. O(1).
 */
void ta_run_unloaded(ta_system *system, const ta_actor *actor);

/*
 * ta_run_abandoned tells the runner that ta_actor_abandon ended the macrostep
 * of `actor`: as for ta_run_actor_gone, the slot is freed at once, so the next
 * input, an operator's included, can start before ta_run reports the end.
 * Never allocates. O(1).
 */
void ta_run_abandoned(ta_system *system, const ta_actor *actor);
