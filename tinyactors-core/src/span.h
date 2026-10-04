/**
 * @file span.h
 *
 * # Span
 *
 * `ta_span32` names a run of consecutive elements by the index of the
 * first, `offset`, and their number, `length`, without a pointer: the
 * array it indexes is known from where the span is stored. A compiled
 * definition keeps its states, transitions, actions and parameters in
 * flat arrays, and each list inside it is a span into one of them; a
 * ring record names its variable-length fields as spans of its tail.
 *
 * ## Contract
 *
 * - A span with `length` 0 is empty, whatever its `offset`.
 * - `offset + length` never exceeds the length of the array the span
 *   indexes. Whoever writes a span ensures it (the compiler, a record
 *   writer); a validating reader checks it for bytes it did not write.
 *
 * ## Memory
 *
 * A span is 8 bytes the caller embeds; it owns nothing and points at
 * nothing, so it stays valid when the array it indexes moves. Spans stay
 * compact and local to what holds them: a definition's spans index that
 * definition's arrays, never another's.
 *
 * ## Complexity and failure
 *
 * There are no operations here: nothing costs anything, and nothing fails.
 *
 * ## Users
 *
 * Compiled definitions and the interpreter that walks them, continuations,
 * export, the ring records and their codec.
 */
#pragma once

#include <stdint.h>

/** Elements `offset .. offset + length - 1` of an array known from context. */
typedef struct {
    uint32_t offset;
    uint32_t length;
} ta_span32;
