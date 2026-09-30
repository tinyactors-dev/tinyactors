#pragma once
/*
 * The input log (kernel ADR 0002; napi ADRs 0032 to 0034): a write-ahead
 * log of the named actors of the logged families. The kernel records every
 * input such an actor takes, at its own call sites, as records in the
 * layout wal.h describes, and hands one turn's records to the binding as
 * bytes, one entry per address, with the time it next needs each touched
 * actor back.
 *
 * Recording happens before core is called: the kernel writes the record
 * into the turn's buffer for the actor's address, calls core with the
 * input's time, and keeps the record when core took the input or cuts it
 * off again when it did not (a mark, tah_input_mark). A value is written by
 * the data model's codec (tah_inputs_set_datamodel), so an input carrying a
 * value with no durable form is refused before core sees it
 * (TA_ERR_UNSUPPORTED). Entries' byte buffers are reset, not freed, after
 * the batch is taken, and reused.
 *
 * A message has an identity from the moment the kernel accepts it into a
 * mailbox, or into the wait for a loader (kernel ADR 0002 section 2): 64
 * bits, the incarnation's hash in the high half and a counter in the low
 * half. ACCEPTED carries its content once; DELIVERED (or JUMP) says when
 * core took it; DROPPED says it will never be delivered. An invocation's
 * error report is a message like any other.
 *
 * Record numbers (seq) count a resident actor's records in its life, from
 * 1 at SPAWNED; a record written while an address has no resident actor
 * (mail waiting for a loader) has seq 0.
 *
 * Wakes (napi ADR 0033 section 4): every address a record names in a turn
 * is touched, and when the batch is taken its wake is worked out: the
 * earlier of its first pending delayed send's due time and the time its
 * oldest undelivered message was accepted, or none for an actor that is
 * resident with neither, or finished. An address with no resident actor
 * and no undelivered mail keeps the wake it had (nothing is reported).
 *
 * With no log installed the scheduler has no observer and nothing here runs.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "containers.h"
#include "scheduler.h"
#include "tinyactors.h"
#include "wal.h"

typedef struct tah_system tah_system;

/* One address's records from this turn, back to back: the log's buffer. */
typedef struct {
    ta_address *address; /* retained */
    tah_vec bytes;       /* uint8_t */
} tah_input_buffer;

/* One address's records from this turn, as the binding takes them: borrowed
 * from the log until tah_inputs_release(). */
typedef struct {
    ta_address *address; /* the log holds its reference */
    const uint8_t *bytes;
    size_t length;
} tah_input_entry;

/* A wake worked out for a batch: `has_at` false is none (JavaScript's null). */
typedef struct {
    ta_actor_address address; /* borrowed from the log's arena */
    bool has_at;
    ta_time at;
} tah_input_wake;

/* An address touched this turn, retained until the batch is taken. */
typedef struct {
    ta_address *address;
    bool fixed;  /* `has_at` and `at` were given (an eviction, a destroy) */
    bool has_at;
    ta_time at;
} tah_input_touch;

/*
 * A record written ahead of the core call it belongs to: undone
 * (tah_inputs_undo) when core refused the input. `open` false is nothing
 * to undo.
 */
typedef struct {
    bool open;
    uint32_t entry;
    uint32_t start;
    ta_actor_id actor;  /* whose record number it took, or 0 */
    uint64_t seq_before;
} tah_input_mark;

typedef struct {
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
    /* The record written ahead of the delivery or report in progress. */
    tah_input_mark pending;
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
} tah_inputs;

void tah_inputs_init(tah_inputs *inputs, ta_allocator allocator);
/* Releases everything; the log is off afterwards. Call it while core is alive. */
void tah_inputs_destroy(tah_system *system);

/*
 * The data model whose codec writes values into records and reads them
 * back (core ADR 0090 section 2): the binding's, since a data model encodes
 * its own values. Without one, a record with a value is refused.
 */
void tah_inputs_set_datamodel(tah_inputs *inputs, ta_datamodel datamodel);

/*
 * Turns the log on for `families` (count 0 with families NULL turns it
 * off), and installs or removes the scheduler's observer. TA_ERR_NOMEM
 * leaves the log as it was.
 */
ta_result tah_inputs_configure(tah_system *system, const ta_str *families, uint32_t count);

/* Whether `actor` is a published named actor of a logged family; writes its address. */
bool tah_inputs_logged(tah_system *system, ta_actor_id actor, ta_actor_address *address);

/* The scheduler's observer (scheduler.h): `self` is the system. */
ta_result tah_inputs_note(void *self, const tah_sched_note *note);

/* Whether `seq` is a message ID this system gave. */
bool tah_inputs_seq_is_ours(const tah_inputs *inputs, uint64_t seq);

/* A logged actor was spawned (tah_hook_actor_spawned): SPAWNED, with its
 * data as it was bound. Anything but TA_OK refuses the spawn. */
ta_result tah_inputs_spawned(tah_system *system, ta_actor_id actor, tah_input_mark *mark);
/* Writes `evicted` for the logged actor about to be destroyed, and fixes
 * its wake: `has_wake` false is none. */
ta_result tah_inputs_evicted(tah_system *system, ta_actor_id actor, bool has_wake, ta_time wake);
/* The logged actor is about to be destroyed by an application (not evicted,
 * not finished, not replaced): DESTROYED, and nothing needs its address
 * back. Anything but TA_OK should keep the actor. */
ta_result tah_inputs_destroying(tah_system *system, ta_actor_id actor);
/* The actor is gone: forget its record number. */
void tah_inputs_removed(tah_system *system, ta_actor_id actor);

/*
 * A delayed send of `sender` was dispatched (a timer fired, kernel ADR 0002
 * section 3): FIRED for a logged sender. Undo it when the send was not
 * taken after all. TA_ERR_NOMEM: the send should wait, as a RETRY.
 */
ta_result tah_inputs_fired(tah_system *system, const ta_actor_ref *sender, ta_send_id send, tah_input_mark *mark);
/*
 * The ACCEPTED record of the message in `envelope` as message `id`, its
 * texts rendered into the log's text arena, which the next record written
 * resets; `session` renders a computed origin (0 when none). For a
 * checkpoint's owed messages (kernel ADR 0002 section 7).
 */
ta_result tah_inputs_accepted_record(tah_system *system, const tah_envelope *envelope, uint64_t id, uint64_t session,
                                     tah_wal_record *out);
/* Cuts off a record written ahead of a core call that refused its input. */
void tah_inputs_undo(tah_system *system, tah_input_mark *mark);

/* The record number the actor's next record takes minus one (its last), 0
 * for none; and sets it (a replay published with the log's last). */
uint64_t tah_inputs_actor_seq(const tah_inputs *inputs, ta_actor_id actor);
void tah_inputs_set_actor_seq(tah_inputs *inputs, ta_actor_id actor, uint64_t seq);

/*
 * An operator's actions, recorded ahead of the core call (kernel ADR 0002
 * section 2): ta_actor_withdraw_send() as WITHDRAWN, and ta_actor_abandon()
 * as ABANDONED, followed by the scheduler's abandon hook. They answer
 * core's result; a record that cannot be written refuses the action.
 */
ta_result tah_system_withdraw_send(tah_system *system, ta_actor_id actor, ta_send_id send);
ta_result tah_system_abandon(tah_system *system, ta_actor_id actor);

/*
 * Marks the next message queued for a logged actor as one a replay owes
 * (napi ADR 0033 section 5): it keeps `seq`, gets no ACCEPTED record since
 * the log has one, and counts as waiting since `since`. Call it right before
 * the post; tah_inputs_redelivered() clears it.
 */
void tah_inputs_redeliver(tah_inputs *inputs, uint64_t seq, ta_time since);
void tah_inputs_redelivered(tah_inputs *inputs);

/*
 * The wakes of the addresses touched since the last take (napi ADR 0033
 * section 4), in the log's arena, and forgets them. Call it before
 * tah_inputs_release().
 */
tah_input_wake *tah_inputs_wakes(tah_system *system, uint32_t *count);
/* Empties this turn's entries (their buffers go back for reuse) and resets the arena. */
void tah_inputs_release(tah_inputs *inputs);

/*
 * The batch a binding takes after a turn (napi ADR 0033): whether the log
 * is on, the addresses touched since the last take, and this turn's entries
 * by index, from 0 to tah_inputs_entry_count(). An entry whose only record
 * was refused has length 0.
 */
bool tah_inputs_enabled(const tah_inputs *inputs);
uint32_t tah_inputs_touched_count(const tah_inputs *inputs);
uint32_t tah_inputs_entry_count(const tah_inputs *inputs);
tah_input_entry tah_inputs_entry_at(const tah_inputs *inputs, uint32_t index);

/* The bytes of the first whole record at `bytes`, or of the checkpoint unit
 * it opens when `unit`; 0 when they are not all there yet (tah_wal_span). */
size_t tah_record_span(const uint8_t *bytes, size_t length, bool unit);
