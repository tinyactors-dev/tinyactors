#pragma once
/*
 * Scheduling (ADRs 0005, 0007, 0008). This module has no Node-API in it, so
 * native tests drive it directly.
 *
 * The scheduling table has one 16-byte entry per slot of core's actor table
 * (ADR 0002). It grows in fixed pages that are never moved or copied. An
 * entry holds host-owned state that core does not have: the generation of the
 * actor the addon knows in that slot, its scheduling state, a flag or two,
 * and (step 6) the first and last envelope of its mailbox.
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
    TNA_SCHED_READY,       /* has mail and is between macrosteps (step 6) */
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
    uint32_t first; /* mailbox head envelope (step 6) */
    uint32_t last;  /* mailbox tail envelope (step 6) */
} tna_sched_entry;

typedef enum {
    TNA_RECORD_DONE,  /* the actor reached a top-level final state */
    TNA_RECORD_FAULT  /* a core call for the actor failed (ADR 0008) */
} tna_record_kind;

typedef struct tna_turn_record {
    tna_record_kind kind;
    ta_actor_id actor;
    const char *operation; /* FAULT: "resume", ... */
    ta_result code;        /* FAULT: core's result */
    struct tna_turn_record *next;
} tna_turn_record;

typedef struct {
    ta_allocator allocator; /* the scheduling category */
    tna_vec pages;          /* tna_sched_entry *, one per page */
    tna_ring runnable;      /* ta_actor_id */
    uint32_t known;         /* entries not in state NONE */
    /* The records of the current turn, in the turn arena. */
    tna_turn_record *records;
    tna_turn_record *last_record;
    uint32_t record_count;
} tna_sched;

typedef struct {
    uint32_t resumes;    /* resume calls in one turn */
    uint32_t microsteps; /* core's `budget` for one resume */
    uint32_t actions;    /* core's `action_budget` for one resume; 0: no limit */
} tna_turn_budgets;

typedef struct {
    ta_system *core;
    ta_arena *arena;   /* the turn arena: records live here */
    bool *inside_core; /* the reentrancy flag (ADR 0013 point 4) */
    tna_turn_budgets budgets;
} tna_turn_params;

void tna_sched_init(tna_sched *sched, ta_allocator allocator);
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
/* The actor is gone (destroyed, unloaded): clear its entry. */
void tna_sched_remove(tna_sched *sched, ta_actor_id actor);
/* Puts a quarantined actor back on the runnable queue (ADR 0008). */
ta_result tna_sched_release(tna_sched *sched, ta_actor_id actor);

/* True when a turn has nothing to do. */
bool tna_sched_quiet(const tna_sched *sched);

/*
 * One bounded turn over the runnable queue. The turn's records start empty
 * and stay valid until the turn arena is reset. Never fails: a failure is an
 * actor's, and becomes a record.
 */
void tna_sched_turn(tna_sched *sched, const tna_turn_params *params);
