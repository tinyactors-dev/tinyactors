/**
 * @file vec.h
 *
 * # Growable array
 *
 * `ta_vec` is a contiguous array of fixed-size elements that grows by
 * doubling. The element size and alignment are fixed at `ta_vec_init`
 * (`TA_VEC_LAYOUT(T)` gives them for a type), and elements are addressed by
 * `uint32_t` index from 0.
 *
 * ## Contract and invariants
 *
 * - `count <= capacity`; elements `0 .. count - 1` are in use.
 *   `items` is NULL exactly when `capacity` is 0.
 * - `capacity` is 0 or 8 times a power of two: the first growth makes room
 *   for 8 elements, and each later one doubles until the request fits.
 * - Growing moves `items`. A pointer from `ta_vec_at` is valid only until
 *   the next `ta_vec_reserve`, `ta_vec_extend` or `ta_vec_push`; callers
 *   keep indices, never element pointers, across them.
 * - `ta_vec_extend` and `ta_vec_push` append elements; nothing here
 *   removes one. A caller shrinks the array by lowering `count`
 *   itself; the memory stays until `ta_vec_destroy`.
 * - `ta_vec_at(vec, index)` does not check `index`: `index < count` is the
 *   caller's precondition.
 *
 * ## Memory
 *
 * The vector owns `items`, allocated from the `ta_allocator` it was
 * initialised with and freed by `ta_vec_destroy` (or by the copy of a
 * growth). The `ta_vec` struct itself belongs to the caller, who embeds it.
 * `ta_vec_init`, `ta_vec_at` and `ta_vec_destroy` never allocate.
 * `ta_vec_reserve`, `ta_vec_extend` and `ta_vec_push` allocate when the
 * request exceeds `capacity`; a caller that must not allocate at that
 * point reserves beforehand, at a point where it may.
 *
 * ## Complexity
 *
 * `ta_vec_at` and `ta_vec_init` are O(1). `ta_vec_reserve` copies every
 * element when it grows, O(count); `ta_vec_push` and `ta_vec_extend` are
 * O(1) amortised plus the zeroing of the new elements. `ta_vec_destroy`
 * is O(1).
 *
 * ## Failure
 *
 * Growth answers `TA_ERR_NOMEM` when the allocator fails or the capacity
 * would pass `UINT32_MAX`, and the vector is unchanged: its elements,
 * `count` and `capacity` stay as they were.
 *
 * ## Users
 *
 * The atom table, the event-name trie, the page directories of addresses,
 * sessions, I/O origins, timers and index pools (the actor table's), an
 * actor's activation effects and pending invokes, the system's
 * definitions and registrations, the builder's state lists, the
 * compiler's frames and the interpreter's exit frames (ADR 0060), and the
 * session image.
 */
#pragma once

#include "../include/tinyactors.h"

/** A growable array of fixed-size elements; see the file comment. */
typedef struct {
    ta_allocator allocator;
    void *items;
    uint32_t count;
    uint32_t capacity;
    size_t element_size;
    size_t element_alignment;
} ta_vec;

/** The element layout of type `T`, for `ta_vec_init`. */
#define TA_VEC_LAYOUT(T) (&(ta_tree_list_layout){ .element_size = sizeof(T), .element_alignment = _Alignof(T) })

/** Makes `*vec` an empty vector of elements of `layout`, over `allocator`. Never allocates. */
void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout);
/** Frees the elements and leaves `*vec` empty, still usable. Never allocates. */
void ta_vec_destroy(ta_vec *vec);
/**
 * Makes room for `count` elements in all. May allocate; answers
 * `TA_ERR_NOMEM`, changing nothing, when it cannot.
 */
ta_result ta_vec_reserve(ta_vec *vec, uint32_t count);
/**
 * Appends `count` zeroed elements and stores the index of the first in
 * `*out` (when `out` is not NULL). May allocate; answers `TA_ERR_NOMEM`,
 * changing nothing, when it cannot.
 */
ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out);
/**
 * Appends a copy of the element at `element`. May allocate; answers
 * `TA_ERR_NOMEM`, changing nothing, when it cannot.
 */
ta_result ta_vec_push(ta_vec *vec, const void *element);
/** The address of element `index`, which is below `count`; valid until the vector next grows. */
void *ta_vec_at(const ta_vec *vec, uint32_t index);
