#include "documents.h"

#include <string.h>

#include "allocator.h"

#define TNA_DOCUMENT_NONE UINT32_MAX

enum { TNA_DOCUMENT_FREE = 0, TNA_DOCUMENT_RESERVED, TNA_DOCUMENT_COMMITTED };

typedef struct {
    uint64_t hash;
    uint8_t *bytes;
    uint32_t length;
    ta_definition_id definition;
    uint32_t newer;     /* toward the most recently used, or NONE */
    uint32_t older;     /* toward the least recently used, or NONE */
    uint32_t next_same; /* the next entry with the same hash, or the next free entry */
    uint8_t state;
} tna_document;

/* FNV-1a, 64 bits: no table, and good enough spread for a map key. */
static uint64_t tna_documents_hash(ta_bytes bytes) {
    uint64_t hash = 14695981039346656037u;
    for (uint32_t at = 0; at < bytes.len; at += 1u) {
        hash ^= bytes.ptr[at];
        hash *= 1099511628211u;
    }
    return hash;
}

static tna_document *tna_entry(const tna_documents *documents, uint32_t index) {
    return tna_vec_at(&documents->entries, index);
}

static void tna_free_bytes(tna_documents *documents, tna_document *entry) {
    tna_free(documents->allocator, &(ta_allocator_free_params){ .ptr = entry->bytes,
                                                                .size = entry->length == 0 ? 1u : entry->length,
                                                                .alignment = 1 });
    entry->bytes = NULL;
}

void tna_documents_init(tna_documents *documents, ta_allocator allocator, tna_documents_limits limits) {
    *documents = (tna_documents){ .allocator = allocator, .limits = limits, .hash = tna_documents_hash,
                                  .free_head = TNA_DOCUMENT_NONE,
                                  .newest = TNA_DOCUMENT_NONE, .oldest = TNA_DOCUMENT_NONE };
    tna_vec_init(&documents->entries, allocator, TNA_LAYOUT(tna_document));
    tna_map_init(&documents->heads, allocator);
}

void tna_documents_set_limits(tna_documents *documents, tna_documents_limits limits) {
    documents->limits = limits;
}

/* ------------------------------------------------------------ LRU order */

static void tna_lru_unlink(tna_documents *documents, uint32_t index) {
    tna_document *entry = tna_entry(documents, index);
    if (entry->newer != TNA_DOCUMENT_NONE) {
        tna_entry(documents, entry->newer)->older = entry->older;
    } else {
        documents->newest = entry->older;
    }
    if (entry->older != TNA_DOCUMENT_NONE) {
        tna_entry(documents, entry->older)->newer = entry->newer;
    } else {
        documents->oldest = entry->newer;
    }
    entry->newer = TNA_DOCUMENT_NONE;
    entry->older = TNA_DOCUMENT_NONE;
}

static void tna_lru_push_newest(tna_documents *documents, uint32_t index) {
    tna_document *entry = tna_entry(documents, index);
    entry->newer = TNA_DOCUMENT_NONE;
    entry->older = documents->newest;
    if (documents->newest != TNA_DOCUMENT_NONE) {
        tna_entry(documents, documents->newest)->newer = index;
    }
    documents->newest = index;
    if (documents->oldest == TNA_DOCUMENT_NONE) {
        documents->oldest = index;
    }
}

/* -------------------------------------------------------- hash chains */

/* Takes `index` out of its hash's chain. Replacing a key never allocates. */
static void tna_chain_unlink(tna_documents *documents, uint32_t index) {
    tna_document *entry = tna_entry(documents, index);
    uint64_t head = TNA_DOCUMENT_NONE;
    if (!tna_map_get(&documents->heads, entry->hash, &head)) {
        return;
    }
    if (head == index) {
        if (entry->next_same == TNA_DOCUMENT_NONE) {
            tna_map_remove(&documents->heads, entry->hash);
        } else {
            (void)tna_map_put(&documents->heads, entry->hash, entry->next_same);
        }
        return;
    }
    for (uint32_t at = (uint32_t)head; at != TNA_DOCUMENT_NONE; at = tna_entry(documents, at)->next_same) {
        tna_document *before = tna_entry(documents, at);
        if (before->next_same == index) {
            before->next_same = entry->next_same;
            return;
        }
    }
}

/* Gives `index` back to the free list, with its bytes. */
static void tna_entry_release(tna_documents *documents, uint32_t index) {
    tna_document *entry = tna_entry(documents, index);
    tna_free_bytes(documents, entry);
    entry->state = TNA_DOCUMENT_FREE;
    entry->definition = 0;
    entry->next_same = documents->free_head;
    documents->free_head = index;
}

/* Removes a committed entry entirely. */
static void tna_entry_remove(tna_documents *documents, uint32_t index) {
    tna_document *entry = tna_entry(documents, index);
    documents->count -= 1u;
    documents->bytes -= entry->length;
    tna_lru_unlink(documents, index);
    tna_chain_unlink(documents, index);
    tna_entry_release(documents, index);
}

/* ------------------------------------------------------------ the API */

bool tna_documents_find(tna_documents *documents, ta_bytes bytes, ta_definition_id *out) {
    uint64_t head = TNA_DOCUMENT_NONE;
    if (!tna_map_get(&documents->heads, documents->hash(bytes), &head)) {
        return false;
    }
    for (uint32_t at = (uint32_t)head; at != TNA_DOCUMENT_NONE; at = tna_entry(documents, at)->next_same) {
        const tna_document *entry = tna_entry(documents, at);
        if (entry->state == TNA_DOCUMENT_COMMITTED && entry->length == bytes.len &&
            (bytes.len == 0 || memcmp(entry->bytes, bytes.ptr, bytes.len) == 0)) {
            *out = entry->definition;
            if (documents->newest != at) {
                tna_lru_unlink(documents, at);
                tna_lru_push_newest(documents, at);
            }
            return true;
        }
    }
    return false;
}

ta_result tna_documents_reserve(tna_documents *documents, ta_bytes bytes, uint32_t *out) {
    uint8_t *copy = tna_alloc(documents->allocator, bytes.len == 0 ? 1u : bytes.len, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (bytes.len != 0) {
        memcpy(copy, bytes.ptr, bytes.len);
    }
    uint32_t index = documents->free_head;
    bool fresh = index == TNA_DOCUMENT_NONE;
    if (fresh) {
        tna_document blank = { 0 };
        if (documents->entries.count == TNA_DOCUMENT_NONE ||
            tna_vec_push(&documents->entries, &blank) != TA_OK) {
            tna_free(documents->allocator, &(ta_allocator_free_params){ .ptr = copy, .size = bytes.len == 0 ? 1u : bytes.len,
                                                                        .alignment = 1 });
            return TA_ERR_NOMEM;
        }
        index = documents->entries.count - 1u;
    } else {
        documents->free_head = tna_entry(documents, index)->next_same;
    }
    uint64_t hash = documents->hash(bytes);
    uint64_t head = TNA_DOCUMENT_NONE;
    bool chained = tna_map_get(&documents->heads, hash, &head);
    if (tna_map_put(&documents->heads, hash, index) != TA_OK) {
        tna_document *entry = tna_entry(documents, index);
        *entry = (tna_document){ .bytes = copy, .length = bytes.len };
        tna_entry_release(documents, index);
        return TA_ERR_NOMEM;
    }
    *tna_entry(documents, index) = (tna_document){
        .hash = hash,
        .bytes = copy,
        .length = bytes.len,
        .newer = TNA_DOCUMENT_NONE,
        .older = TNA_DOCUMENT_NONE,
        .next_same = chained ? (uint32_t)head : TNA_DOCUMENT_NONE,
        .state = TNA_DOCUMENT_RESERVED,
    };
    *out = index;
    return TA_OK;
}

void tna_documents_commit(tna_documents *documents, uint32_t index, ta_definition_id definition) {
    tna_document *entry = tna_entry(documents, index);
    entry->definition = definition;
    entry->state = TNA_DOCUMENT_COMMITTED;
    documents->count += 1u;
    documents->bytes += entry->length;
    tna_lru_push_newest(documents, index);
}

void tna_documents_cancel(tna_documents *documents, uint32_t index) {
    tna_chain_unlink(documents, index);
    tna_entry_release(documents, index);
}

bool tna_documents_evict(tna_documents *documents, ta_definition_id *out) {
    bool over = documents->count > documents->limits.max_count || documents->bytes > documents->limits.max_bytes;
    uint32_t oldest = documents->oldest;
    if (!over || oldest == TNA_DOCUMENT_NONE || oldest == documents->newest) {
        return false;
    }
    *out = tna_entry(documents, oldest)->definition;
    tna_entry_remove(documents, oldest);
    return true;
}

bool tna_documents_forget(tna_documents *documents, ta_definition_id definition) {
    for (uint32_t at = 0; at < documents->entries.count; at += 1u) {
        const tna_document *entry = tna_entry(documents, at);
        if (entry->state == TNA_DOCUMENT_COMMITTED && entry->definition == definition) {
            tna_entry_remove(documents, at);
            return true;
        }
    }
    return false;
}

void tna_documents_destroy(tna_documents *documents) {
    for (uint32_t at = 0; at < documents->entries.count; at += 1u) {
        tna_document *entry = tna_entry(documents, at);
        if (entry->state != TNA_DOCUMENT_FREE) {
            tna_free_bytes(documents, entry);
        }
    }
    tna_vec_destroy(&documents->entries);
    tna_map_destroy(&documents->heads);
    *documents = (tna_documents){ 0 };
}
