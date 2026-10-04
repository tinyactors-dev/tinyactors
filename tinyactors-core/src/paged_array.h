/**
 * @file paged_array.h
 *
 * # Paged array
 *
 * `ta_paged_array` is a dense array of fixed-size entries at positions
 * `0` to `capacity - 1`, stored in pages of a power-of-two number of
 * entries. With `2^k` entries per page, position `p` lives in page
 * `p >> k` at entry `p & (2^k - 1)`, so every access costs a shift, a mask
 * and one indirection through the directory of pages.
 *
 * It is the dense counterpart of an index pool: positions are dense, as a
 * heap's are, so there is no free list and no index 0 reserved for "none".
 * The array does not count which positions are in use; its user does (a
 * heap keeps its own length).
 *
 * ## Why pages
 *
 * Growing adds pages and copies no entry, and no growth needs one
 * contiguous block for the entries. At the largest envelope pool, one
 * reallocated array would copy 64 MB at a single refill and need 64 MB in
 * one piece. Only the directory, one pointer per page, is ever copied, and
 * only when it fills up.
 *
 * ## Contract and invariants
 *
 * - `capacity == page_count << page_shift`: the array always holds whole
 *   pages, and `ta_paged_array_grow(n)` rounds `n` up to them.
 * - A position below the capacity may be read and written. Reading or
 *   writing one at or above it is a programmer error, not checked.
 * - A page never moves while it is allocated. The address `at` answers for
 *   a position stays valid until a shrink frees that position's page or
 *   the array is destroyed; growing does not move it. Callers still
 *   address entries by position and look the address up again in each
 *   call, as every index-addressed structure in core does.
 * - A page that grow adds reads as zero bytes, also when it replaces a
 *   page that a shrink freed. A shrink keeps the values of every page it
 *   keeps, including positions at or above `n` in a page partly below it.
 * - `page_count == 0` exactly when the directory is not allocated.
 *
 * ## Memory
 *
 * The array owns its directory and its pages, allocated from the
 * `ta_allocator` given to `ta_paged_array_init`, which it borrows and
 * which must outlive it. A `ta_paged_array` itself is embedded where its
 * user likes (the deadline index).
 *
 * - `ta_paged_array_init` allocates nothing.
 * - `ta_paged_array_grow` allocates: the pages it adds and, when the
 *   directory is full, a larger directory. It may be called only at a safe
 *   point (the envelope pool's refill), never inside a macrostep.
 * - `ta_paged_array_shrink` and `ta_paged_array_destroy` only free. Shrink
 *   gives back the directory too when no page remains; while pages remain
 *   the directory keeps its size. Shrink runs only at a safe point: when
 *   the host calls `ta_system_trim`, never on core's own initiative.
 * - `ta_paged_array_at`, `ta_paged_array_get`, `ta_paged_array_set` and
 *   `ta_paged_array_capacity` never allocate, and may run inside a
 *   macrostep.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_paged_array_at`, `_get`, `_set`, `_capacity` | O(1): a shift, a mask, one indirection (get and set copy one entry) |
 * | `ta_paged_array_grow(n)` | one allocation per page added, plus one for the directory when it is full (it then doubles, copying one pointer per page); zeroes the added pages |
 * | `ta_paged_array_shrink(n)` | one free per page freed, plus the directory when no page remains |
 * | `ta_paged_array_init` | O(1) |
 * | `ta_paged_array_destroy` | one free per page, and the directory |
 *
 * ## Failure
 *
 * - `ta_paged_array_init` answers `TA_ERR_INVALID` for an entry size of 0,
 *   an alignment that is not a power of two, an entry size that is not a
 *   multiple of the alignment, a `page_shift` above 31, or a page whose
 *   size in bytes does not fit in `size_t`. It then leaves `*array`
 *   untouched.
 * - `ta_paged_array_grow` answers `TA_ERR_NOMEM` when the allocator
 *   refuses a page or the directory, or when the directory's size in bytes
 *   does not fit in `size_t`. A failed grow changes nothing: it frees what
 *   it had allocated, and the capacity, the pages and their values stay as
 *   they were.
 * - Nothing else fails. Positions out of range are preconditions.
 *
 * ## Users
 *
 * The **deadline index** keeps its heap positions here, one `uint32_t`
 * envelope index per position. The envelope pool's refill grows it
 * together with the pool, so inserting a delayed send never allocates, and
 * `ta_system_trim` shrinks it to the pages the pending delayed sends
 * occupy, as it trims the pool's free pages.
 */
#pragma once

#include "../include/tinyactors.h"
#include "memory.h"

/** What `ta_paged_array_init` needs. */
typedef struct {
    /** Where pages and the directory come from; borrowed, and must outlive
     * the array. */
    ta_allocator allocator;
    /** Bytes per entry: not 0, and a multiple of `entry_alignment`, as
     * `sizeof` of a type always is. */
    size_t entry_size;
    /** Alignment of each entry: a power of two. */
    size_t entry_alignment;
    /** Each page holds `1 << page_shift` entries; at most 31. */
    uint32_t page_shift;
} ta_paged_array_params;

/** A dense array of fixed-size entries in pages; see the file comment. */
typedef struct {
    ta_allocator allocator;
    /** The directory: `page_count` page addresses, room for
     * `directory_capacity`. NULL while `page_count` is 0. */
    unsigned char **pages;
    uint32_t page_count;
    uint32_t directory_capacity;
    size_t entry_size;
    size_t entry_alignment;
    uint32_t page_shift;
    /** `(1 << page_shift) - 1`. */
    uint32_t page_mask;
} ta_paged_array;

/**
 * Sets up an empty array (capacity 0) for entries described by `params`.
 * Allocates nothing. Answers `TA_ERR_INVALID`, leaving `*array` untouched,
 * when `params` breaks a rule of `ta_paged_array_params`.
 */
ta_result ta_paged_array_init(ta_paged_array *array, const ta_paged_array_params *params);

/**
 * Frees every page and the directory, and leaves an empty array that
 * allocates nothing until it is set up again. Never fails.
 */
void ta_paged_array_destroy(ta_paged_array *array);

/**
 * Makes the capacity at least `count`, rounded up to whole pages, by
 * adding zeroed pages. Copies no entry; when the directory is full it is
 * replaced by one twice as large (or large enough), copying only page
 * addresses. A `count` within the capacity changes nothing and allocates
 * nothing.
 *
 * Allocates, so it runs only at a safe point, never inside a macrostep.
 * Answers `TA_ERR_NOMEM` when an allocation fails or the directory would
 * not fit in `size_t`; the array is then exactly as before.
 */
ta_result ta_paged_array_grow(ta_paged_array *array, uint32_t count);

/**
 * Frees every page that lies wholly at or above position `count`, keeping
 * the pages that hold positions `0` to `count - 1` and their values. A
 * page partly below `count` stays whole. With no page left, the directory
 * is freed too. A `count` at or above the capacity changes nothing.
 *
 * Only frees, and never fails. It runs at a safe point, when the host asks
 * core to give memory back (`ta_system_trim`).
 */
void ta_paged_array_shrink(ta_paged_array *array, uint32_t count);

/** Positions the array holds now: whole pages, `page_count << page_shift`.
 * At most 2^32, so the answer is 64 bits wide. Never allocates. */
static inline uint64_t ta_paged_array_capacity(const ta_paged_array *array) {
    return (uint64_t)array->page_count << array->page_shift;
}

/**
 * The address of the entry at `position`, which must be below the
 * capacity. It stays valid until a shrink frees its page or the array is
 * destroyed. Never allocates; may run inside a macrostep.
 */
static inline void *ta_paged_array_at(const ta_paged_array *array, uint32_t position) {
    return array->pages[position >> array->page_shift] +
           (size_t)(position & array->page_mask) * array->entry_size;
}

/**
 * Copies the entry at `position` (below the capacity) into `out`, which
 * has room for one entry. Never allocates; may run inside a macrostep.
 */
static inline void ta_paged_array_get(const ta_paged_array *array, uint32_t position, void *out) {
    ta_mem_copy(out, ta_paged_array_at(array, position), array->entry_size);
}

/**
 * Copies one entry from `entry` to `position` (below the capacity). Never
 * allocates; may run inside a macrostep.
 */
static inline void ta_paged_array_set(ta_paged_array *array, uint32_t position, const void *entry) {
    ta_mem_copy(ta_paged_array_at(array, position), entry, array->entry_size);
}
