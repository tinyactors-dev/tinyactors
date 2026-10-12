/* base/output.h — bytes written one piece after another into a room a caller
 * gives, grown through an optional allocator (docs/adr/0010 in the
 * repository root): the host's outputs, a blob written in one pass, and the
 * CBOR writer.
 *
 * Without an allocator (or when it refuses), what does not fit is not
 * written and `length` goes on counting, so one pass tells a caller what the
 * output needs; ta_output_fits says whether all of it is there. With one, a
 * full room is replaced by a block twice as big (at least what the piece
 * needs): the bytes so far are copied over, and the old block is given back
 * unless it is the caller's room. The base layer calls no allocator of its
 * own: only the one in `allocator`. */
#ifndef TINYACTORS_BASE_OUTPUT_H
#define TINYACTORS_BASE_OUTPUT_H

#include <tinyactors/allocator.h>
#include <tinyactors/types.h>

typedef struct ta_output {
    unsigned char *bytes;
    size_t capacity, length;
    const ta_allocator *allocator; /* NULL: the room never grows */
    unsigned char *room;           /* the caller's room, never given back (NULL: none) */
} ta_output;

/* An output over the caller's `room` (its pointer and capacity), growing
 * through `allocator` (NULL: never). */
ta_output ta_output_over(ta_buffer room, const ta_allocator *allocator);
/* Room for `more` bytes past `length`: 1, or 0 when it cannot grow. */
int ta_output_reserve(ta_output *output, size_t more);
/* Append `length` bytes (counted only when they do not fit). */
void ta_output_write(ta_output *output, const void *bytes, size_t length);
/* Overwrite bytes already written, at `offset` (nothing when they are not
 * there: the output did not fit). */
void ta_output_patch(ta_output *output, size_t offset, ta_bytes bytes);
/* 1 when everything written is there (nothing was only counted). */
int ta_output_fits(const ta_output *output);
/* Give back the block it grew (not the caller's room); the output is empty. */
void ta_output_free(ta_output *output);

#endif /* TINYACTORS_BASE_OUTPUT_H */
