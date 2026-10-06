#pragma once
/*
 * Package scheduler is the kernel's half of the application's input: the
 * code that admits a post into core, remembers what a send() is waiting for,
 * reads the notices core writes when it runs, and runs the turn
 * (ADRs 0005, 0007, 0008; tinyactors-core ADR 0099). It has no Node-API in
 * it, so native tests drive it directly.
 *
 * What it is for. Core keeps every pending input in one pool and one run
 * queue (ADR 0099): ta_actor_deliver() admits an event and never runs
 * anything, ta_run() runs what was admitted, in the order it was admitted.
 * The scheduler therefore holds no queue of its own. A post goes straight
 * into core and comes back as a token; ta_run() says what became of each
 * token in its notices, and the scheduler turns those notices into what the
 * application hears: a send() that settles, a dead letter, a finished actor,
 * a runaway. There is no table with an entry per actor, no mailbox, no
 * ready or runnable queue, and no quarantine: nothing a macrostep does makes
 * the host stop running the actor (that is core's decision, and its
 * abandon of a runaway macrostep leaves the actor stable).
 *
 * Lifetimes. Each thing here has exactly one.
 *
 *   - system: the tah_sched control block (embedded in the kernel's system
 *     record), the vector of waiter records and its free list, the table
 *     of followed inputs (indexed by ta_input_slot() of the token), the
 *     verdict ring and the settlement ring. They grow with the most that was
 *     ever pending and are given back by tah_sched_destroy().
 *   - record (waiter): a send()'s promise half, from tah_sched_waiter_take()
 *     until tah_sched_waiter_give(). While it is PENDING it is named by the
 *     token of its input in the table of followed inputs; SETTLING means the scheduler is
 *     done with it and the binding gives it back after it settled the promise.
 *     Its generation changes at each give, so a token JavaScript holds goes
 *     stale instead of reaching the next send.
 *   - envelope (a token's life): from a successful tah_sched_post() until the
 *     notice that ends the input (COMPLETED, REFUSED, or the abandon that
 *     core reports as COMPLETED) or until tah_sched_cancel() withdraws it.
 *     The table entry of a token (its waiter, its retained bytes) exists
 *     exactly that long; core owns the event for that long, so the scheduler copies none
 *     of it. The only thing the kernel keeps of a message is the token.
 *   - verdict: the answer to a post that core refused for a reason that is the
 *     application's business (the target is gone or finished, an address has
 *     maxWaiting messages waiting). It owns a reference to the event's data
 *     and to the address, from the post until the next turn turns it into a
 *     dead-letter record. The ring is the only place a post waits in the
 *     kernel, and it holds answers, not inputs.
 *   - turn: the records of a turn (a finished actor, a runaway, a dead
 *     letter) are made in the turn arena and live until the host has reported
 *     them (tah_sched_release_records(), called by tah_system_end_turn()); a
 *     record owns the data of a dead letter until then.
 *
 * Ownership of a post's event. tah_sched_post() borrows the event: core takes
 * its own references when it admits the input, and a verdict takes its own.
 * The caller keeps its references on every result and releases them itself.
 *
 * When it allocates. Only at the call that admits: the table of followed
 * inputs grows to the slot of a send()'s token, or of a post with a
 * retained-bytes estimate when maxRetainedBytes is set (it never shrinks, so
 * after warm-up nothing allocates), the verdict ring, and core's pool
 * refill inside ta_actor_deliver(). A plain post of an event with no waiter
 * allocates nothing after the pool has warmed up: it is one call into core
 * and a counter. The turn allocates in the turn arena (records) and when a
 * settlement ring chunk is needed. Every allocation failure leaves nothing
 * behind: a post whose table entry cannot be made withdraws its input before
 * it answers TA_ERR_NOMEM, so no token escapes that nothing follows.
 *
 * Back-pressure. Core's TA_ERR_FULL is the application's `queue-full`: the
 * pool is at its watermark. maxQueuedMessages and maxRetainedBytes (ADR 0005
 * point 9) are enforced here, without a queue: the first bounds the inputs
 * the application admitted that core has not finished (core's gauge,
 * ta_counters.host_inputs, plus the verdicts waiting for the next turn), the
 * second the bytes the binding estimated for the events of those inputs,
 * counted in a map from token to bytes that exists only while the limit is
 * set. Both answer TA_ERR_FULL.
 *
 * Complexity. A post is O(1) plus core's admission. A notice costs O(1) map
 * operations. A turn costs the inputs core finishes in it (bounded by
 * `inputs`) plus the verdicts it reports (bounded by the same), never the
 * number of actors.
 *
 * Operator transitions (ADR 0026, core ADR 0082): a message may carry one. It
 * does not queue: it starts in core's running slot at once
 * (ta_actor_transition()), or the post answers TA_ERR_BUSY when the actor is
 * not idle; a jump the actor does not accept (its definition changed, or its
 * source is not active) answers TA_ERR_INVALID.
 *
 * Usage (what the pump does, in outline):
 *
 *     tah_sched_post(&sched, &post);           // admit the application's post
 *     tah_sched_turn(&sched, &turn_params);    // ta_run, settle the notices
 *     // then read the turn arena's records and call the hooks
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "host_metrics.h"
#include "tinyactors.h"
#include "watch.h"

/* Why a message became a dead letter (ADR 0008). */
typedef enum {
    TAH_DEAD_NOT_FOUND = 0, /* no resident actor has the target's ID, or the address names nothing */
    TAH_DEAD_DONE,          /* the target reached a top-level final state */
    TAH_DEAD_DESTROYED,     /* the target was destroyed or unloaded with the message waiting */
    TAH_DEAD_REFUSED,       /* core refused the message itself (a replaying actor's time, or an abandoned macrostep) */
    TAH_DEAD_UNLOADED,      /* the target is unloaded (a named address, or an actor unloaded with mail waiting) */
    /* Mail that waited for a loader (napi ADR 0020): */
    TAH_DEAD_REJECTED,      /* the loader answered "not now" */
    TAH_DEAD_LOAD_FAILED,   /* the loader threw, or its result could not be used */
    TAH_DEAD_LOAD_TIMEOUT,  /* the loader did not settle in time */
    TAH_DEAD_WAITING_FULL   /* the address already had maxWaiting messages waiting */
} tah_dead_reason;

/* The reason's name as JavaScript sees it ("not-found", "load-timeout", ...). */
const char *tah_dead_reason_name(tah_dead_reason reason);

/* An operator transition to accept with a message (core ADR 0082). */
typedef struct {
    ta_definition_id definition; /* the definition the state IDs belong to */
    ta_operator_transition transition;
} tah_message_jump;

/*
 * A message to admit. Lifetime: call; every pointer is borrowed for the call
 * and nothing is kept of it but what core and the verdict retain themselves.
 */
typedef struct {
    ta_actor_id target;     /* the actor, or 0 when `address` is given */
    ta_address *address;    /* a named target: core resolves it, and makes the message wait when nothing is resident */
    const ta_event *event;
    uint32_t bytes;         /* retained-bytes estimate of its data, 0 for none */
    uint32_t waiter;        /* a send() waiting for it, or 0 */
    const ta_trace_parent *trace; /* its trace parent, or NULL (core ADR 0067) */
    /* An operator transition, or NULL; only for a target named by ID. */
    const tah_message_jump *jump;
} tah_message;

typedef enum {
    TAH_WAITER_FREE = 0,
    TAH_WAITER_PENDING,  /* its input is admitted and not finished; a token names it */
    TAH_WAITER_SETTLING  /* settled or cancelled; the Node-API layer gives it back */
} tah_waiter_state;

typedef struct tah_waiter tah_waiter;

typedef enum {
    TAH_SETTLE_STABLE, /* the message's macrostep ended with the actor stable */
    TAH_SETTLE_DONE,   /* ... with the actor finished */
    TAH_SETTLE_RUNAWAY, /* ... or reached the microstep limit and was abandoned (core ADR 0099 section 7); the actor is stable */
    TAH_SETTLE_DEAD,   /* the message became a dead letter, or its actor went away */
    TAH_SETTLE_ABANDONED /* an operator abandoned the macrostep (core ADR 0082) */
} tah_settle_kind;

typedef struct {
    uint32_t waiter;
    uint8_t kind;    /* tah_settle_kind */
    uint8_t reason;  /* DEAD: tah_dead_reason */
    /* STABLE, DONE: the message enabled no transition (core ADR 0089
     * section 8), SendResult.consumed false (napi ADR 0032 section 4). */
    uint8_t ignored;
    ta_actor_id actor;
} tah_settlement;

typedef enum {
    TAH_RECORD_DONE,       /* the actor reached a top-level final state */
    TAH_RECORD_FAULT,      /* a macrostep reached the microstep limit and core abandoned it (ADR 0103 section 8) */
    TAH_RECORD_DEAD_LETTER /* a message whose target is gone or finished */
} tah_record_kind;

typedef struct tah_turn_record {
    tah_record_kind kind;
    ta_actor_id actor;     /* DEAD_LETTER: the target, or 0 when it was a name with no actor */
    /* DEAD_LETTER to a named target with no actor: its text, copied into the turn arena. */
    bool has_address;
    ta_actor_address address;
    const char *operation; /* FAULT: "run" */
    ta_event_name_id event; /* DEAD_LETTER */
    tah_dead_reason reason; /* DEAD_LETTER */
    /* DEAD_LETTER: the message's data, owned by the record until
     * tah_sched_release_records(), so a host can post it again (napi ADR
     * 0020's restart outbox). Absent when the message had none. */
    ta_value data;
    /* DEAD_LETTER: the message's trace parent, when it had one. */
    bool has_trace;
    ta_trace_parent trace;
    struct tah_turn_record *next;
} tah_turn_record;

/*
 * What the recorder hears from the scheduler (kernel ADR 0006 section 2):
 * the inputs core takes, the macrosteps that end, the actors that settle or
 * finish. `event` is borrowed until the next ta_run(). The observer must not
 * call back into the scheduler.
 */
typedef enum {
    /* `actor` took an input (a host input, mail, or an error report) with `event`, admitted at `time`. */
    TAH_NOTE_TAKEN,
    TAH_NOTE_FINISHED,   /* `actor` reached a top-level final state */
    TAH_NOTE_SETTLED,    /* `actor` ended a macrostep and is stable: its pending sends may have changed */
    TAH_NOTE_ENDED       /* `actor`'s macrostep ended: stable, or finished (before SETTLED or FINISHED) */
} tah_note_kind;

typedef struct {
    tah_note_kind kind;
    ta_actor_id actor;
    const ta_event *event; /* TAKEN */
    ta_time time;          /* TAKEN */
} tah_sched_note;

typedef struct {
    void *self; /* NULL: no observer, and the scheduler notes nothing */
    void (*note)(void *self, const tah_sched_note *note);
} tah_sched_observer;

typedef struct tah_sched tah_sched;

typedef struct {
    /*
     * Inputs finished in one turn, and verdicts reported: messageBudget. The
     * turn asks it between calls of ta_run(), so a turn may finish up to one
     * call's worth more (the call's notice array and its microstep budget
     * bound that), never more than a few dozen.
     */
    uint32_t inputs;
    uint32_t microsteps; /* core's `budget` for one ta_run() call */
    uint32_t actions;    /* core's `action_budget` for one ta_run() call; 0: no limit */
} tah_turn_budgets;

typedef struct {
    ta_system *core;
    ta_arena *arena;   /* the turn arena: records live here */
    bool *inside_core; /* the reentrancy flag (ADR 0013 point 4) */
    tah_turn_budgets budgets;
    /* Asked after each call into core: true ends the turn there, and what is
     * left waits for the next one (napi ADR 0035's staging bound). NULL:
     * never. */
    bool (*stop)(void *context);
    void *stop_context;
    /* Called with stop_context after each call into core that may have traced
     * (a ta_run call): the kernel reads the tel ring there (core ADR 0098).
     * NULL: nothing. */
    void (*left_core)(void *context);
    /* Called with stop_context for each LOAD_NEEDED notice of a ta_run() call, after the call returned (core ADR
     * 0099 section 4): core is waiting for an address to become resident. NULL: such notices are ignored. */
    void (*load_needed)(void *context, const ta_run_notice *notice);
} tah_turn_params;

/* `allocator` for the maps and rings, `envelopes` for the waiter records and verdicts. */
void tah_sched_init(tah_sched *sched, ta_allocator allocator, ta_allocator envelopes);
/*
 * Releases what the verdicts hold (data, addresses). Call it while core is
 * still alive (a named origin and an address are core handles), before
 * ta_system_destroy().
 */
void tah_sched_discard(tah_sched *sched);
void tah_sched_destroy(tah_sched *sched);

/*
 * The limits a post must fit (ADR 0005 point 9), 0 for none. Set once, at
 * creation, by the kernel from the parameters.
 */
void tah_sched_set_limits(tah_sched *sched, uint32_t max_queued, uint64_t max_retained);

/*
 * tah_sched_post admits one message into core and returns at once; it never
 * runs a macrostep. `post->core` is the system, `post->message` the message
 * (borrowed). The results:
 * - TA_OK: admitted. The message is a token core will report (COMPLETED or
 *   REFUSED), or it became a verdict that the next turn reports as a dead
 *   letter (the target is not found or has finished; an address already has
 *   `max_waiting` messages waiting). Its send() waiter, if any, is PENDING
 *   or about to be settled DEAD by that turn.
 * - TA_ERR_FULL: core's pool is at its watermark, the target already holds
 *   65,535 inputs, or maxQueuedMessages or maxRetainedBytes would pass
 *   (napi `queue-full`). Nothing was accepted.
 * - TA_ERR_BUSY: an operator transition while the actor, or core's running
 *   slot, is busy (napi `busy`). Nothing was accepted.
 * - TA_ERR_INVALID: a malformed event or transition, an event name this
 *   system never issued, a replaying actor's time (core's caller-error
 *   answer). The actor is not harmed. Nothing was accepted.
 * - TA_ERR_NOMEM: a pool, map or ring could not grow. Nothing was accepted
 *   and nothing remains: an input already admitted is withdrawn first.
 * The event stays the caller's on every result. Allocates only as the
 * package comment says.
 */
typedef struct {
    ta_system *core;
    const tah_message *message;
    ta_time time;            /* the input's logical time: core's time now */
    uint32_t max_waiting;    /* an address's limit on waiting messages, 0 for none */
} tah_post;
ta_result tah_sched_post(tah_sched *sched, const tah_post *post);

/*
 * Whether a post of `bytes` would pass the limits now: maxQueuedMessages
 * against core's gauge of admitted inputs and the verdicts waiting,
 * maxRetainedBytes against the bytes counted so far. tah_sched_post() asks it;
 * it is exposed for tests.
 */
bool tah_sched_full(const tah_sched *sched, const ta_system *core, uint32_t bytes);

/* The scheduler's counters, as a binding reports them (napi's stats() and metrics()). */
typedef struct {
    uint32_t queued;      /* inputs the application admitted that core has not finished, and verdicts waiting */
    uint64_t delivered;   /* messages core admitted, and mail its receivers ran */
    uint64_t dead_letters; /* dead letters reported so far */
    uint64_t dead_by_reason[TAH_DEAD_REASONS]; /* by tah_dead_reason */
    uint64_t faults[TAH_FAULT_CODES];          /* by -ta_result: only the runaway counter moves */
} tah_sched_stats;
void tah_sched_read_stats(const tah_sched *sched, const ta_system *core, tah_sched_stats *out);

/*
 * send() waiters. tah_sched_waiter_take() returns a free waiter's index, or
 * 0 when there is no memory; give it to exactly one tah_sched_post() (or
 * back with tah_sched_waiter_give()). A settlement names the waiter it
 * settles, which is then SETTLING until the Node-API layer gives it back.
 */
uint32_t tah_sched_waiter_take(tah_sched *sched);
tah_waiter *tah_sched_waiter(const tah_sched *sched, uint32_t index);
void tah_sched_waiter_give(tah_sched *sched, uint32_t index);
bool tah_sched_settlement_pop(tah_sched *sched, tah_settlement *out);
/*
 * What a binding keeps with a waiter: its `host` (napi's promise), set
 * after the take, and NULL for a waiter not taken; its generation, the
 * other half of the token the binding hands out (0 for an index never
 * issued); and the next taken waiter after `after` (0 to start), or 0 when
 * there is none, to settle them all at close.
 */
void tah_sched_waiter_set_host(tah_sched *sched, uint32_t index, void *host);
void *tah_sched_waiter_host(const tah_sched *sched, uint32_t index);
uint32_t tah_sched_waiter_generation(const tah_sched *sched, uint32_t index);
uint32_t tah_sched_next_waiter(const tah_sched *sched, uint32_t after);
tah_waiter_state tah_sched_waiter_state(const tah_sched *sched, uint32_t index);

/*
 * Cancels a pending waiter (abort or timeout). An input core has not taken
 * is withdrawn with ta_input_withdraw(), so it is never delivered; one core
 * is running, or a verdict still waiting for the turn, is not undone (a
 * verdict is still reported, without its waiter). Returns true when the
 * waiter was pending; it is then SETTLING. Never allocates.
 */
bool tah_sched_cancel(tah_sched *sched, ta_system *core, uint32_t index);

/* True when a send() waits for the input `token` (ta_pending_input.input). Constant time, no allocation. */
bool tah_sched_awaited(const tah_sched *sched, uint64_t token);

/*
 * The replay driver (kernel ADR 0006 section 3) runs an actor itself:
 * notices for an actor in the replaying set are not turned into records, sends
 * or notes by the turn. Add an actor before its first replayed event and
 * remove it at tah_replay_events_done(). TA_ERR_NOMEM: nothing changed.
 */
ta_result tah_sched_replaying_add(tah_sched *sched, ta_actor_id actor);
void tah_sched_replaying_remove(tah_sched *sched, ta_actor_id actor);
bool tah_sched_replaying(const tah_sched *sched, ta_actor_id actor);

/*
 * True when the scheduler has nothing to do on its own: no verdict waits
 * for a turn and no settlement waits for the binding. Core's own pending
 * work (ta_run_pending()) is the kernel's to add.
 */
bool tah_sched_quiet(const tah_sched *sched);

/*
 * One bounded turn: report the verdicts, then run core until it has no more
 * to do or the turn has finished `budgets.inputs` inputs (ADR 0007 point 1).
 * The turn's records start where the last turn left them and stay valid until
 * tah_sched_release_records(). Never fails: a failure is an actor's, and
 * becomes a record.
 */
void tah_sched_turn(tah_sched *sched, const tah_turn_params *params);

/* The records of the last turn, in the order they happened, until
 * tah_sched_release_records(); NULL when there are none. */
const tah_turn_record *tah_sched_records(const tah_sched *sched);
/*
 * Releases what the turn's records own (a dead letter's data) and forgets
 * the records. The host calls it once it has reported them, and close calls
 * it before core goes, since values outlive nothing. The turn does not call
 * it: a verdict made between two turns is reported once, by the next turn,
 * whose records the host has not yet released.
 */
void tah_sched_release_records(tah_sched *sched);
