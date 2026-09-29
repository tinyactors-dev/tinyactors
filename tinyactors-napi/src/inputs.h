#pragma once
/*
 * The input log (napi ADRs 0032 to 0034): core writes each input a named
 * actor of a logged family accepts as a record in its portable byte layout
 * (core ADR 0090), through the sink this module installs, and napi hands
 * one turn's records to JavaScript as bytes, one entry per address, with
 * the time napi next needs each touched actor back.
 *
 * The sink files each record under its actor's address: core names the
 * actor while it writes (ta_system_input_actor), and a record napi writes
 * for an address with no actor (a message waiting for a load) names its
 * address through `host_address`. A record that fails halfway is cut off
 * again (end(keep=false)). Entries' byte buffers are reset, not freed,
 * after the batch is taken, and reused.
 *
 * The mailbox is napi's (core ADR 0029), so napi writes the records that
 * make it durable (core ADR 0090 section 6): `queued` when a message for a
 * logged actor is accepted into its mailbox, or waits for its loader, with
 * a host number unique across processes (the incarnation's hash in its
 * high half), which core's `event` record carries when the message is
 * delivered (tna_inputs_host_seq); `dropped` when it leaves undelivered;
 * `evicted` before evict() destroys an actor. Writing `queued` is part of
 * accepting: a message whose record cannot be written is not accepted.
 *
 * Wakes (ADR 0033 section 4): every address a record names in a turn is
 * touched, and when the batch is taken its wake is worked out: the earlier
 * of its first pending delayed send's due time and the time its oldest
 * undelivered queued message was accepted, or none for an actor that is
 * resident with neither, or finished. An address with no resident actor
 * and no queued mail keeps the wake it had (nothing is reported).
 *
 * With no log installed the scheduler has no observer, core has no input
 * sink, and nothing here runs.
 *
 * This module has no Node-API in it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "containers.h"
#include "scheduler.h"
#include "tinyactors.h"

typedef struct tna_system tna_system;

/* One address's records from this turn, back to back. */
typedef struct {
    ta_address *address; /* retained */
    tna_vec bytes;       /* uint8_t */
} tna_input_entry;

/* A wake worked out for a batch: `has_at` false is none (JavaScript's null). */
typedef struct {
    ta_actor_address address; /* borrowed from the log's arena */
    bool has_at;
    ta_time at;
} tna_input_wake;

/* An address touched this turn, retained until the batch is taken. */
typedef struct {
    ta_address *address;
    bool fixed;  /* `has_at` and `at` were given (an eviction, a destroy) */
    bool has_at;
    ta_time at;
} tna_input_touch;

typedef struct {
    bool enabled;
    ta_allocator allocator; /* the other category */
    ta_arena arena;         /* the wakes' address text, until the batch is released */
    bool arena_ready;
    tna_vec families;       /* ta_str, each allocated */
    /* This turn's records by address, and byte buffers kept for reuse. */
    tna_vec entries;        /* tna_input_entry */
    tna_map entry_index;    /* address -> index in `entries` */
    tna_vec spare;          /* tna_vec of uint8_t, emptied */
    /* The record being written: its entry and where it began. */
    bool open;
    uint32_t open_entry;
    uint32_t open_start;
    /* Set while napi writes a record for an address with no actor. */
    ta_address *host_address;
    /* Records kept this turn, for tna_system_quiet(). */
    uint32_t count;
    /* The mailbox: host numbers by envelope, their addresses, and each
     * address's undelivered messages. */
    tna_map seqs;           /* envelope index -> host_seq */
    tna_map queued_address; /* envelope index -> its address (ta_address *, retained) */
    tna_map mail_count;     /* address -> queued messages not yet delivered */
    tna_map mail_since;     /* address -> when the first of them was accepted */
    tna_vec touched;        /* tna_input_touch, this turn's addresses */
    tna_map touched_index;  /* address -> index in `touched` */
    /* tna_inputs_redeliver(): the next queued message is one a replay owes,
     * which keeps its host number and gets no new `queued` record. */
    bool redelivering;
    uint64_t redeliver_seq;
    ta_time redeliver_since;
    uint64_t seq_base;      /* the incarnation's hash, high half */
    uint64_t next_seq;
    uint64_t lost;          /* records or wakes lost for lack of memory */
} tna_inputs;

void tna_inputs_init(tna_inputs *inputs, ta_allocator allocator);
/* Releases everything; the log is off afterwards. Call it while core is alive. */
void tna_inputs_destroy(tna_system *system);

/*
 * Turns the log on for `families` (count 0 with families NULL turns it
 * off): tells core which families to log, installs or removes core's input
 * sink and the scheduler's observer. TA_ERR_NOMEM leaves the log as it was.
 */
ta_result tna_inputs_configure(tna_system *system, const ta_str *families, uint32_t count);

/* Whether `actor` is a published named actor of a logged family; writes its address. */
bool tna_inputs_logged(tna_system *system, ta_actor_id actor, ta_actor_address *address);

/* The scheduler's observer (scheduler.h): `self` is the system. */
ta_result tna_inputs_note(void *self, const tna_sched_note *note);
/* The host number of the queued message behind `envelope`, 0 when none. */
uint64_t tna_inputs_host_seq(void *self, uint32_t envelope);

/* Whether `seq` is a host number this system gave. */
bool tna_inputs_seq_is_ours(const tna_inputs *inputs, uint64_t seq);

/* Writes `evicted` for the logged actor about to be destroyed, and fixes
 * its wake: `has_wake` false is none. */
ta_result tna_inputs_evicted(tna_system *system, ta_actor_id actor, bool has_wake, ta_time wake);
/* The logged actor at `address` is destroyed: nothing will need it back. */
void tna_inputs_forget(tna_system *system, ta_address *address);

/*
 * Marks the next message queued for a logged actor as one a replay owes
 * (ADR 0033 section 5): it keeps `seq`, gets no `queued` record since the
 * log has one, and counts as waiting since `since`. Call it right before
 * the post; tna_inputs_redelivered() clears it.
 */
void tna_inputs_redeliver(tna_inputs *inputs, uint64_t seq, ta_time since);
void tna_inputs_redelivered(tna_inputs *inputs);

/*
 * The wakes of the addresses touched since the last take (ADR 0033 section
 * 4), in the log's arena, and forgets them. Call it before tna_inputs_release().
 */
tna_input_wake *tna_inputs_wakes(tna_system *system, uint32_t *count);
/* Empties this turn's entries (their buffers go back for reuse) and resets the arena. */
void tna_inputs_release(tna_inputs *inputs);

/*
 * The bytes of the first whole record at the start of `bytes`, or of the
 * whole unit it opens when `unit` (a checkpoint and its parts, core ADR
 * 0091); 0 when they are not all there yet.
 */
size_t tna_record_span(const uint8_t *bytes, size_t length, bool unit);
