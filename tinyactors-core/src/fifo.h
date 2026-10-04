#pragma once

#include "types.h"
#include "layout.h"

/*
 * Package fifo is the fixed-entry FIFO (building block B4 of
 * docs/proposals/ring-proto): a circular array of equal-sized entries over
 * storage that is fixed at init.
 *
 * What it is for. The internal event queue: one live queue in the running
 * workspace and one per nested interpreter. The events raised inside a
 * macrostep go in at the tail and are taken from the head, in order. Before
 * this unit the queue was a chain of lent, doubling event blocks that grew
 * while the interpreter ran; here its capacity is decided before the run
 * (by a quota, see quota.h) and the queue itself never grows, so a push has
 * no allocation to fail.
 *
 * Contract. Entries are opaque: the fifo copies entry_size bytes in on push
 * and out on pop, and never reads them. Order is first in, first out.
 * ta_fifo_peek exposes any waiting entry in place, oldest first, without
 * removing it. That is how a taken event stays visible: ADR 0069 keeps the
 * taken event at the head, as `_event`, until the next take pops it. The
 * fifo does not know the difference, so an owner that keeps a head taken
 * counts the entries still waiting as ta_fifo_count minus one.
 *
 * Pairing with quotas. A fifo does not enforce the ordinary and the
 * mandatory limits; the owner reserves in two ta_quota values before it
 * pushes. The fifo's capacity must be at least the sum of the two quotas'
 * capacities, or a mandatory consume could succeed and the push after it
 * still fail. That is an owner invariant, not checked here.
 *
 * Lifetimes.
 *   - The ta_fifo struct belongs to its owner and is embedded by value:
 *     system for the running workspace's queue, call for a nested
 *     interpreter's. A zeroed ta_fifo is a destroyed one.
 *   - The storage (capacity * entry_size bytes) is either given by the owner
 *     (any lifetime the owner chose, typically a region of the workspace
 *     layout; it must outlive the fifo and is never freed here) or allocated
 *     once by ta_fifo_init, at a safe point, and freed by ta_fifo_destroy.
 *     It never grows, moves or shrinks, so a pointer from ta_fifo_peek stays
 *     valid until that entry is popped or the fifo is cleared or destroyed;
 *     pushes, including wrapping ones, never move an entry.
 *   - The entries live for a macrostep: ta_fifo_clear drops them at its end,
 *     or the owner moves them to the park area if the slot parks. The fifo
 *     forgets entries without scrubbing them and without releasing what they
 *     reference. A reference inside an entry (a value reference, say) is the
 *     owner's to release before it pops-and-discards, clears or destroys.
 *
 * When it allocates. Only ta_fifo_init, and only when params->storage is
 * NULL and capacity > 0: one allocation, sized with ta_layout. Push, pop,
 * peek, clear, count and capacity never allocate. ta_fifo_destroy frees
 * the one block with the size and alignment it was allocated with.
 *
 * Failure behaviour. ta_fifo_init answers TA_ERR_INVALID for bad parameters
 * and TA_ERR_NOMEM when the size does not fit or the allocation fails; on
 * failure the fifo is zeroed and nothing is held. Push on a full fifo and
 * pop on an empty one answer false and change nothing. Full is a quota
 * decision for the owner (error.execution.capacity.internal, or the
 * mandatory path), not an allocation failure, hence bool and not ta_result.
 *
 * Invariants. count <= capacity; head < capacity when capacity > 0, else 0;
 * the oldest entry is at index head, the next push lands at (head + count)
 * modulo capacity; capacity * entry_size fits in a size_t (proved once at
 * init), so no offset computed later can overflow.
 *
 * Complexity. Every operation is O(1); push, pop and peek copy or address
 * one entry. Memory: capacity * entry_size bytes plus the struct.
 *
 * Usage.
 *
 *	typedef struct { uint32_t name; uint32_t pad; uint64_t payload[5]; } entry; // 48 bytes
 *	ta_fifo queue;
 *	ta_fifo_params params = { .allocator = allocator, .storage = NULL,
 *	                          .entry_size = sizeof(entry), .entry_alignment = _Alignof(entry),
 *	                          .capacity = 16 };
 *	if (ta_fifo_init(&queue, &params) != TA_OK) { return TA_ERR_NOMEM; }
 *	entry raised = { 0 };
 *	if (!ta_fifo_push(&queue, &raised)) { ... }   // full: owner decides
 *	entry *taken = ta_fifo_peek(&queue, 0);       // the taken `_event`, still queued
 *	ta_fifo_pop(&queue, NULL);                    // next take drops it
 *	ta_fifo_destroy(&queue);
 */

typedef struct ta_fifo_params {
    ta_allocator allocator; /* used only when storage == NULL; destroy frees through it */
    void *storage;          /* NULL: allocate once; else capacity * entry_size bytes aligned to entry_alignment, outliving the fifo */
    size_t entry_size;      /* bytes of one entry; > 0 and a multiple of entry_alignment */
    size_t entry_alignment; /* a power of two */
    uint32_t capacity;      /* entries; 0 is valid: every push fails and nothing is allocated */
} ta_fifo_params;

/* Fields are private to fifo.c. */
typedef struct ta_fifo {
    ta_allocator allocator;
    unsigned char *entries;
    size_t entry_size;
    size_t entry_alignment;
    size_t bytes; /* capacity * entry_size, finished once with ta_layout, reused for the sized free */
    uint32_t capacity;
    uint32_t head; /* index of the oldest entry */
    uint32_t count;
    bool owned; /* the fifo allocated `entries` and must free it */
} ta_fifo;

/*
 * ta_fifo_init prepares a fifo over owner storage, or over one block it
 * allocates now. It answers TA_ERR_INVALID for a NULL argument, entry_size
 * 0 or not a multiple of the alignment, an alignment that is not a power of
 * two, or provided storage that is NULL-but-needed or misaligned, and
 * TA_ERR_NOMEM when capacity * entry_size does not fit or the allocation
 * fails. On any failure *fifo is zeroed and nothing is held. Call it at a
 * safe point: it is the only function here that may allocate.
 */
ta_result ta_fifo_init(ta_fifo *fifo, const ta_fifo_params *params);

/*
 * ta_fifo_destroy frees the storage if and only if the fifo allocated it
 * (with the size and alignment it was allocated with), never touches
 * provided storage, and zeroes *fifo. It is idempotent on a zeroed fifo.
 * It does not release what the entries reference.
 */
void ta_fifo_destroy(ta_fifo *fifo);

/*
 * ta_fifo_push copies entry_size bytes from `entry` to the tail and answers
 * true, or answers false and changes nothing when the fifo is full. It
 * never allocates. `entry` must not point into the fifo's own storage.
 */
bool ta_fifo_push(ta_fifo *fifo, const void *entry);

/*
 * ta_fifo_pop removes the oldest entry, copying it to `out` first unless
 * `out` is NULL, and answers true; it answers false and changes nothing
 * when the fifo is empty. The removed bytes are not scrubbed.
 */
bool ta_fifo_pop(ta_fifo *fifo, void *out);

/*
 * ta_fifo_peek answers the address of the index-th oldest entry (0 is the
 * head, the taken `_event`), or NULL when index >= count. It removes
 * nothing. The address is valid until that entry is popped or the fifo is
 * cleared or destroyed.
 */
void *ta_fifo_peek(const ta_fifo *fifo, uint32_t index);

/*
 * ta_fifo_clear forgets every entry (count and head become 0) without
 * scrubbing them or releasing what they reference. Storage is kept.
 */
void ta_fifo_clear(ta_fifo *fifo);

/* ta_fifo_count answers the number of entries now queued, including a taken head. */
uint32_t ta_fifo_count(const ta_fifo *fifo);

/* ta_fifo_capacity answers the fixed number of entries the fifo can hold. */
uint32_t ta_fifo_capacity(const ta_fifo *fifo);
