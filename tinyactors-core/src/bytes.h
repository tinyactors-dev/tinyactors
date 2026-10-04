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
 *   memory, handed out in the reader's pieces.
 *
 * Each is a small state struct the caller embeds (usually on its stack)
 * and an adapter, `ta_arena_sink_as_byte_sink` or
 * `ta_memory_source_as_byte_source`, that hands it to a data model as the
 * public `ta_byte_sink` or `ta_byte_source`.
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
 * - The adapters and `ta_arena_sink_init` / `ta_memory_source_init`
 *   allocate nothing. An adapter points at the state struct, so the
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
 *
 * ## Failure
 *
 * - The arena sink's `write` answers `TA_OP_NOMEM` when the arena cannot
 *   give a larger run, or the unit's size would not fit in `size_t`. A
 *   failed write changes nothing: the same run, length and capacity, and
 *   the bytes written before it remain. The writer then calls
 *   `end(keep=false)`, as the contract asks. `begin` and `end` never fail.
 * - The memory source never fails.
 *
 * ## Users
 *
 * Unloading a session into an image writes each data value through an
 * arena sink into the image's arena, and loading reads each one back
 * through a memory source over its stored bytes (ADR 0090).
 */
#pragma once

#include "types.h"

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
