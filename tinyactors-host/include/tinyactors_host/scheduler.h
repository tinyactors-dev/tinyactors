#pragma once
/*
 * Scheduling (ADRs 0005, 0007, 0008). This module has no Node-API in it, so
 * native tests drive it directly.
 *
 * The scheduling table has one 16-byte entry per slot of core's actor table
 * (ADR 0002). It grows in fixed pages that are never moved or copied. An
 * entry holds host-owned state that core does not have: the generation of the
 * actor the addon knows in that slot, its scheduling state, a flag or two,
 * and the first and last envelope of its mailbox.
 *
 * Mailboxes (ADR 0005): an envelope is a record in the system's envelope
 * store, addressed by a 32-bit index (0 means none). A mailbox is the list
 * threaded through envelopes from the entry's `first` to its `last`; an
 * empty mailbox costs nothing beyond the entry. Freed envelopes go on the
 * store's free list, so after warm-up a post makes no allocator call
 * (ADR 0014). An idle actor that gets mail becomes `ready` and joins the
 * ready queue; an actor in any other state only collects mail. A turn
 * delivers the head envelope of up to `messages` ready actors, acting on
 * ta_actor_deliver()'s result alone (tinyactors-core ADR 0055).
 *
 * Dead letters: mail whose target is gone or finished moves to the dead
 * list, and each turn reports at most `messages` of them as records, so one
 * dead actor with a deep mailbox cannot make a turn long. Mail for a stale
 * ID is a dead letter at once, so it never joins the mailbox of the slot's
 * next actor.
 *
 * The runnable queue holds the actors whose macrostep has something to do:
 * a bootstrap, a delivered event, or a macrostep paused by a budget. An actor
 * stays `runnable` while its macrostep runs, so work that arrives for it then
 * only waits. A queue entry whose actor was destroyed or replaced in the
 * meantime is skipped when it is popped. Core holds the inputs themselves
 * (tinyactors-core ADR 0099): ta_actor_deliver() admits an event into core's
 * pool and ta_run() runs what was admitted, in the order it was admitted, so a
 * resume here is a call of ta_run() whose notices settle every actor they
 * name, not just the one that was popped.
 *
 * send() waiters (ADR 0005 point 11): a message may carry a waiter, the
 * native half of a send() promise. While the message is queued the waiter
 * names its envelope; when core accepts the message the waiter moves to the
 * `inflight` map, keyed by the actor that runs the macrostep; when that
 * macrostep ends (stable, done, a fault, or the actor is gone) or the message
 * becomes a dead letter, the scheduler queues a settlement. The Node-API layer
 * pops settlements after the turn and settles the promises. Waiters are
 * reused through a free list, so a send makes no allocator call after
 * warm-up.
 *
 * One turn calls ta_run() for up to `resumes` runnable actors, each call with
 * a microstep budget and an action budget (core ADR 0061); a call that finds
 * the actor already settled by an earlier one does nothing. The turn stops
 * once `resumes` inputs are completed: it drops the entries of actors those
 * calls settled, leaves the first entry that still needs a call and everything
 * behind it in order, so its cost is its inputs plus the entries they settled,
 * never the queue's length. What the turn learns that JavaScript must hear
 * about (an actor finished, an actor faulted) becomes a record in the turn
 * arena; the pump calls the hooks for them after the last core call of the
 * turn (ADR 0007 point 5).
 *
 * Operator transitions (ADR 0026, core ADR 0082): a message may carry one. It
 * queues behind the mail before it, like any message, and reaches
 * ta_actor_deliver() with it. A jump the actor no longer accepts (its source
 * is not active by then, or the actor runs another revision) is a dead letter
 * with reason `refused` and does not quarantine the actor: the operator's
 * input is at fault, not the actor.
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
    TAH_SCHED_IDLE,        /* between macrosteps, nothing to do */
    TAH_SCHED_READY,       /* has mail, is between macrosteps, and is on the ready queue */
    TAH_SCHED_RUNNABLE,    /* on the runnable queue */
    TAH_SCHED_QUARANTINED  /* faulted: out of scheduling until released (ADR 0008) */
} tah_sched_state;

enum {
    TAH_SCHED_BACKOFF = 1u << 0,  /* ran out of memory: skip one turn (ADR 0008) */
    TAH_SCHED_FINISHED = 1u << 1, /* reached a top-level final state */
    /* A replaying actor (kernel ADR 0006): the replay resumes it itself,
     * and a turn leaves it alone until it is published. */
    TAH_SCHED_REPLAYING = 1u << 5,
    /* READY or RUNNABLE, but on neither queue: putting it back ran out of
     * memory. The next turn puts it on the queue its state names, and the
     * scheduler is not quiet until it has. */
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
    /* Mail that waited for a loader (napi ADR 0020): */
    TAH_DEAD_REJECTED,      /* the loader answered "not now" */
    TAH_DEAD_LOAD_FAILED,   /* the loader threw, or its result could not be used */
    TAH_DEAD_LOAD_TIMEOUT,  /* the loader did not settle in time */
    TAH_DEAD_WAITING_FULL   /* the address already had maxWaiting messages waiting */
} tah_dead_reason;

/* The reason's name as JavaScript sees it ("not-found", "load-timeout", ...). */
const char *tah_dead_reason_name(tah_dead_reason reason);

typedef struct tah_mailbox tah_mailbox;

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
    ta_actor_id target;     /* unused for a named post */
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
} tah_message;

typedef enum {
    TAH_WAITER_FREE = 0,
    TAH_WAITER_QUEUED,   /* its message waits in a mailbox or on the dead list */
    TAH_WAITER_INFLIGHT, /* core accepted the message; its macrostep runs */
    TAH_WAITER_SETTLING  /* settled or cancelled; the Node-API layer gives it back */
} tah_waiter_state;

typedef struct tah_waiter tah_waiter;

typedef enum {
    TAH_SETTLE_STABLE, /* the message's macrostep ended with the actor stable */
    TAH_SETTLE_DONE,   /* ... with the actor finished */
    TAH_SETTLE_RUNAWAY, /* ... or reached the microstep limit and was abandoned (core ADR 0099 section 7); the actor is stable */
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
    const char *operation; /* FAULT: "resume", "deliver" */
    ta_result code;        /* FAULT: core's result */
    /* FAULT: the macrostep reached the microstep limit and core abandoned it (ADR 0103 section 8). `operation` is
     * "run" and `code` is unused; the actor is not quarantined, and its parent is not told. */
    bool runaway;
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
 * it; a macrostep that ended; an actor that settled or finished. `message`
 * is the envelope, borrowed for the call. The observer must not call back
 * into the scheduler.
 */
typedef enum {
    TAH_NOTE_DELIVERING, /* ta_actor_deliver() is about to be offered `message` */
    TAH_NOTE_DELIVERED,  /* ta_actor_deliver() took `message` */
    TAH_NOTE_UNDELIVERED, /* ... or did not */
    TAH_NOTE_REPORTING,  /* ta_actor_report_invoke() is about to be offered the report `message` */
    TAH_NOTE_REPORTED,   /* ta_actor_report_invoke() took the report `message` */
    TAH_NOTE_FINISHED,   /* `actor` reached a top-level final state */
    TAH_NOTE_SETTLED,    /* `actor` ended a macrostep and is stable: its pending sends may have changed */
    TAH_NOTE_ENDED,      /* `actor`'s macrostep ended: stable, finished, or stopped by a fault (before SETTLED or FINISHED) */
    /* Mail between sessions never passes through this scheduler: core queues it (core ADR 0099 section 4). When an
     * observer is installed, core says when it takes such an input; `event` is borrowed until the next ta_run(). */
    TAH_NOTE_MAIL_TAKEN  /* `actor` took the event of mail (or of a recycled error) from another session */
} tah_note_kind;

typedef struct {
    tah_note_kind kind;
    ta_actor_id actor;
    ta_address *address;
    uint32_t envelope;
    const tah_envelope *message;
    const ta_event *event; /* MAIL_TAKEN */
} tah_sched_note;

typedef struct {
    void *self; /* NULL: no observer, and the scheduler notes nothing */
    void (*note)(void *self, const tah_sched_note *note);
} tah_sched_observer;

typedef struct tah_sched tah_sched;

typedef struct {
    uint32_t resumes;    /* ta_run() calls in one turn */
    uint32_t messages;   /* deliveries in one turn, and dead letters reported */
    uint32_t microsteps; /* core's `budget` for one ta_run() call */
    uint32_t actions;    /* core's `action_budget` for one ta_run() call; 0: no limit */
} tah_turn_budgets;

typedef struct {
    ta_system *core;
    ta_arena *arena;   /* the turn arena: records live here */
    bool *inside_core; /* the reentrancy flag (ADR 0013 point 4) */
    tah_turn_budgets budgets;
    /* Watched sessions and definitions (watch.h); NULL or empty: none. */
    tah_trace_watch *watch;
    /* Asked after each resume and delivery: true ends the turn there, and
     * what is left waits for the next one (napi ADR 0035's staging bound).
     * NULL: never. */
    bool (*stop)(void *context);
    void *stop_context;
    /* Called with stop_context after each call into core that may have traced
     * (a resume, a delivery, an invocation report): the kernel reads the tel
     * ring there (core ADR 0098). NULL: nothing. */
    void (*left_core)(void *context);
    /* Called with stop_context for each LOAD_NEEDED notice of a ta_run() call, after the call returned (core ADR
     * 0099 section 4): core is waiting for an address to become resident. NULL: such notices are ignored. */
    void (*load_needed)(void *context, const ta_run_notice *notice);
} tah_turn_params;

/* `allocator` for the table and queues, `envelopes` for the envelope store. */
void tah_sched_init(tah_sched *sched, ta_allocator allocator, ta_allocator envelopes);
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
 * A new actor (spawned, loaded, reconstructed, prepared): fill its entry and
 * put it on the runnable queue for bootstrap. TA_ERR_NOMEM leaves the table
 * as it was.
 */
ta_result tah_sched_add(tah_sched *sched, ta_actor_id actor);
/* The actor is gone (destroyed, unloaded): dead-letter its mailbox and clear its entry. */
void tah_sched_remove(tah_sched *sched, ta_actor_id actor, tah_dead_reason reason);
/*
 * A named actor was replaced in place (ROADMAP step 14): the mail still
 * waiting for `from` moves, in order, to the front of `to`'s mailbox, with
 * its send() waiters. That mail was accepted for the address `to` now
 * holds, and it is older than anything queued for `to` directly. An idle
 * `to` becomes ready. TA_ERR_NOT_FOUND when the table knows either actor
 * not; TA_ERR_NOMEM (nothing moved) when `to` cannot join the ready queue.
 */
typedef struct {
    ta_actor_id from;
    ta_actor_id to;
} tah_sched_transfer_params;
ta_result tah_sched_transfer(tah_sched *sched, const tah_sched_transfer_params *params);

/*
 * Accepts a message for `message->target` (ADR 0005 point 3). On TA_OK the
 * envelope owns the event's references and the waiter follows the message;
 * otherwise the caller keeps both.
 * - A target the table does not know (stale or unknown ID) or one that has
 *   finished: accepted as a dead letter at once.
 * - TA_ERR_BUSY: the queue is at max_queued, or the message's bytes would
 *   pass max_retained; nothing was accepted.
 * - TA_ERR_NOMEM: nothing was accepted.
 */
ta_result tah_sched_post(tah_sched *sched, const tah_message *message);
/*
 * Accepts a message that is a dead letter already: its target is a name the
 * directory has no resident actor for (ADR 0006 point 4). On TA_OK the
 * envelope owns `event`'s references and the `address` reference.
 * TA_ERR_BUSY and TA_ERR_NOMEM as for tah_sched_post().
 */
typedef struct {
    ta_address *address;
    const tah_message *message;
    tah_dead_reason reason;
} tah_dead_post;
ta_result tah_sched_post_dead(tah_sched *sched, const tah_dead_post *post);
/*
 * Accepts a message into the waiting mailbox `box` of `address` (napi ADR
 * 0020). The envelope takes its own reference to `address`. On TA_OK it
 * owns the event's references and the waiter follows the message, as for
 * tah_sched_post(); the limits of ADR 0005 point 9 count it. TA_ERR_BUSY
 * and TA_ERR_NOMEM as for tah_sched_post().
 */
typedef struct {
    tah_mailbox *box;
    ta_address *address;
    const tah_message *message;
} tah_waiting_post;
ta_result tah_sched_post_waiting(tah_sched *sched, const tah_waiting_post *post);
/*
 * The actor for the address was loaded: the waiting mail moves, in order,
 * to the front of `to`'s mailbox, with its send() waiters, as replacement
 * moves an incumbent's mail. It is older than anything posted to `to`
 * directly. An idle `to` becomes ready. TA_ERR_NOT_FOUND when the table
 * does not know `to`; TA_ERR_NOMEM (nothing moved) when `to` cannot join
 * the ready queue. `box` is empty afterwards on TA_OK.
 */
ta_result tah_sched_hand_over(tah_sched *sched, tah_mailbox *box, ta_actor_id to);
/* The load failed: every waiting message becomes a dead letter with `reason`. */
void tah_sched_bury_waiting(tah_sched *sched, tah_mailbox *box, tah_dead_reason reason);
/*
 * Withdraws the envelope at `index` from the waiting mailbox `box` and
 * frees it, as cancelling a send() does for mail in an actor's mailbox (ADR
 * 0005 point 11). False, changing nothing, when `box` does not hold it.
 */
bool tah_sched_withdraw_waiting(tah_sched *sched, tah_mailbox *box, uint32_t index);
/* The envelope at `index` (for tests), or NULL. */
tah_envelope *tah_sched_envelope(const tah_sched *sched, uint32_t index);
/*
 * Takes `actor` out of scheduling as a fault does (ADR 0008), for a fault
 * the host found outside a core call. True when it was not quarantined
 * before; release() puts it back.
 */
bool tah_sched_quarantine(tah_sched *sched, ta_actor_id actor);
/* Messages waiting in `actor`'s mailbox. */
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
} tah_queued_message;
/* The first envelope in `actor`'s mailbox, 0 when it is empty or unknown. */
uint32_t tah_sched_mailbox_first(const tah_sched *sched, ta_actor_id actor);
/* The message in `envelope`, an index a mailbox walk gave. */
tah_queued_message tah_sched_queued_message(const tah_sched *sched, uint32_t envelope);

/* The scheduler's counters, as a binding reports them (napi's stats() and metrics()). */
typedef struct {
    uint32_t runnable;    /* actors on the runnable queue */
    uint32_t ready;       /* actors on the ready queue */
    uint32_t quarantined; /* actors in quarantine now */
    uint32_t queued;      /* accepted messages not yet delivered or reported */
    uint64_t delivered;   /* messages core accepted */
    uint64_t dead_letters; /* dead letters reported so far */
    uint64_t dead_by_reason[TAH_DEAD_REASONS]; /* by tah_dead_reason */
    uint64_t faults[TAH_FAULT_CODES];          /* by -ta_result */
} tah_sched_stats;
void tah_sched_read_stats(const tah_sched *sched, tah_sched_stats *out);
/*
 * Core gave `actor` a macrostep to run outside a delivery (for example an
 * invocation reported unavailable, which queues an internal event): an idle
 * or ready actor becomes runnable. A runnable or quarantined actor is left
 * as it is. TA_ERR_NOT_FOUND for an actor the table does not know.
 */
ta_result tah_sched_wake(tah_sched *sched, ta_actor_id actor);
/* Puts a quarantined actor back on the runnable queue (ADR 0008). */
ta_result tah_sched_release(tah_sched *sched, ta_actor_id actor);

/*
 * An operator abandoned the actor's macrostep (core ADR 0082): the send()
 * that follows it settles as ABANDONED, and a runnable actor stands between
 * macrosteps again (ready or idle). Its entry in the runnable queue is
 * skipped when popped. A quarantined one stays quarantined.
 * TA_ERR_NOT_FOUND for an actor the table does not know.
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
 * Cancels a pending waiter (abort or timeout). A message still in a mailbox
 * is withdrawn, so it is never delivered; one already on the dead list is
 * still reported, without its waiter; one already delivered is not undone.
 * Returns true when the waiter was pending; it is then SETTLING.
 */
bool tah_sched_cancel(tah_sched *sched, uint32_t index);

/* True when a turn has nothing to do. */
bool tah_sched_quiet(const tah_sched *sched);

/*
 * One bounded turn: resume runnable actors, deliver to ready actors, then
 * report dead letters (ADR 0007 point 1). The turn's records start empty
 * and stay valid until the turn arena is reset. Never fails: a failure is an
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
