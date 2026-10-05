#pragma once

#include "types.h"
#include "arena.h"
#include "chain.h"

/*
 * Package byte_io is the byte sinks and sources of core (building block D1 of
 * docs/proposals/ring-proto; ADR 0090 for ta_byte_sink and ta_byte_source in
 * include/tinyactors.h). Each type here adapts one kind of storage to the
 * public interfaces, so that a data model writes or reads one unit of bytes
 * without knowing where they go or come from.
 *
 * What it is for. A data model encodes a value by calling a sink in pieces
 * and decodes one by asking a source for pieces. Core has three places that
 * hold such bytes: an arena (an offload's encoded values), a caller's buffer
 * (the bytes of an offloaded value being read back), and a chunk chain (A6:
 * an envelope's name area, a parked continuation, later a host blob). This
 * unit holds one adapter per place, so no caller writes its own.
 *
 * Lifetimes. Every adapter is call lifetime: a struct of a few stack lines in
 * the caller, made for one write or read of one unit, dropped after. None of
 * them owns the storage it works on; each says who does.
 *
 *	ta_arena_sink    the bytes belong to the arena, and live until the arena
 *	                 is reset (the offload's lifetime). The sink frees nothing.
 *	ta_memory_source the bytes belong to whoever lent them (the offload
 *	                 buffer); they must outlive the read. Nothing is owned.
 *	ta_chain_sink    the chunks belong to the chain, which belongs to its
 *	                 owner (envelope or parked lifetime), in a pool of system
 *	                 lifetime. The sink takes chunks, and gives them back only
 *	                 when the unit is discarded with end(keep=false).
 *	ta_chain_source  reads a chain without owning it; the chain must be
 *	                 neither written nor freed, nor the pool trimmed, meanwhile.
 *
 * When it allocates. Only ta_arena_sink, and only inside write when the
 * current run is too small: it asks the arena it was given for a doubled run
 * (at least 64 bytes), so it is used only where ta_arena_alloc is allowed.
 * init, as, begin and end never allocate. The memory source and both chain
 * adapters never allocate: the chain sink takes chunks with ta_chain_write,
 * which serves from the pool's free list and extent only. Making room
 * (ta_pool_refill) and reserving it against a quota stay with the chain's
 * owner at a safe point, before begin, sized with ta_chain_need.
 *
 * Failure. A sink answers TA_OP_NOMEM when its storage is full and changes
 * nothing for the piece that failed. Pieces written before it stay until the
 * writer calls end(keep=false), which discards the whole unit: that is how a
 * unit is all or nothing (the writer must call end after a failed write, as
 * ADR 0090 says). Size sums are checked: a unit that would overflow size_t
 * (arena) or UINT32_MAX bytes (chain) answers TA_OP_NOMEM. For a chain the
 * rollback is ta_chain_free, which is why the chain sink starts only on an
 * empty chain; appending a unit to a non-empty chain with rollback would
 * need a truncate in chain.h, which does not exist yet.
 *
 * Complexity. A write is O(piece), plus the copy of the unit so far when the
 * arena sink grows (amortised O(1) per byte, as the run doubles). A read is
 * O(piece).
 *
 * Usage, a unit through the arena and back:
 *
 *	ta_arena_sink state;
 *	ta_arena_sink_init(&state, arena);
 *	ta_byte_sink sink = ta_arena_sink_as(&state);
 *	sink.v->begin(sink.self);
 *	... datamodel->value_write(..., .sink = sink, ...);
 *	sink.v->end(sink.self, status == TA_OP_OK);
 *	ta_memory_source memory;
 *	ta_memory_source_init(&memory, (ta_bytes){ state.ptr, state.len });
 *	ta_byte_source source = ta_memory_source_as(&memory);
 */

/*
 * ta_arena_sink gathers one unit into one run of an arena (ADR 0090). The
 * run doubles as the unit grows; the arena keeps the runs it outgrew until it
 * is reset. The fields are public because the caller reads ptr and len after
 * end(keep=true); they are valid until the arena is reset.
 */
typedef struct ta_arena_sink {
    ta_arena *arena;
    uint8_t *ptr;
    size_t len;
    size_t cap;
} ta_arena_sink;

/* ta_arena_sink_init makes *sink an empty sink over `arena` (borrowed). It never allocates. */
void ta_arena_sink_init(ta_arena_sink *sink, ta_arena *arena);

/*
 * ta_arena_sink_as returns the ta_byte_sink that writes into *sink. The
 * adapter borrows *sink, which must outlive it. begin starts a unit (len = 0,
 * the run is reused); write appends and allocates only when the run is full;
 * end(keep=false) sets len to 0 and frees nothing.
 */
ta_byte_sink ta_arena_sink_as(ta_arena_sink *sink);

/* ta_memory_source hands out the bytes of one unit in the reader's pieces. */
typedef struct ta_memory_source {
    ta_bytes bytes;
    uint32_t at;
} ta_memory_source;

/* ta_memory_source_init makes *source read `bytes` (borrowed; they must outlive the read) from the start. */
void ta_memory_source_init(ta_memory_source *source, ta_bytes bytes);

/*
 * ta_memory_source_as returns the ta_byte_source that reads *source. read
 * copies min(capacity, remaining) bytes and sets *got to the count; *got == 0
 * means the unit has ended. It always answers TA_OP_OK and never allocates.
 */
ta_byte_source ta_memory_source_as(ta_memory_source *source);

/*
 * ta_chain_sink_params says where a chain sink writes. It is read by
 * ta_chain_sink_init and kept in the sink, so everything it points to is
 * borrowed and must outlive the sink.
 */
typedef struct ta_chain_sink_params {
    ta_pool *pool;    /* a chunk pool made by ta_chain_pool_init */
    ta_chain *chain;  /* must be empty at begin; the owner keeps it, and nothing else may write it meanwhile */
    uint32_t reserve; /* bytes the unit is expected to take; 0 means do not preflight */
} ta_chain_sink_params;

/*
 * ta_chain_sink appends one unit to an empty chain (lifetime: call). The
 * fields after params are the sink's state; the owner reads `freed` after
 * end(keep=false) to release that many chunks from its quota.
 */
typedef struct ta_chain_sink {
    ta_chain_sink_params params;
    uint32_t freed; /* chunks given back by the last end(keep=false) */
    bool open;      /* between begin and end */
} ta_chain_sink;

/* ta_chain_sink_init makes *sink a closed sink over `params`. It never allocates. */
void ta_chain_sink_init(ta_chain_sink *sink, const ta_chain_sink_params *params);

/*
 * ta_chain_sink_as returns the ta_byte_sink that writes into *sink; it
 * borrows *sink. None of its operations allocates.
 *
 * begin answers TA_OP_ERROR if the sink is open or the chain is not empty,
 * and TA_OP_NOMEM, taking nothing, if params.reserve is not 0 and
 * ta_chain_need(reserve) exceeds the pool's free chunks. write answers
 * TA_OP_ERROR if the sink is not open, and TA_OP_NOMEM, changing nothing,
 * when the piece needs more chunks than the pool has free or would take the
 * unit past UINT32_MAX bytes (it relies on ta_chain_write's all-or-nothing
 * check, so the failing piece takes no chunk; earlier pieces stay until
 * end). end(keep=true) leaves the chain with its owner, who frees it later
 * with ta_chain_free; end(keep=false) gives every chunk of the unit back to
 * the pool, zeroes the chain and records the count in `freed`. end on a
 * closed sink does nothing.
 */
ta_byte_sink ta_chain_sink_as(ta_chain_sink *sink);

/* ta_chain_source reads a chain back from its first byte, in order (lifetime: call). */
typedef struct ta_chain_source {
    ta_chain_reader reader;
} ta_chain_source;

/*
 * ta_chain_source_init makes *source read `chain` from its first byte. The
 * chain and pool are borrowed: nothing may write or free the chain, or trim
 * the pool, until the read is done. It never allocates.
 */
void ta_chain_source_init(ta_chain_source *source, const ta_pool *pool, const ta_chain *chain);

/*
 * ta_chain_source_as returns the ta_byte_source that reads *source. read
 * copies min(capacity, remaining) bytes across chunk boundaries and sets
 * *got to the count; *got == 0 means the unit has ended. It always answers
 * TA_OP_OK and never allocates. O(piece).
 */
ta_byte_source ta_chain_source_as(ta_chain_source *source);
