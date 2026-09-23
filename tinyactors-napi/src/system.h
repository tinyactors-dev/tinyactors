#pragma once
/*
 * The native system (ADR 0012, ADR 0014): one ta_system and everything the
 * addon keeps for it. This module has no Node-API in it, so native tests can
 * create, close and free systems directly; napi_system.{c,h} wraps it as the
 * JavaScript System class.
 *
 * Lifecycle:
 * - tna_system_create() allocates the record with malloc (it holds the
 *   counting allocator, so it cannot count itself), initializes the
 *   allocator and the scratch arenas, and creates the ta_system with the
 *   allocator's core lane as core's allocator.
 * - tna_system_close() is idempotent and authoritative: it destroys the
 *   ta_system and the arenas. After it, the allocator's bytes in use are
 *   zero unless something leaked, and tna_system_leaked_bytes() says how
 *   many.
 * - tna_system_free() closes the system if needed and frees the record.
 *
 * Scheduling (ADRs 0005, 0007): the system owns the scheduling table and
 * its queues (scheduler.h). Adding work asks for a turn through `wake`,
 * which the Node-API layer sets (pump.c) and native tests may set to anything;
 * a flag coalesces the requests until the turn runs.
 *
 * Routing (ADR 0006): the directory and the router are installed on core
 * when the system is created. Close uninstalls both, gives back the
 * directory's address references and the mailboxes' events, and only then
 * destroys core, because core frees its address records at the end of
 * ta_system_destroy().
 *
 * Later steps add more plumbing (values); each one is released by
 * tna_system_close() in the order ADR 0012 gives.
 */
#include <stdbool.h>
#include <stddef.h>

#include "allocator.h"
#include "directory.h"
#include "scheduler.h"
#include "tinyactors.h"

/* Defaults for SchedulingOptions (ADR 0007). */
#define TNA_DEFAULT_RESUMES_PER_TURN 1024u
#define TNA_DEFAULT_RESUME_BUDGET 64u
#define TNA_DEFAULT_ACTION_BUDGET 1024u
#define TNA_DEFAULT_MESSAGE_BUDGET 1024u

typedef struct tna_system tna_system;

/* Asks the host to run tna_system_turn() soon, never inside the caller. */
typedef void (*tna_wake_fn)(tna_system *system, void *context);

struct tna_system {
    tna_allocator allocator;
    tna_scratch scratch;
    ta_system *core;
    tna_sched sched;
    tna_directory directory; /* installed on core at creation (ADR 0006) */
    tna_turn_budgets budgets;
    tna_wake_fn wake;
    void *wake_context;
    bool turn_pending; /* wake was called and the turn has not run yet */
    bool closed;
    /*
     * Set around every core call (ADR 0013 point 4). A generated entry point
     * that finds it set throws "reentrant": data-model, I/O and invoker
     * callbacks may run JavaScript, but that JavaScript cannot call core.
     */
    bool inside_core;
    /* Bytes still in use when close finished; nonzero means a leak. */
    size_t leaked_bytes;
};

/* TA_ERR_NOMEM when the record or core cannot be allocated. */
ta_result tna_system_create(tna_system **out);
void tna_system_close(tna_system *system);
bool tna_system_closed(const tna_system *system);
size_t tna_system_leaked_bytes(const tna_system *system);
void tna_system_free(tna_system *system);

/*
 * Accepts a message for `target` and asks for a turn (ADR 0005). On TA_OK the
 * system owns `event`'s references. TA_ERR_BUSY: the queue is full
 * (max_queued); TA_ERR_NOMEM: nothing accepted.
 */
ta_result tna_system_post(tna_system *system, ta_actor_id target, const ta_event *event);

/* Asks for a turn unless one is pending or the system is quiet or closed. */
void tna_system_want_turn(tna_system *system);
/*
 * Runs one turn (ADR 0007 point 1). The turn's records stay in
 * system->sched until tna_system_end_turn(), which resets the turn arena and
 * asks for the next turn when work remains.
 */
void tna_system_turn(tna_system *system);
void tna_system_end_turn(tna_system *system);
/* No runnable or ready actors and no dead letters to report (step 8: nothing due). */
bool tna_system_quiet(const tna_system *system);
