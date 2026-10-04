/**
 * @file text_writer.h
 *
 * # Bounded text writer
 *
 * `ta_text_writer` writes text into a caller's buffer while there is room
 * and always counts the full length, so one pass both sizes and fills a
 * buffer: run a writer with no buffer to measure, or with a buffer that
 * may be too small and learn from `ta_text_finish` how much was needed.
 * `ta_text_render` builds on it to write text of unknown length into
 * memory of its own.
 *
 * ## Contract and invariants
 *
 * - `length` counts every byte put, whether or not it was stored.
 * - A writer with `skip` s stores text bytes s to s + capacity - 1 at
 *   `buffer[0 ..]`, so a long text is rendered through a small buffer a
 *   window at a time (ADR 0090). With `skip` 0 it stores from the start.
 * - A writer with a NULL `buffer` stores nothing and only counts.
 * - `ta_text_finish` NUL-terminates the text when the whole text and its
 *   NUL fit, and reports the bytes needed, the NUL included, either way.
 *   It assumes `skip` 0.
 * - `ta_text_render` calls its `write` function twice, once to measure and
 *   once to fill, and `write` must put the same text both times.
 *
 * ## Memory
 *
 * The writer owns nothing: its buffer belongs to the caller, and the
 * struct is a value the caller keeps on its stack. `ta_text_put`,
 * `ta_text_put_u64` and `ta_text_finish` never allocate and may run
 * inside a macrostep. `ta_text_render` allocates the text, NUL-terminated,
 * from the allocator it is given; the caller owns it and frees it with
 * that allocator, `length + 1` bytes at alignment 1.
 *
 * ## Complexity
 *
 * `ta_text_put` is O(length of the text); `ta_text_put_u64` is O(digits);
 * `ta_text_finish` is O(1); `ta_text_render` is two runs of `write` and one
 * allocation.
 *
 * ## Failure
 *
 * Putting never fails: text beyond the buffer is counted, not stored.
 * `ta_text_finish` answers `TA_ERR_NOMEM` when the text and its NUL did not
 * fit (and what was written is not useful). `ta_text_render` answers
 * `TA_ERR_NOT_FOUND` when `write` reports there is no text, and
 * `TA_ERR_NOMEM` when the allocation fails; on failure it allocates
 * nothing and leaves `*out` alone.
 *
 * ## Users
 *
 * Everything that answers text to a host or writes it into a trace: event
 * names, send IDs, origins and SCXML URIs, compile errors and the builder,
 * the data model, definitions, export, the codec, I/O and the interpreter.
 */
#pragma once

#include "../include/tinyactors.h"

/** Where text goes; see the file comment. Zero-initialise, then set `buffer`, `capacity` and `skip`. */
typedef struct {
    char *buffer;
    uint32_t capacity;
    uint32_t length;
    /* Bytes of the text before the window `buffer` holds: a writer with
     * skip s stores text bytes s to s + capacity - 1, so a long text is
     * rendered through a small buffer a window at a time (ADR 0090). */
    uint32_t skip;
} ta_text_writer;

/** Puts `text`, storing what falls in the window and counting all of it. Borrows `text`; never allocates. */
void ta_text_put(ta_text_writer *writer, ta_str text);
/** Puts `value` in decimal. Never allocates. */
void ta_text_put_u64(ta_text_writer *writer, uint64_t value);
/**
 * NUL-terminates the text and stores in `*needed` the bytes it takes with
 * its NUL. Answers `TA_ERR_NOMEM` (and nothing useful written) when it
 * did not fit. Never allocates.
 */
ta_result ta_text_finish(ta_text_writer *writer, uint32_t *needed);
/**
 * Writes text of unknown length into memory from `allocator`: `write` runs
 * once to measure and once to fill. It answers false when there is no text,
 * which becomes TA_ERR_NOT_FOUND.
 */
typedef bool (*ta_text_write_fn)(const void *context, ta_text_writer *writer);
/** What `ta_text_render` writes, with what, and where it stores the text. */
typedef struct {
    ta_text_write_fn write;
    const void *context;
    ta_str *out;
} ta_text_render_params;
/**
 * Renders `params->write`'s text into a NUL-terminated block from
 * `allocator` and stores it in `*params->out`; the caller owns the block.
 * Allocates. Answers `TA_ERR_NOT_FOUND` when there is no text and
 * `TA_ERR_NOMEM` when the allocation fails, changing nothing.
 */
ta_result ta_text_render(ta_allocator allocator, const ta_text_render_params *params);
