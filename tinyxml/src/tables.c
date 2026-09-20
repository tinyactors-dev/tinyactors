/*
 * The DTD tables: general entities and ATTLIST defaults.
 *
 * Both are looked up by string, an entity at every reference and the defaults
 * of an element at every start tag, so a list would make the cost of a parse
 * the product of two document-controlled numbers. Each is an open-addressing
 * table of pointers that doubles in the parser arena.
 */
#include "parser.h"

/* ------------------------------------------------------------------------
 * The generic table.
 */

static uint32_t hash_bytes(uint32_t hash, txml_str text) {
    for (uint32_t i = 0u; i < text.length; i++) {
        hash = (hash ^ (uint8_t)text.ptr[i]) * 16777619u;
    }
    return hash;
}

static uint32_t hash_key(const table_key *key) {
    uint32_t hash = hash_bytes(2166136261u, key->first);
    hash = (hash ^ 0xFFu) * 16777619u;
    return hash_bytes(hash, key->second);
}

static bool keys_equal(const table_key *left, const table_key *right) {
    return txml_str_equal(left->first, right->first) && txml_str_equal(left->second, right->second);
}

static void *table_find(const hashed *table, const table_kind *kind, const table_key *key) {
    if (table->capacity == 0u) {
        return NULL;
    }
    const uint32_t mask = table->capacity - 1u;
    for (uint32_t slot = hash_key(key) & mask; table->slots[slot] != NULL; slot = (slot + 1u) & mask) {
        const table_key found = kind->key(table->slots[slot]);
        if (keys_equal(&found, key)) {
            return table->slots[slot];
        }
    }
    return NULL;
}

typedef struct {
    void **slots;
    uint32_t capacity;
} slot_array;

static void place(const slot_array *array, uint32_t hash, void *item) {
    void **slots = array->slots;
    const uint32_t capacity = array->capacity;
    const uint32_t mask = capacity - 1u;
    uint32_t slot = hash & mask;
    while (slots[slot] != NULL) {
        slot = (slot + 1u) & mask;
    }
    slots[slot] = item;
}

typedef struct {
    hashed *table;
    const table_kind *kind;
} table_target;

static scan table_insert(txml_parser *p, const table_target *target, void *item) {
    hashed *table = target->table;
    const table_kind *kind = target->kind;
    if ((table->count + 1u) * 2u > table->capacity) {
        const uint32_t capacity = table->capacity == 0u ? 64u : table->capacity * 2u;
        void **slots = (void **)txml_arena_alloc(&p->parser_arena, (size_t)capacity * sizeof(void *), sizeof(void *));
        if (slots == NULL) {
            return SCAN_NOMEM;
        }
        for (uint32_t i = 0u; i < capacity; i++) {
            slots[i] = NULL;
        }
        for (uint32_t i = 0u; i < table->capacity; i++) {
            if (table->slots[i] != NULL) {
                const table_key key = kind->key(table->slots[i]);
                place(&(slot_array){ slots, capacity }, hash_key(&key), table->slots[i]);
            }
        }
        table->slots = slots;
        table->capacity = capacity;
    }
    const table_key key = kind->key(item);
    place(&(slot_array){ table->slots, table->capacity }, hash_key(&key), item);
    table->count += 1u;
    return SCAN_OK;
}

/* ------------------------------------------------------------------------
 * Keys.
 */

static table_key entity_key(const void *item) {
    const entity *e = (const entity *)item;
    return (table_key){ e->name, { "", 0u } };
}

static table_key default_key(const void *item) {
    const attlist_default *d = (const attlist_default *)item;
    return (table_key){ d->element, d->name };
}

static table_key element_key(const void *item) {
    const element_defaults *e = (const element_defaults *)item;
    return (table_key){ e->element, { "", 0u } };
}

static const table_kind entity_kind = { entity_key };
static const table_kind default_kind = { default_key };
static const table_kind element_kind = { element_key };

const entity *txp_find_entity(const txml_parser *p, txml_str name) {
    return (const entity *)table_find(&p->entities, &entity_kind, &(table_key){ name, { "", 0u } });
}

const element_defaults *txp_find_element_defaults(const txml_parser *p, txml_str element) {
    return (const element_defaults *)table_find(&p->element_defaults, &element_kind, &(table_key){ element, { "", 0u } });
}

/* ------------------------------------------------------------------------
 * Adding declarations.
 */

static bool remember(txml_parser *p, txml_str text, txml_str *copy) {
    char *bytes = text.length == 0u ? NULL : (char *)txml_arena_alloc(&p->parser_arena, text.length, 1u);
    if (text.length != 0u && bytes == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < text.length; i++) {
        bytes[i] = text.ptr[i];
    }
    *copy = (txml_str){ bytes == NULL ? "" : bytes, text.length };
    return true;
}

static scan charge(txml_parser *p, uint64_t bytes) {
    p->table_bytes += bytes;
    if (p->table_bytes > p->limits.max_document_bytes) {
        return txp_fail(p, PROBLEM_LIMIT(TXML_LIMIT_DOCUMENT_BYTES, "the DTD tables exceed max_document_bytes"), p->construct);
    }
    return SCAN_OK;
}

scan txp_add_entity(txml_parser *p, txml_str name, txml_str value) {
    if (txp_find_entity(p, name) != NULL) {
        return SCAN_OK; /* the first declaration binds */
    }
    txml_str expanded;
    scan outcome = txp_expand_character_references(p, value, &expanded);
    if (outcome != SCAN_OK) {
        return outcome;
    }
    outcome = charge(p, (uint64_t)name.length + expanded.length + sizeof(entity));
    if (outcome != SCAN_OK) {
        return outcome;
    }
    entity *e = (entity *)txml_arena_alloc(&p->parser_arena, sizeof(entity), _Alignof(entity));
    if (e == NULL || !remember(p, name, &e->name)) {
        return SCAN_NOMEM;
    }
    e->value = expanded;
    return table_insert(p, &(table_target){ &p->entities, &entity_kind }, e);
}

scan txp_add_default(txml_parser *p, const default_request *request) {
    if (table_find(&p->default_keys, &default_kind, &(table_key){ request->element, request->name }) != NULL) {
        return SCAN_OK;
    }
    const scan charged = charge(p, (uint64_t)request->element.length + request->name.length + request->value.length +
                                       sizeof(attlist_default));
    if (charged != SCAN_OK) {
        return charged;
    }
    attlist_default *d = (attlist_default *)txml_arena_alloc(&p->parser_arena, sizeof(attlist_default), _Alignof(attlist_default));
    if (d == NULL || !remember(p, request->element, &d->element) || !remember(p, request->name, &d->name) ||
        !remember(p, request->value, &d->value)) {
        return SCAN_NOMEM;
    }
    d->next = NULL;

    element_defaults *list = (element_defaults *)table_find(&p->element_defaults, &element_kind,
                                                            &(table_key){ request->element, { "", 0u } });
    if (list == NULL) {
        list = (element_defaults *)txml_arena_alloc(&p->parser_arena, sizeof(element_defaults), _Alignof(element_defaults));
        if (list == NULL) {
            return SCAN_NOMEM;
        }
        list->element = d->element;
        list->first = list->last = NULL;
        const scan inserted = table_insert(p, &(table_target){ &p->element_defaults, &element_kind }, list);
        if (inserted != SCAN_OK) {
            return inserted;
        }
    }
    const scan keyed = table_insert(p, &(table_target){ &p->default_keys, &default_kind }, d);
    if (keyed != SCAN_OK) {
        return keyed;
    }
    if (list->last == NULL) {
        list->first = d;
    } else {
        list->last->next = d;
    }
    list->last = d;
    return SCAN_OK;
}
