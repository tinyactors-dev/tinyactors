#pragma once

#include "arena.h"
#include "atom.h"

/*
 * Interned named addresses (ADR 0065). A system keeps one record per
 * distinct (family, name) pair that anyone holds; a record is freed with its
 * last reference.
 *
 * The table is a linear hash table (Litwin): it grows one bucket at a time,
 * each insert splitting at most one bucket, so no single operation's cost
 * grows with the number of addresses. Buckets live in fixed pages found
 * through a small page directory, as the actor table's slots do (ADR 0063),
 * so growing never copies the buckets. A failed page allocation only skips
 * a split; the only allocation that can fail an intern is the record's own.
 */

#define TA_ADDRESS_PAGE_BUCKETS 1024u
#define TA_ADDRESS_INITIAL_BUCKETS 16u

typedef struct ta_address_table ta_address_table;

struct ta_address {
    /* Null for a detached record: a session image's own copy (see below). */
    ta_address_table *table;
    struct ta_address *next;
    uint32_t references;
    uint32_t hash;
    uint32_t family_len;
    uint32_t name_len;
    /* The family, a NUL, the name, a NUL. */
    char text[];
};

struct ta_address_table {
    ta_allocator allocator;
    ta_vec pages; /* ta_address ** pages of TA_ADDRESS_PAGE_BUCKETS buckets */
    /* Linear hashing: `base` buckets hashed with base - 1, and the first
     * `split` of them already split with 2 * base - 1. */
    uint32_t base;
    uint32_t split;
    uint32_t count;
};

void ta_address_table_init(ta_address_table *table, ta_allocator allocator);
/* Frees every record, held or not; references held past this are dangling. */
void ta_address_table_destroy(ta_address_table *table);
ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address,
                                  ta_address **out);

/*
 * A detached copy in `arena`: the same strings, no table, no counting.
 * Retain and release do nothing on it. Session images carry their addresses
 * this way, so an image does not depend on the system that captured it
 * (ADR 0028); loading interns them again.
 */
ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out);
bool ta_address_in(const ta_address *address, const ta_address_table *table);
