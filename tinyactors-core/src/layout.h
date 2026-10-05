/**
 * @file layout.h
 *
 * # Layout builder
 *
 * A `ta_layout` lays several regions out in one block of memory, so that
 * an object made of a header and arrays whose lengths are known only at
 * run time takes one allocation instead of one per region. The caller
 * adds the regions in order, each as an entry of `count` items of `size`
 * bytes aligned to `alignment`, and reads back each entry's offset from
 * the start of the block, then the block's total size and alignment. The
 * arithmetic is checked: a region or a total that would not fit in a
 * `size_t` is refused, where a hand-written sum would wrap and hand the
 * allocator a size too small for what is then written into it.
 *
 * ```c
 * ta_layout layout;
 * ta_layout_init(&layout);
 * size_t header = 0, items = 0;
 * ta_result result = ta_layout_add(&layout, TA_LAYOUT_ENTRY(my_header, 1), &header);
 * if (result == TA_OK) {
 *     result = ta_layout_add(&layout, TA_LAYOUT_ENTRY(uint64_t, count), &items);
 * }
 * if (result != TA_OK) {
 *     return result;
 * }
 * void *block = ta_mem_alloc(allocator, layout.bytes, layout.alignment);
 * ```
 *
 * ## Contract
 *
 * - `ta_layout_init` starts an empty layout: `bytes` 0, `alignment` 1.
 * - `ta_layout_add` places an entry at the first offset at or after the
 *   current end that is a multiple of the entry's alignment. The entry
 *   then ends `size * count` bytes later, and that end is the layout's new
 *   `bytes`. `alignment` becomes the larger of itself and the entry's.
 * - An entry of no bytes (a `count` or a `size` of 0) still gets an
 *   aligned offset, and still moves `bytes` up to it and raises
 *   `alignment`: its array is empty, but where it starts is defined, and
 *   an accessor that points at it points inside or at the end of the
 *   block.
 * - An alignment is any non-zero number of bytes; offsets are rounded up
 *   to a multiple of it, as `ta_align_up` does. Every alignment core
 *   passes is a power of two except, possibly, the one a data model
 *   reports for its instance. With powers of two, every entry's offset is
 *   a multiple of its alignment in a block aligned to `alignment`.
 * - `bytes` is the end of the last entry. It is not rounded up to
 *   `alignment`: a caller that places blocks one after another rounds the
 *   total itself, as another entry would.
 * - The same entries added in the same order give the same offsets and
 *   total every time, so a unit may lay its block out again from the
 *   counts it stored, to find a region, instead of storing the offsets.
 *   A layout that succeeded once succeeds again with the same entries.
 *
 * ## Memory
 *
 * The unit allocates nothing and owns nothing: a `ta_layout` is two
 * numbers the caller keeps, usually on the stack, and the block it
 * describes is allocated and owned by the caller. No function here
 * allocates, so each may run anywhere, inside a macrostep included.
 *
 * ## Complexity
 *
 * `ta_layout_init` and `ta_layout_add` are O(1); `ta_layout_add` costs one
 * division (the rounding to a non-power-of-two alignment) and three
 * comparisons. Laying out a block is O(entries).
 *
 * ## Failure
 *
 * `ta_layout_add` answers `TA_ERR_NOMEM` when the entry's bytes
 * (`size * count`), its aligned offset or its end would exceed `SIZE_MAX`:
 * no allocator can serve a block that size, and `TA_ERR_NOMEM` is what the
 * allocation it sizes would answer. It answers `TA_ERR_INVALID` for an
 * alignment of 0. A failed call changes nothing: the layout keeps its
 * `bytes` and `alignment`, and `*offset` is not written. A NULL layout or
 * entry is a programmer error and is not checked.
 *
 * ## Users
 *
 * The actor block (the `ta_actor` record, its session storage and its
 * data-model instance, in one allocation; ADR 0063); the continuation a
 * stopped run is saved into (its header, the four state sets, the data
 * declarations, the default-history map, the enabled transitions and the
 * executor's frames; ADR 0061, ADR 0070); an operator transition and its
 * targets (ADR 0082); the system's interpreter regions and the table of
 * nested regions (ADR 0092); and the interpreter scratch a definition
 * needs, which the compiler checks against `interpreterBytes` (ADR 0092).
 */
#pragma once

#include "../include/tinyactors.h"

/**
 * One region of a layout: `count` items of `size` bytes each, the region
 * aligned to `alignment` bytes (never 0).
 */
typedef struct {
    size_t size;
    size_t alignment;
    size_t count;
} ta_layout_entry;

/**
 * A block being laid out: `bytes` is the end of the last entry added (the
 * block's size so far) and `alignment` the largest alignment of any entry
 * (1 with none). Read them; change them only through the functions below.
 */
typedef struct {
    size_t bytes;
    size_t alignment;
} ta_layout;

/** A pointer to an entry of `n` items of type `T`, for `ta_layout_add`. */
#define TA_LAYOUT_ENTRY(T, n) (&(ta_layout_entry){ .size = sizeof(T), .alignment = _Alignof(T), .count = (n) })

/** Makes `*layout` empty: no bytes, an alignment of 1. Never allocates; runs anywhere. */
void ta_layout_init(ta_layout *layout);

/**
 * Adds `*entry` at the end of `*layout`, at the first offset that is a
 * multiple of its alignment, and writes that offset to `*offset` unless
 * `offset` is NULL. Grows `bytes` to the entry's end and raises
 * `alignment` to the entry's. Borrows `entry` for the call. Never
 * allocates; may run inside a macrostep.
 *
 * Answers `TA_ERR_NOMEM` when the entry's bytes, its offset or its end
 * would exceed `SIZE_MAX`, and `TA_ERR_INVALID` for an alignment of 0;
 * either way the layout and `*offset` are unchanged.
 */
ta_result ta_layout_add(ta_layout *layout, const ta_layout_entry *entry, size_t *offset);
