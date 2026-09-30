#pragma once
/*
 * The kernel's own layout of what tinyactors_host/inputs.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/inputs.h"

/* One address's records from this turn, back to back: the log's buffer. */
typedef struct tah_input_buffer {
    ta_address *address; /* retained */
    tah_vec bytes;       /* uint8_t */
} tah_input_buffer;

/* An address touched this turn, retained until the batch is taken. */
typedef struct tah_input_touch {
    ta_address *address;
    bool fixed;  /* `has_at` and `at` were given (an eviction, a destroy) */
    bool has_at;
    ta_time at;
} tah_input_touch;

/*
 * A macrostep's first record (DELIVERED, JUMP or RELEASED, or SPAWNED for
 * a bootstrap), held from the start of the macrostep until it ends: it goes
 * into the batch of the turn the macrostep ends in, beside the ACCEPTED
 * records of what it sent, so a crash keeps both or neither (kernel ADR
 * 0004). Its bytes are written when the macrostep starts, with that time;
 * its seq is filled in when it goes into the batch.
 */
typedef struct {
    ta_actor_id actor;
    ta_address *address; /* retained */
    tah_vec bytes;       /* uint8_t: one record, seq 0 */
    bool ended;          /* the macrostep ended and the record could not be written: the next turn tries again */
    bool owed;           /* its message still counts as waiting for the address (its wake) until it is written */
    bool life;           /* SPAWNED: the actor's records before it are outside its life, and have seq 0 */
} tah_input_held;

/*
 * A record written ahead of the core call it belongs to: undone
 * (tah_inputs_undo) when core refused the input. `open` false is nothing
 * to undo.
 */
struct tah_input_mark {
    bool open;
    uint32_t entry;
    uint32_t start;
    ta_actor_id actor;  /* whose record number it took, or 0 */
    uint64_t seq_before;
};

/*
 * Writes `actor`'s held first record now, when its macrostep ended while
 * memory ran out (kernel ADR 0004): a checkpoint must follow it.
 * TA_ERR_BUSY while the macrostep is open, TA_ERR_NOMEM when it cannot.
 */
ta_result tah_inputs_write_first(tah_system *system, ta_actor_id actor);

struct tah_inputs {
    bool enabled;
    ta_allocator allocator; /* the other category */
    ta_arena arena;         /* the wakes' address text, until the batch is released */
    bool arena_ready;
    ta_arena text;          /* one record's rendered text, reset after it */
    bool text_ready;
    ta_datamodel datamodel; /* whose codec writes and reads values */
    tah_vec families;       /* ta_str, each allocated */
    /* This turn's records by address, and byte buffers kept for reuse. */
    tah_vec entries;        /* tah_input_buffer */
    tah_map entry_index;    /* address -> index in `entries` */
    tah_vec spare;          /* tah_vec of uint8_t, emptied */
    /* Records kept this turn, for tah_system_quiet(). */
    uint32_t count;
    /* A resident logged actor's last record number (actor ID -> seq). */
    tah_map actor_seq;
    /* The open macrosteps of logged actors: their first records, held. */
    tah_vec held;           /* tah_input_held */
    tah_map held_index;     /* actor ID -> index in `held` */
    uint32_t ended;         /* held records whose macrostep ended, waiting for memory */
    /* The actor whose first record the delivery or report in progress
     * holds, until core takes it or refuses it; 0 for none. */
    ta_actor_id offering;
    /* The mailbox: message IDs by envelope, their addresses, and each
     * address's undelivered messages. */
    tah_map seqs;           /* envelope index -> message ID */
    tah_map queued_address; /* envelope index -> its address (ta_address *, retained) */
    tah_map mail_count;     /* address -> queued messages not yet delivered */
    tah_map mail_since;     /* address -> when the first of them was accepted */
    tah_vec touched;        /* tah_input_touch, this turn's addresses */
    tah_map touched_index;  /* address -> index in `touched` */
    /* tah_inputs_redeliver(): the next queued message is one a replay owes,
     * which keeps its ID and gets no new ACCEPTED record. */
    bool redelivering;
    uint64_t redeliver_seq;
    ta_time redeliver_since;
    uint64_t seq_base;      /* the incarnation's hash, high half */
    uint64_t next_seq;
    uint64_t lost;          /* records or wakes lost for lack of memory */
};
