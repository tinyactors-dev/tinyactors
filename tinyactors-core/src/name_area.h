/**
 * @file name_area.h
 *
 * # The name area
 *
 * `ta_name_area` holds the text of a system's envelopes: every name that
 * is not interned because it was made at run time (ADR 0100), and the
 * other variable parts of a pending input. It is a chunk area
 * (`chunks.h`) of 64-byte chunks, 60 bytes of text and a `uint32_t` link
 * each, bounded by `nameAreaBytes`, with a quota (`quota.h`) counting its
 * chunks against that bound (ADR 0099 decision 3).
 *
 * Envelopes are freed out of order (a timer outlives the mail around it),
 * so a bump arena would fragment; fixed chunks from a free list take and
 * free in O(1) with no fragmentation. Most envelopes need one chunk; the
 * overhead is 4 bytes in 64 plus the unused end of the last chunk.
 *
 * ## Fields
 *
 * An envelope's `text` is one chain of **fields**, each a `uint16_t`
 * length, little-endian, followed by that many bytes. A field holds at
 * most 65,535 bytes. The text a field may hold: the event name, the
 * origin, origintype and invokeid of external input, a delayed send's
 * target, the I/O processor type, a send ID that came from outside, and
 * an invocation error's reason. The order is the envelope's to choose;
 * the area only writes and reads fields in the order they were written.
 * A field's prefix and bytes may straddle a chunk boundary; the reader
 * hides that.
 *
 * ## The trace parent
 *
 * A traced envelope (its trace ID is not zero) carries the trace parent
 * its receiver's macrostep runs under (ADRs 0067, 0073) as its **last**
 * field, 40 bytes in all, so traces stay linked across sessions while
 * mail and timers wait in core. An untraced envelope adds nothing.
 *
 * | Part | Bytes | Meaning |
 * |---|---|---|
 * | length | 2 | 38, as for every field |
 * | context | 18 | trace ID high and low (8 each), W3C trace-flags (1), local detail (1) |
 * | cause | 20 | the causing step's key: session (8), macrostep (8), microstep (4) |
 *
 * Every number is little-endian. The step key's `family` is not kept: a
 * trace parent read back has family 0, and an envelope that needs it keeps
 * it in a field of its own (`run.h`). A short name plus the trace
 * parent passes 60 bytes, so a traced envelope usually takes a second
 * chunk. The envelope's flag `TA_ENVELOPE_FLAG_TRACED` says whether its
 * text ends with one; the field's length alone cannot, since another
 * field may also be 38 bytes long.
 *
 * ## Admission and the reserve
 *
 * The quota's capacity is the area's chunks, and it counts the chunks
 * that chains hold. A write made for admitted external input
 * (`admission` in its params) succeeds only while the chunks in use stay
 * at or below the **admission watermark**, `max(1, chunks ×
 * watermark_percent / 100)`, after the write; the rest is headroom for
 * what sessions send each other. `ta_name_area_admitting` says whether
 * one more chunk fits below it, for the control block's `admitting`.
 *
 * The **reserve** is the number of free chunks the area keeps allocated
 * at each safe point: `ta_name_area_refill` makes `min(reserve, chunks -
 * used)` chunks free. A macrostep writes only into chunks that already
 * exist.
 *
 * ## Contract and invariants
 *
 * - `quota.used` is the number of chunks every chain written through the
 *   area holds, and equals `chunks.pool.used` as long as every chain is
 *   written and freed through these functions, not through `chunks.h`
 *   directly.
 * - A write is all or nothing: it checks that its whole field (prefix
 *   and bytes) fits, against both the free chunks and the quota, before
 *   it writes anything. The needs of several fields onto one chain add
 *   up (`ta_name_area_field_need`), so a caller that checks the total
 *   first, and takes no chunk in between, writes them all.
 * - A chain has one owner: an envelope, or the waiting entry of an
 *   address that is not resident. The owner frees it once, with
 *   `ta_name_area_free`.
 *
 * ## Memory
 *
 * The area owns its chunk pages, allocated through the `ta_allocator`
 * given to `ta_name_area_init`, borrowed and outliving the area. Each
 * chain's chunks belong to the chain's owner until it frees them.
 *
 * - **Never allocate, and may run inside a macrostep:** `field_need`,
 *   `admitting`, `write`, `write_trace_parent`, `free`, `reader_of`,
 *   `next`, `read` and `trace_parent`. A write that finds no room answers
 *   `TA_ERR_FULL`; it never grows the area.
 * - **`ta_name_area_refill` allocates** pages, up to the bound. It runs
 *   only at a safe point (during admission, after a macrostep ends, at
 *   system creation), never inside a macrostep.
 * - **`ta_name_area_trim` and `ta_name_area_destroy` only free.** Trim
 *   gives back whole free pages above the reserve, only when the host
 *   asks core to give memory back; core never trims on its own.
 * - A span `ta_name_area_read` answers points into a chunk, and is valid
 *   until the chain is freed.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `field_need`, `admitting`, `reader_of` | O(1) |
 * | `write`, `write_trace_parent` of `n` bytes | O(n), one take per new chunk; appending never walks the chain |
 * | `next`, `read` | O(1) per chunk they cross |
 * | `trace_parent` | walks the chain to its last 40 bytes: O(chunks) |
 * | `free` | O(chunks of the chain) |
 * | `refill`, `trim` | as `index_pool.h`: one allocation or free per page |
 *
 * ## Failure
 *
 * - `ta_name_area_init` answers `TA_ERR_INVALID` for parameters that
 *   break a rule of `ta_name_area_params`, leaving `*area` untouched.
 * - A write answers `TA_ERR_INVALID` for a field longer than 65,535
 *   bytes, or a trace parent with no trace ID, and `TA_ERR_FULL` when the
 *   field does not fit: too few chunks free, the bound reached, or, for
 *   admission, the watermark. It changes nothing. A `<send>` then fails as
 *   `error.execution.capacity.names`, and an admitted input is bounced.
 * - `ta_name_area_next` answers `TA_ERR_NOT_FOUND` past the last field;
 *   `ta_name_area_trace_parent` answers `TA_ERR_NOT_FOUND` when the chain
 *   cannot end with a trace parent (shorter than 40 bytes, or its last 40
 *   bytes do not start with the length 38). `ta_name_area_next` answers
 *   `TA_ERR_CORRUPT` for a chain that ends inside a field, which a chain
 *   written through the area never does. A failed read changes neither
 *   the reader nor what it would have written.
 * - `ta_name_area_refill` answers `TA_ERR_NOMEM` when an allocation
 *   fails, and changes nothing.
 *
 * ## Users
 *
 * The system creates one name area beside its envelope pool. Admission
 * copies an input's text into it; a `<send>` writes the evaluated event
 * name and the sender's trace parent; a waiting entry keeps the address of
 * a target that is not resident once, however many envelopes wait for it.
 * When an envelope is taken, its name is resolved to atoms from these
 * bytes (`names.h`), never interned.
 */
#pragma once

#include "../include/tinyactors.h"
#include "chunks.h"
#include "quota.h"

/** Bytes of one chunk of the name area, link included. */
#define TA_NAME_AREA_CHUNK_BYTES 64u
/** Bytes of text one chunk holds. */
#define TA_NAME_AREA_TEXT_BYTES (TA_NAME_AREA_CHUNK_BYTES - TA_CHUNKS_LINK_BYTES)
/** Bytes of a field's length prefix. */
#define TA_NAME_AREA_PREFIX_BYTES 2u
/** The longest field, bytes. */
#define TA_NAME_AREA_FIELD_MAX 65535u
/** Bytes of a trace parent field's text. */
#define TA_NAME_AREA_TRACE_PARENT_BYTES 38u
/** Bytes a trace parent adds to a chain, its prefix included. */
#define TA_NAME_AREA_TRACE_FIELD_BYTES (TA_NAME_AREA_PREFIX_BYTES + TA_NAME_AREA_TRACE_PARENT_BYTES)
/** The smallest area, bytes: one chunk. */
#define TA_NAME_AREA_MIN_BYTES TA_NAME_AREA_CHUNK_BYTES
/** The largest area, bytes: 1 GiB. */
#define TA_NAME_AREA_MAX_BYTES 1073741824u
/** Each page of the area holds `1 << TA_NAME_AREA_PAGE_SHIFT` chunks. */
#define TA_NAME_AREA_PAGE_SHIFT 6u

/** What `ta_name_area_init` needs. */
typedef struct {
    /** Where pages come from; borrowed, and must outlive the area. */
    ta_allocator allocator;
    /** `nameAreaBytes`: from `TA_NAME_AREA_MIN_BYTES` to
     * `TA_NAME_AREA_MAX_BYTES`. The area holds `bytes / 64` chunks; a
     * remainder is not used. */
    uint32_t bytes;
    /** The admission watermark, percent of the chunks: from 1 to 99. */
    uint32_t watermark_percent;
    /** Free chunks refill keeps at a safe point: from 1 to the chunks. */
    uint32_t reserve;
} ta_name_area_params;

/** The text of one system's envelopes; see the file comment. Fields are
 * for reading only. */
typedef struct {
    /** The chunks; refill and trim through this unit. */
    ta_chunks chunks;
    /** The area's chunks, and the chunks chains hold. */
    ta_quota quota;
    /** The most chunks in use admission may leave:
     * `max(1, chunks × watermark_percent / 100)`. */
    uint32_t watermark;
    /** Free chunks refill keeps. */
    uint32_t reserve;
} ta_name_area;

/** What `ta_name_area_write` appends. */
typedef struct {
    /** The chain's writer (`chunks.h`), borrowed for the call. */
    ta_chunk_writer *writer;
    /** The field's bytes, borrowed for the call and copied. */
    ta_bytes text;
    /** A write for admitted external input, held to the watermark. */
    bool admission;
} ta_name_area_write_params;

/** What `ta_name_area_write_trace_parent` appends. */
typedef struct {
    /** The chain's writer, borrowed for the call. */
    ta_chunk_writer *writer;
    /** The trace parent, borrowed for the call; its trace ID is not zero. */
    const ta_trace_parent *parent;
    /** A write for admitted external input, held to the watermark. */
    bool admission;
} ta_name_area_trace_params;

/** Reads a chain field by field; see `ta_name_area_reader_of`. */
typedef struct {
    /** Where the next byte is. */
    ta_chunk_reader chunks;
    /** Bytes of the current field not read yet. */
    uint32_t field;
} ta_name_area_reader;

/**
 * Sets up an empty area, with no page yet. Allocates nothing; may run
 * anywhere. Answers `TA_ERR_INVALID`, leaving `*area` untouched, when a
 * parameter breaks its rule.
 */
ta_result ta_name_area_init(ta_name_area *area, const ta_name_area_params *params);

/**
 * Frees every page and leaves the area empty; every chain in it is gone,
 * freed or not. Only frees; never fails; never runs inside a macrostep.
 */
void ta_name_area_destroy(ta_name_area *area);

/**
 * Makes `min(reserve, chunks - used)` chunks free, allocating pages when
 * fewer are. Runs only at a safe point, never inside a macrostep. Answers
 * `TA_OK`, or `TA_ERR_NOMEM` when an allocation fails, changing nothing.
 */
ta_result ta_name_area_refill(ta_name_area *area);

/**
 * Gives whole free pages back to the allocator while at least `reserve`
 * chunks stay free. Only frees; never fails. Runs at a safe point, only
 * when the host asks core to give memory back.
 */
void ta_name_area_trim(ta_name_area *area);

/**
 * Whether one more chunk fits at or below the watermark: what the
 * control block's `admitting` reports for the area. It does not say
 * whether a chunk is free. O(1). Never allocates.
 */
bool ta_name_area_admitting(const ta_name_area *area);

/**
 * The chunks a field of `text_length` bytes, its prefix included, takes
 * when appended to a chain of `chain_length` bytes, or `UINT32_MAX` for a
 * field longer than `TA_NAME_AREA_FIELD_MAX` or a chain that would pass
 * `UINT32_MAX` bytes. Only counts. O(1). Never allocates; never fails.
 */
uint32_t ta_name_area_field_need(const ta_name_area *area, uint32_t chain_length, uint32_t text_length);

/**
 * Appends one field, `params->text` behind its length, to the writer's
 * chain. Never allocates; may run inside a macrostep. Answers `TA_OK`;
 * `TA_ERR_INVALID` for text longer than `TA_NAME_AREA_FIELD_MAX`; or
 * `TA_ERR_FULL` when the field does not fit (see the file comment). A
 * failed write changes nothing.
 */
ta_result ta_name_area_write(ta_name_area *area, const ta_name_area_write_params *params);

/**
 * Appends `params->parent` as a trace parent field, 40 bytes, to the
 * writer's chain; it is the chain's last field. Never allocates; may run
 * inside a macrostep. Answers `TA_OK`; `TA_ERR_INVALID` when the trace ID
 * is zero (an untraced envelope carries none); or `TA_ERR_FULL` as
 * `ta_name_area_write`. A failed write changes nothing.
 */
ta_result ta_name_area_write_trace_parent(ta_name_area *area, const ta_name_area_trace_params *params);

/**
 * Returns every chunk of `*chain` to the area and its quota, and sets
 * `*chain` empty; an empty chain changes nothing. O(its chunks). Never
 * allocates; may run inside a macrostep. Cannot fail.
 */
void ta_name_area_free(ta_name_area *area, ta_chunk_chain *chain);

/** A reader before the first field of `chain`. O(1); borrows nothing. */
static inline ta_name_area_reader ta_name_area_reader_of(ta_chunk_chain chain) {
    return (ta_name_area_reader){ .chunks = ta_chunk_reader_of(chain), .field = 0u };
}

/**
 * Skips what is left of the current field, reads the next field's length
 * into `*length`, and stands at its first byte. O(chunks crossed). Never
 * allocates. Answers `TA_OK`; `TA_ERR_NOT_FOUND` after the last field; or
 * `TA_ERR_CORRUPT` when the chain ends inside a field. A failure changes
 * neither the reader nor `*length`.
 */
ta_result ta_name_area_next(const ta_name_area *area, ta_name_area_reader *reader, uint32_t *length);

/**
 * The next span of the current field, at most `max` bytes and within one
 * chunk, and moves past it; length 0 once the field is read. The span is
 * valid until the chain is freed. O(1). Never allocates; never fails.
 */
ta_bytes ta_name_area_read(const ta_name_area *area, ta_name_area_reader *reader, uint32_t max);

/**
 * Reads the trace parent at the end of `chain` into `*out`, its cause's
 * family 0. O(chunks of the chain). Never allocates; may run inside a
 * macrostep. Answers `TA_OK`, or `TA_ERR_NOT_FOUND` (see the file
 * comment), leaving `*out` alone.
 */
ta_result ta_name_area_trace_parent(const ta_name_area *area, ta_chunk_chain chain, ta_trace_parent *out);
