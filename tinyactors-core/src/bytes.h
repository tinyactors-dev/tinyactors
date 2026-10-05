/**
 * @file bytes.h
 *
 * # Byte sinks and sources in core
 *
 * A data model writes a value into a `ta_byte_sink` and reads one back
 * from a `ta_byte_source` (ADR 0090, `include/tinyactors.h`, "byte sinks
 * and sources"). The binding provides the sinks and sources that reach
 * the host; this unit holds the ones core provides itself:
 *
 * - `ta_arena_sink`, a sink that gathers one unit's pieces into one
 *   contiguous run of an arena;
 * - `ta_memory_source`, a source over the bytes of one unit already in
 *   memory, handed out in the reader's pieces;
 * - `ta_chunk_sink`, a sink that writes one unit into a new chunk chain
 *   (`chunks.h`);
 * - `ta_chunk_source`, a source over the bytes of one chunk chain.
 *
 * Each is a small state struct the caller embeds (usually on its stack)
 * and an adapter, `ta_arena_sink_as_byte_sink`,
 * `ta_memory_source_as_byte_source`, `ta_chunk_sink_as_byte_sink` or
 * `ta_chunk_source_as_byte_source`, that hands it to a data model or a
 * serializer as the public `ta_byte_sink` or `ta_byte_source`.
 *
 * ## Contract and invariants
 *
 * Both follow the public contract of sinks and sources:
 *
 * - A sink receives one unit between `begin` and `end`. `begin` starts an
 *   empty unit. `write` appends a piece, which is borrowed for the call
 *   and copied. `end(keep=true)` keeps the unit; `end(keep=false)`
 *   discards everything written since `begin`.
 * - A source holds exactly one unit. `read` fills up to `capacity` bytes
 *   of `buffer`, in order, sets `*got` to the count, and answers
 *   `TA_OP_OK`; `*got == 0` means the unit has ended, and every later read
 *   answers the same.
 *
 * Arena sink invariants: `len <= cap`; `ptr` is NULL while `cap` is 0,
 * and otherwise points at `cap` bytes of the arena aligned for `uint64_t`,
 * of which the first `len` are the unit written so far. After
 * `end(keep=true)` the unit is `ptr[0 .. len)`, which the caller reads
 * from the struct's fields. `begin` and `end(keep=false)` set `len` to 0
 * and keep the run, so the next unit reuses it.
 *
 * Memory source invariant: `at <= bytes.len`; the bytes not yet read are
 * `bytes.ptr[at .. bytes.len)`.
 *
 * Chunk sink invariants: between `begin` and `end`, `writer.chain` is the
 * unit written so far, a chain the sink owns. `begin` starts a new, empty
 * chain and leaves alone the chain of the unit before it, which belongs to
 * whoever took it. After `end(keep=true)` the unit is `writer.chain`, and
 * the caller takes it over from the struct's fields: from then on the
 * caller owns the chain and frees it with `ta_chunks_free`.
 * `end(keep=false)` returns every chunk of the unit to the pool and leaves
 * an empty chain.
 *
 * Chunk source invariant: `reader` is positioned at the first byte not
 * yet handed out. The source reads the chain without changing it; the
 * chain's owner frees it when the source is done.
 *
 * ## Memory
 *
 * - The arena sink owns nothing. It borrows the `ta_arena` given to
 *   `ta_arena_sink_init`, which must outlive it, and its run belongs to
 *   that arena: it stays valid until the arena is reset or destroyed.
 *   When a write does not fit, the sink takes a run of twice the size
 *   (at least 64 bytes) from the arena and copies the unit into it; the
 *   arena keeps the runs the sink outgrew until it is reset, so the
 *   arena's lifetime must be the unit's (an offload's own arena). A write
 *   may therefore allocate, through the arena, from the arena's
 *   `ta_allocator`. It is used at a safe point (unloading a session into
 *   an image), never inside a macrostep.
 * - The memory source owns nothing and never allocates. It borrows the
 *   bytes given to `ta_memory_source_init`, which must stay valid and
 *   unchanged until the reader is done with it. It may run inside a
 *   macrostep.
 * - The chunk sink and the chunk source borrow the `ta_chunks` area given
 *   to their init, which must outlive them, and never allocate: the sink
 *   takes only chunks already free in the area's pool, so it may run
 *   inside a macrostep, and the area's user refills the pool at a safe
 *   point before (with a count `ta_chunks_need` gave for the unit's
 *   size). The sink owns the unit's chain until `end`: kept, it passes to
 *   the caller; discarded, its chunks go back to the pool. The source
 *   borrows the chain, which must not be written or freed until the
 *   reader is done with it; it may run inside a macrostep.
 * - The adapters and `ta_arena_sink_init` / `ta_memory_source_init` /
 *   `ta_chunk_sink_init` / `ta_chunk_source_init` allocate nothing. An adapter points at the state struct, so the
 *   struct must not move or end while the adapter is in use.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_arena_sink_init`, `ta_memory_source_init`, the adapters | O(1) |
 * | arena sink `begin`, `end` | O(1) |
 * | arena sink `write` of n bytes | O(n), plus, when it grows, one arena allocation and a copy of the unit so far; doubling makes the copies O(1) amortized per byte |
 * | memory source `read` of up to n bytes | O(n) |
 * | `ta_chunk_sink_init`, `ta_chunk_source_init` | O(1) |
 * | chunk sink `begin`, `end(keep=true)` | O(1) |
 * | chunk sink `end(keep=false)` | O(chunks of the unit): returns each to the pool |
 * | chunk sink `write` of n bytes | O(n), one pool take per new chunk; appending never walks the chain |
 * | chunk source `read` of up to n bytes | O(n), one step per chunk crossed |
 *
 * ## Failure
 *
 * - The arena sink's `write` answers `TA_OP_NOMEM` when the arena cannot
 *   give a larger run, or the unit's size would not fit in `size_t`. A
 *   failed write changes nothing: the same run, length and capacity, and
 *   the bytes written before it remain. The writer then calls
 *   `end(keep=false)`, as the contract asks. `begin` and `end` never fail.
 * - The memory source never fails.
 * - The chunk sink's `write` answers `TA_OP_NOMEM` when the area's pool
 *   has fewer free chunks than the piece needs (`ta_chunks_write`
 *   answered `TA_ERR_FULL`). A failed write changes nothing: no chunk
 *   taken, and the unit written before it remains. The writer then calls
 *   `end(keep=false)`, which returns the unit's chunks. `begin` and `end`
 *   never fail.
 * - The chunk source never fails.
 *
 * ## Users
 *
 * Unloading a session into an image writes each data value through an
 * arena sink into the image's arena, and loading reads each one back
 * through a memory source over its stored bytes (ADR 0090).
 *
 * Parking writes a slot's parked execution through a chunk sink into the
 * park area, inside a macrostep and so without allocating, and resuming
 * reads it back through a chunk source.
 */
#pragma once

#include "../include/tinyactors.h"
#include "chunks.h"

/** A sink that gathers one unit into one run of an arena; see the file
 * comment. Read `ptr` and `len` after `end(keep=true)`. */
typedef struct {
    /** Where runs come from; borrowed. */
    ta_arena *arena;
    /** The current run, `cap` bytes; NULL while `cap` is 0. */
    uint8_t *ptr;
    /** Bytes of the unit written so far. */
    size_t len;
    /** Bytes in the current run. */
    size_t cap;
} ta_arena_sink;

/** A source over the bytes of one unit in memory; see the file comment. */
typedef struct {
    /** The unit; borrowed. */
    ta_bytes bytes;
    /** Bytes already handed out. */
    uint32_t at;
} ta_memory_source;

/**
 * Starts an arena sink over `arena`, with no run yet. Borrows `arena`,
 * which must outlive the sink. Allocates nothing and never fails.
 */
void ta_arena_sink_init(ta_arena_sink *sink, ta_arena *arena);

/**
 * The public `ta_byte_sink` over `sink`, to hand to a data model's
 * `value_write`. Borrows `sink` for as long as the answer is used. Its
 * `write` may allocate from the arena and answers `TA_OP_NOMEM`, changing
 * nothing, when the arena refuses; so it is used at a safe point, never
 * inside a macrostep. This call itself allocates nothing.
 */
ta_byte_sink ta_arena_sink_as_byte_sink(ta_arena_sink *sink);

/**
 * Starts a memory source over `bytes`, positioned at its first byte.
 * Borrows the bytes, which must stay valid and unchanged until the reader
 * is done. Allocates nothing and never fails.
 */
void ta_memory_source_init(ta_memory_source *source, ta_bytes bytes);

/**
 * The public `ta_byte_source` over `source`, to hand to a data model's
 * `value_read`. Borrows `source` for as long as the answer is used. Its
 * `read` never allocates and never fails, and may run inside a macrostep.
 */
ta_byte_source ta_memory_source_as_byte_source(ta_memory_source *source);

/** A sink that writes one unit into a new chunk chain; see the file
 * comment. Take `writer.chain` after `end(keep=true)`. */
typedef struct {
    /** Where chunks come from; borrowed. */
    ta_chunks *chunks;
    /** The unit's chain and its last chunk. */
    ta_chunk_writer writer;
} ta_chunk_sink;

/** A source over the bytes of one chunk chain; see the file comment. */
typedef struct {
    /** The area the chain is in; borrowed. */
    const ta_chunks *chunks;
    /** The position of the next byte to hand out. */
    ta_chunk_reader reader;
} ta_chunk_source;

/**
 * Starts a chunk sink over `chunks`, with an empty chain. Borrows
 * `chunks`, which must outlive the sink. Allocates nothing and never
 * fails.
 */
void ta_chunk_sink_init(ta_chunk_sink *sink, ta_chunks *chunks);

/**
 * The public `ta_byte_sink` over `sink`. Borrows `sink` for as long as the
 * answer is used. Its `write` never allocates, so it may run inside a
 * macrostep, and answers `TA_OP_NOMEM`, changing nothing, when the area
 * has too few free chunks for the piece. This call itself allocates
 * nothing.
 */
ta_byte_sink ta_chunk_sink_as_byte_sink(ta_chunk_sink *sink);

/**
 * Starts a chunk source over `chain` in `chunks`, positioned at its first
 * byte. Borrows both: the chain must not be written or freed until the
 * reader is done. Allocates nothing and never fails.
 */
void ta_chunk_source_init(ta_chunk_source *source, const ta_chunks *chunks, ta_chunk_chain chain);

/**
 * The public `ta_byte_source` over `source`. Borrows `source` for as long
 * as the answer is used. Its `read` never allocates and never fails, and
 * may run inside a macrostep.
 */
ta_byte_source ta_chunk_source_as_byte_source(ta_chunk_source *source);
