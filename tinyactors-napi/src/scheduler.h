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
 * meantime is skipped when it is popped.
 *
 * One turn resumes up to `resumes` runnable actors, each with a microstep
 * budget and an action budget (core ADR 0061). What the turn learns that
 * JavaScript must hear about (an actor finished, an actor faulted) becomes a
 * record in the turn arena; the pump calls the hooks for them after the last
 * core call of the turn (ADR 0007 point 5).
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

#define TNA_SCHED_PAGE_BITS 12u
#define TNA_SCHED_PAGE_ENTRIES (1u << TNA_SCHED_PAGE_BITS) /* 4,096 entries, 64 KiB */

typedef enum {
    TNA_SCHED_NONE = 0,    /* no actor the addon knows */
    TNA_SCHED_IDLE,        /* between macrosteps, nothing to do */
    TNA_SCHED_READY,       /* has mail, is between macrosteps, and is on the ready queue */
    TNA_SCHED_RUNNABLE,    /* on the runnable queue */
    TNA_SCHED_QUARANTINED  /* faulted: out of scheduling until released (ADR 0008) */
} tna_sched_state;

enum {
    TNA_SCHED_BACKOFF = 1u << 0,  /* ran out of memory: skip one turn (ADR 0008) */
    TNA_SCHED_FINISHED = 1u << 1  /* reached a top-level final state */
};

typedef struct {
    uint32_t generation;
    uint8_t state; /* tna_sched_state */
    uint8_t flags;
    uint16_t reserved;
    uint32_t first; /* mailbox head envelope, 0 when empty */
    uint32_t last;  /* mailbox tail envelope, 0 when empty */
} tna_sched_entry;

/* Why a message became a dead letter (ADR 0008). */
typedef enum {
    TNA_DEAD_NOT_FOUND = 0, /* no resident actor has the target's ID */
    TNA_DEAD_DONE,          /* the target reached a top-level final state */
    TNA_DEAD_DESTROYED,     /* the target was destroyed or unloaded with mail waiting */
    TNA_DEAD_REFUSED,       /* core refused the message itself (for example a malformed event) */
    TNA_DEAD_UNLOADED       /* a named target that is unloaded; loading it is ROADMAP step 14 */
} tna_dead_reason;

/*
 * One queued message. `event` holds its own references (data, named origin).
 * `address` is set only for a dead letter to a named target that had no
 * resident actor; it holds a reference too.
 */
typedef struct {
    ta_event event;
    ta_actor_id target;
    ta_address *address;
    uint32_t next;  /* the next envelope in its list, 0 when last */
    uint8_t reason; /* tna_dead_reason, while on the dead list */
    uint8_t live;   /* taken from the store */
} tna_envelope;

#define TNA_ENVELOPE_PAGE_BITS 10u
#define TNA_ENVELOPE_PAGE_ENTRIES (1u << TNA_ENVELOPE_PAGE_BITS) /* 1,024 envelopes */

/* Envelopes in fixed pages, addressed by index; index 0 is never issued. */
typedef struct {
    ta_allocator allocator; /* the envelopes category */
    tna_vec pages;          /* tna_envelope *, one per page */
    uint32_t issued;        /* indices handed out so far, index 0 included */
    uint32_t free_head;     /* free list, threaded through `next` */
    uint32_t live;
} tna_envelopes;

typedef enum {
    TNA_RECORD_DONE,       /* the actor reached a top-level final state */
    TNA_RECORD_FAULT,      /* a core call for the actor failed (ADR 0008) */
    TNA_RECORD_DEAD_LETTER /* a message whose target is gone or finished */
} tna_record_kind;

typedef struct tna_turn_record {
    tna_record_kind kind;
    ta_actor_id actor;     /* DEAD_LETTER: the target, or 0 when it was a name with no actor */
    /* DEAD_LETTER to a named target with no actor: its text, copied into the turn arena. */
    bool has_address;
    ta_actor_address address;
    const char *operation; /* FAULT: "resume", "deliver" */
    ta_result code;        /* FAULT: core's result */
    ta_event_name_id event; /* DEAD_LETTER */
    tna_dead_reason reason; /* DEAD_LETTER */
    struct tna_turn_record *next;
} tna_turn_record;

typedef struct {
    ta_allocator allocator; /* the scheduling category */
    tna_vec pages;          /* tna_sched_entry *, one per page */
    tna_ring runnable;      /* ta_actor_id */
    tna_ring ready;         /* ta_actor_id */
    tna_envelopes envelopes;
    uint32_t known;         /* entries not in state NONE */
    /* Accepted messages not yet delivered or reported: mailboxes plus the dead list. */
    uint32_t queued;
    uint32_t max_queued;    /* 0: no limit (ADR 0005 point 9) */
    /* Dead letters waiting to be reported, threaded through `next`. */
    uint32_t dead_first;
    uint32_t dead_last;
    uint32_t dead_count;
    uint64_t dead_letters;  /* reported so far, with or without a hook */
    uint64_t delivered;     /* messages core accepted */
    /* The records of the current turn, in the turn arena. */
    tna_turn_record *records;
    tna_turn_record *last_record;
    uint32_t record_count;
} tna_sched;

typedef struct {
    uint32_t resumes;    /* resume calls in one turn */
    uint32_t messages;   /* deliveries in one turn, and dead letters reported */
    uint32_t microsteps; /* core's `budget` for one resume */
    uint32_t actions;    /* core's `action_budget` for one resume; 0: no limit */
} tna_turn_budgets;

typedef struct {
    ta_system *core;
    ta_arena *arena;   /* the turn arena: records live here */
    bool *inside_core; /* the reentrancy flag (ADR 0013 point 4) */
    tna_turn_budgets budgets;
} tna_turn_params;

/* `allocator` for the table and queues, `envelopes` for the envelope store. */
void tna_sched_init(tna_sched *sched, ta_allocator allocator, ta_allocator envelopes);
/*
 * Releases every queued message's references. Call it while core is still
 * alive (a named origin is a core handle), before ta_system_destroy().
 */
void tna_sched_discard_mail(tna_sched *sched);
void tna_sched_destroy(tna_sched *sched);

/* The entry of `slot`, or NULL when its page does not exist. */
tna_sched_entry *tna_sched_entry_at(const tna_sched *sched, uint32_t slot);
/* The entry of the actor `actor` names, or NULL when the addon knows another
 * actor (or none) in that slot. */
tna_sched_entry *tna_sched_lookup(const tna_sched *sched, ta_actor_id actor);

/*
 * A new actor (spawned, loaded, reconstructed, prepared): fill its entry and
 * put it on the runnable queue for bootstrap. TA_ERR_NOMEM leaves the table
 * as it was.
 */
ta_result tna_sched_add(tna_sched *sched, ta_actor_id actor);
/* The actor is gone (destroyed, unloaded): dead-letter its mailbox and clear its entry. */
void tna_sched_remove(tna_sched *sched, ta_actor_id actor, tna_dead_reason reason);

/*
 * Accepts a message for `target` (ADR 0005 point 3). On TA_OK the envelope
 * owns `event`'s references; otherwise the caller keeps them.
 * - A target the table does not know (stale or unknown ID) or one that has
 *   finished: accepted as a dead letter at once.
 * - TA_ERR_BUSY: the queue is at max_queued; nothing was accepted.
 * - TA_ERR_NOMEM: nothing was accepted.
 */
ta_result tna_sched_post(tna_sched *sched, ta_actor_id target, const ta_event *event);
/*
 * Accepts a message that is a dead letter already: its target is a name the
 * directory has no resident actor for (ADR 0006 point 4). On TA_OK the
 * envelope owns `event`'s references and the `address` reference.
 * TA_ERR_BUSY and TA_ERR_NOMEM as for tna_sched_post().
 */
typedef struct {
    ta_address *address;
    const ta_event *event;
    tna_dead_reason reason;
} tna_dead_post;
ta_result tna_sched_post_dead(tna_sched *sched, const tna_dead_post *post);
/* The envelope at `index` (for tests and the router), or NULL. */
tna_envelope *tna_sched_envelope(const tna_sched *sched, uint32_t index);
/* Messages waiting in `actor`'s mailbox. */
uint32_t tna_sched_mailbox_count(const tna_sched *sched, ta_actor_id actor);
/* Puts a quarantined actor back on the runnable queue (ADR 0008). */
ta_result tna_sched_release(tna_sched *sched, ta_actor_id actor);

/* True when a turn has nothing to do. */
bool tna_sched_quiet(const tna_sched *sched);

/*
 * One bounded turn: resume runnable actors, deliver to ready actors, then
 * report dead letters (ADR 0007 point 1). The turn's records start empty
 * and stay valid until the turn arena is reset. Never fails: a failure is an
 * actor's, and becomes a record.
 */
void tna_sched_turn(tna_sched *sched, const tna_turn_params *params);
