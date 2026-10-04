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
 * from it (offsets, a total, an alignment) outlive it, and they are stored
 * by the thing they describe, once, where it is allocated:
 *   - the actor block: offsets in the definition (the offsets are fixed at
 *     compile time and the sized free at ta_actor_destroy depends on them);
 *   - a continuation: its array offsets and `bytes` in its header
 *     (ta_continuation_bytes, called by ta_reserve_stop and the restore of
 *     an image);
 *   - an operator jump: `bytes` in its header (ta_operator_jump_bytes);
 *   - a system's arena region: the total in the system, for the sized free
 *     at ta_system_destroy.
 * Nothing recomputes such a number later from counts, in a function that
 * could not report that the layout failed.
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
 * number must not be used until ta_layout_finish has answered TA_OK for the
 * whole layout. Checking once at the end is what makes a long sequence of
 * entries as easy to write as the unchecked arithmetic was.
 *
 * "Overflowed" is representable only as a ta_result. There is no function
 * that answers a size with 0 or SIZE_MAX for failure: the only way to read a
 * total from a layout is ta_layout_finish, which answers TA_ERR_NOMEM and
 * writes nothing on failure. It is called where the allocation is made, a
 * safe point that can fail cleanly, and its result is always used. The
 * fields of a ta_layout are private to layout.c (their names start with
 * `internal_`). `mise run check-layout` enforces this, with ast-grep rules
 * (mise/ast-grep/rules/layout-*.yml):
 *   - a function that calls ta_layout_add also calls ta_layout_finish;
 *   - the result of ta_layout_finish is never discarded;
 *   - no code outside layout.c reads or writes a ta_layout's fields;
 *   - nothing is allocated, copied or freed between the adds and the finish
 *     of a function, and ta_layout_add is not used as an operand.
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
 * so that the checks cannot be skipped, and the names are unique so that a
 * static check can find any other use (see the package comment). It is a
 * value, copyable (a caller may finish a copy to learn the size of a prefix
 * and go on adding to the original), with no pointer in it.
 */
typedef struct ta_layout {
    size_t internal_end;       /* bytes laid out so far, counted from the layout's origin */
    size_t internal_alignment; /* the strictest alignment any entry asked for; at least 1 */
    bool internal_overflowed;  /* a size computation did not fit, or an alignment was zero; sticky */
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
 * the alignment is zero, it marks the layout failed and returns 0; 0 is also
 * a real offset, so the caller must not use any offset until
 * ta_layout_finish has answered TA_OK.
 * It never allocates and never reads memory behind the entry.
 */
size_t ta_layout_add(ta_layout *layout, const ta_layout_entry *entry);

/*
 * ta_layout_finish reports the unpadded total in `*bytes` and the strictest
 * alignment in `*alignment`; an output may be NULL if the caller has no use
 * for it. It answers TA_ERR_NOMEM and writes neither output if any step of
 * the layout failed, so a caller that propagates the result leaves no
 * half-built state: nothing was allocated, and nothing was written. The
 * result must be used: it is the only place a failed layout shows.
 */
ta_result ta_layout_finish(const ta_layout *layout, size_t *bytes, size_t *alignment);

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
