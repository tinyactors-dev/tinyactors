#pragma once
/*
 * Scheduling (ADRs 0005, 0007, 0008), over core's pool of pending inputs and
 * ta_run() (core ADRs 0099, 0103). This module has no Node-API in it, so
 * native tests drive it directly.
 *
 * Core keeps the queue: every message the kernel accepts is admitted into
 * core's pool (ta_actor_deliver(), ta_actor_report_invoke()), and a turn
 * calls ta_run(), which takes the inputs in arrival order with one
 * macrostep slot and reports what became of each. Mail between resident
 * actors never reaches the kernel: core queues it itself (core ADR 0099),
 * and a TAKEN outcome carries its event, which the recorder keeps. Mail
 * between actors that comes here is for an actor that is not resident. The kernel keeps what
 * core does not have: a send()'s waiter, the message's data for a dead
 * letter, the recorder's copy of an event, and the scheduling state a
 * binding shows.
 *
 * The scheduling table has one 16-byte entry per slot of core's actor table
 * (ADR 0002). It grows in fixed pages that are never moved or copied. An
 * entry holds the generation of the actor the addon knows in that slot, its
 * scheduling state, a flag or two, and the first and last envelope of its
 * mailbox.
 *
 * Mailboxes (ADR 0005): an envelope is a record in the system's envelope
 * store, addressed by a 32-bit index (0 means none). A mailbox is the list
 * threaded through envelopes from the entry's `first` to its `last`: the
 * actor's messages that its macrostep has not taken yet, in the order they
 * were accepted, admitted into core's pool or held. Freed envelopes go on
 * the store's free list, so after warm-up a post makes no allocator call
 * (ADR 0014). Each envelope is core's input under a correlation (its index
 * and a serial), and leaves the mailbox when core reports it TAKEN.
 *
 * Admission: a message is admitted into core's pool when it is posted.
 * Core answers TA_ERR_FULL at its admission watermark: a message the host
 * posted for an actor ID is then refused with TA_ERR_FULL, which a binding
 * reports as its queue-full; mail between actors (for an actor a load
 * brought back) and invocation reports are held in the mailbox, and the
 * actor joins the held queue. Mail between actors is also held past the
 * limits of ADR 0005 point 9, as overflow (kernel ADR 0003), counted apart
 * until it is moved in. Each turn admits
 * held mail first, the actors in the order they first held it, until core
 * is full again; an actor's later mail is held behind its held mail, so
 * one sender's mail stays in order.
 *
 * The turn calls ta_run() up to `resumes` times, each with a microstep
 * budget and an action budget (core ADR 0061) and room for one input's
 * outcomes, so a call takes at most one input, until core answers
 * TA_RUN_IDLE, or TA_RUN_PAUSED: a macrostep its budget paused has had its
 * one slice of the turn, which ends the turn's run of core, and the next
 * turn continues it. It acts on each outcome: a TAKEN message leaves its
 * mailbox and its send() follows the macrostep; COMPLETED, IGNORED and
 * FINISHED settle it, and a finished actor's held mail becomes dead
 * letters; FAULTED quarantines the actor (out of memory instead releases it
 * at the next turn, ADR 0008's backoff), and core keeps its input and
 * defers its later mail until release() lets it go on; REFUSED makes a
 * dead letter. What the turn learns that JavaScript must hear about (an
 * actor finished, an actor faulted) becomes a record in the turn arena; the
 * pump calls the hooks for them after the last core call of the turn (ADR
 * 0007 point 5).
 *
 * Dead letters: mail whose target is gone or finished moves to the dead
 * list, and each turn reports at most `messages` of them as records, so one
 * dead actor with a deep mailbox cannot make a turn long. Mail for a stale
 * ID is a dead letter at once, so it never joins the mailbox of the slot's
 * next actor. The mail of an actor the kernel removes is withdrawn from
 * core's pool and becomes dead letters with the reason of its removal.
 *
 * send() waiters (ADR 0005 point 11): a message may carry a waiter, the
 * native half of a send() promise. While the message is queued the waiter
 * names its envelope; cancelling it withdraws the message, from core's pool
 * too (ta_system_withdraw_input()), so it is never run. When core takes the
 * message the waiter follows the macrostep; when that ends (stable, done, a
 * fault, or the actor is gone) or the message becomes a dead letter, the
 * scheduler queues a settlement. The Node-API layer pops settlements after
 * the turn and settles the promises. Waiters are reused through a free
 * list, so a send makes no allocator call after warm-up.
 *
 * Operator transitions (ADR 0026, core ADR 0082): a message may carry one.
 * It is checked against the actor's definition when it is posted, and core
 * checks its source when it takes it. A jump the actor does not accept (its
 * source is not active, or the actor runs another revision) is a dead
 * letter with reason `refused` and does not quarantine the actor: the
 * operator's input is at fault, not the actor.
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "host_metrics.h"
#include "tinyactors.h"
#include "watch.h"

#define TAH_SCHED_PAGE_BITS 12u
#define TAH_SCHED_PAGE_ENTRIES (1u << TAH_SCHED_PAGE_BITS) /* 4,096 entries, 64 KiB */

typedef enum {
    TAH_SCHED_NONE = 0,    /* no actor the addon knows */
    TAH_SCHED_IDLE,        /* between macrosteps, nothing waits */
    TAH_SCHED_READY,       /* between macrosteps, with mail in its mailbox */
    TAH_SCHED_RUNNABLE,    /* a macrostep in progress in core: begun, waiting to begin, or paused */
    TAH_SCHED_QUARANTINED  /* a fault stopped its macrostep: core keeps it until released (ADR 0008) */
} tah_sched_state;

enum {
    TAH_SCHED_BACKOFF = 1u << 0,  /* ran out of memory: released at the next turn (ADR 0008) */
    TAH_SCHED_FINISHED = 1u << 1, /* reached a top-level final state */
    /* Its mail is held, but the held queue had no room for it: the next
     * turn queues it, and the scheduler is not quiet until it has. */
    TAH_SCHED_STRANDED = 1u << 7
};

typedef struct tah_sched_entry tah_sched_entry;

/* Why a message became a dead letter (ADR 0008). */
typedef enum {
    TAH_DEAD_NOT_FOUND = 0, /* no resident actor has the target's ID */
    TAH_DEAD_DONE,          /* the target reached a top-level final state */
    TAH_DEAD_DESTROYED,     /* the target was destroyed or unloaded with mail waiting */
    TAH_DEAD_REFUSED,       /* core refused the message itself (for example a malformed event) */
    TAH_DEAD_UNLOADED,      /* the target is unloaded (a named address, or an actor unloaded with mail waiting) */
    /* Why a load failed (napi ADR 0020), as a chart's error.communication
     * names it (system.c): */
    TAH_DEAD_REJECTED,      /* the loader answered "not now" */
    TAH_DEAD_LOAD_FAILED,   /* the loader threw, or its result could not be used */
    TAH_DEAD_LOAD_TIMEOUT   /* the loader did not settle in time */
} tah_dead_reason;

/* The reason's name as JavaScript sees it ("not-found", "load-timeout", ...). */
const char *tah_dead_reason_name(tah_dead_reason reason);

/*
 * An invocation's error report that waits in its parent's mailbox (napi ADR
 * 0032 section 3, core ADR 0089 section 4): core takes a report only
 * between macrosteps, so it queues behind the mail before it and reaches
 * ta_actor_report_invoke() in turn. Its envelope's event carries the
 * invocation (`invoke_id`) and the report's data.
 */
typedef enum {
    TAH_REPORT_NONE = 0,
    TAH_REPORT_UNAVAILABLE, /* the service is lost; ends the invocation */
    TAH_REPORT_QUARANTINED, /* an invoked child faulted; the invocation stays (ADR 0025) */
    TAH_REPORT_FAILED       /* the service could not start; ends the invocation */
} tah_report_kind;

/*
 * An operator transition a message carries (core ADR 0082), in one
 * allocation: the definition its state IDs belong to, and the transition.
 */
typedef struct {
    ta_definition_id definition;
    ta_state_id source; /* TA_STATE_NONE: the root */
    uint32_t count;
    ta_state_id targets[];
} tah_jump;

typedef struct tah_envelope tah_envelope;

/* An operator transition to accept with a message (core ADR 0082). */
typedef struct {
    ta_definition_id definition; /* the definition the state IDs belong to */
    ta_operator_transition transition;
} tah_message_jump;

/* A message to accept: the target, the event, and what it carries along. */
typedef struct {
    ta_actor_id target;     /* unused for a post by address */
    /* A post by address (core ADR 0099), or NULL: core resolves it, and it
     * waits in core for the address's load when no actor is resident there.
     * Borrowed; the envelope retains it. Not with `jump` or `report`. */
    ta_address *address;
    const ta_event *event;
    uint32_t bytes;         /* retained-bytes estimate of its data, 0 for none */
    uint32_t waiter;        /* a send() waiting for it, or 0 */
    const ta_trace_parent *trace; /* its trace parent, or NULL (core ADR 0067) */
    /* An operator transition, or NULL; only for a target named by ID. The
     * scheduler copies it. */
    const tah_message_jump *jump;
    /* An error report instead of a message (tah_report_kind); `event`
     * carries its invocation and data. A report for a gone target is
     * dropped, never a dead letter. */
    uint8_t report;
    /* Mail between actors (kernel ADR 0003): past the limits it is accepted
     * as overflow, and with core's pool full it is held, instead of
     * refused. A report is held the same way. */
    bool may_overflow;
} tah_message;

typedef enum {
    TAH_WAITER_FREE = 0,
    TAH_WAITER_QUEUED,   /* its message waits in a mailbox, in core's pool, or on the dead list */
    TAH_WAITER_INFLIGHT, /* core took the message; its macrostep runs */
    TAH_WAITER_SETTLING  /* settled or cancelled; the Node-API layer gives it back */
} tah_waiter_state;

typedef struct tah_waiter tah_waiter;

typedef enum {
    TAH_SETTLE_STABLE, /* the message's macrostep ended with the actor stable */
    TAH_SETTLE_DONE,   /* ... with the actor finished */
    TAH_SETTLE_DEAD,   /* the message became a dead letter, or its actor went away */
    TAH_SETTLE_FAULT,  /* the macrostep failed and the actor is quarantined */
    TAH_SETTLE_ABANDONED /* an operator abandoned the macrostep (core ADR 0082) */
} tah_settle_kind;

typedef struct {
    uint32_t waiter;
    uint8_t kind;    /* tah_settle_kind */
    uint8_t reason;  /* DEAD: tah_dead_reason */
    /* STABLE, DONE: the message enabled no transition (core ADR 0089
     * section 8), SendResult.consumed false (napi ADR 0032 section 4). */
    uint8_t ignored;
    ta_result code;  /* FAULT: core's result */
    ta_actor_id actor;
} tah_settlement;

#define TAH_ENVELOPE_PAGE_BITS 10u
#define TAH_ENVELOPE_PAGE_ENTRIES (1u << TAH_ENVELOPE_PAGE_BITS) /* 1,024 envelopes */

typedef enum {
    TAH_RECORD_DONE,       /* the actor reached a top-level final state */
    TAH_RECORD_FAULT,      /* a core call for the actor failed (ADR 0008) */
    TAH_RECORD_DEAD_LETTER /* a message whose target is gone or finished */
} tah_record_kind;

typedef struct tah_turn_record {
    tah_record_kind kind;
    ta_actor_id actor;     /* DEAD_LETTER: the target, or 0 when it was a name with no actor */
    /* DEAD_LETTER to a named target with no actor: its text, copied into the turn arena. */
    bool has_address;
    ta_actor_address address;
    const char *operation; /* FAULT: "resume", the run of its macrostep */
    ta_result code;        /* FAULT: core's result */
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
 * a message or report about to be offered to core, and whether core took
 * it, or mail between actors core took from its own queue; a macrostep
 * that ended; an actor that settled or finished. `message` is the
 * envelope, borrowed for the call. The observer must not call back
 * into the scheduler.
 */
typedef enum {
    TAH_NOTE_DELIVERED,  /* core took `message`: its macrostep begins now */
    TAH_NOTE_REPORTED,   /* core took the report `message`: its macrostep begins now */
    TAH_NOTE_FINISHED,   /* `actor` reached a top-level final state */
    TAH_NOTE_SETTLED,    /* `actor` ended a macrostep and is stable: its pending sends may have changed */
    TAH_NOTE_ENDED       /* `actor`'s macrostep ended: stable, finished, or stopped by a fault (before SETTLED or FINISHED) */
} tah_note_kind;

typedef struct {
    tah_note_kind kind;
    ta_actor_id actor;
    ta_address *address;
    uint32_t envelope;
    const tah_envelope *message;
    /* DELIVERED of mail core queued itself, which has no envelope here:
     * the event core took (ta_input_outcome's), borrowed for the call;
     * null otherwise. */
    const ta_event *event;
} tah_sched_note;

typedef struct {
    void *self; /* NULL: no observer, and the scheduler notes nothing */
    void (*note)(void *self, const tah_sched_note *note);
} tah_sched_observer;

typedef struct tah_sched tah_sched;

typedef struct {
    uint32_t resumes;    /* ta_run() calls in one turn */
    uint32_t messages;   /* actors whose held mail is admitted in one turn, and dead letters reported */
    uint32_t microsteps; /* core's `budget` for one ta_run() */
    uint32_t actions;    /* core's `action_budget` for one ta_run(); 0: no limit */
} tah_turn_budgets;

typedef struct {
    ta_arena *arena;   /* the turn arena: records live here */
    tah_turn_budgets budgets;
    /* Asked after each ta_run(): true ends the turn there, and what is left
     * waits for the next one (napi ADR 0035's staging bound). NULL: never. */
    bool (*stop)(void *context);
    void *stop_context;
} tah_turn_params;

/* `allocator` for the table and queues, `envelopes` for the envelope store. */
void tah_sched_init(tah_sched *sched, ta_allocator allocator, ta_allocator envelopes);
/*
 * The core the scheduler admits mail into and runs, the reentrancy flag
 * (ADR 0013 point 4) it sets around each core call, and the watched
 * sessions and definitions (watch.h) whose mail it marks as it admits it
 * (NULL: none). Set once, after core is created, before anything is posted.
 */
typedef struct {
    ta_system *core;
    bool *inside_core;
    tah_trace_watch *watch;
} tah_sched_attach_params;
void tah_sched_attach(tah_sched *sched, const tah_sched_attach_params *params);
/*
 * Releases every queued message's references. Call it while core is still
 * alive (a named origin is a core handle), before ta_system_destroy().
 */
void tah_sched_discard_mail(tah_sched *sched);
void tah_sched_destroy(tah_sched *sched);

/* The entry of `slot`, or NULL when its page does not exist. */
tah_sched_entry *tah_sched_entry_at(const tah_sched *sched, uint32_t slot);
/* The entry of the actor `actor` names, or NULL when the addon knows another
 * actor (or none) in that slot. */
tah_sched_entry *tah_sched_lookup(const tah_sched *sched, ta_actor_id actor);

/*
 * A new actor (spawned, loaded, reconstructed, prepared): fill its entry;
 * core admitted its bootstrap, or the macrostep it was restored in, and a
 * turn runs it. TA_ERR_NOMEM leaves the table as it was.
 */
ta_result tah_sched_add(tah_sched *sched, ta_actor_id actor);
/*
 * The actor is gone (destroyed, unloaded): its mail, withdrawn from core's
 * pool, becomes dead letters with `reason`, its taken input's send()
 * settles as a dead letter, and its entry is cleared.
 */
void tah_sched_remove(tah_sched *sched, ta_actor_id actor, tah_dead_reason reason);
/*
 * A named actor was replaced in place (ROADMAP step 14): the mail still
 * waiting for `from` is withdrawn from core's pool and moves, in order, to
 * the front of `to`'s mailbox, held, with its send() waiters; the next turn
 * admits it for `to`, before `to`'s own mail, which is held behind it. That
 * mail was accepted for the address `to` now holds, before anything posted
 * to `to` directly. TA_ERR_NOT_FOUND when the table knows either actor not.
 */
typedef struct {
    ta_actor_id from;
    ta_actor_id to;
} tah_sched_transfer_params;
ta_result tah_sched_transfer(tah_sched *sched, const tah_sched_transfer_params *params);

/*
 * Accepts a message for `message->target` (ADR 0005 point 3) and admits it
 * into core's pool. On TA_OK the envelope owns the event's references and
 * the waiter follows the message; otherwise the caller keeps both.
 * - A target the table does not know (stale or unknown ID) or one that has
 *   finished: accepted as a dead letter at once; so is an operator
 *   transition for another revision, or one core refuses (`refused`).
 * - TA_ERR_FULL: core's pool is at its admission watermark, and the
 *   message is neither mail between actors (`may_overflow`) nor a report,
 *   which are held instead; nothing was accepted.
 * - TA_ERR_NOMEM: nothing was accepted.
 * May run inside a core call (the router, an invoker), where core admits
 * from what is free.
 */
ta_result tah_sched_post(tah_sched *sched, const tah_message *message);
/*
 * The load of `address` failed with `reason` (rejected, load-failed,
 * load-timeout, ...): mail JavaScript posted there, which core refuses
 * next, becomes a dead letter with that reason. Never fails; out of memory
 * the letters say not-found.
 */
void tah_sched_note_failed_load(tah_sched *sched, const ta_address *address, tah_dead_reason reason);
/* `address` is asked about again: a reason noted for it no longer applies. */
void tah_sched_forget_failed_load(tah_sched *sched, const ta_address *address);
/* The envelope at `index` (for tests), or NULL. */
tah_envelope *tah_sched_envelope(const tah_sched *sched, uint32_t index);
/* Messages waiting in `actor`'s mailbox, its overflow included. */
uint32_t tah_sched_mailbox_count(const tah_sched *sched, ta_actor_id actor);
/* The scheduling state of `actor`: TAH_SCHED_NONE for one the table does not know. */
tah_sched_state tah_sched_state_of(const tah_sched *sched, ta_actor_id actor);

/*
 * A queued message as a binding lists it (napi's mailbox()): borrowed from
 * the scheduler until it next changes. Walk a mailbox from
 * tah_sched_mailbox_first() through `next`; 0 ends it.
 */
typedef struct {
    const ta_event *event;
    const tah_jump *jump; /* its operator transition (core ADR 0082), or NULL */
    uint32_t waiter;      /* the send() waiting for it, or 0 */
    uint32_t next;        /* the next envelope in its mailbox, 0 when last */
    bool overflow;        /* it waits in the overflow for room (kernel ADR 0003) */
    bool held;            /* core has not admitted it yet: its pool was full, or it is overflow */
} tah_queued_message;
/* The first envelope in `actor`'s mailbox, 0 when it is empty or unknown. */
uint32_t tah_sched_mailbox_first(const tah_sched *sched, ta_actor_id actor);
/* The message in `envelope`, an index a mailbox walk gave. */
tah_queued_message tah_sched_queued_message(const tah_sched *sched, uint32_t envelope);

/* The scheduler's counters, as a binding reports them (napi's stats() and metrics()). */
typedef struct {
    uint32_t runnable;    /* actors with a macrostep in progress */
    uint32_t ready;       /* actors between macrosteps with mail waiting */
    uint32_t quarantined; /* actors in quarantine now */
    uint32_t queued;      /* accepted messages not yet taken or reported, the overflow included */
    uint32_t overflow;    /* ... of them accepted past the limits and not moved in yet (kernel ADR 0003) */
    uint64_t delivered;   /* messages core took */
    uint64_t dead_letters; /* dead letters reported so far */
    uint64_t dead_by_reason[TAH_DEAD_REASONS]; /* by tah_dead_reason */
    uint64_t faults[TAH_FAULT_CODES];          /* by -ta_result */
} tah_sched_stats;
void tah_sched_read_stats(const tah_sched *sched, tah_sched_stats *out);
/*
 * Core gave `actor` a macrostep to run outside a delivery (for example an
 * invocation reported unavailable, which queues an internal event) and
 * queued the input that runs it: an idle or ready actor becomes runnable,
 * and the next turn runs core. A quarantined actor is left as it is.
 * TA_ERR_NOT_FOUND for an actor the table does not know.
 */
ta_result tah_sched_wake(tah_sched *sched, ta_actor_id actor);
/*
 * Core queued inputs itself outside a turn's ta_run() calls (delayed sends
 * to resident actors that fell due, core ADR 0099): the next turn runs
 * core. Never fails.
 */
void tah_sched_core_work(tah_sched *sched);
/*
 * Lets a quarantined actor go on (ADR 0008): core continues the macrostep a
 * fault stopped (ta_actor_release(), without the microstep limit after a
 * runaway), and the actor is runnable again. An actor that is not
 * quarantined is left as it is. TA_ERR_NOT_FOUND for an actor the table
 * does not know.
 */
ta_result tah_sched_release(tah_sched *sched, ta_actor_id actor);

/*
 * A ta_run() the kernel made outside a turn (a replay, through its session
 * filter) ended `actor`'s macrostep, `finished` or stable: it stands
 * between macrosteps. A quarantined or unknown actor is left as it is.
 */
void tah_sched_ran(tah_sched *sched, ta_actor_id actor, bool finished);
/*
 * An outcome that a ta_run() the kernel made outside a turn (a replay,
 * through its session filter) reported for a message the kernel admitted
 * (a correlation other than 0): the scheduler acts on it at the start of
 * the next turn, as on the turn's own outcomes, so its send() settles and
 * its hooks run then. Outcomes with correlation 0 are core's or the
 * caller's own, and ignored. Allocates to keep it; without memory it acts
 * on it at once, and a record it would write is lost.
 */
void tah_sched_outcome_outside(tah_sched *sched, const ta_input_outcome *outcome);

/*
 * An operator abandoned the actor's macrostep (core ADR 0082): the send()
 * that follows it settles as ABANDONED, and the actor stands between
 * macrosteps again (ready or idle), a quarantined one included, since its
 * stopped macrostep is gone. TA_ERR_NOT_FOUND for an actor the table does
 * not know.
 */
ta_result tah_sched_abandoned(tah_sched *sched, ta_actor_id actor);

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
/*
 * Cancels a pending waiter (abort or timeout). A message not yet taken is
 * withdrawn, from its mailbox and from core's pool, so it is never run; one
 * already on the dead list is still reported, without its waiter; one
 * already taken is not undone. Returns true when the waiter was pending; it
 * is then SETTLING.
 */
bool tah_sched_cancel(tah_sched *sched, uint32_t index);

/* True when a turn has nothing to do. */
bool tah_sched_quiet(const tah_sched *sched);

/*
 * One bounded turn (ADR 0007 point 1): admit held mail, release the actors
 * out of memory last turn, run core until it is idle or the budgets are
 * spent, then report dead letters. The turn's records start empty and stay
 * valid until the turn arena is reset. Never fails: a failure is an
 * actor's, and becomes a record.
 */
void tah_sched_turn(tah_sched *sched, const tah_turn_params *params);
/* The records of the last turn, in the order they happened, until
 * tah_sched_release_records(); NULL when there are none. */
const tah_turn_record *tah_sched_records(const tah_sched *sched);
/*
 * Releases what the turn's records own (a dead letter's data) and forgets
 * the records. The turn calls it first, so a caller that runs turns back to
 * back leaks nothing; the host calls it once it has reported them, and
 * close calls it before core goes, since values outlive nothing.
 */
void tah_sched_release_records(tah_sched *sched);
