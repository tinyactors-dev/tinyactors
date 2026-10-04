/**
 * @file fifo.h
 *
 * # Fixed-entry FIFO
 *
 * `ta_fifo` is a first-in, first-out queue of fixed-size entries in one
 * circular array. Entries leave in the order they came in. Its capacity
 * is set at a safe point and never changes while the queue is used:
 * `ta_fifo_push` never grows it, and a push into a full queue fails.
 *
 * ## Contract and invariants
 *
 * - The queue holds `count` entries, `0 <= count <= capacity`. The oldest
 *   one, the head, is at slot `head` of the array; the entry `i` places
 *   behind it is at slot `(head + i) mod capacity`.
 * - `ta_fifo_push` adds at the tail, `ta_fifo_pop` removes the head, and
 *   nothing else changes the order. There is no priority: every entry,
 *   whatever quota its owner counts it against, keeps the order of its
 *   push.
 * - `ta_fifo_peek` answers the head without removing it, so an entry can
 *   stay at the head while its owner uses it (the internal event that is
 *   `_event`) and leave with the next pop.
 * - The queue copies entries in and out as bytes. It never reads their
 *   contents, so it never releases what an entry refers to (a data value,
 *   name chunks). Before `ta_fifo_pop` discards an entry or
 *   `ta_fifo_clear` empties the queue, the owner releases what the entries
 *   hold, walking them with `ta_fifo_at` when it must.
 * - The queue does not divide its capacity. How much of it ordinary and
 *   mandatory entries may use is the owner's accounting, in quotas beside
 *   the queue; the queue only refuses a push when every slot is taken.
 *
 * ## Memory
 *
 * The queue owns one array of `capacity` entries, allocated from the
 * `ta_allocator` given to `ta_fifo_init`, which it borrows and which must
 * outlive it. A `ta_fifo` itself is embedded where its user likes (the
 * running workspace, a nested interpreter's workspace).
 *
 * - `ta_fifo_init` allocates nothing; the capacity is 0.
 * - `ta_fifo_reserve` allocates a larger array and moves the entries into
 *   it in order. It runs only at a safe point (registration, spawn or
 *   load), never inside a macrostep.
 * - `ta_fifo_destroy` only frees.
 * - `ta_fifo_push`, `ta_fifo_pop`, `ta_fifo_peek`, `ta_fifo_at`,
 *   `ta_fifo_clear`, `ta_fifo_count` and `ta_fifo_capacity` never
 *   allocate, and may run inside a macrostep.
 *
 * An address `ta_fifo_peek` or `ta_fifo_at` answers stays valid until that
 * entry is popped, the queue is cleared, reserved larger or destroyed.
 * Pushing moves no entry.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_fifo_push`, `ta_fifo_pop` | O(1): copies one entry |
 * | `ta_fifo_peek`, `ta_fifo_at`, `ta_fifo_count`, `ta_fifo_capacity` | O(1) |
 * | `ta_fifo_clear` | O(1): touches no entry |
 * | `ta_fifo_reserve(n)` | one allocation and one free, and copies every entry once |
 * | `ta_fifo_init` | O(1) |
 * | `ta_fifo_destroy` | one free |
 *
 * ## Failure
 *
 * - `ta_fifo_init` answers `TA_ERR_INVALID` for an entry size of 0, an
 *   alignment that is not a power of two, or an entry size that is not a
 *   multiple of the alignment, and leaves `*fifo` untouched.
 * - `ta_fifo_reserve` answers `TA_ERR_NOMEM` when the allocator refuses or
 *   the array's size in bytes does not fit in `size_t`. A failed reserve
 *   changes nothing: the same array, capacity, entries and order.
 * - `ta_fifo_push` answers `TA_ERR_FULL`, and changes nothing, when the
 *   queue holds `capacity` entries. A full queue is a state that arises
 *   at run time (a chart that raises in a loop), so the caller answers it:
 *   a `<raise>` beyond the ordinary quota becomes
 *   `error.execution.capacity.internal`, and a mandatory event with no
 *   room left is a runaway.
 * - `ta_fifo_pop` answers `TA_ERR_NOT_FOUND`, and changes nothing, when
 *   the queue is empty. `ta_fifo_peek` answers NULL when it is empty, and
 *   `ta_fifo_at` answers NULL for a place at or behind the count.
 * - Nothing else fails.
 *
 * ## Users
 *
 * The **internal queue** of `<raise>`, `done.state.*` and `error.*`
 * events is this queue, with one fixed entry per event (its name, its
 * data value and `_event`'s other fields). Its capacity is the ordinary
 * quota (`internalQueueEvents`) plus the mandatory floor the compiler
 * sizes for one round of every phase; two quotas beside the queue count
 * which entries may take what. There is one live internal queue in the
 * running workspace, reserved at registration for the largest
 * definition, and one per nested interpreter (ADR 0060). A parked slot's
 * pending entries are read out with `ta_fifo_at` into the park area and
 * pushed back in order when it resumes. A runaway clears the queue.
 */
#pragma once

#include "../include/tinyactors.h"

/** What `ta_fifo_init` needs. */
typedef struct {
    /** Where the array comes from; borrowed, and must outlive the queue. */
    ta_allocator allocator;
    /** Bytes per entry: not 0, and a multiple of `entry_alignment`, as
     * `sizeof` of a type always is. */
    size_t entry_size;
    /** Alignment of each entry: a power of two. */
    size_t entry_alignment;
} ta_fifo_params;

/** A first-in, first-out queue of fixed-size entries; see the file
 * comment. */
typedef struct {
    ta_allocator allocator;
    /** `capacity` entries of `entry_size` bytes; NULL while `capacity` is
     * 0. */
    unsigned char *entries;
    size_t entry_size;
    size_t entry_alignment;
    uint32_t capacity;
    /** The slot of the oldest entry; 0 when the queue is empty. */
    uint32_t head;
    uint32_t count;
} ta_fifo;

/**
 * Sets up an empty queue with capacity 0 for entries described by
 * `params`. Allocates nothing. Answers `TA_ERR_INVALID`, leaving `*fifo`
 * untouched, when `params` breaks a rule of `ta_fifo_params`.
 */
ta_result ta_fifo_init(ta_fifo *fifo, const ta_fifo_params *params);

/**
 * Frees the array, and leaves an empty queue with capacity 0 that
 * allocates nothing until it is reserved again. Does not look at the
 * entries; the owner releases what they hold first. Never fails.
 */
void ta_fifo_destroy(ta_fifo *fifo);

/**
 * Makes the capacity at least `capacity` entries. A `capacity` within the
 * current one changes nothing and allocates nothing. Otherwise allocates
 * an array of exactly `capacity` entries, copies the entries into it in
 * order from its first slot, and frees the old array.
 *
 * Allocates, so it runs only at a safe point (registration, spawn or
 * load), never inside a macrostep. Answers `TA_ERR_NOMEM` when the
 * allocation fails or its size does not fit in `size_t`; the queue is
 * then exactly as before.
 */
ta_result ta_fifo_reserve(ta_fifo *fifo, uint32_t capacity);

/** How many entries the queue holds. Never allocates. */
static inline uint32_t ta_fifo_count(const ta_fifo *fifo) { return fifo->count; }

/** How many entries the queue has room for. Never allocates. */
static inline uint32_t ta_fifo_capacity(const ta_fifo *fifo) { return fifo->capacity; }

/**
 * The address of the entry `place` places behind the head (0 is the
 * head), or NULL when `place` is not below the count. Never allocates;
 * may run inside a macrostep.
 */
static inline void *ta_fifo_at(const ta_fifo *fifo, uint32_t place) {
    if (place >= fifo->count) {
        return NULL;
    }
    /* head < capacity and place < capacity, so neither branch wraps. */
    uint32_t to_end = fifo->capacity - fifo->head;
    uint32_t slot = place < to_end ? fifo->head + place : place - to_end;
    return fifo->entries + (size_t)slot * fifo->entry_size;
}

/**
 * The address of the head, the oldest entry, which stays in the queue;
 * NULL when the queue is empty. Never allocates; may run inside a
 * macrostep.
 */
static inline void *ta_fifo_peek(const ta_fifo *fifo) { return ta_fifo_at(fifo, 0); }

/**
 * Copies one entry from `entry` to the tail. Never grows the queue and
 * never allocates; may run inside a macrostep. O(1).
 *
 * Answers `TA_OK`, or `TA_ERR_FULL` and changes nothing when the queue
 * already holds `capacity` entries.
 */
ta_result ta_fifo_push(ta_fifo *fifo, const void *entry);

/**
 * Removes the head. When `out` is not NULL, copies the entry into it
 * first; `out` has room for one entry. With `out` NULL the entry is
 * discarded, so its owner releases what it holds before. Never
 * allocates; may run inside a macrostep. O(1).
 *
 * Answers `TA_OK`, or `TA_ERR_NOT_FOUND` and changes nothing when the
 * queue is empty.
 */
ta_result ta_fifo_pop(ta_fifo *fifo, void *out);

/**
 * Empties the queue, keeping its array and capacity. Touches no entry:
 * the owner releases what the entries hold first. Never allocates; may
 * run inside a macrostep. Never fails. O(1).
 */
static inline void ta_fifo_clear(ta_fifo *fifo) {
    fifo->head = 0;
    fifo->count = 0;
}
