/**
 * @file chunks.h
 *
 * # Chunk chains
 *
 * `ta_chunks` stores byte streams, *chains*, in fixed-size chunks taken
 * from an index pool (`index_pool.h`) it embeds. Each chunk holds a
 * `uint32_t` link to the next chunk of its chain (0 at the last) and then
 * its payload, `chunk_size - 4` bytes of the stream. One `ta_chunks`
 * serves every chain of one area; the chunk size is its parameter, so one
 * unit gives the name area (64-byte chunks, 60 bytes of text each) and the
 * park area (256-byte chunks).
 *
 * A chain is what its owner stores: `ta_chunk_chain`, the index of its
 * first chunk and its length in bytes, 8 bytes in all. An empty chain is
 * `{0, 0}` and holds no chunk, as index 0 names no item of the pool.
 *
 * - `ta_chunks_need` gives the chunks a write of `n` more bytes takes, for
 *   preflight;
 * - a `ta_chunk_writer` appends bytes to a chain;
 * - a `ta_chunk_reader` hands a chain back as spans, one chunk's payload
 *   (or part of it) at a time;
 * - `ta_chunks_free` returns every chunk of a chain to the pool.
 *
 * ## Contract and invariants
 *
 * - A chain of `length` bytes holds exactly `ceil(length / payload)`
 *   chunks, linked in order from `first`; byte `i` of the stream is byte
 *   `i % payload` of the payload of chunk `i / payload`. Every chunk but
 *   the last is full; the last chunk's link is 0. Only the bytes up to
 *   `length` are the stream; the rest of the last chunk is unspecified.
 * - **One owner per chain**: an envelope, a waiting entry or a parked
 *   slot. Its chunks belong to no other chain. Whoever holds the
 *   `ta_chunk_chain` frees it, once; a copy of the struct is not a second
 *   chain.
 * - **`ta_chunks_need` only counts; it reserves nothing.** It answers how
 *   many chunks a write of `n` bytes onto a chain of `length` bytes takes:
 *   `ceil((length + n) / payload) - ceil(length / payload)`. The space
 *   left in the last chunk is used first, so a write that fits there needs
 *   0. A write fails only when its need passes the pool's free count
 *   (`chunks.pool.free`), so a write made after `need <= free`, with no
 *   chunk taken from the pool in between, cannot fail. A caller that
 *   checks the total of several writes onto one chain, and then makes
 *   them, has the same guarantee: the needs add up.
 * - A chain holds at most `UINT32_MAX` bytes. For a write that would pass
 *   it, `ta_chunks_need` answers `UINT32_MAX`, more than the pool can
 *   ever have free (the ceiling is below `UINT32_MAX`), so the write
 *   answers `TA_ERR_FULL` like any other that does not fit.
 * - A writer borrows its chain from the owner while it writes, and the
 *   owner takes the chain back from `writer.chain` when it is done. A
 *   reader copies the chain's first index and length and never changes
 *   the chain; it stays valid while the chain is neither written nor
 *   freed.
 *
 * ## Memory
 *
 * The chunks live in the embedded `ta_index_pool`, whose pages come from
 * the `ta_allocator` given to `ta_chunks_init`, borrowed and outliving the
 * area. The area owns the pool; each chain's chunks belong to the chain's
 * owner until it frees them.
 *
 * - Taking chunks never allocates: `ta_chunks_write` takes only chunks
 *   already on the pool's free list. Every function here except
 *   `ta_chunks_destroy` allocates nothing and may run inside a macrostep.
 * - The area grows only when its user calls
 *   `ta_index_pool_refill(&chunks->pool, min_free)` at a safe point,
 *   typically with a count `ta_chunks_need` gave, and gives free pages
 *   back only by `ta_index_pool_trim(&chunks->pool, keep)` when the host
 *   asks core to give memory back. Chunks never move, so trim frees only
 *   pages whose chunks are all free.
 * - A span a reader answers points into a chunk. It is valid until the
 *   chain is freed; read it at once and keep the reader, not the span.
 *
 * ## Complexity
 *
 * `payload` is `chunk_size - 4`.
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_chunks_init`, `ta_chunks_need`, `ta_chunk_writer_new`, `ta_chunk_reader_of` | O(1) |
 * | `ta_chunk_writer_open` on a chain of `k` chunks | O(k): walks to the last chunk |
 * | `ta_chunks_write` of `n` bytes | O(n), one pool take per new chunk; the last chunk is the writer's, so appending never walks the chain |
 * | `ta_chunks_read` | O(1) to find the span, plus the caller's use of it |
 * | `ta_chunks_free` of a chain of `k` chunks | O(k): walks the chain, one pool give per chunk |
 * | `ta_chunks_destroy` | the pool's destroy: one free per page |
 *
 * ## Failure
 *
 * - `ta_chunks_init` answers `TA_ERR_INVALID` for parameters that break a
 *   rule of `ta_chunks_params`, leaving `*chunks` untouched.
 * - `ta_chunks_write` answers `TA_ERR_FULL` when the pool has fewer free
 *   chunks than the write needs, or the chain would pass `UINT32_MAX`
 *   bytes; it changes nothing: no chunk taken, no byte written, the
 *   writer as it was. The user refills at its next safe point, or answers
 *   for the area that is full.
 * - Nothing else fails. Writing to or freeing a chain that is not this
 *   area's, freeing one twice, or reading a chain after it was freed are
 *   programmer errors, not checked.
 *
 * ## Users
 *
 * The **name area** keeps the length-prefixed text fields of an envelope,
 * and the address of a target that is not resident once per waiting
 * entry, in 64-byte chunks. The **park area** keeps a parked execution
 * (its continuation and pending internal events) in 256-byte chunks,
 * written through the chunk-chain byte sink and read back through the
 * chunk-chain byte source (`bytes.h`).
 */
#pragma once

#include "../include/tinyactors.h"
#include "index_pool.h"

/** Bytes of a chunk's link, at the start of every chunk. */
#define TA_CHUNKS_LINK_BYTES 4u

/** What `ta_chunks_init` needs. */
typedef struct {
    /** Where the pool's pages come from; borrowed, and must outlive the area. */
    ta_allocator allocator;
    /** Bytes per chunk, link included: a multiple of 4, at least 8. */
    uint32_t chunk_size;
    /** Each pool page holds `1 << page_shift` chunks; as for `ta_index_pool_params`. */
    uint32_t page_shift;
    /** The most chunks the area may hold: from 1 to `UINT32_MAX - 1`. */
    uint32_t ceiling;
} ta_chunks_params;

/**
 * An area of chunks for chains; see the file comment. `pool` is refilled
 * and trimmed by the area's user; `payload` is for reading only.
 */
typedef struct {
    /** The chunks: items of `chunk_size` bytes, linked through their first 4 bytes. */
    ta_index_pool pool;
    /** Stream bytes per chunk: `chunk_size - 4`. */
    uint32_t payload;
} ta_chunks;

/** A chain as its owner stores it: its first chunk (0 when empty) and its length in bytes. */
typedef struct {
    uint32_t first;
    uint32_t length;
} ta_chunk_chain;

/** Appends to one chain; see `ta_chunk_writer_new` and `ta_chunk_writer_open`. */
typedef struct {
    /** The chain written so far; the owner takes it back from here. */
    ta_chunk_chain chain;
    /** The chain's last chunk, 0 while it is empty. */
    uint32_t last;
} ta_chunk_writer;

/** Hands a chain out as spans; see `ta_chunk_reader_of`. */
typedef struct {
    /** The chunk the next span starts in; meaningless once `remaining` is 0. */
    uint32_t chunk;
    /** Where in that chunk's payload the next span starts. */
    uint32_t offset;
    /** Stream bytes not yet handed out. */
    uint32_t remaining;
} ta_chunk_reader;

/**
 * Sets up an area with no chunk yet, over an index pool of `chunk_size`
 * items linked through their first 4 bytes. Allocates nothing; may run
 * anywhere. Answers `TA_ERR_INVALID`, leaving `*chunks` untouched, when
 * `chunk_size` is not a multiple of 4 or is below 8, the ceiling is 0 or
 * `UINT32_MAX`, or the pool refuses the page shift.
 */
ta_result ta_chunks_init(ta_chunks *chunks, const ta_chunks_params *params);

/**
 * Frees every page of the area and leaves it empty; every chain in it is
 * gone, freed or not. Only frees; never fails. Runs where the area's
 * owner is torn down, never inside a macrostep.
 */
void ta_chunks_destroy(ta_chunks *chunks);

/**
 * The chunks a write of `n` bytes onto a chain of `length` bytes takes:
 * `ceil((length + n) / payload) - ceil(length / payload)`, or `UINT32_MAX`
 * when `length + n` would pass `UINT32_MAX`. Pass 0 for a new chain. Only
 * counts and reserves nothing; a write after `need <= chunks->pool.free`,
 * with no chunk taken in between, cannot fail. O(1). Never allocates; may
 * run inside a macrostep. Never fails.
 */
uint32_t ta_chunks_need(const ta_chunks *chunks, uint32_t length, uint32_t n);

/**
 * A writer of a new, empty chain. O(1); allocates nothing and takes no
 * chunk; may run inside a macrostep.
 */
static inline ta_chunk_writer ta_chunk_writer_new(void) {
    return (ta_chunk_writer){ .chain = { 0u, 0u }, .last = 0u };
}

/**
 * A writer that appends to `chain`, which it borrows until the owner
 * takes it back from `writer.chain`. Walks the chain to its last chunk:
 * O(chunks of the chain). Allocates nothing; may run inside a macrostep.
 * Never fails.
 */
ta_chunk_writer ta_chunk_writer_open(const ta_chunks *chunks, ta_chunk_chain chain);

/**
 * Appends `bytes` (borrowed for the call, and copied) to the writer's
 * chain: it fills the space left in the last chunk, then takes the chunks
 * `ta_chunks_need` counts from the pool's free list and links them in
 * order. A write of no bytes changes nothing.
 *
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_FULL` when
 * fewer chunks are free than the write needs, or the chain would pass
 * `UINT32_MAX` bytes; the pool, the chain and the writer are then exactly
 * as before.
 */
ta_result ta_chunks_write(ta_chunks *chunks, ta_chunk_writer *writer, ta_bytes bytes);

/**
 * A reader positioned at the first byte of `chain`. It copies what it
 * needs from the chain and borrows nothing. O(1); allocates nothing; may
 * run inside a macrostep.
 */
static inline ta_chunk_reader ta_chunk_reader_of(ta_chunk_chain chain) {
    return (ta_chunk_reader){ .chunk = chain.length == 0u ? 0u : chain.first, .offset = 0u, .remaining = chain.length };
}

/**
 * The next span of the reader's chain, at most `max` bytes, all within one
 * chunk: from the reader's position to the end of that chunk's payload or
 * of the chain, whichever comes first, cut to `max`. Moves the reader past
 * it, to the next chunk when it reached the end of this one. Answers a
 * span of length 0 when the chain has been read to its end, and every
 * later call answers the same; `max` 0 answers length 0 and does not move.
 * Pass `UINT32_MAX` to get each chunk's payload whole. The span points
 * into the chunk and is valid until the chain is freed.
 *
 * O(1). Allocates nothing; may run inside a macrostep. Never fails.
 */
ta_bytes ta_chunks_read(const ta_chunks *chunks, ta_chunk_reader *reader, uint32_t max);

/**
 * Returns every chunk of `*chain` to the pool's free list and sets
 * `*chain` to the empty chain. Freeing an empty chain does nothing. Walks
 * the chain: O(its chunks). Never allocates; may run inside a macrostep.
 * Never fails.
 */
void ta_chunks_free(ta_chunks *chunks, ta_chunk_chain *chain);
