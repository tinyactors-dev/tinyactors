#pragma once
/*
 * Loading an unloaded actor when mail arrives for it (napi ADR 0020). This
 * module has no Node-API in it, so native tests drive it, and it moves to the
 * host kernel with the directory and the scheduler.
 *
 * A definition family can have a loader, registered here by name with its
 * `max_waiting`. Mail for a name in such a family that has no resident actor
 * (the directory's entry is unloaded, or there is none: the family is
 * virtual) starts a **load**: a record for the address with a mailbox of its
 * own (scheduler.h's tah_mailbox). Mail that arrives while the load is in
 * flight joins that mailbox, whether or not the actor has become resident
 * meanwhile, so it keeps its order; at most `max_waiting` messages wait, and
 * the rest are dead letters with reason `waiting-full`. There is one load
 * per address at a time.
 *
 * Each new load is a **request** for JavaScript, which calls the loader
 * after the turn and finishes the load with its outcome: loaded or spawned
 * (the waiting mail moves to the front of the actor's mailbox), or not
 * found, rejected, failed or timed out (it becomes dead letters). The
 * record's generation guards against a stale finish.
 *
 * **Wake-ups.** An unloaded actor's delayed sends travel in its image, so a
 * host registers the image's next deadline for its address. When that time
 * comes and the address still has no resident actor and no load, a load
 * starts with `why` = timer and an empty mailbox. The wake-ups are a
 * min-heap by due time; replacing an address's wake-up leaves the old heap
 * entry in place, and it is skipped when it comes up.
 *
 * **Mail from charts** waits in core (core ADR 0099), which asks the host to
 * load its target once per destination: tah_loads_ask() starts a load for
 * the request, or joins the one in flight, and the load answers core when
 * it finishes (tah_system_finish_load()). Its mailbox holds only the mail
 * JavaScript posted.
 *
 * **Sessions** (napi ADR 0032 section 2). Mail a chart sends to a session
 * that has gone, of a family with a loader, starts a load with `why` =
 * session. Its address is the family and the session's text as the name,
 * `{ family, "<incarnation>.<session>.<family>" }`: a key, never a directory
 * entry. Mail for the same session while the load is in flight joins it. A
 * session this system cannot name (another incarnation's, or another family
 * holding its ID: core gives it session 0) starts a load of its own for each
 * request, since two such messages may be for different sessions.
 *
 * Every record and heap entry holds one reference to its address, given back
 * when it goes; tah_loads_close() gives back all of them while core is alive.
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "directory.h"
#include "host_metrics.h"
#include "scheduler.h"
#include "tinyactors.h"

/* SESSION: mail for a session that has gone, in a family with a loader
 * (napi ADR 0032 section 2). */
typedef enum { TAH_LOAD_WHY_MAIL = 0, TAH_LOAD_WHY_TIMER, TAH_LOAD_WHY_SESSION } tah_load_why;

typedef enum {
    TAH_LOAD_LOADED = 0, /* the loader returned an image */
    TAH_LOAD_SPAWNED,    /* the loader asked for a new actor */
    TAH_LOAD_NOT_FOUND,
    TAH_LOAD_REJECTED,   /* "not now" */
    TAH_LOAD_FAILED,
    TAH_LOAD_TIMED_OUT,
    TAH_LOAD_DONE,       /* a replay reached a final state: the actor is done (napi ADR 0033) */
    TAH_LOAD_OUTCOMES
} tah_load_outcome;

/* The outcome's name for metrics ("loaded", "not_found", ...). */
const char *tah_load_outcome_name(tah_load_outcome outcome);
/* The dead-letter reason a load that ended with `outcome` gives its mail. */
tah_dead_reason tah_load_dead_reason(tah_load_outcome outcome);

typedef struct tah_load tah_load;

typedef struct tah_loads tah_loads;

void tah_loads_init(tah_loads *loads, ta_allocator allocator);
/*
 * Gives back every address reference and frees everything. The waiting
 * envelopes are the scheduler's: tah_sched_discard_mail() releases them.
 * Call it while core is alive.
 */
void tah_loads_close(tah_loads *loads);

/*
 * Withdraws the waiting envelope at `index` from its load's mailbox, when
 * it is there: a send() waiting for it was cancelled. False when it is not
 * waiting (it is in an actor's mailbox or on the dead list).
 */
bool tah_loads_withdraw(tah_loads *loads, tah_sched *sched, uint32_t index);

/* Registers `family`'s loader limit; `max_waiting` 0 removes the loader. */
ta_result tah_loads_set_family(tah_loads *loads, ta_str family, uint32_t max_waiting);

/*
 * Routes a message for the named `address` (router.h): into its load's
 * mailbox when a load is in flight; otherwise, when it has no resident actor
 * and its family has a loader, a new load (and a request) with the message
 * as its first. TA_ERR_NOT_FOUND: neither, and the caller routes as before.
 * TA_OK: accepted, perhaps as a `waiting-full` dead letter. TA_ERR_NOMEM:
 * nothing accepted.
 */
typedef struct {
    tah_sched *sched;
    ta_address *address;
    const tah_message *message;
    bool resident; /* the directory has a resident actor at `address` */
    uint64_t now_ns;
} tah_load_route;
ta_result tah_loads_route(tah_loads *loads, const tah_load_route *route);

/*
 * Starts a load for a load request core owes (ta_load_request, router.h),
 * or joins the load in flight for `address` when `shared`: the load
 * remembers the request, which tah_system_finish_load() answers. Mail core
 * keeps waiting stays in core; `waiting` is how many, for the load's info.
 * TA_ERR_NOT_FOUND: the family has no loader, and nothing changes;
 * TA_ERR_NOMEM: nothing changes.
 */
typedef struct {
    ta_address *address; /* borrowed; the load retains it */
    tah_load_why why;
    bool shared;         /* later mail for the address joins it; false for a session this system cannot name */
    uint64_t request;
    uint32_t waiting;
    const ta_trace_parent *trace;
    uint64_t now_ns;
} tah_load_ask;
ta_result tah_loads_ask(tah_loads *loads, const tah_load_ask *ask);
/* The load request core owes that the load answers, or 0. */
uint64_t tah_loads_core_request(const tah_loads *loads, uint32_t index, uint32_t generation);
/* Whether `family` has a loader. */
bool tah_loads_family_loads(const tah_loads *loads, ta_str family);

/* True when a load for `address` is in flight. */
bool tah_loads_waiting(const tah_loads *loads, const ta_address *address);
/* Whether mail for `address` would start a load: its family has a loader,
 * or actors of it were offloaded (napi ADR 0020, core ADR 0074). */
bool tah_loads_would_load(const tah_loads *loads, const ta_address *address);

/* A load JavaScript has not been told about, oldest first. */
typedef struct {
    uint32_t index;
    uint32_t generation;
    const ta_address *address; /* the record's reference: valid until it finishes */
    tah_load_why why;
} tah_load_request;
bool tah_loads_next_request(tah_loads *loads, tah_load_request *out);
/* Loads JavaScript has not been told about yet. */
uint32_t tah_loads_pending_requests(const tah_loads *loads);

/* The load a token names, or NULL when it has finished (a stale token). */
tah_load *tah_loads_find(const tah_loads *loads, uint32_t index, uint32_t generation);

/* A load in flight, as a binding reports it: `address` NULL when the token
 * names none (it finished). Borrowed until the load finishes. */
typedef struct {
    const ta_address *address;
    uint32_t waiting;      /* messages in its mailbox, and those core keeps for it */
    ta_trace_parent trace; /* the first message's trace parent, or all zero */
} tah_load_info;
tah_load_info tah_loads_info(const tah_loads *loads, uint32_t index, uint32_t generation);

/* The loads' counters (napi's metrics() and flush()). */
typedef struct {
    uint32_t in_flight; /* loads started and not finished */
    uint32_t waiting;   /* messages in every load's mailbox */
    uint64_t outcomes[TAH_LOAD_OUTCOMES];
} tah_loads_stats;
void tah_loads_read_stats(const tah_loads *loads, tah_loads_stats *out);

/*
 * Finishes a load. LOADED and SPAWNED: `actor` must be the resident actor
 * at the address, or for a session's load any resident actor, which is the
 * new instance (TA_ERR_NOT_FOUND otherwise, and nothing changes); the
 * waiting mail moves to the front of its mailbox, and its wake-up goes.
 * Other outcomes: the waiting mail becomes dead letters. Records the outcome
 * and the load's duration in `metrics`. TA_ERR_STALE: the token names no
 * load in flight. TA_ERR_NOMEM: the hand-over could not reserve the actor's
 * ready-queue entry, so the mail became `load-failed` dead letters.
 */
typedef struct {
    tah_sched *sched;
    const tah_directory *directory;
    tah_host_metrics *metrics;
    uint32_t index;
    uint32_t generation;
    tah_load_outcome outcome;
    ta_actor_id actor;
    uint64_t now_ns;
    uint64_t wall_ns; /* for the duration's exemplar */
} tah_load_finish;
ta_result tah_loads_finish(tah_loads *loads, const tah_load_finish *finish);

/* Wakes `address` at `due` (replacing an earlier wake-up for it). */
ta_result tah_loads_wake_at(tah_loads *loads, ta_address *address, ta_time due);
/* Forgets the wake-up of `address`, if any. */
void tah_loads_cancel_wake(tah_loads *loads, const ta_address *address);
/*
 * Fires the wake-ups due at `now`: each whose address has no resident actor
 * and no load, in a family with a loader, starts a load with `why` = timer
 * and an empty mailbox. The rest are dropped. A load that cannot start for
 * lack of memory keeps its wake-up for the next turn.
 */
typedef struct {
    const tah_directory *directory;
    ta_time now;
    uint64_t now_ns;
} tah_wake_fire;
void tah_loads_fire_wakes(tah_loads *loads, const tah_wake_fire *fire);
