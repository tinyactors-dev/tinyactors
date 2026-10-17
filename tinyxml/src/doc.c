/*
 * The document: paged columns over two arenas, the byte pool, and name
 * interning. See docs/internal/document-layout.md and memory-architecture.md.
 */
#include "doc.h"

#define COLUMN_PAGE_SHIFT 12u
#define COLUMN_PAGE_ENTRIES ((uint32_t)1u << COLUMN_PAGE_SHIFT)
#define COLUMN_PAGE_MASK (COLUMN_PAGE_ENTRIES - 1u)
#define NAME_SLOTS_INITIAL 64u
#define POOL_PAGE_MIN 1024u
#define POOL_PAGE_MAX 65536u

/* ------------------------------------------------------------------------
 * Columns.
 */

typedef struct {
    uint32_t entry_size;
    uint64_t max_document_bytes;
} column_shape;

static txml_result column_init(txml_column *column, txml_arena *arena, const column_shape *shape) {
    const uint32_t entry_size = shape->entry_size;
    const uint64_t max_document_bytes = shape->max_document_bytes;
    const uint64_t page_bytes = (uint64_t)COLUMN_PAGE_ENTRIES * entry_size;
    const uint64_t capacity = max_document_bytes / page_bytes + 1u;

    column->directory = (void **)txml_arena_alloc(arena, (size_t)capacity * sizeof(void *), sizeof(void *));
    if (column->directory == NULL) {
        return TXML_ERR_NOMEM;
    }
    for (uint64_t i = 0u; i < capacity; i++) {
        column->directory[i] = NULL;
    }
    column->directory_capacity = (uint32_t)capacity;
    column->count = 0u;
    column->entry_size = entry_size;
    return TXML_OK;
}

static void *column_entry(const txml_column *column, uint32_t index) {
    unsigned char *page = (unsigned char *)column->directory[index >> COLUMN_PAGE_SHIFT];
    return page + (size_t)(index & COLUMN_PAGE_MASK) * column->entry_size;
}

static void *column_at(const txml_column *column, uint32_t index) {
    return index < column->count ? column_entry(column, index) : NULL;
}

static txml_result column_push(txml_column *column, txml_arena *arena, void **entry) {
    const uint32_t page = column->count >> COLUMN_PAGE_SHIFT;

    if (page >= column->directory_capacity || column->count >= TXML_INDEX_NONE - 1u) {
        return TXML_ERR_LIMIT;
    }
    if (column->directory[page] == NULL) {
        void *fresh = txml_arena_alloc(arena, (size_t)COLUMN_PAGE_ENTRIES * column->entry_size, 8u);
        if (fresh == NULL) {
            return TXML_ERR_NOMEM;
        }
        column->directory[page] = fresh;
    }
    *entry = column_entry(column, column->count);
    column->count += 1u;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * The pool.
 */

#define POOL_DIRECTORY_INITIAL 16u

/* The most slots the directory needs: every page of the largest document the
 * limits allow, and the pages an oversized run skips to. */
static uint32_t pool_directory_limit(const txml_doc *doc) {
    return (uint32_t)(doc->limits.max_document_bytes >> doc->pool_shift) + 2u;
}

/* Room in the pool directory for slot `last`. The directory grows with the
 * document, doubling: a new array in the values arena takes the page
 * pointers, and the old one is abandoned there (counted in `used`, about
 * the size of the final one in all). The pages do not move, so no offset or
 * txml_str changes; only the index to them is copied. */
static txml_result pool_directory_reserve(txml_doc *doc, uint32_t last) {
    if (last < doc->pool_directory_capacity) {
        return TXML_OK;
    }
    const uint32_t limit = pool_directory_limit(doc);
    if (last >= limit) {
        return TXML_ERR_LIMIT;
    }
    uint64_t capacity = doc->pool_directory_capacity == 0u ? POOL_DIRECTORY_INITIAL : doc->pool_directory_capacity;
    while (capacity <= last) {
        capacity *= 2u;
    }
    if (capacity > limit) {
        capacity = limit;
    }
    unsigned char **directory = (unsigned char **)txml_arena_alloc(
        &doc->values, (size_t)capacity * sizeof(unsigned char *), sizeof(void *));
    if (directory == NULL) {
        return TXML_ERR_NOMEM;
    }
    uint32_t i = 0u;
    for (; i < doc->pool_directory_capacity; i++) {
        directory[i] = doc->pool_directory[i];
    }
    for (; i < (uint32_t)capacity; i++) {
        directory[i] = NULL;
    }
    doc->pool_directory = directory;
    doc->pool_directory_capacity = (uint32_t)capacity;
    return TXML_OK;
}

txml_str txml_doc_pool_str(const txml_doc *doc, uint32_t offset, uint32_t length) {
    static const char empty[1] = { 0 };
    txml_str out = { empty, 0u };

    if (length == 0u || offset > doc->pool_length || length > doc->pool_length - offset) {
        return out;
    }
    const uint32_t mask = ((uint32_t)1u << doc->pool_shift) - 1u;
    out.ptr = (const char *)(doc->pool_directory[offset >> doc->pool_shift] + (offset & mask));
    out.length = length;
    return out;
}

txml_result txml_doc_pool_append(txml_doc *doc, txml_str bytes, uint32_t *offset) {
    const uint32_t page_size = (uint32_t)1u << doc->pool_shift;
    const uint32_t mask = page_size - 1u;
    const uint32_t length = bytes.length;
    uint32_t start = doc->pool_length;

    if (length == 0u) {
        *offset = start;
        return TXML_OK;
    }

    /* A run that does not fit the rest of its page starts the next one; a run
     * larger than a page always starts on a boundary. */
    if (length > page_size || (start & mask) + length > page_size) {
        if (start & mask) {
            const uint32_t skip = page_size - (start & mask);
            if (start > UINT32_MAX - skip) {
                return TXML_ERR_LIMIT;
            }
            start += skip;
        }
    }
    if (start > UINT32_MAX - length) {
        return TXML_ERR_LIMIT;
    }

    const uint32_t first = start >> doc->pool_shift;
    const uint32_t last = (start + length - 1u) >> doc->pool_shift;
    const txml_result reserved = pool_directory_reserve(doc, last);
    if (reserved != TXML_OK) {
        return reserved;
    }

    if (length > page_size) {
        /* One block over consecutive slots keeps the run contiguous. */
        const uint32_t pages = last - first + 1u;
        unsigned char *block = (unsigned char *)txml_arena_alloc(&doc->values, (size_t)pages * page_size, 8u);
        if (block == NULL) {
            return TXML_ERR_NOMEM;
        }
        for (uint32_t k = 0u; k < pages; k++) {
            doc->pool_directory[first + k] = block + (size_t)k * page_size;
        }
    } else if (doc->pool_directory[first] == NULL) {
        unsigned char *page = (unsigned char *)txml_arena_alloc(&doc->values, page_size, 8u);
        if (page == NULL) {
            return TXML_ERR_NOMEM;
        }
        doc->pool_directory[first] = page;
    }

    unsigned char *destination = doc->pool_directory[first] + (start & mask);
    for (uint32_t i = 0u; i < length; i++) {
        destination[i] = (unsigned char)bytes.ptr[i];
    }
    doc->pool_length = start + length;
    *offset = start;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * The run builder.
 */

#define RUN_MINIMUM_CAPACITY 64u

void txml_doc_run_begin(txml_doc *doc, txml_run *run) {
    run->offset = doc->pool_length;
    run->length = 0u;
    run->capacity = 0u;
}

static unsigned char *run_base(const txml_doc *doc, uint32_t offset) {
    const uint32_t mask = ((uint32_t)1u << doc->pool_shift) - 1u;
    return doc->pool_directory[offset >> doc->pool_shift] + (offset & mask);
}

txml_result txml_doc_run_reserve(txml_doc *doc, txml_run *run, uint32_t extra) {
    const uint32_t page_size = (uint32_t)1u << doc->pool_shift;
    const uint32_t mask = page_size - 1u;

    if (extra > UINT32_MAX - run->length) {
        return TXML_ERR_LIMIT;
    }
    const uint32_t need = run->length + extra;
    if (need <= run->capacity || need == 0u) {
        return TXML_OK;
    }

    uint64_t base = run->offset;
    if (run->capacity == 0u) {
        /* First placement: use the tail of the current page when it is big
         * enough, and start a fresh page rather than crowd into a sliver. */
        const uint32_t start = run->offset;
        const uint32_t remaining = page_size - (start & mask);
        const uint32_t wanted = need < RUN_MINIMUM_CAPACITY ? RUN_MINIMUM_CAPACITY : need;
        if (wanted <= remaining) {
            const uint32_t slot = start >> doc->pool_shift;
            const txml_result reserved = pool_directory_reserve(doc, slot);
            if (reserved != TXML_OK) {
                return reserved;
            }
            if (doc->pool_directory[slot] == NULL) {
                unsigned char *page = (unsigned char *)txml_arena_alloc(&doc->values, page_size, 8u);
                if (page == NULL) {
                    return TXML_ERR_NOMEM;
                }
                doc->pool_directory[slot] = page;
            }
            run->capacity = remaining;
            return TXML_OK;
        }
    } else {
        base += run->capacity;
    }

    /* Move to a fresh block of whole pages after the old space. */
    const uint64_t begin = (base + mask) & ~(uint64_t)mask;
    uint64_t want = run->capacity == 0u ? need : (uint64_t)run->capacity * 2u;
    if (want < need) {
        want = need;
    }
    if (want < page_size) {
        want = page_size;
    }
    const uint64_t pages = (want + mask) >> doc->pool_shift;
    const uint64_t capacity = pages << doc->pool_shift;
    if (begin + capacity > UINT32_MAX || (begin >> doc->pool_shift) + pages > pool_directory_limit(doc)) {
        return TXML_ERR_LIMIT;
    }
    if ((uint64_t)doc->values.used + doc->tree.used + capacity > doc->limits.max_document_bytes) {
        return TXML_ERR_LIMIT;
    }
    const txml_result reserved = pool_directory_reserve(doc, (uint32_t)((begin >> doc->pool_shift) + pages - 1u));
    if (reserved != TXML_OK) {
        return reserved;
    }
    unsigned char *block = (unsigned char *)txml_arena_alloc(&doc->values, (size_t)capacity, 8u);
    if (block == NULL) {
        return TXML_ERR_NOMEM;
    }
    if (run->length != 0u) {
        const unsigned char *old = run_base(doc, run->offset);
        for (uint32_t i = 0u; i < run->length; i++) {
            block[i] = old[i];
        }
    }
    const uint32_t first = (uint32_t)(begin >> doc->pool_shift);
    for (uint32_t k = 0u; k < (uint32_t)pages; k++) {
        doc->pool_directory[first + k] = block + (size_t)k * page_size;
    }
    run->offset = (uint32_t)begin;
    run->capacity = (uint32_t)capacity;
    return TXML_OK;
}

txml_result txml_doc_run_append(txml_doc *doc, txml_run *run, txml_str bytes) {
    if (bytes.length == 0u) {
        return TXML_OK;
    }
    const txml_result result = txml_doc_run_reserve(doc, run, bytes.length);
    if (result != TXML_OK) {
        return result;
    }
    unsigned char *destination = run_base(doc, run->offset) + run->length;
    for (uint32_t i = 0u; i < bytes.length; i++) {
        destination[i] = (unsigned char)bytes.ptr[i];
    }
    run->length += bytes.length;
    return TXML_OK;
}

txml_str txml_doc_run_view(const txml_doc *doc, const txml_run *run) {
    static const char empty[1] = { 0 };
    if (run->length == 0u) {
        return (txml_str){ empty, 0u };
    }
    return (txml_str){ (const char *)run_base(doc, run->offset), run->length };
}

void txml_doc_run_finish(txml_doc *doc, const txml_run *run) {
    /* Never backwards: a retry after other pool appends must not rewind them. */
    if (run->length != 0u && run->offset + run->length > doc->pool_length) {
        doc->pool_length = run->offset + run->length;
    }
}

/* ------------------------------------------------------------------------
 * Names and namespaces.
 */

static uint32_t hash_step(uint32_t hash, unsigned char byte) {
    return (hash ^ byte) * 16777619u;
}

static uint32_t name_hash(txml_str uri, txml_str local) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0u; i < uri.length; i++) {
        hash = hash_step(hash, (unsigned char)uri.ptr[i]);
    }
    hash = hash_step(hash, 0xFFu);
    for (uint32_t i = 0u; i < local.length; i++) {
        hash = hash_step(hash, (unsigned char)local.ptr[i]);
    }
    return hash;
}

static txml_str namespace_uri(const txml_doc *doc, uint32_t id) {
    const txml_namespace_entry *entry = (const txml_namespace_entry *)column_at(&doc->namespaces, id);
    return entry == NULL ? txml_doc_pool_str(doc, 0u, 0u) : txml_doc_pool_str(doc, entry->uri_offset, entry->uri_length);
}

static uint32_t find_namespace(const txml_doc *doc, txml_str uri) {
    for (uint32_t id = 0u; id < doc->namespaces.count; id++) {
        if (txml_str_equal(namespace_uri(doc, id), uri)) {
            return id;
        }
    }
    return TXML_NAME_NONE;
}

txml_result txml_doc_intern_namespace(txml_doc *doc, txml_str uri, uint32_t *id) {
    const uint32_t found = find_namespace(doc, uri);
    if (found != TXML_NAME_NONE) {
        *id = found;
        return TXML_OK;
    }

    /* Namespaces are found by scanning, so their number is bounded like names. */
    if (doc->namespaces.count >= doc->limits.max_names) {
        return TXML_ERR_LIMIT;
    }
    uint32_t offset = 0u;
    txml_result result = txml_doc_pool_append(doc, uri, &offset);
    if (result != TXML_OK) {
        return result;
    }
    void *slot = NULL;
    result = column_push(&doc->namespaces, &doc->values, &slot);
    if (result != TXML_OK) {
        return result;
    }
    txml_namespace_entry *entry = (txml_namespace_entry *)slot;
    entry->uri_offset = offset;
    entry->uri_length = uri.length;
    *id = doc->namespaces.count - 1u;
    return TXML_OK;
}

typedef struct {
    uint32_t namespace_id;
    txml_str local;
    uint32_t hash;
} name_key;

static uint32_t probe_name(const txml_doc *doc, const name_key *key, uint32_t *free_slot) {
    const uint32_t namespace_id = key->namespace_id;
    const txml_str local = key->local;
    const uint32_t hash = key->hash;
    const uint32_t mask = doc->name_slot_capacity - 1u;
    uint32_t slot = hash & mask;

    for (;;) {
        const uint32_t id = doc->name_slots[slot];
        if (id == TXML_NAME_NONE) {
            *free_slot = slot;
            return TXML_NAME_NONE;
        }
        const txml_name_entry *entry = (const txml_name_entry *)column_entry(&doc->names, id);
        if (entry->hash == hash && entry->namespace_id == namespace_id &&
            txml_str_equal(txml_doc_pool_str(doc, entry->local_offset, entry->local_length), local)) {
            return id;
        }
        slot = (slot + 1u) & mask;
    }
}

static txml_result grow_name_slots(txml_doc *doc) {
    const uint32_t capacity = doc->name_slot_capacity * 2u;
    uint32_t *slots = (uint32_t *)txml_arena_alloc(&doc->values, (size_t)capacity * sizeof(uint32_t), sizeof(uint32_t));
    if (slots == NULL) {
        return TXML_ERR_NOMEM;
    }
    for (uint32_t i = 0u; i < capacity; i++) {
        slots[i] = TXML_NAME_NONE;
    }
    /* Only IDs move between tables; no address a caller holds changes. */
    for (uint32_t id = 0u; id < doc->names.count; id++) {
        const txml_name_entry *entry = (const txml_name_entry *)column_entry(&doc->names, id);
        uint32_t slot = entry->hash & (capacity - 1u);
        while (slots[slot] != TXML_NAME_NONE) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        slots[slot] = id;
    }
    doc->name_slots = slots;
    doc->name_slot_capacity = capacity;
    return TXML_OK;
}

txml_result txml_doc_intern_name(txml_doc *doc, const txml_intern_params *params, uint32_t *id) {
    if (params->namespace_id >= doc->namespaces.count) {
        return TXML_ERR_INVALID;
    }
    const uint32_t hash = name_hash(namespace_uri(doc, params->namespace_id), params->local);
    uint32_t free_slot = 0u;
    const uint32_t found = probe_name(doc, &(name_key){ params->namespace_id, params->local, hash }, &free_slot);
    if (found != TXML_NAME_NONE) {
        *id = found;
        return TXML_OK;
    }

    if (doc->names.count >= doc->limits.max_names || doc->names.count >= TXML_NAME_ID_LIMIT - 1u) {
        return TXML_ERR_LIMIT;
    }
    if ((doc->names.count + 1u) * 2u > doc->name_slot_capacity) {
        const txml_result grown = grow_name_slots(doc);
        if (grown != TXML_OK) {
            return grown;
        }
        probe_name(doc, &(name_key){ params->namespace_id, params->local, hash }, &free_slot);
    }

    uint32_t local_offset = 0u;
    uint32_t prefix_offset = 0u;
    txml_result result = txml_doc_pool_append(doc, params->local, &local_offset);
    if (result == TXML_OK) {
        result = txml_doc_pool_append(doc, params->prefix, &prefix_offset);
    }
    void *slot = NULL;
    if (result == TXML_OK) {
        result = column_push(&doc->names, &doc->values, &slot);
    }
    if (result != TXML_OK) {
        return result;
    }

    txml_name_entry *entry = (txml_name_entry *)slot;
    entry->namespace_id = params->namespace_id;
    entry->local_offset = local_offset;
    entry->local_length = params->local.length;
    entry->prefix_offset = prefix_offset;
    entry->prefix_length = params->prefix.length;
    entry->hash = hash;
    *id = doc->names.count - 1u;
    doc->name_slots[free_slot] = *id;
    return TXML_OK;
}

uint32_t txml_doc_find_name(const txml_doc *doc, const txml_qname *name) {
    if (doc == NULL || name == NULL) {
        return TXML_NAME_NONE;
    }
    const uint32_t namespace_id = find_namespace(doc, name->ns);
    if (namespace_id == TXML_NAME_NONE) {
        return TXML_NAME_NONE;
    }
    uint32_t unused = 0u;
    return probe_name(doc, &(name_key){ namespace_id, name->local, name_hash(name->ns, name->local) }, &unused);
}

uint32_t txml_doc_name_count(const txml_doc *doc) {
    return doc == NULL ? 0u : doc->names.count;
}

txml_result txml_doc_name_at(const txml_doc *doc, uint32_t name, txml_qname *out) {
    const txml_name_entry *entry = txml_doc_name(doc, name);
    if (entry == NULL) {
        return TXML_ERR_NOT_FOUND;
    }
    out->ns = namespace_uri(doc, entry->namespace_id);
    out->local = txml_doc_pool_str(doc, entry->local_offset, entry->local_length);
    return TXML_OK;
}

const txml_namespace_entry *txml_doc_namespace(const txml_doc *doc, uint32_t id) {
    return doc == NULL ? NULL : (const txml_namespace_entry *)column_at(&doc->namespaces, id);
}

const txml_name_entry *txml_doc_name(const txml_doc *doc, uint32_t id) {
    return doc == NULL ? NULL : (const txml_name_entry *)column_at(&doc->names, id);
}

/* ------------------------------------------------------------------------
 * Nodes and attributes.
 */

const txml_node *txml_doc_node(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_node *)column_at(&doc->nodes, index);
}

const txml_attribute_entry *txml_doc_attribute(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_attribute_entry *)column_at(&doc->attributes, index);
}

const txml_span *txml_doc_span(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_span *)column_at(&doc->positions, index);
}

const txml_position *txml_doc_position(const txml_doc *doc, uint32_t index) {
    const txml_span *span = txml_doc_span(doc, index);
    return span == NULL ? NULL : &span->start;
}

const txml_attribute_source *txml_doc_attribute_source(const txml_doc *doc, uint32_t index) {
    return doc == NULL ? NULL : (const txml_attribute_source *)column_at(&doc->attribute_sources, index);
}

uint32_t txml_doc_attribute_flags(const txml_doc *doc, const txml_attribute_entry *attribute) {
    static const char xmlns[] = "http://www.w3.org/2000/xmlns/";
    const txml_name_entry *entry = txml_doc_name(doc, attribute->name & TXML_ATTRIBUTE_NAME_MASK);
    uint32_t flags = (attribute->name & TXML_ATTRIBUTE_DEFAULTED) != 0u ? TXML_ATTR_DEFAULTED : 0u;
    if (entry != NULL && txml_str_equal(namespace_uri(doc, entry->namespace_id), TXML_STR(xmlns))) {
        flags |= TXML_ATTR_NS_DECL;
    }
    return flags;
}

uint32_t txml_doc_node_count(const txml_doc *doc) {
    return doc == NULL ? 0u : doc->nodes.count;
}

txml_result txml_doc_append_node(txml_doc *doc, const txml_node_append *append, uint32_t *index) {
    void *node_slot = NULL;
    void *position_slot = NULL;

    txml_result result = column_push(&doc->nodes, &doc->tree, &node_slot);
    if (result != TXML_OK) {
        return result;
    }
    result = column_push(&doc->positions, &doc->tree, &position_slot);
    if (result != TXML_OK) {
        doc->nodes.count -= 1u; /* keep the columns parallel */
        return result;
    }
    *(txml_node *)node_slot = append->node;
    *(txml_span *)position_slot = (txml_span){ append->position, append->end };
    *index = doc->nodes.count - 1u;
    return TXML_OK;
}

void txml_doc_close_node(txml_doc *doc, uint32_t index, const txml_close *close) {
    txml_node *node = (txml_node *)column_at(&doc->nodes, index);
    txml_span *span = (txml_span *)column_at(&doc->positions, index);
    if (node != NULL && span != NULL) {
        node->end = close->end;
        node->flags = (uint8_t)(node->flags & ~TXML_NODE_OPEN);
        span->end = close->position;
    }
}

void txml_doc_complete(txml_doc *doc, txml_position end) {
    txml_doc_close_node(doc, 0u, &(txml_close){ doc->nodes.count, end });
    doc->complete = true;
}

txml_result txml_doc_append_attribute(txml_doc *doc, const txml_attribute_append *append, uint32_t *index) {
    void *slot = NULL;
    void *source_slot = NULL;
    txml_result result = column_push(&doc->attributes, &doc->tree, &slot);
    if (result != TXML_OK) {
        return result;
    }
    result = column_push(&doc->attribute_sources, &doc->tree, &source_slot);
    if (result != TXML_OK) {
        doc->attributes.count -= 1u; /* keep the columns parallel */
        return result;
    }
    *(txml_attribute_source *)source_slot = append->source;
    *(txml_attribute_entry *)slot = append->entry;
    *index = doc->attributes.count - 1u;
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Prefixes.
 */

static uint32_t name_of(const txml_doc *doc, bool attribute, uint32_t index) {
    if (attribute) {
        const txml_attribute_entry *entry = txml_doc_attribute(doc, index);
        return entry == NULL ? TXML_NAME_NONE : entry->name & TXML_ATTRIBUTE_NAME_MASK;
    }
    const txml_node *node = txml_doc_node(doc, index);
    return node == NULL ? TXML_NAME_NONE : node->name;
}

txml_str txml_doc_prefix_of(const txml_doc *doc, bool attribute, uint32_t index) {
    const txml_column *exceptions = attribute ? &doc->attribute_prefixes : &doc->node_prefixes;
    uint32_t low = 0u;
    uint32_t high = exceptions->count;

    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        const txml_prefix_entry *entry = (const txml_prefix_entry *)column_entry(exceptions, middle);
        if (entry->index == index) {
            return txml_doc_pool_str(doc, entry->offset, entry->length);
        }
        if (entry->index < index) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    const txml_name_entry *name = txml_doc_name(doc, name_of(doc, attribute, index));
    return name == NULL ? txml_doc_pool_str(doc, 0u, 0u) : txml_doc_pool_str(doc, name->prefix_offset, name->prefix_length);
}

txml_result txml_doc_record_prefix(txml_doc *doc, const txml_prefix_request *request) {
    const txml_name_entry *name = txml_doc_name(doc, name_of(doc, request->attribute, request->index));
    if (name == NULL) {
        return TXML_ERR_INVALID;
    }
    if (txml_str_equal(request->prefix, txml_doc_pool_str(doc, name->prefix_offset, name->prefix_length))) {
        return TXML_OK;
    }

    uint32_t offset = 0u;
    void *slot = NULL;
    txml_column *exceptions = request->attribute ? &doc->attribute_prefixes : &doc->node_prefixes;
    txml_result result = txml_doc_pool_append(doc, request->prefix, &offset);
    if (result == TXML_OK) {
        result = column_push(exceptions, &doc->tree, &slot);
    }
    if (result != TXML_OK) {
        return result;
    }
    *(txml_prefix_entry *)slot = (txml_prefix_entry){ request->index, offset, request->prefix.length };
    return TXML_OK;
}

/* ------------------------------------------------------------------------
 * Lifetime.
 */

const txml_arena *txml_doc_arena(const txml_doc *doc, txml_arena_kind which) {
    if (doc == NULL) {
        return NULL;
    }
    return which == TXML_ARENA_VALUES ? &doc->values : &doc->tree;
}

void txml_doc_destroy(txml_doc *doc) {
    if (doc == NULL) {
        return;
    }
    const txml_allocator allocator = doc->allocator;
    txml_arena_destroy(&doc->values);
    txml_arena_destroy(&doc->tree);

    txml_allocator_free_params params = { .ptr = doc, .size = sizeof(txml_doc), .alignment = _Alignof(txml_doc) };
    allocator.v->free(allocator.self, &params);
}

static txml_result doc_build(txml_doc *doc) {
    const uint64_t budget = doc->limits.max_document_bytes;

    txml_result result = column_init(&doc->nodes, &doc->tree, &(column_shape){ sizeof(txml_node), budget });
    if (result == TXML_OK) {
        result = column_init(&doc->attributes, &doc->tree, &(column_shape){ sizeof(txml_attribute_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->positions, &doc->tree, &(column_shape){ sizeof(txml_span), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->attribute_sources, &doc->tree, &(column_shape){ sizeof(txml_attribute_source), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->node_prefixes, &doc->tree, &(column_shape){ sizeof(txml_prefix_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->attribute_prefixes, &doc->tree, &(column_shape){ sizeof(txml_prefix_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->names, &doc->values, &(column_shape){ sizeof(txml_name_entry), budget });
    }
    if (result == TXML_OK) {
        result = column_init(&doc->namespaces, &doc->values, &(column_shape){ sizeof(txml_namespace_entry), budget });
    }
    if (result != TXML_OK) {
        return result;
    }

    /* the pool directory grows with the document (pool_directory_reserve) */
    result = pool_directory_reserve(doc, 0u);
    if (result != TXML_OK) {
        return result;
    }
    doc->name_slots = (uint32_t *)txml_arena_alloc(
        &doc->values, (size_t)NAME_SLOTS_INITIAL * sizeof(uint32_t), sizeof(uint32_t));
    if (doc->name_slots == NULL) {
        return TXML_ERR_NOMEM;
    }
    for (uint32_t i = 0u; i < NAME_SLOTS_INITIAL; i++) {
        doc->name_slots[i] = TXML_NAME_NONE;
    }
    doc->name_slot_capacity = NAME_SLOTS_INITIAL;

    /* Namespace 0 is the empty URI, and node 0 is the open document node. */
    void *slot = NULL;
    result = column_push(&doc->namespaces, &doc->values, &slot);
    if (result != TXML_OK) {
        return result;
    }
    ((txml_namespace_entry *)slot)->uri_offset = 0u;
    ((txml_namespace_entry *)slot)->uri_length = 0u;

    const txml_node document = {
        .kind = TXML_NODE_DOCUMENT,
        .flags = TXML_NODE_OPEN,
        .depth = 0u,
        .parent = TXML_INDEX_NONE,
        .end = TXML_INDEX_NONE,
        .name = TXML_NAME_NONE,
        .as.element = { 0u, 0u },
    };
    uint32_t index = 0u;
    const txml_node_append append = { document, { 0u, 1u, 1u }, { 0u, 1u, 1u } };
    return txml_doc_append_node(doc, &append, &index);
}

txml_result txml_doc_create(txml_doc **out, const txml_parser_config *config) {
    if (out == NULL || config == NULL || config->allocator.v == NULL || config->allocator.v->alloc == NULL ||
        config->allocator.v->free == NULL) {
        return TXML_ERR_INVALID;
    }
    if (config->limits.max_depth > 65535u || config->limits.max_document_bytes > UINT32_MAX ||
        config->limits.max_document_bytes == 0u) {
        return TXML_ERR_INVALID;
    }

    txml_doc *doc = (txml_doc *)config->allocator.v->alloc(config->allocator.self, sizeof(txml_doc), _Alignof(txml_doc));
    if (doc == NULL) {
        return TXML_ERR_NOMEM;
    }

    const size_t chunk_size = config->chunk_size == 0u ? TXML_ARENA_CHUNK_DEFAULT : config->chunk_size;
    uint32_t pool_page = POOL_PAGE_MAX;
    if (chunk_size < POOL_PAGE_MAX) {
        pool_page = chunk_size < POOL_PAGE_MIN ? POOL_PAGE_MIN : (uint32_t)chunk_size;
    }
    uint32_t shift = 0u;
    while (((uint32_t)2u << shift) <= pool_page) {
        shift += 1u;
    }

    doc->allocator = config->allocator;
    doc->limits = config->limits;
    doc->pool_shift = shift;
    doc->pool_length = 0u;
    doc->pool_directory = NULL;
    doc->pool_directory_capacity = 0u;
    doc->name_slots = NULL;
    doc->name_slot_capacity = 0u;
    doc->complete = false;
    doc->nodes = (txml_column){ 0 };
    doc->attributes = (txml_column){ 0 };
    doc->positions = (txml_column){ 0 };
    doc->attribute_sources = (txml_column){ 0 };
    doc->node_prefixes = (txml_column){ 0 };
    doc->attribute_prefixes = (txml_column){ 0 };
    doc->names = (txml_column){ 0 };
    doc->namespaces = (txml_column){ 0 };

    txml_result result = txml_arena_init(&doc->values, config->allocator, chunk_size);
    if (result == TXML_OK) {
        result = txml_arena_init(&doc->tree, config->allocator, chunk_size);
    }
    if (result == TXML_OK) {
        result = doc_build(doc);
    }
    if (result != TXML_OK) {
        txml_doc_destroy(doc);
        return result;
    }
    *out = doc;
    return TXML_OK;
}
