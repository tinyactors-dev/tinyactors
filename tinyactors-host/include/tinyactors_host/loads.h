#pragma once
/*
 * Loading an unloaded actor when mail arrives for it (napi ADR 0020). This
 * module has no Node-API in it, so native tests drive it, and it moves to the
 * host kernel with the directory and the scheduler.
 *
 * A definition family can have a loader, registered here by name with its
 * `max_waiting`. Mail for a name in such a family that has no resident actor
 * (the directory's entry is unloaded, or there is none: the family is
 * virtual) starts a **load**: a record for the address. Every message that
 * waits for it waits in one place, core's waiting list for the address (core
 * ADR 0099 section 4), the host's own posts and the mail between sessions
 * alike, in the order they arrived. Core reports the first of them with a
 * LOAD_NEEDED notice, which starts the load here (tah_loads_wanted), unless
 * one is in flight already. There is one load per address at a time. At most
 * `max_waiting` messages wait for an address (tah_loads_limit() says what the
 * limit is, ta_address_waiting() how many wait); the rest of the host's posts
 * are dead letters with reason `waiting-full`, and chart mail is counted but
 * bounded by the pool alone.
 *
 * Each new load is a **request** for JavaScript, which calls the loader
 * after the turn and finishes the load with its outcome: loaded or spawned
 * (core moves its list when the actor becomes resident, or when
 * ta_address_loaded() names the instance of a session's key), or not found,
 * rejected, failed or timed out (core recycles its list: the host's posts
 * come back as REFUSED notices, and the senders of chart mail hear
 * error.communication). The record's generation guards against a stale
 * finish.
 *
 * **Wake-ups.** An unloaded actor's delayed sends travel in its image, so a
 * host registers the image's next deadline for its address. When that time
 * comes and the address still has no resident actor and no load, a load
 * starts with `why` = timer and an empty mailbox. The wake-ups are a
 * min-heap by due time; replacing an address's wake-up leaves the old heap
 * entry in place, and it is skipped when it comes up.
 *
 * **Sessions** (napi ADR 0032 section 2). Mail a chart sends to a session
 * that has gone, of a family with a loader, waits in core on a key address
 * made of the family and the session's text as the name,
 * `{ family, "<incarnation>.<session>.<family>" }`: a key, never a directory
 * entry. Its LOAD_NEEDED notice says so, and starts a load with `why` =
 * session. Mail for the same session while the load is in flight joins the
 * same list. A session this system cannot name (another incarnation's, or
 * another family holding its ID: core gives it session 0) waits on the key
 * '<incarnation>.0.<family>' like any other; only a session ID with no
 * family is undeliverable, and its sender hears error.communication at once.
 *
 * Every record and heap entry holds one reference to its address, given back
 * when it goes; tah_loads_close() gives back all of them while core is alive.
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "directory.h"
#include "host_metrics.h"
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

typedef struct tah_load tah_load;

typedef struct tah_loads tah_loads;

void tah_loads_init(tah_loads *loads, ta_allocator allocator);
/*
 * Gives back every address reference and frees everything. Core's waiting
 * lists are core's. Call it while core is alive.
 */
void tah_loads_close(tah_loads *loads);

/* Registers `family`'s loader limit; `max_waiting` 0 removes the loader. */
ta_result tah_loads_set_family(tah_loads *loads, ta_str family, uint32_t max_waiting);

/*
 * The most messages that may wait for `address`: the `max_waiting` of its
 * family's loader, or 0 when the family has none (core then answers the
 * LOAD_NEEDED notice at once, and the message is refused). O(1) for a system
 * with no loader, otherwise a search of the families. Never allocates.
 */
uint32_t tah_loads_limit(const tah_loads *loads, const ta_address *address);

/*
 * Core asked for `address` to be loaded: its first waiting input arrived (a
 * LOAD_NEEDED notice, core ADR 0099 section 4). TA_OK: a load for it is in
 * flight (this notice's or an earlier one's), and its request will reach
 * JavaScript. TA_ERR_NOT_FOUND: the address's family has no loader, so nothing
 * would bring it back and the caller answers core at once with
 * ta_address_load_failed(). TA_ERR_NOMEM: the record could not be made, and
 * the caller answers the same. `session` says the address is a session's key
 * (the load's `why` is then session); `trace` is the first input's.
 */
typedef struct {
    ta_address *address;
    const ta_trace_parent *trace;
    bool session;
    uint64_t now_ns;
} tah_load_wanted;
ta_result tah_loads_wanted(tah_loads *loads, const tah_load_wanted *wanted);

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
    ta_trace_parent trace; /* the first message's trace parent, or all zero */
} tah_load_info;
tah_load_info tah_loads_info(const tah_loads *loads, uint32_t index, uint32_t generation);

/* The loads' counters (napi's metrics() and flush()). */
typedef struct {
    uint32_t in_flight; /* loads started and not finished */
    uint32_t waiting;   /* messages waiting on core's lists for the addresses of the loads in flight */
    uint64_t outcomes[TAH_LOAD_OUTCOMES];
} tah_loads_stats;
/* Reads the counters; `core` is asked how many messages wait for each load's address (O(waiting messages)). */
void tah_loads_read_stats(const tah_loads *loads, const ta_system *core, tah_loads_stats *out);

/*
 * Finishes a load. LOADED and SPAWNED: `actor` must be the resident actor
 * at the address, or for a session's load any resident actor, which is the
 * new instance (TA_ERR_NOT_FOUND otherwise, and nothing changes); its
 * wake-up goes. Core has already moved its waiting list to an actor that
 * holds the address; for a session's key, which no actor holds, this call
 * names the instance to core (ta_address_loaded). Other outcomes: core
 * recycles its list (ta_address_load_failed), so every message that waited
 * for the address is refused. Records the outcome and the load's duration in
 * `metrics`. TA_ERR_STALE: the token names no load in flight. Never
 * allocates.
 */
typedef struct {
    ta_system *core;
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
