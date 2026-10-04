#pragma once

#include "types.h"

/*
 * Package layout computes where the regions of one allocation go.
 *
 * Several things in core are one allocation holding a header and then
 * arrays or sub-objects behind it: the actor block (the actor, its session
 * state, the data model's instance), a continuation (sets, declarations,
 * history, enabled transitions, frames), the interpreter's scratch (eleven
 * state sets and a handful of stacks), a system's arena region (structural
 * arena, render arena, table of nested regions), an operator jump (header
 * and its targets). Each needs the same arithmetic: round the running end
 * up to the next region's alignment, add the region's `count * size`, and
 * remember the strictest alignment seen. This unit is that arithmetic, once,
 * with every step checked, so that a size an input can influence is either
 * correct or reported as a failure, and is never a number that wrapped
 * around and is smaller than the data it must hold.
 *
 * Lifetime: call. A ta_layout is a small value on the caller's stack. It
 * owns nothing, points at nothing and never allocates, so there is nothing
 * to free and no failure that leaves state behind. Only the numbers computed
 * from it (offsets, a total, an alignment) outlive it; they are as long-lived
 * as the thing they describe: a definition for the actor block (the offsets
 * are fixed at compile time and the sized free at ta_actor_destroy depends on
 * them), a system for its arena region, and a single call for a continuation
 * or a scratch figure, which are recomputed from counts when needed.
 *
 * Contract:
 *   - Entries are placed in the order they are added. Each entry's offset is
 *     the running end rounded up to a multiple of the entry's alignment;
 *     the running end then moves by `size * count`.
 *   - The total is the unpadded end of the last entry. It is not rounded up
 *     to the layout's strictest alignment. That is deliberate: it is what
 *     the hand-written calculators this unit replaced returned, and the
 *     size of an actor block, a continuation or a scratch figure is a sized
 *     free (ta_mem_free checks the size it is given) and a per-actor cost
 *     that must not grow. A caller that needs a padded total asks for the
 *     alignment too and pads it itself (ta_layout_finish gives both).
 *   - A zero count takes no bytes, but the entry's offset still honours its
 *     alignment, as an array of length zero at that place would.
 *   - An alignment is a non-zero number; the arithmetic is a remainder, so
 *     it needs no power of two, but every alignment in core is one.
 *   - Offsets are measured from the layout's origin, and the first byte is
 *     at `start` (a block header, or the data offset of an arena block).
 *     The origin is assumed to be aligned to the strictest alignment of the
 *     layout, which is what allocating the result with that alignment makes
 *     true.
 *
 * Failure: every size computation is checked. When one does not fit in a
 * size_t (on a 32-bit host such as WebAssembly that is 4 GiB, and counts
 * are 32-bit values times element sizes, so it is reachable), or an
 * alignment is zero, the layout is marked failed, and the mark is sticky:
 * later entries cannot clear it. ta_layout_add keeps returning a number so
 * that a caller can build all its offsets in straight-line code, but that
 * number must not be used until ta_layout_finish (or ta_layout_bytes) has
 * said the whole layout is sound. Checking once at the end is what makes a
 * long sequence of entries as easy to write as the unchecked arithmetic was.
 *
 * Allocation: none, ever. A function that builds a layout may therefore be
 * called at any point, including where allocation is forbidden. The
 * allocation a layout describes is made by the caller, once, at a safe point
 * (with ta_mem_alloc, with the finished size and alignment), and freed with
 * the same size and alignment.
 *
 * Complexity: O(1) per entry, O(entries) per layout, O(1) space.
 *
 * Usage, an object of a header, an array of 16-bit counters and a table of
 * 64-bit words, allocated as one block:
 *
 *	ta_layout layout;
 *	ta_layout_init(&layout, 0);
 *	size_t header = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(header_t), _Alignof(header_t), 1 });
 *	size_t counters = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint16_t), _Alignof(uint16_t), n });
 *	size_t words = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint64_t), _Alignof(uint64_t), m });
 *	size_t bytes, alignment;
 *	if (ta_layout_finish(&layout, &bytes, &alignment) != TA_OK) {
 *		return TA_ERR_NOMEM;
 *	}
 *	unsigned char *block = ta_mem_alloc(allocator, bytes, alignment);
 *	// header, counters and words are now valid offsets into block.
 */

/*
 * A running layout. Its fields are for layout.c: callers use the functions,
 * so that the checks cannot be skipped. It is a value, copyable, with no
 * pointer in it.
 */
typedef struct ta_layout {
    size_t end;       /* bytes laid out so far, counted from the layout's origin */
    size_t alignment; /* the strictest alignment any entry asked for; at least 1 */
    bool overflowed;  /* a size computation did not fit, or an alignment was zero; sticky */
} ta_layout;

/*
 * One region: `count` items of `size` bytes each, placed at a multiple of
 * `alignment`. A passed entry is read during the call and not kept.
 */
typedef struct ta_layout_entry {
    size_t size;      /* bytes of one item */
    size_t alignment; /* never 0 */
    size_t count;     /* items; 0 is allowed and takes no bytes, but the offset still honours alignment */
} ta_layout_entry;

/*
 * ta_layout_init starts a layout whose first free byte is at offset `start`
 * from the origin. A block header goes here (`sizeof` of it) or the data
 * offset of an arena block; 0 for a layout that begins at the origin. It
 * never fails and never allocates.
 */
void ta_layout_init(ta_layout *layout, size_t start);

/*
 * ta_layout_add appends one entry and returns its offset from the origin:
 * the running end rounded up to the entry's alignment. It then moves the
 * end past `size * count` bytes and raises the layout's alignment to the
 * entry's if that is stricter. When any step does not fit in a size_t, or
 * the alignment is zero, it marks the layout failed and returns 0, and the
 * caller must not use an offset until ta_layout_finish has answered TA_OK.
 * It never allocates and never reads memory behind the entry.
 */
size_t ta_layout_add(ta_layout *layout, const ta_layout_entry *entry);

/*
 * ta_layout_finish reports the unpadded total in `*bytes` and the strictest
 * alignment in `*alignment`; an output may be NULL if the caller has no use
 * for it. It answers TA_ERR_NOMEM and writes neither output if any step of
 * the layout failed, so a caller that propagates the result leaves no
 * half-built state: nothing was allocated, and nothing was written.
 */
ta_result ta_layout_finish(const ta_layout *layout, size_t *bytes, size_t *alignment);

/*
 * ta_layout_bytes is the unpadded total for a caller that has no use for the
 * alignment and no way to report an error, such as a sized free that
 * recomputes a size or an estimate. It returns 0 when the layout failed.
 * Zero is never a real total, because every layout in core has a header of
 * at least one byte, so 0 is not ambiguous. A caller that allocates with a
 * size it got this way must refuse 0 before it allocates; a caller that
 * frees with one relies on the size having been accepted when the memory was
 * allocated, because the counts it is computed from do not change.
 */
size_t ta_layout_bytes(const ta_layout *layout);

/*
 * ta_checked_mul stores `a * b` in `*out` and answers true, or answers false
 * and leaves `*out` untouched when the product does not fit in a size_t.
 * It is the multiplication underneath ta_layout_add, exported for the size
 * estimates that sum several layouts (session_image.c).
 */
bool ta_checked_mul(size_t a, size_t b, size_t *out);

/*
 * ta_checked_add stores `a + b` in `*out` and answers true, or answers false
 * and leaves `*out` untouched when the sum does not fit in a size_t.
 */
bool ta_checked_add(size_t a, size_t b, size_t *out);
