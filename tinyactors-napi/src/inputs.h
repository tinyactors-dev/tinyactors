#pragma once
/*
 * The input log (napi ADR 0032 section 6, ADR 0033): a write-ahead log of
 * what core accepted for the named actors of the logged families, recorded
 * as it happens and handed to JavaScript once per turn, as trace records
 * are (ADR 0015), with the time napi next needs each touched actor back.
 *
 * A record names the actor by its address and carries the logical time
 * core accepted the input at. Its text (the address, the origin, the send
 * and invoke IDs) is copied into the log's own arena, and its value (an
 * event's data, a report's data, a spawn's binding) is retained, so a
 * record outlives the envelope and the turn it came from. The Node-API
 * layer takes the records after the turn's services (tna_inputs_take),
 * turns them into JavaScript objects, and gives them back
 * (tna_inputs_release), which releases the values and resets the arena.
 *
 * The scheduler tells the log about mail (scheduler.h, tna_sched_observer):
 * a message accepted into a mailbox is `queued` with a sequence number
 * unique in the system, kept by envelope index until the message is
 * delivered, when its `event` (or `jump`) record carries the same number,
 * or leaves undelivered, when a `dropped` record does. A delayed send a
 * logged actor dispatches is `fired` (tna_inputs_fired). The host's own
 * operations (spawn, withdraw, abandon, release, evict) are recorded by the
 * Node-API layer through tna_inputs_host().
 *
 * Wakes (ADR 0033 section 4): every address a record names in a turn is
 * touched, and when the batch is taken its wake is worked out: the earlier
 * of its first pending delayed send's due time and the time its oldest
 * undelivered queued message was accepted, or none for an actor that is
 * resident with neither, or finished. An address with no resident actor
 * and no queued mail keeps the wake it had (nothing is reported).
 *
 * With no log installed the scheduler has no observer and nothing here
 * runs: a post, a delivery or a turn costs nothing more.
 *
 * This module has no Node-API in it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "scheduler.h"
#include "tinyactors.h"

typedef struct tna_system tna_system;

typedef enum {
    TNA_INPUT_SPAWNED = 0,
    TNA_INPUT_QUEUED,
    TNA_INPUT_EVENT,
    TNA_INPUT_REPORT,
    TNA_INPUT_WITHDRAWN,
    TNA_INPUT_ABANDONED,
    TNA_INPUT_RELEASED,
    TNA_INPUT_EVICTED,
    TNA_INPUT_FINISHED,
    TNA_INPUT_FIRED,   /* a delayed send was dispatched (ADR 0033 section 3) */
    TNA_INPUT_DROPPED, /* a queued message will not be delivered */
    TNA_INPUT_JUMP,    /* an operator transition (core ADR 0082) */
    TNA_INPUT_KINDS
} tna_input_kind;

/* The kind's name as JavaScript sees it ("queued", "event", ...). */
const char *tna_input_kind_name(tna_input_kind kind);

typedef struct tna_input {
    tna_input_kind kind;
    ta_time time;
    ta_actor_address address; /* copied */
    ta_actor_id actor;        /* 0 for mail that waits for a load */
    /* QUEUED: its number; EVENT, JUMP, DROPPED: the number of its queued
     * record, 0 when none was made. */
    uint64_t seq;
    /* QUEUED, EVENT, JUMP. */
    ta_event_name_id name;
    ta_event_type type;
    /* QUEUED, EVENT, REPORT: the data; SPAWNED: the binding. Retained. */
    bool has_value;
    ta_value value;
    /* Copied text; ptr NULL when absent. */
    ta_str send_id;     /* QUEUED, EVENT, JUMP; WITHDRAWN, FIRED: the send */
    ta_str origin;      /* QUEUED, EVENT */
    ta_str origin_type; /* QUEUED, EVENT */
    ta_str invoke_id;   /* QUEUED, EVENT, REPORT */
    ta_str reason;      /* REPORT */
    ta_str revision;    /* SPAWNED */
    bool ends;          /* REPORT */
    /* JUMP: the definition its state IDs belong to, and the transition. */
    ta_definition_id definition;
    ta_state_id source;
    uint32_t target_count;
    ta_state_id *targets; /* in the arena */
    struct tna_input *next;
} tna_input;

/* A wake worked out for a batch: `has_at` false is none (JavaScript's null). */
typedef struct {
    ta_actor_address address; /* borrowed from the log's arena */
    bool has_at;
    ta_time at;
} tna_input_wake;

/* An address touched this turn, retained until the batch is taken. */
typedef struct {
    ta_address *address;
    bool fixed;  /* `has_at` and `at` were given (an eviction, a finish) */
    bool has_at;
    ta_time at;
} tna_input_touch;

typedef struct {
    bool enabled;
    ta_allocator allocator; /* the other category: family names, the arena's blocks */
    ta_arena arena;         /* records and their text, until tna_inputs_release() */
    bool arena_ready;
    tna_vec families;       /* ta_str, each allocated */
    tna_map seqs;           /* envelope index -> seq of its queued record */
    tna_map queued_address; /* envelope index -> its address (ta_address *, retained) */
    tna_map mail_count;     /* address -> queued messages not yet delivered */
    tna_map mail_since;     /* address -> when the first of them was accepted */
    tna_vec touched;        /* tna_input_touch, this turn's addresses */
    tna_map touched_index;  /* address -> index in `touched` */
    /* tna_inputs_redeliver(): the next queued message is not recorded, and
     * takes this acceptance time; its seq is written to `redelivered`. */
    bool redelivering;
    ta_time redeliver_since;
    uint64_t redelivered;
    uint64_t next_seq;
    tna_input *first;
    tna_input *last;
    uint32_t count;
    uint64_t lost;          /* records lost for lack of memory */
} tna_inputs;

void tna_inputs_init(tna_inputs *inputs, ta_allocator allocator);
/* Releases every record and the family list; the log is off afterwards.
 * Call it while core is alive (records hold values). */
void tna_inputs_destroy(tna_inputs *inputs);

/*
 * Turns the log on for `families` (count 0 with families NULL turns it
 * off). Installs or removes the scheduler's observer and clears its
 * per-actor cache. Records not taken yet are kept. TA_ERR_NOMEM leaves the
 * log as it was.
 */
ta_result tna_inputs_configure(tna_system *system, const ta_str *families, uint32_t count);

/* Whether `actor` is a named actor of a logged family; writes its address. */
bool tna_inputs_logged(tna_system *system, ta_actor_id actor, ta_actor_address *address);

/* What the host did, for tna_inputs_host(). */
typedef struct {
    tna_input_kind kind; /* SPAWNED, WITHDRAWN, ABANDONED, RELEASED, EVICTED */
    ta_actor_id actor;   /* resident when the record is made */
    const ta_value *value; /* SPAWNED: the binding, or NULL */
    ta_str text;           /* SPAWNED: the revision; WITHDRAWN: the send ID */
    /* EVICTED: the actor's wake, fixed now since it is about to go. */
    bool has_wake;
    ta_time wake;
} tna_input_host;
/* Records a host operation for a logged actor; nothing for any other. */
void tna_inputs_host(tna_system *system, const tna_input_host *input);

/* The scheduler's observer (scheduler.h): `self` is the system. */
void tna_inputs_note(void *self, const tna_sched_note *note);

/* A delayed send of `sender` was dispatched (the router's or an I/O
 * processor's call during ta_system_dispatch_due()): `fired`, when the
 * sender is a logged actor. */
void tna_inputs_fired(tna_system *system, const ta_actor_ref *sender, ta_send_id send);

/*
 * Marks the next message queued for a logged actor as one a replay owes
 * (ADR 0033 section 5): it gets a seq but no `queued` record, since the
 * log has one already, and counts as waiting since `since`. Call it right
 * before the post; tna_inputs_redelivered() then answers its seq, 0 when
 * none was given (the actor is not logged, or no memory).
 */
void tna_inputs_redeliver(tna_inputs *inputs, ta_time since);
uint64_t tna_inputs_redelivered(tna_inputs *inputs);

/* The records made since the last take, oldest first; the caller releases them. */
tna_input *tna_inputs_take(tna_inputs *inputs, uint32_t *count);
/*
 * The wakes of the addresses touched since the last take (ADR 0033 section
 * 4), in the log's arena, and forgets them. Call it after tna_inputs_take()
 * and before tna_inputs_release().
 */
tna_input_wake *tna_inputs_wakes(tna_system *system, uint32_t *count);
/* Releases what taken records retain and resets the arena. */
void tna_inputs_release(tna_inputs *inputs, tna_input *first);
