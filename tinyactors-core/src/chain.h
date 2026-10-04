#pragma once

#include "types.h"
#include "pool.h"

/*
 * Package chain is the chunk chain (building block A6 of docs/proposals/
 * ring-proto): a byte stream stored in fixed-size chunks that are taken from
 * an A5 index pool (pool.h), where each chunk holds a payload and the u32
 * index of the next chunk.
 *
 * What it is for. Two later tables hold variable-length bytes whose length
 * the system cannot bound by a struct: the text of an envelope (its name
 * area: event name, target, sender, ... as u16-length-prefixed fields; phase
 * 4.2, 64-byte chunks) and the saved state of a parked execution (the park
 * area; phase 4.5, 256-byte chunks). Neither wants a malloc per record, and
 * both want to count what they use against a quota (C3, quota.h) before they
 * commit to it. A chain gives them one shape: bytes in chunks of one pool,
 * addressed by a 32-bit index, appended without ever allocating, read as a
 * sequence of spans, and given back in one call. One pool exists per chunk
 * size; chains of that size share it, so their chunks interleave in index
 * order and a freed chunk is reused by whichever chain writes next.
 *
 * Chunk layout. A chunk is chunk_size bytes: bytes 0..3 are the u32 `next`
 * (0 ends the chain), bytes 4..chunk_size-1 are payload. So a 64-byte chunk
 * carries 60 bytes of text and a 256-byte chunk 252 bytes of parked state.
 * The pool's link_offset is 0, which makes the pool's free-list link and the
 * chain's `next` the same four bytes. That is safe because the two uses
 * never overlap in time: while a chunk is free the pool owns the word, and
 * from ta_pool_take to ta_pool_give the chain does. pool.h promises that a
 * taken item's link bytes are zero, so a chunk that ends a chain has
 * next == 0 straight from the take, and nothing here writes a terminator.
 *
 * Lifetimes. The pool is system lifetime: ta_chain_pool_init makes the
 * control struct, the owner's safe points call ta_pool_refill on it, and
 * ta_pool_destroy at system destroy returns the pages; chain code never
 * gives a page back to the allocator. A chunk lives from the take inside a
 * write to the give inside ta_chain_free, and belongs for that time to one
 * chain. A ta_chain is a 12-byte value with no pointers, stored by value in
 * its owner (an envelope: envelope lifetime; a parked slot: parked
 * lifetime). A chain has exactly one owner; copying the struct copies the
 * ownership, not the bytes, so the copy's source must be zeroed. A zeroed
 * chain is an empty chain that holds no chunk, so there is nothing to free
 * for it and freeing it twice is harmless. A reader and the spans it yields
 * have call lifetime (a few stack lines): a span points into page memory and
 * is valid until the chain is freed or the pool is trimmed or destroyed. A
 * write only fills the unused end of the tail chunk or adds chunks; it never
 * moves a byte, but a span that ended at the old length does not show the
 * appended bytes.
 *
 * When it allocates. Never. Writing takes chunks with ta_pool_take, which
 * serves from the pool's free list or its never-issued extent, both already
 * inside pages. Making room is the owner's job at a safe point
 * (ta_pool_refill), and reserving it against a quota is the owner's job too:
 * ta_chain_need says how many chunks a write will take, the owner reserves
 * that many from its C3 quota, refills the pool if fewer are free, and then
 * writes. The chain does no quota accounting, so it depends on pool.h alone.
 *
 * Failure. A write is all or nothing. It first checks that length + bytes
 * fits a u32 and that the pool has ta_chain_need chunks free, and only then
 * takes chunks and copies. A failed write has changed neither the chain, nor
 * the pool's counters, nor its free list, nor any chunk byte. The pool check
 * is defence in depth: a caller that reserved with need and refilled never
 * sees it fail. All arithmetic is in uint32_t and uint64_t with the bounds
 * checked first, so no size an input influences can overflow.
 *
 * Invariants. length is the sum of the bytes in the chunks; head == 0 iff
 * tail == 0 iff length == 0; the chain from head, following next, reaches
 * tail after ceil(length / payload) chunks; every chunk but the tail is
 * full, so the tail holds length - (chunks - 1) * payload bytes (in
 * particular the fill of the tail is length % payload, or payload when that
 * is 0); the tail's next is 0; a chunk is on exactly one chain.
 *
 * Complexity. need, payload, the reader's init, span and remaining are O(1).
 * write is O(bytes) and free O(chunks); the tail index makes an append O(1)
 * to find its place instead of a walk. Space: 4 bytes per chunk of overhead
 * and, on average, half a chunk of slack per chain.
 *
 * Misuse that is not checked, because checking would cost a walk: writing a
 * chain with one pool and reading or freeing it with another; two owners
 * holding the same chain; reading while writing.
 *
 * Usage, text of an envelope in 64-byte chunks:
 *
 *	ta_pool names;
 *	ta_chain_pool_init(&names, allocator, &(ta_chain_pool_params){
 *		.chunk_size = 64, .page_chunks = 256 });
 *	...
 *	ta_chain text = { 0 };
 *	uint32_t chunks = ta_chain_need(&names, &text, size);
 *	// reserve `chunks` from the quota; at a safe point ta_pool_refill(&names, chunks)
 *	ta_chain_write(&names, &text, bytes, size);       // cannot fail now
 *	ta_chain_reader reader;
 *	ta_chain_span span;
 *	ta_chain_reader_init(&reader, &names, &text);
 *	while (ta_chain_reader_span(&reader, &span)) {
 *		use(span.bytes, span.length);
 *	}
 *	release(ta_chain_free(&names, &text));            // chunks back to the quota
 *	ta_pool_destroy(&names);
 */

/*
 * ta_chain is one byte stream (lifetime: its owner's, by value). A zeroed
 * chain is empty. The fields are for chain.c; owners store and zero the struct.
 */
typedef struct ta_chain {
    uint32_t head;   /* first chunk index, 0 when empty */
    uint32_t tail;   /* last chunk index, 0 when empty; lets a write append without a walk */
    uint32_t length; /* bytes stored; a chain cannot hold more than UINT32_MAX */
} ta_chain;

/* ta_chain_pool_params describes one chunk pool. It is read by ta_chain_pool_init and not kept. */
typedef struct ta_chain_pool_params {
    uint32_t chunk_size;  /* bytes per chunk including the 4-byte link: a multiple of 4, at least 8 */
    uint32_t page_chunks; /* chunks per page; a power of two (A5 rule) */
    uint32_t max_chunks;  /* ceiling on the capacity; 0 means as many as A5 allows (A5 semantics) */
} ta_chain_pool_params;

/* ta_chain_span is a contiguous run of payload bytes inside one chunk (lifetime: call). */
typedef struct ta_chain_span {
    const unsigned char *bytes;
    uint32_t length;
} ta_chain_span;

/* ta_chain_reader walks a chain from its first byte (lifetime: call). Its fields are for chain.c. */
typedef struct ta_chain_reader {
    const ta_pool *pool;
    uint32_t chunk;     /* the chunk the next byte is in; 0 when finished */
    uint32_t offset;    /* payload bytes of that chunk already read */
    uint32_t remaining; /* bytes of the chain not yet read */
} ta_chain_reader;

/*
 * ta_chain_pool_init initialises *pool as a chunk pool: ta_pool_init with
 * item_size = chunk_size, item_alignment 4, link_offset 0 and no template
 * (chunks start zeroed). It allocates nothing, so it needs no safe point.
 * It answers TA_ERR_INVALID for a chunk_size under 8 or not a multiple of 4,
 * and whatever ta_pool_init answers for the other fields; on failure the
 * pool is zeroed.
 */
ta_result ta_chain_pool_init(ta_pool *pool, ta_allocator allocator, const ta_chain_pool_params *params);

/*
 * ta_chain_payload returns the payload bytes per chunk of a chunk pool
 * (chunk_size - 4). The pool must have been made by ta_chain_pool_init and
 * not destroyed. It never allocates. O(1).
 */
uint32_t ta_chain_payload(const ta_pool *pool);

/*
 * ta_chain_need returns how many chunks appending `bytes` to `chain` takes:
 * only the part that does not fit in the unused end of the tail chunk needs
 * new chunks. `chain` may be NULL or empty, meaning a new chain:
 * ceil(bytes / payload), and 0 for 0 bytes. It never allocates and cannot
 * overflow. It does not check that length + bytes fits a u32 (ta_chain_write
 * does). The owner reserves this many chunks from its quota before writing;
 * nothing may write the same chain between the need and the write.
 * O(1).
 */
uint32_t ta_chain_need(const ta_pool *pool, const ta_chain *chain, uint32_t bytes);

/*
 * ta_chain_write appends `bytes` bytes from `data` to the chain, first
 * filling the unused end of the tail chunk and then taking chunks from the
 * pool. It never allocates: the chunks come from the pool's free list or its
 * extent. It is all or nothing: it checks that length + bytes fits a u32 and
 * that the pool has ta_chain_need chunks free (ta_pool_counts_get), and only
 * then takes and copies; on failure the chain, the pool's counters and free
 * list, and every chunk byte are as they were. It answers TA_ERR_NOMEM for
 * either failure (a caller that reserved with ta_chain_need and refilled
 * never sees the pool one, and sees the length one only past 4 GiB) and
 * TA_ERR_INVALID for a NULL pool or chain, or NULL data with bytes != 0. A
 * write of 0 bytes succeeds and changes nothing. O(bytes).
 */
ta_result ta_chain_write(ta_pool *pool, ta_chain *chain, const void *data, uint32_t bytes);

/*
 * ta_chain_reader_init starts a read at the first byte of `chain`. The
 * reader keeps a pointer to the pool and a copy of the chain's length, so
 * the chain must not be written or freed, nor the pool trimmed, while it is
 * read. It never allocates. O(1).
 */
void ta_chain_reader_init(ta_chain_reader *reader, const ta_pool *pool, const ta_chain *chain);

/* ta_chain_reader_remaining returns the bytes of the chain not yet read. O(1). */
uint32_t ta_chain_reader_remaining(const ta_chain_reader *reader);

/*
 * ta_chain_reader_span yields the next span into *out and advances past it,
 * or answers false when the chain is exhausted. A span is never empty, is at
 * most payload bytes, and ends at the chain's length, so the spans cover the
 * chain exactly once and in order. The bytes belong to the chain (see the
 * package comment for how long they stay valid): the caller does not write
 * through them. It never allocates. O(1).
 */
bool ta_chain_reader_span(ta_chain_reader *reader, ta_chain_span *out);

/*
 * ta_chain_reader_copy copies exactly `bytes` bytes to `out` and advances,
 * across chunk boundaries (for fields, such as a u16-length-prefixed string,
 * that may straddle two chunks). If fewer than `bytes` remain it answers
 * false and neither copies nor advances. Copying 0 bytes succeeds. It never
 * allocates. O(bytes).
 */
bool ta_chain_reader_copy(ta_chain_reader *reader, void *out, uint32_t bytes);

/*
 * ta_chain_free gives every chunk of the chain back to the pool and zeroes
 * *chain. It reads each chunk's next before the give, because the give
 * overwrites that word with the pool's link. It returns how many chunks it
 * gave back, which the owner releases from its quota. A zeroed chain is a
 * no-op that returns 0, so freeing twice is safe. It never allocates and
 * never returns memory to the allocator (pages stay with the pool). O(chunks).
 */
uint32_t ta_chain_free(ta_pool *pool, ta_chain *chain);
