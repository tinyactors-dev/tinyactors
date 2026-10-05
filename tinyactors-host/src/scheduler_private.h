#pragma once
/*
 * The kernel's own layout of what tinyactors_host/scheduler.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/scheduler.h"

struct tah_sched_entry {
    uint32_t generation;
    uint8_t state; /* tah_sched_state */
    uint8_t flags;
    /* 1: on the room queue, READY with its head in the overflow (kernel ADR
     * 0003); 0 for a stale entry of the queue, which is skipped. */
    uint8_t parked;
    uint8_t reserved;
    uint32_t first; /* mailbox head envelope, 0 when empty */
    uint32_t last;  /* mailbox tail envelope, 0 when empty */
};

/*
 * A mailbox that belongs to an address, not an actor: mail waiting while a
 * loader brings the actor back (napi ADR 0020). Its envelopes have no
 * target yet; each holds a reference to the address.
 */
struct tah_mailbox {
    uint32_t first;
    uint32_t last;
    uint32_t count;
};

/*
 * One queued message. `event` holds its own references (data, named origin).
 * `address` is set only for a dead letter to a named target that had no
 * resident actor; it holds a reference too. `jump` is set only for a message
 * that carries an operator transition, which only a message to an actor ID
 * does, so the two share their room and `has_jump` says which it is.
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
    uint8_t reason;  /* tah_dead_reason, while on the dead list */
    uint8_t report;  /* tah_report_kind: an error report, not a message */
    uint8_t live : 1;     /* taken from the store */
    uint8_t has_jump : 1; /* `jump`, not `address` */
    /* Accepted past the limits (kernel ADR 0003): counted in `overflow`, not
     * in `queued` and `retained`, and not delivered until it is moved in. */
    uint8_t overflow : 1;
};

struct tah_waiter {
    uint32_t generation; /* part of the token JavaScript holds; never 0 */
    uint32_t envelope;   /* QUEUED: the message's envelope */
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

/*
 * What a caller of tah_sched_run wants to learn about one actor's input: the
 * replay driver, which runs an actor the scheduler leaves alone. Notices for
 * that actor are written here and not settled by the scheduler.
 */
typedef struct tah_run_watch {
    ta_actor_id actor;
    bool completed; /* an input of `actor` ended */
    bool done;      /* ... and the session finished */
    bool ignored;   /* ... and its event enabled no transition */
} tah_run_watch;

/*
 * Calls ta_run() once (more, while its notice array is full) with the turn's
 * budgets and settles every actor the notices name, as a resume used to settle
 * one: the waiter of a send(), the entry's state, the recorder's notes, a
 * record when a session finished. `watch`, when given, receives the notices of
 * its actor instead. The status of the last call is stored in `*status`.
 * TA_ERR_NOMEM is ta_run()'s: the input stays where it was and a later call
 * continues it.
 */
ta_result tah_sched_run(tah_sched *sched, const tah_turn_params *params, tah_run_watch *watch, ta_run_status *status);

struct tah_sched {
    ta_allocator allocator; /* the scheduling category */
    tah_vec pages;          /* tah_sched_entry *, one per page */
    tah_ring runnable;      /* ta_actor_id */
    tah_ring ready;         /* ta_actor_id */
    tah_envelopes envelopes;
    /* Trace parents of queued traced messages (ta_trace_parent). */
    tah_free_list traces;
    /* Inputs the running turn may still complete (its `resumes`), while it
     * runs actors; `turn_bounded` is false outside the turn's first phase, as
     * in a replay's own calls, which no turn limit bounds. */
    uint32_t turn_left;
    bool turn_bounded;
    /* Count of entries the most recent turn's first phase popped from the runnable queue. It is bounded by the resume
     * limit, so tests can check that a turn's work does not grow with the queue's length. */
    uint32_t turn_visited;
    /* Calls of ta_run() the most recent turn made, in all its phases. Each call serves up to a full notice array of
     * inputs, so a turn's calls are bounded by the inputs it completes plus the refills; tests compare them with the
     * work asked for, in counts and not clocks. */
    uint32_t turn_run_calls;
    /* ta_run() calls since the system was created, for tests and stats. */
    uint64_t run_calls_total;
    uint32_t known;         /* entries not in state NONE */
    uint32_t stranded;      /* entries marked TAH_SCHED_STRANDED, at most (a turn recounts) */
    /* Accepted messages not yet delivered or reported: mailboxes plus the dead list. */
    uint32_t queued;
    uint32_t max_queued;    /* 0: no limit (ADR 0005 point 9) */
    /* Mail between actors accepted past the limits and not moved in yet
     * (kernel ADR 0003): it counts in neither `queued` nor `retained`. */
    uint32_t overflow;
    /* ta_actor_id: ready actors whose head waits in the overflow, in the
     * order they asked for room (kernel ADR 0003). */
    tah_ring room;
    /* Retained-bytes estimates of queued messages, and their limit (0: none). */
    uint64_t retained;
    uint64_t max_retained;
    /* send() waiters: records by index (0 is never issued), a free list,
     * the actors whose macrostep a waiter follows, and settlements. */
    tah_vec waiters;        /* tah_waiter */
    uint32_t waiter_free;
    uint32_t waiters_live;
    tah_map inflight;       /* ta_actor_id -> waiter index */
    tah_ring settled;       /* tah_settlement */
    /* Dead letters waiting to be reported, threaded through `next`. */
    uint32_t dead_first;
    uint32_t dead_last;
    uint32_t dead_count;
    uint64_t dead_letters;  /* reported so far, with or without a hook */
    uint64_t delivered;     /* messages core accepted */
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
