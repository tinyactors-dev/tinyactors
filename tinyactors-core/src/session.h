#pragma once

#include "address.h"

/*
 * Sessions whose ID is not a resident actor's runtime ID (ADR 0066). A
 * session's ID is the runtime ID it was spawned with, so a never-paged
 * session is found through the actor table and has no entry here. An entry
 * is either a reloaded resident session (`actor` set) or an unloaded named
 * one (`actor` null, `address` held). Lookups prefer a resident entry.
 *
 * The table is a linear hash table over fixed pages, like the address table
 * (ADR 0065): each insert splits at most one bucket, growing never copies
 * the table, and a failed page allocation only postpones a split. Its first
 * page is allocated at init, so inserting never fails.
 */

#define TA_SESSION_PAGE_BUCKETS 1024u
#define TA_SESSION_INITIAL_BUCKETS 16u

typedef struct ta_session_node {
    uint64_t id;
    struct ta_session_node *next;
    /* The resident actor holding the session, or null when it is unloaded. */
    struct ta_actor *actor;
    /* An unloaded session's address, one reference held; null otherwise. */
    ta_address *address;
} ta_session_node;

typedef struct {
    ta_allocator allocator;
    ta_vec pages; /* ta_session_node ** pages of TA_SESSION_PAGE_BUCKETS buckets */
    uint32_t base;
    uint32_t split;
    uint32_t count;
} ta_session_table;

ta_result ta_session_table_init(ta_session_table *table, ta_allocator allocator);
/* Frees every entry left, releasing the addresses unloaded entries hold. */
void ta_session_table_destroy(ta_session_table *table);

ta_result ta_session_node_new(ta_session_table *table, ta_session_node **out);
/* Frees an entry that is not in the table, releasing its address. */
void ta_session_node_free(ta_session_table *table, ta_session_node *node);
void ta_session_insert(ta_session_table *table, ta_session_node *node);
void ta_session_remove(ta_session_table *table, ta_session_node *node);
/* The entry for `id`: a resident one if there is one, else an unloaded one. */
const ta_session_node *ta_session_find(const ta_session_table *table, uint64_t id);

/* Whether a resident session holds `id`: a never-paged actor or a reloaded one. */
bool ta_session_resident(const ta_system *system, uint64_t id);
/* The session ID of the resident actor `runtime`, or `runtime` itself. */
uint64_t ta_session_id_of(const ta_system *system, ta_actor_id runtime);
/*
 * Where an SCXML send to `ref` goes (ADR 0066 section 3): a runtime ID that
 * is not a resident actor's is looked up as a session ID. A reloaded session
 * gives its current runtime ID; an unloaded one gives its address, retained,
 * in which case this returns true and the caller releases *out. Otherwise
 * *out is `ref` and this returns false.
 */
bool ta_session_route(const ta_system *system, const ta_actor_ref *ref, ta_actor_ref *out);
