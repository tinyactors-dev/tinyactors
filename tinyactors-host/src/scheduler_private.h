#pragma once
/*
 * The kernel's own layout of what tinyactors_host/scheduler.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/scheduler.h"
#include "events_private.h"

/*
 * An actor's mailbox lists the messages its macrostep has not taken, in the
 * order they were accepted; while a macrostep runs one, that one is at the
 * head, TAKEN, out of every count and view.
 */
struct tah_sched_entry {
    uint32_t generation;
    uint8_t state; /* tah_sched_state */
    uint8_t flags;
    /* 1: on the held queue, with mail in its mailbox that core has not
     * admitted yet; 0 for a stale entry of the queue, which is skipped. */
    uint8_t held;
    uint8_t reserved;
    uint32_t first; /* mailbox head envelope, 0 when empty */
    uint32_t last;  /* mailbox tail envelope, 0 when empty */
};

/* Where a kernel envelope stands; see tah_envelope. */
typedef enum {
    TAH_ENVELOPE_HELD = 0,  /* in its target's mailbox; core has not admitted it */
    /* in core's pool, not yet taken, and in its target's mailbox; a message
     * posted by address is in none until core takes it */
    TAH_ENVELOPE_ADMITTED,
    TAH_ENVELOPE_TAKEN,     /* taken by its target's macrostep, out of the mailbox */
    /* admitted for an actor the kernel removed inside a ta_run(), before
     * the turn read what core did with it: the outcome makes it a dead letter */
    TAH_ENVELOPE_ORPHANED,
    TAH_ENVELOPE_DEAD       /* on the dead list */
} tah_envelope_phase;

/*
 * One message from its post until core reports what became of it. `event`
 * holds its own references (data, named origin) also while core holds its
 * own, so a refused message's data reaches its dead letter. `address` is
 * set only for a message posted by address (core ADR 0099), which names no
 * actor until core takes it; it holds a reference too. `jump` is set
 * only for a message that carries an operator transition, which only a
 * message to an actor ID does, so the two share their room and `has_jump`
 * says which it is. Core names it by its index (the correlation).
 */
struct tah_envelope {
    ta_event event;
    ta_actor_id target;
    union {
        ta_address *address;
        tah_jump *jump;
    };
    /* The trace parent the message's macrostep runs under (core ADR 0067),
     * from the scheduler's free list, or NULL: only traced mail pays for
     * one. */
    ta_trace_parent *trace;
    uint32_t next;   /* the next envelope in its list, 0 when last */
    uint32_t waiter; /* the send() waiting for this message, or 0 */
    uint32_t bytes;  /* counted against max_retained (ADR 0005 point 9) */
    uint8_t reason;  /* tah_dead_reason, while on the dead list or orphaned */
    uint8_t report;  /* tah_report_kind: an error report, not a message */
    uint8_t phase;   /* tah_envelope_phase */
    uint8_t live : 1;     /* taken from the store */
    uint8_t has_jump : 1; /* `jump`, not `address` */
    /* Accepted past the limits (kernel ADR 0003): counted in `overflow`, not
     * in `queued` and `retained`, and held until it is moved in. */
    uint8_t overflow : 1;
    /* Posted by the host for an actor ID, not mail between actors: core's
     * full pool refuses it at the post instead of holding it. */
    uint8_t external : 1;
};

struct tah_waiter {
    uint32_t generation; /* part of the token JavaScript holds; never 0 */
    uint32_t envelope;   /* QUEUED and INFLIGHT: the message's envelope */
    ta_actor_id actor;   /* QUEUED: the target; INFLIGHT: the actor running it */
    uint32_t next_free;
    uint8_t state;       /* tah_waiter_state */
    void *host;          /* the Node-API layer's promise (a napi_deferred) */
};

/* Envelopes in fixed pages, addressed by index; index 0 is never issued. */
typedef struct tah_envelopes {
    ta_allocator allocator; /* the envelopes category */
    tah_vec pages;          /* tah_envelope *, one per page */
    uint32_t issued;        /* indices handed out so far, index 0 included */
    uint32_t free_head;     /* free list, threaded through `next` */
    uint32_t live;
} tah_envelopes;

struct tah_sched {
    ta_allocator allocator; /* the scheduling category */
    /* The names of the messages it holds, as text (events.h). */
    tah_names names;
    tah_vec pages;          /* tah_sched_entry *, one per page */
    /* The core whose pool admits the mail, the reentrancy flag set around
     * every call into it (ADR 0013 point 4), and the watched sessions whose
     * mail is marked as it is admitted (watch.h); set by
     * tah_sched_attach(). */
    ta_system *core;
    bool *inside_core;
    tah_trace_watch *watch;
    /* Core may have an input to run: something was admitted, created,
     * released or woken since a ta_run() last answered TA_RUN_IDLE. */
    bool core_work;
    /* The last admission of held mail found core's pool, or the limits,
     * full; room appears when an envelope is given back or core ends an
     * input. A scheduler whose only work is blocked held mail is quiet. */
    bool held_blocked;
    /* Entries in each state, for the stats. */
    uint32_t ready_count;
    uint32_t runnable_count;
    /* Faults the kernel found outside a turn (core refused a message when
     * it was posted), each { actor, code }, recorded at the next turn. */
    tah_vec faults_pending;
    /* Outcomes a ta_run() outside a turn (a replay's session filter)
     * reported for messages the kernel admitted (ta_input_outcome), acted
     * on at the next turn. */
    tah_vec outcomes_pending;
    /* Why the last load of an address failed, by the address's pointer
     * (tah_sched_note_failed_load): the reason the mail JavaScript posted
     * there is refused with. An entry goes when the address is asked about
     * again, before any of its later posts can be refused. */
    tah_map failed_loads;
    tah_envelopes envelopes;
    /* Trace parents of queued traced messages (ta_trace_parent). */
    tah_free_list traces;
    uint32_t known;         /* entries not in state NONE */
    uint32_t stranded;      /* entries marked TAH_SCHED_STRANDED, at most (a turn recounts) */
    /* Accepted messages not yet taken or reported: mailboxes and the dead list. */
    uint32_t queued;
    uint32_t max_queued;    /* 0: no limit (ADR 0005 point 9) */
    /* Mail between actors accepted past the limits and not moved in yet
     * (kernel ADR 0003): it counts in neither `queued` nor `retained`. */
    uint32_t overflow;
    /* ta_actor_id: actors with held mail, in the order they first held it
     * (kernel ADR 0003). */
    tah_ring held;
    /* ta_actor_id: actors whose input ran out of memory, released next turn
     * (ADR 0008's backoff). */
    tah_ring backoff;
    /* Retained-bytes estimates of queued messages, and their limit (0: none). */
    uint64_t retained;
    uint64_t max_retained;
    /* send() waiters: records by index (0 is never issued), a free list,
     * and settlements. */
    tah_vec waiters;        /* tah_waiter */
    uint32_t waiter_free;
    uint32_t waiters_live;
    tah_ring settled;       /* tah_settlement */
    /* Dead letters waiting to be reported, threaded through `next`. */
    uint32_t dead_first;
    uint32_t dead_last;
    uint32_t dead_count;
    uint64_t dead_letters;  /* reported so far, with or without a hook */
    uint64_t delivered;     /* messages core took */
    /* For metrics (napi ADR 0017 section 7): dead letters by tah_dead_reason,
     * faults by -ta_result, and the actors in quarantine now. */
    uint64_t dead_by_reason[TAH_DEAD_REASONS];
    uint64_t faults[TAH_FAULT_CODES];
    uint32_t quarantined;
    /* The records of the current turn, in the turn arena. */
    tah_turn_record *records;
    tah_turn_record *last_record;
    uint32_t record_count;
    /* The recorder's observer (kernel ADR 0006 section 2), or none. */
    tah_sched_observer observer;
};
