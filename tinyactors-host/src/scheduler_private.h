#pragma once
/*
 * The kernel's own layout of what tinyactors_host/scheduler.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/scheduler.h"

/*
 * One send() promise's native half. Lifetime: record (see scheduler.h).
 * `token` is the input it follows while it is PENDING, 0 for a waiter whose
 * message became a verdict. `generation` is half of the token JavaScript
 * holds and never 0.
 */
struct tah_waiter {
    uint32_t generation;
    uint32_t next_free;
    uint64_t token;
    uint8_t state; /* tah_waiter_state */
    void *host;    /* the Node-API layer's promise (a napi_deferred) */
};

/*
 * The answer to a post that core refused, waiting for the next turn to
 * report it as a dead letter. It owns its references: the event's data (and
 * origin), and the address. Lifetime: verdict (see scheduler.h).
 */
typedef struct tah_verdict {
    ta_event event;
    ta_actor_id target;       /* the target actor, or 0 when it was a name */
    ta_address *address;      /* a reference, or NULL */
    ta_trace_parent trace;
    uint32_t waiter;          /* the send() that follows it, or 0 */
    uint32_t waiter_generation;
    uint8_t reason;           /* tah_dead_reason */
    uint8_t has_trace;
} tah_verdict;

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
 * budgets and settles every actor the notices name: the waiter of a send(),
 * the recorder's notes, a record when a session finished or ran away.
 * `watch`, when given, receives the notices of its actor instead. The status
 * of the last call is stored in `*status`. TA_ERR_NOMEM is ta_run()'s: the
 * input stays where it was and a later call continues it.
 */
ta_result tah_sched_run(tah_sched *sched, const tah_turn_params *params, tah_run_watch *watch, ta_run_status *status);

/*
 * What the kernel follows of one admitted input: its send() waiter, and the
 * bytes it estimated for it. The table of these is indexed by ta_input_slot()
 * of the token, so lookup costs one array access and the table allocates only
 * while it grows to the most inputs that were ever pending at once. `token` is
 * 0 for a slot that follows nothing; a slot is reused, so the whole token is
 * compared before an entry is trusted. Lifetime: the input's.
 */
typedef struct tah_follow {
    uint64_t token;
    uint32_t waiter; /* the send() that follows the input, or 0 */
    uint32_t bytes;  /* retained-bytes estimate counted for it, or 0 */
} tah_follow;

struct tah_sched {
    ta_allocator allocator; /* the scheduling category */
    ta_allocator envelopes; /* the envelopes category: waiter records and verdicts */
    /* send() waiters: records by index (0 is never issued) and a free list. */
    tah_vec waiters;        /* tah_waiter */
    uint32_t waiter_free;
    uint32_t waiters_live;
    /* The inputs the application follows, by the slot of their token (core's, ta_actor_deliver's `input`). */
    tah_vec follows;        /* tah_follow: only sends, and posts with bytes while max_retained != 0 */
    uint32_t followed;      /* entries in use */
    uint64_t retained_total;
    uint32_t max_queued;    /* 0: no limit (ADR 0005 point 9) */
    uint64_t max_retained;  /* 0: no limit */
    /* Sessions a replay drives itself (replay.c), by actor ID. */
    tah_map replaying;
    tah_ring verdicts;      /* tah_verdict: answers waiting for the next turn */
    tah_ring settled;       /* tah_settlement */
    /* Inputs the running turn may still finish (its `inputs`); `turn_bounded`
     * is false outside the turn, as in a replay's own calls, which no turn
     * limit bounds. */
    uint32_t turn_left;
    bool turn_bounded;
    /* Calls of ta_run() the most recent turn made. Each call serves up to a full notice array of inputs, so a turn's
     * calls are bounded by the inputs it finishes plus the refills; tests compare them with the work asked for, in
     * counts and not clocks. */
    uint32_t turn_run_calls;
    /* ta_run() calls since the system was created, for tests and stats. */
    uint64_t run_calls_total;
    uint64_t dead_letters;  /* reported so far, with or without a hook */
    uint64_t delivered;     /* messages core admitted, and mail its receivers ran */
    /* For metrics (napi ADR 0017 section 7): dead letters by tah_dead_reason, faults by -ta_result. */
    uint64_t dead_by_reason[TAH_DEAD_REASONS];
    uint64_t faults[TAH_FAULT_CODES];
    /* The records of the current turn, in the turn arena. */
    tah_turn_record *records;
    tah_turn_record *last_record;
    uint32_t record_count;
    /* The recorder's observer (kernel ADR 0006 section 2), or none. */
    tah_sched_observer observer;
};
