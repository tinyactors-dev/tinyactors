/* Caller-owned tables for embedder/binding values named by integer handles.
 * Optional base-layer storage: no actor/system region contains this table.
 * A table only stores bytes; the caller owns any objects referenced by rows.
 * Use handles across adds, because growing can move all row pointers. */
#ifndef TINYACTORS_TABLE_H
#define TINYACTORS_TABLE_H

#include <tinyactors/allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_table_config {
    uint32_t size;     /* sizeof(ta_table_config) */
    uint32_t row_size; /* nonzero bytes per row, rounded up to 8 */
    const ta_allocator *allocator; /* borrowed; must outlive the table */
} ta_table_config;

/* Fields are maintained by the API; inspect count/capacity/row_size, but do
 * not change them. rows contains aligned rows followed by slot metadata.
 * Neither the table nor its allocation belongs in relocatable system memory. */
typedef struct ta_table {
    unsigned char *rows;
    const ta_allocator *allocator;
    uint32_t row_size, count, capacity;
    uint32_t free; /* first free slot + 1; 0: none */
} ta_table;

/* Start an empty table without allocating. config is read only during the
 * call; its allocator object/context must remain valid until free. Missing
 * callbacks, a short config, zero/overflowing row size or NULL config leave
 * an inert table whose add refuses. Do not reinitialize a live allocation.
 * Serialize calls on one owning thread; allocator callbacks must not mutate
 * this table. The table takes no references to objects inside rows. */
TA_API void ta_table_init(ta_table *table, const ta_table_config *config);

/* Add one zeroed, 8-aligned row and write its nonzero handle. Storage grows
 * by doubling through the supplied allocator. NULL means refusal (including
 * missing handle output or arithmetic exhaustion), with *handle set to 0
 * when supplied and no change to the table or existing handles/rows.
 * The handle output must point outside table storage, since a successful
 * add may move rows. Reacquire row pointers through get afterward.
 * The handle remains valid until remove or free; a pointer until remove,
 * free or the next successful add. Handles encode generation << 32 | slot+1;
 * generations are odd while live. Exhausted generations retire their slots
 * permanently, so removed handles never become valid in this table lifetime. */
TA_API void *ta_table_add(ta_table *table, uint64_t *handle);

/* Resolve a handle without allocating; NULL for zero, stale or unknown.
 * The returned row is borrowed for the pointer lifetime described by add. */
TA_API void *ta_table_get(const ta_table *table, uint64_t handle);

/* Iterate slot indices from 0 to capacity-1, skipping NULL rows/0 handles.
 * Iteration order is slot order. Neither call allocates or changes the table.
 * As with get, a row pointer cannot be held across an add. */
TA_API void *ta_table_at(const ta_table *table, uint32_t slot);
TA_API uint64_t ta_table_handle(const ta_table *table, uint32_t slot);

/* Invalidate a live handle, zero its row and make its slot reusable unless
 * its generation is exhausted. Stale/unknown handles do nothing. Release
 * row-owned language references/resources BEFORE removing; core cannot do
 * that from raw bytes. Other handles and row pointers remain unchanged. */
TA_API void ta_table_remove(ta_table *table, uint64_t handle);

/* Release storage and leave an inert empty table; repeated free is safe.
 * Release all row-owned resources first. All handles and pointers end here;
 * a later init starts a new handle namespace, not a continuation of this one. */
TA_API void ta_table_free(ta_table *table);

#ifdef __cplusplus
}
#endif
#endif
