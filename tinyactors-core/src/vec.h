#pragma once

#include "types.h"
#include "layout.h"

/*
 * Package vec holds core's two growable tables (building block B6): ta_vec,
 * one array of fixed-size elements, and ta_columns, several parallel arrays
 * that share one count and one capacity and live in a single allocation.
 *
 * Why two. A ta_vec is a 48-byte value (allocator, pointer, count, capacity,
 * element size and alignment). That is the right price for a handful of
 * tables per system (the page tables of the session and address tables, the atom table)
 * and for a per-actor table that is allocated only by an uncommon facility,
 * but it is the wrong price for a table a per-actor record carries twice.
 * A ta_columns is 16 bytes (block, count, capacity): the allocator comes
 * from the caller (the system already owns it) and the element shapes come
 * from a ta_columns_layout that is static const data, one per kind of table,
 * not one per instance. The active-invoke table of an actor session is the
 * user: its public records (ta_active_invoke, which ta_actor_inspect hands
 * to the host as a contiguous array) and its private records
 * (ta_invoke_meta) are two columns of one table.
 *
 * What is deliberately not here. The queue of blocks that one export call
 * builds (export.c) is not a growable array: it is carved once from the
 * call's arena at a size computed up front and checked, and it never grows.
 * It is not a ta_vec either, because a ta_vec frees with the allocator and
 * arena memory is never freed one block at a time. The timer lists and the
 * event-block queue grow by doubling too, but they are deleted by a later
 * phase of the ring-proto plan, so they keep their own code.
 *
 * Lifetime. Both tables are owned by whatever record embeds them and live
 * exactly as long as it does:
 *   - the active-invoke table (a ta_columns): session. It is created lazily
 *     at the first <invoke>, grows only when an invoke starts, and is
 *     released with the session's sparse state (ta_invoke_release);
 *   - pending_invokes and the activation effects (ta_vec): session;
 *   - the page tables of the session and address tables and the atom
 *     table (ta_vec): system, freed by the system's destroy; the actor and
 *     definition slot tables are index pools (pool.h), not ta_vec;
 *   - the exit frames of the interpreter (ta_vec): call.
 * A table never outlives its owner, and nothing but its owner frees it.
 *
 * Moving storage. Growing a table replaces its block. Every pointer into the
 * old block (ta_vec_at, ta_columns_at, or `items`) is dangling afterwards,
 * so a caller keeps an index across anything that can grow the table and
 * re-derives the pointer. The one pointer that is handed out on purpose,
 * ta_actor_inspection.invokes, is valid until the actor's next macrostep.
 *
 * When it allocates. Only ta_vec_reserve, ta_vec_extend, ta_vec_push and
 * ta_columns_grow allocate, and only the owner's safe points call them:
 * the end of a macrostep, loading or restoring a session, building a
 * definition, or creating a system. No unit of executable content (an
 * action, a guard, an expression) may call them: it could not report the
 * failure without leaving the macrostep half done. Every other function in
 * this unit never allocates, including ta_columns_at and the byte counts.
 *
 * Failure. Every size is computed with ta_checked_mul / ta_checked_add or
 * with ta_layout, so a count an input can influence either fits or fails;
 * nothing wraps. An allocation that fails, or a size that does not fit,
 * answers TA_ERR_NOMEM and leaves the table exactly as it was: same block,
 * same count, same capacity, same contents. A grow allocates the new block
 * first, copies, and only then frees the old one, so there is no moment at
 * which the table owns nothing.
 *
 * Sized frees. ta_mem_free is told the size and alignment of the block it
 * frees. Both are recomputed from the capacity by the same function that
 * sized the allocation (ta_vec_bytes, ta_columns_bytes), so the free
 * matches the allocation exactly; a strict test allocator reports any
 * mismatch as a bad free. Nothing else in core computes these sizes.
 *
 * Growth policy. A ta_vec starts at 8 elements and doubles; a ta_columns
 * starts at its layout's first_capacity and doubles. Doubling keeps pushes
 * amortised O(1) and never leaves more than half the capacity unused.
 *
 * Complexity. ta_vec_at is O(1). ta_columns_at is O(columns) (it places the
 * columns with ta_layout; a table has two or three). A grow is O(count) time
 * and O(capacity) space for the new block.
 *
 * Invariants. count <= capacity; capacity == 0 exactly when the table owns
 * no block; the zero value of a ta_columns is an empty table, valid for
 * every function here; a ta_vec must have been through ta_vec_init.
 *
 * Usage, a table of two parallel arrays (an identifier and a counter):
 *
 *	static const ta_column columns[] = {
 *		{ sizeof(uint32_t), _Alignof(uint32_t) },
 *		{ sizeof(uint16_t), _Alignof(uint16_t) },
 *	};
 *	static const ta_columns_layout layout = { columns, 2, 4 };
 *
 *	ta_columns table = { 0 };
 *	if (ta_columns_grow(allocator, &layout, &table) != TA_OK) {
 *		return TA_ERR_NOMEM;       // table is untouched
 *	}
 *	uint32_t index = table.count;
 *	table.count += 1;
 *	((uint32_t *)ta_columns_at(&layout, &table, 0))[index] = id;
 *	((uint16_t *)ta_columns_at(&layout, &table, 1))[index] = 0;
 *	...
 *	ta_columns_release(allocator, &layout, &table);
 */

/*
 * A growable array of fixed-size elements. `items` is reallocated as it
 * grows, so callers keep indices, never element pointers, across a push.
 */
typedef struct {
    ta_allocator allocator;
    void *items;
    uint32_t count;
    uint32_t capacity;
    size_t element_size;
    size_t element_alignment;
} ta_vec;

#define TA_VEC_LAYOUT(T) (&(ta_tree_list_layout){ .element_size = sizeof(T), .element_alignment = _Alignof(T) })

/*
 * ta_vec_init makes an empty vector of elements shaped by `layout`, taking
 * memory from `allocator` for as long as the vector lives. It allocates
 * nothing and cannot fail; `layout` is read during the call and not kept.
 */
void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout);

/*
 * ta_vec_destroy frees the vector's block with the exact size and alignment
 * it was allocated with, and leaves an empty vector that may be used again.
 * It never allocates. Calling it on an empty vector is a no-op.
 */
void ta_vec_destroy(ta_vec *vec);

/*
 * ta_vec_reserve makes room for `count` elements in total (not `count` more),
 * doubling the capacity from 8. It allocates, so only a safe point may call
 * it. On TA_ERR_NOMEM (the allocator refused, or the size does not fit in a
 * size_t) the vector is unchanged, with its old block, count and contents.
 * Any pointer into the old block is invalid after a successful grow.
 */
ta_result ta_vec_reserve(ta_vec *vec, uint32_t count);

/*
 * ta_vec_extend appends `count` zeroed elements and stores the index of the
 * first in `*out` (which may be NULL). It allocates through
 * ta_vec_reserve and has the same failure behaviour: on TA_ERR_NOMEM
 * nothing was appended and `*out` was not written.
 */
ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out);

/*
 * ta_vec_push copies one element, of the vector's element size, to the end.
 * It allocates through ta_vec_extend; on failure nothing was appended.
 */
ta_result ta_vec_push(ta_vec *vec, const void *element);

/*
 * ta_vec_at returns the address of element `index`. It never allocates and
 * does not check the index: it must be below capacity. The address is valid
 * until the next call that can grow the vector.
 */
void *ta_vec_at(const ta_vec *vec, uint32_t index);

/*
 * ta_vec_bytes is the size of the vector's block (capacity * element size),
 * 0 when it owns none. It is the size ta_vec_destroy frees, and what
 * ta_actor_inspect reports as the vector's cost. The product of a vector
 * that grew fits in a size_t, because the grow checked it.
 */
size_t ta_vec_bytes(const ta_vec *vec);

/*
 * One column of a ta_columns table: the size and alignment of one item.
 */
typedef struct {
    size_t size;
    size_t alignment;
} ta_column;

/*
 * The shape of a kind of ta_columns table. It is static const data shared by
 * every table of that kind: it is not stored in the table, which is why a
 * table costs 16 bytes. `first_capacity` is the capacity of the first block
 * and must not be 0. The columns sit in the block in order, each at a
 * multiple of its alignment, so column 0 is at offset 0.
 */
typedef struct {
    const ta_column *columns;
    uint32_t column_count;
    uint32_t first_capacity;
} ta_columns_layout;

/*
 * A table of parallel arrays in one allocation. `block` is column 0 as well
 * as the start of the allocation (it may be read as a pointer to column 0's
 * items, as ta_actor_sparse_state does for `invokes`). Fields are public so
 * that the owner can read count and bump it after filling in an item. The
 * zero value is an empty table.
 */
typedef struct {
    void *block;
    uint32_t count;
    uint32_t capacity;
} ta_columns;

/*
 * ta_columns_grow makes room for one more item: it does nothing when
 * count < capacity, and otherwise doubles the capacity (from the layout's
 * first_capacity). It allocates one block, copies the first `count` items
 * of every column, then frees the old block with its exact size. Only a
 * safe point may call it. On TA_ERR_NOMEM (the allocator refused, or a size
 * does not fit) `*table` is untouched. Pointers from ta_columns_at are
 * invalid after a successful grow that reallocated. (One item at a time,
 * not "n more": core's rule of at most three parameters leaves no room for
 * a count, and the only user adds one invoke at a time.)
 */
ta_result ta_columns_grow(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table);

/*
 * ta_columns_at returns the first item of column `index`. The offset is
 * recomputed from the capacity (O(columns)), so a function that uses a
 * column in a loop reads it once into a local. It never allocates. It
 * answers NULL for an empty table or a column out of range. The pointer is
 * valid until the next ta_columns_grow or ta_columns_release.
 */
void *ta_columns_at(const ta_columns_layout *layout, const ta_columns *table, uint32_t index);

/*
 * ta_columns_bytes is the size of the table's block, 0 when empty: what
 * ta_columns_release frees and what an allocated-bytes figure adds up. It
 * never allocates.
 */
size_t ta_columns_bytes(const ta_columns_layout *layout, const ta_columns *table);

/*
 * ta_columns_release frees the block with the size and alignment it was
 * allocated with and leaves the empty table. It never allocates, and calling
 * it on an empty table is a no-op. The owner has already dealt with the
 * items (cancelled an invoke, released a child): this frees memory only.
 */
void ta_columns_release(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table);
