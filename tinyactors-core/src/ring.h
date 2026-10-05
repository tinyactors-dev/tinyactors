/**
 * @file ring.h
 *
 * # Single-producer, single-consumer byte ring
 *
 * `ta_ring` hands records from one producer to one consumer without a
 * call per record. It is a circular buffer of bytes, not of slots:
 * records have different sizes, so a ring holds many small records or a
 * few large ones. Both sides run on the system's own thread and take
 * turns, so the ring needs no lock and no atomics; its state is plain
 * counters.
 *
 * ## Records and pads
 *
 * Every record starts with an 8-byte prefix the ring reads, followed by
 * the record's sequence number:
 *
 * | Offset | Field | Type | Meaning |
 * |---|---|---|---|
 * | 0 | `kind` | u16 | what the record is; 0 is a pad |
 * | 2 | `flags` | u16 | the record's own; the ring never reads them |
 * | 4 | `size` | u32 | the whole record in bytes, a multiple of 8 |
 * | 8 | `seq` | u64 | the record's number in this ring (not in a pad) |
 *
 * The rest of the record (the 32-byte common header's `session` and
 * `at`, the fixed fields and the tail) belongs to whoever fills it.
 *
 * - **Every record is 8-byte aligned and contiguous.** Its size is
 *   rounded up to 8, at least `TA_RING_RECORD_MIN_BYTES` and at most the
 *   ring's `max_record_bytes`. A record never splits across the end of
 *   the buffer.
 * - **A pad fills the end of the buffer** when the next record does not
 *   fit before it: `kind = 0`, `flags = 0`, `size` = the bytes to the
 *   end. It carries no seq. The reader skips `size` bytes and reads on.
 *   A pad costs at most `max_record_bytes - 8` bytes per wrap.
 *
 * ## Contract and invariants
 *
 * - **Counters only grow.** `head` is the bytes ever committed and
 *   `tail` the bytes ever released, both `u64`. `tail <= head <= write
 *   <= tail + capacity`, where `write` is the producer's private
 *   position: reserved but not yet committed bytes lie between `head`
 *   and `write`. Every counter is a multiple of 8. A counter's offset in
 *   the buffer is `counter mod capacity`.
 * - **The producer reserves, fills, commits.** `ta_ring_reserve` takes
 *   room for one record at `write`, writing a pad first when the record
 *   does not fit before the end, and stamps the record's `size` and
 *   `seq`. The producer fills the rest in place; the consumer sees
 *   nothing until `ta_ring_commit` stores `head = write`. A producer may
 *   reserve several records and commit them at once.
 * - **The consumer peeks, uses, releases.** `ta_ring_peek` answers the
 *   record at `tail` in place; `ta_ring_release` releases it, which is
 *   what makes room for the producer. The consumer copies whatever it
 *   keeps past the release, including the bytes the record's spans name.
 * - **Sequence numbers are gap-free from 1.** The ring numbers each
 *   reserved record with `next_seq` and counts it up; pads take no
 *   number. On a **lossless** ring a record whose seq is not one more
 *   than the last one read is corruption: a reader that sees seq jump
 *   has a bug or a corrupted ring, and on h2c that is where the gap check
 *   lives. On a **lossy** ring a dropped record still takes its number,
 *   so the gap tells the reader records were lost; there the seq must
 *   only grow.
 * - **When the ring is full** (the record and its pad do not fit beside
 *   the unreleased bytes):
 *   - a **lossless** ring refuses: `ta_ring_reserve` answers
 *     `TA_ERR_FULL` and changes nothing;
 *   - a **lossy** ring drops the record: it counts it in `dropped`,
 *     remembers it among the unreported drops, consumes its seq, and
 *     answers `TA_ERR_FULL`. `ta_ring_take_drops` answers how many were
 *     dropped since it was last called and the seq the first of them
 *     would have had, which is what the next drop report says.
 * - **A pad alone guarantees progress.** When the pad and the record
 *   together exceed the whole capacity, no release can ever make them
 *   fit. If nothing is reserved and uncommitted, `ta_ring_reserve` then
 *   writes and commits the pad alone, so `head` reaches the start of the
 *   buffer, and answers `TA_ERR_FULL`; once the consumer releases the
 *   pad, the next reservation starts at offset 0. This is the one case
 *   where a lossless reserve that answers `TA_ERR_FULL` changes the ring,
 *   and it makes a ring of exactly `max_record_bytes` work at any offset.
 * - **The consumer checks the framing.** Before it answers a record,
 *   `ta_ring_peek` checks that its `size` is a multiple of 8, lies inside
 *   the committed bytes and inside the buffer, that a pad ends exactly at
 *   the end of the buffer, and that a record is at least
 *   `TA_RING_RECORD_MIN_BYTES` (8 for a pad) and at most
 *   `max_record_bytes`, and that its seq follows the rule of the mode.
 *   Any other record answers `TA_ERR_CORRUPT`, and the consumer stops
 *   reading. Reading never leaves the buffer, whatever bytes it holds.
 *
 * ## Memory
 *
 * The ring owns one buffer of `capacity` bytes, aligned to 8, allocated
 * from the `ta_allocator` given to `ta_ring_init`, which it borrows and
 * which must outlive it. A `ta_ring` itself is embedded where its user
 * likes (beside the control block of a system).
 *
 * - `ta_ring_init` allocates the buffer, at system creation, never
 *   inside a macrostep. Nothing changes the size afterwards: the host
 *   holds positions in the ring.
 * - `ta_ring_destroy` only frees.
 * - `ta_ring_reserve`, `ta_ring_commit`, `ta_ring_peek`,
 *   `ta_ring_release`, `ta_ring_take_drops` and the counters never
 *   allocate, and may run inside a macrostep.
 *
 * The buffer stays owned by the ring. Its owner may later place it
 * elsewhere only if nothing else about the ring changes; there is no
 * call for that.
 *
 * The address a reservation answers stays valid until the record is
 * released or the ring destroyed. The address `ta_ring_peek` answers
 * stays valid until that record is released or the ring destroyed. The ring never reads or releases what a record refers
 * to (a vref, a blob); its owner does.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_ring_reserve`, `ta_ring_commit`, `ta_ring_release`, `ta_ring_take_drops` | O(1) |
 * | `ta_ring_peek` | O(1) per record, plus one step per pad it skips |
 * | `ta_ring_used`, `ta_ring_available`, `ta_ring_capacity` | O(1) |
 * | `ta_ring_init` | one allocation |
 * | `ta_ring_destroy` | one free |
 *
 * ## Failure
 *
 * - `ta_ring_init` answers `TA_ERR_INVALID` for a mode that is neither
 *   lossless nor lossy, a `max_record_bytes` that is not a multiple of 8
 *   or below `TA_RING_RECORD_MIN_BYTES`, or a capacity that is not a
 *   multiple of 8 or below `max_record_bytes`; and `TA_ERR_NOMEM` when
 *   the allocator refuses. Either way `*ring` is untouched.
 * - `ta_ring_reserve` answers `TA_ERR_INVALID`, changing nothing, for a
 *   size below `TA_RING_RECORD_MIN_BYTES` or above `max_record_bytes`;
 *   and `TA_ERR_FULL` when the ring is full, as above.
 * - `ta_ring_peek` and `ta_ring_release` answer `TA_ERR_NOT_FOUND` when
 *   nothing committed is left to read, and `TA_ERR_CORRUPT` for a record
 *   whose framing or seq is wrong; they then change nothing but the pads
 *   they have already skipped.
 * - `ta_ring_take_drops` answers `TA_ERR_NOT_FOUND` when nothing was
 *   dropped since it was last called.
 *
 * ## Users
 *
 * Three instances, beside the control block that publishes their
 * `capacity`, `head`, `tail` and `next_seq`:
 *
 * - **h2c**, host to core, lossless: the host refuses at its edge when it
 *   is full. Core is the consumer and its gap check is `ta_ring_peek`'s.
 * - **c2h**, core to host, lossless: core is the producer and commits
 *   after every record. Its mandatory reserve, a quota beside the ring,
 *   counts for each reserved record its aligned size plus one pad of up
 *   to `max_record_bytes - 8`, which holds however several slots'
 *   records interleave.
 * - **tel**, core to host, lossy and optional: a full ring drops
 *   telemetry and counts it, and the next `tel.dropped` reports the
 *   count and the first lost seq. Core reserves that record first and
 *   only then calls `ta_ring_take_drops` to fill it, so a refused report
 *   is counted with the rest.
 *
 * Memory comes from the injected allocator (core ADR 0023). A ring's size
 * is fixed when the system is created (core ADR 0092).
 */
#pragma once

#include "../include/tinyactors.h"

/** Every record and pad starts at a multiple of this, and its size is
 * one. */
#define TA_RING_ALIGNMENT 8u
/** The prefix the ring reads: `kind`, `flags` and `size`; a pad is just
 * this. */
#define TA_RING_PREFIX_BYTES 8u
/** The smallest record: the prefix and its `seq`. */
#define TA_RING_RECORD_MIN_BYTES 16u

/** What a full ring does with a record. */
typedef enum {
    /** Full means refuse; seq has no gaps. */
    TA_RING_LOSSLESS = 0,
    /** Full means drop the record, count it, and consume its seq. */
    TA_RING_LOSSY = 1
} ta_ring_mode;

/** What `ta_ring_init` needs. */
typedef struct {
    /** Where the buffer comes from; borrowed, and must outlive the ring. */
    ta_allocator allocator;
    /** Bytes in the buffer: a multiple of 8, at least `max_record_bytes`. */
    uint32_t capacity;
    /** The largest record: a multiple of 8, at least
     * `TA_RING_RECORD_MIN_BYTES`. */
    uint32_t max_record_bytes;
    ta_ring_mode mode;
} ta_ring_params;

/** One record in the buffer: a reservation the producer fills, or a
 * committed record the consumer reads. */
typedef struct {
    /** The record's first byte, 8-byte aligned. */
    unsigned char *bytes;
    /** The record's size, a multiple of 8, as its prefix says. */
    uint32_t size;
    /** The record's `kind`; 0 in a fresh reservation. */
    uint16_t kind;
    /** The record's sequence number. */
    uint64_t seq;
} ta_ring_record;

/** Records a lossy ring dropped and has not yet reported. */
typedef struct {
    uint64_t count;
    /** The seq the first of them would have had. */
    uint64_t first_seq;
} ta_ring_drops;

/** A single-producer, single-consumer byte ring; see the file comment. */
typedef struct {
    ta_allocator allocator;
    /** `capacity` bytes, aligned to 8. */
    unsigned char *bytes;
    uint32_t capacity;
    uint32_t max_record_bytes;
    ta_ring_mode mode;
    /** Bytes ever committed; written by the producer. */
    uint64_t head;
    /** Bytes ever released; written by the consumer. */
    uint64_t tail;
    /** Bytes ever reserved; the producer's private position. */
    uint64_t write;
    /** The seq the next reserved or dropped record takes; starts at 1. */
    uint64_t next_seq;
    /** The seq of the last record released; 0 before the first. */
    uint64_t read_seq;
    /** Records dropped since creation (lossy only). */
    uint64_t dropped;
    /** Dropped and not yet taken by `ta_ring_take_drops`. */
    ta_ring_drops unreported;
} ta_ring;

/**
 * Sets up an empty ring as `params` describe and allocates its buffer.
 * The first record takes seq 1. Runs only at a safe point, never inside
 * a macrostep.
 *
 * Answers `TA_ERR_INVALID` when `params` breaks a rule of
 * `ta_ring_params`, and `TA_ERR_NOMEM` when the allocator refuses; either
 * way `*ring` is untouched and nothing is allocated.
 */
ta_result ta_ring_init(ta_ring *ring, const ta_ring_params *params);

/**
 * Frees the buffer and leaves a ring of capacity 0 that must be set up
 * again with `ta_ring_init` before use. Does not look at the records;
 * their owners release what they refer to first. Never fails.
 */
void ta_ring_destroy(ta_ring *ring);

/**
 * Reserves one record of `size` bytes, rounded up to 8, at the
 * producer's position, writing a pad first when it does not fit before
 * the end of the buffer. Stamps the record's prefix (`kind` 0, `flags`
 * 0, its `size`) and its `seq`, takes the next seq, and answers the
 * record in `*out`; the producer fills the rest in place and makes it
 * visible with `ta_ring_commit`. Never allocates; may run inside a
 * macrostep. O(1).
 *
 * Answers `TA_ERR_INVALID`, changing nothing, for a size below
 * `TA_RING_RECORD_MIN_BYTES` or above `max_record_bytes`. When the
 * record and its pad do not fit beside the unreleased and reserved
 * bytes, answers `TA_ERR_FULL`: a lossless ring changes nothing, and a
 * lossy ring counts the record as dropped and consumes its seq. In
 * either mode, when the pad and the record could never fit together and
 * nothing is reserved, the pad alone is written and committed first, so
 * the next reservation after a release starts at offset 0.
 */
ta_result ta_ring_reserve(ta_ring *ring, uint32_t size, ta_ring_record *out);

/**
 * Makes every reserved record visible to the consumer: `head = write`.
 * The producer fills a record completely, its `kind` not 0, before it
 * commits. Never allocates; may run inside a macrostep. Never fails.
 */
static inline void ta_ring_commit(ta_ring *ring) { ring->head = ring->write; }

/**
 * Answers in `*out` the oldest committed record, in place, without
 * releasing it. Skips and releases the pads before it, which hold
 * nothing. Never allocates; may run inside a macrostep. O(1) per record,
 * plus one step per pad.
 *
 * Answers `TA_ERR_NOT_FOUND` when no committed record is left, and
 * `TA_ERR_CORRUPT` when the next record's framing or seq is wrong (see
 * the file comment); the ring is then unchanged but for the pads already
 * skipped, and `*out` is not written.
 */
ta_result ta_ring_peek(ta_ring *ring, ta_ring_record *out);

/**
 * Releases the oldest committed record, the one `ta_ring_peek` answers,
 * and the pads before it, so the producer may overwrite their bytes.
 * The address of that record is no longer valid. Never allocates; may
 * run inside a macrostep. O(1).
 *
 * Answers what `ta_ring_peek` would, and releases nothing but pads
 * unless that is `TA_OK`.
 */
ta_result ta_ring_release(ta_ring *ring);

/**
 * Answers in `*out` the records a lossy ring dropped since this was last
 * called, and the seq the first of them would have had, and forgets
 * them; `dropped` keeps counting since creation. Never allocates; may
 * run inside a macrostep. O(1).
 *
 * Answers `TA_ERR_NOT_FOUND`, and changes nothing, when nothing was
 * dropped since.
 */
ta_result ta_ring_take_drops(ta_ring *ring, ta_ring_drops *out);

/** Bytes in the buffer. Never allocates. */
static inline uint32_t ta_ring_capacity(const ta_ring *ring) { return ring->capacity; }

/** Bytes committed and not yet released: `head - tail`. Never
 * allocates. */
static inline uint32_t ta_ring_used(const ta_ring *ring) { return (uint32_t)(ring->head - ring->tail); }

/** Bytes neither released nor reserved, which the producer may still
 * reserve, pads included: `capacity - (write - tail)`. Never
 * allocates. */
static inline uint32_t ta_ring_available(const ta_ring *ring) {
    return ring->capacity - (uint32_t)(ring->write - ring->tail);
}
