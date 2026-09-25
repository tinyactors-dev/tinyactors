#pragma once
/*
 * The cache of child documents the SCXML invoker imports (napi ADR 0023).
 *
 * Keyed by a 64-bit FNV-1a hash of the document's bytes in a map, with the
 * entries of one hash chained; a match is confirmed by comparing the bytes,
 * so a collision never answers with the wrong definition. Entries are kept
 * in least-recently-used order and evicted past two bounds, a count and a
 * number of document bytes. The entry added last is never evicted.
 *
 * Adding is two steps, so that running out of memory changes nothing (core
 * ADR 0070): reserve() copies the bytes and takes an entry before the
 * import, then commit() or cancel(). A reserved entry is not found.
 *
 * The cache only remembers definition IDs; retiring an evicted definition
 * is the caller's job. This module has no Node-API in it.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef struct {
    uint32_t max_count; /* at least 1 */
    size_t max_bytes;   /* at least 1 */
} tna_documents_limits;

#define TNA_DOCUMENTS_MAX_COUNT_DEFAULT 256u
#define TNA_DOCUMENTS_MAX_BYTES_DEFAULT ((size_t)16u * 1024u * 1024u)

typedef struct {
    ta_allocator allocator;
    tna_documents_limits limits;
    /* FNV-1a by default; a test may set one that collides. */
    uint64_t (*hash)(ta_bytes bytes);
    tna_vec entries;   /* tna_document: in use, reserved, or free */
    tna_map heads;      /* hash -> index of the first entry with that hash */
    uint32_t free_head; /* first free entry, chained through `next_same` */
    uint32_t newest;    /* most recently used committed entry */
    uint32_t oldest;    /* least recently used committed entry */
    uint32_t count;     /* committed entries */
    size_t bytes;       /* bytes of committed entries */
} tna_documents;

void tna_documents_init(tna_documents *documents, ta_allocator allocator, tna_documents_limits limits);
/* Changes the bounds; the next evict() calls bring the cache within them. */
void tna_documents_set_limits(tna_documents *documents, tna_documents_limits limits);
/*
 * The definition cached for exactly these bytes, made the most recently
 * used. False when none is: a miss.
 */
bool tna_documents_find(tna_documents *documents, ta_bytes bytes, ta_definition_id *out);
/* Takes an entry and a copy of `bytes`. TA_ERR_NOMEM leaves the cache as it was. */
ta_result tna_documents_reserve(tna_documents *documents, ta_bytes bytes, uint32_t *entry);
/* The reserved entry now names `definition`, as the most recently used. */
void tna_documents_commit(tna_documents *documents, uint32_t entry, ta_definition_id definition);
/* Gives a reserved entry back: the import failed. */
void tna_documents_cancel(tna_documents *documents, uint32_t entry);
/*
 * Evicts the least recently used entry while the cache is past a bound,
 * one per call, never the newest. True and its definition in `*out` when it
 * evicted one; false when the cache is within its bounds.
 */
bool tna_documents_evict(tna_documents *documents, ta_definition_id *out);
/* Drops the entry naming `definition`, if any: it was retired or freed. */
bool tna_documents_forget(tna_documents *documents, ta_definition_id definition);
void tna_documents_destroy(tna_documents *documents);
