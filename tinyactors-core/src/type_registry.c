#include "type_registry.h"
#include "hash.h"

/* The package comment is in type_registry.h. */

/* Slots per full index page: the registry holds a handful of names. */
#define TA_TYPE_PAGE_SLOTS 64u
/* Slots of the first page; ta_table_grow doubles it to a full page before it adds one. */
#define TA_TYPE_FIRST_SLOTS 16u

#define TA_TYPE_STR(literal) { .ptr = (literal), .len = sizeof(literal) - 1u }

static const ta_str ta_type_io_aliases[] = { TA_TYPE_STR("scxml") };
static const ta_type_builtin ta_type_io_items[] = {
    { .name = TA_TYPE_STR("http://www.w3.org/TR/scxml/#SCXMLEventProcessor"), .aliases = ta_type_io_aliases, .alias_count = 1 },
};
const ta_type_builtin_set ta_type_registry_io_builtins = { .items = ta_type_io_items, .count = 1 };

static const ta_str ta_type_invoker_aliases[] = { TA_TYPE_STR("scxml"), TA_TYPE_STR("http://www.w3.org/TR/scxml") };
static const ta_type_builtin ta_type_invoker_items[] = {
    { .name = TA_TYPE_STR("http://www.w3.org/TR/scxml/"), .aliases = ta_type_invoker_aliases, .alias_count = 2 },
};
const ta_type_builtin_set ta_type_registry_invoker_builtins = { .items = ta_type_invoker_items, .count = 1 };

/*
 * ta_type_hash returns the 32-bit table hash of an interned name. The atom
 * number is already unique per string, so mixing it (not the bytes) gives a
 * well-spread hash in constant time. It never allocates and has no state.
 */
static uint32_t ta_type_hash(ta_atom name) {
    return (uint32_t)ta_mix64(name);
}

/* The entry index of `name`; false when there is none. The hash may be shared by other atoms, so the entry's atom is compared. */
static bool ta_type_find(const ta_type_registry *registry, ta_atom name, uint32_t *out) {
    ta_table_probe probe;
    uint32_t index = 0;
    ta_table_probe_start(&registry->index, ta_type_hash(name), &probe);
    while (ta_table_probe_next(&probe, &index)) {
        const ta_type_entry *entry = ta_vec_at(&registry->entries, index);
        if (entry->name == name) {
            *out = index;
            return true;
        }
    }
    return false;
}

/*
 * ta_type_add adds the entry (name, canonical) for a name that is not
 * declared yet. Every fallible step comes first (the slot, the grow, which
 * may fail without harm, and the room in the page), then the push and the
 * insert, which cannot fail: a failure leaves the registry unchanged. Safe
 * points only. The entry count must stay below TA_TABLE_NONE, the value a
 * table cannot store, so the largest index is TA_TABLE_NONE - 1.
 */
static ta_result ta_type_add(ta_type_registry *registry, ta_atom name, ta_atom canonical) {
    uint32_t index = registry->entries.count;
    if (index >= TA_TABLE_NONE - 1u) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_vec_reserve(&registry->entries, index + 1u);
    if (result != TA_OK) {
        return result;
    }
    if (ta_table_wants_grow(&registry->index)) {
        (void)ta_table_grow(&registry->index); /* a failed grow only postpones */
    }
    result = ta_table_prepare(&registry->index, ta_type_hash(name));
    if (result != TA_OK) {
        return result;
    }
    ta_type_entry entry = { .name = name, .canonical = canonical };
    (void)ta_vec_push(&registry->entries, &entry);                      /* reserved above */
    (void)ta_table_insert(&registry->index, ta_type_hash(name), index); /* prepared above */
    return TA_OK;
}

/*
 * ta_type_add_text interns `text` and declares it as an alias of `canonical`,
 * or as its own canonical when that is TA_ATOM_NONE. A name that is already
 * declared is left as it was and reported through *out. Safe points only.
 */
static ta_result ta_type_add_text(ta_type_registry *registry, ta_str text, ta_atom canonical, ta_atom *out) {
    if (text.len == 0 || text.ptr == NULL) {
        return TA_ERR_INVALID;
    }
    ta_atom name;
    ta_result result = ta_atom_intern(registry->atoms, text, &name);
    if (result != TA_OK) {
        return result;
    }
    uint32_t existing;
    if (ta_type_find(registry, name, &existing)) {
        if (out != NULL) {
            *out = ((const ta_type_entry *)ta_vec_at(&registry->entries, existing))->canonical;
        }
        return TA_OK;
    }
    ta_atom target = canonical == TA_ATOM_NONE ? name : canonical;
    result = ta_type_add(registry, name, target);
    if (result == TA_OK && out != NULL) {
        *out = target;
    }
    return result;
}

ta_result ta_type_registry_init(ta_type_registry *registry, ta_allocator allocator, const ta_type_registry_params *params) {
    *registry = (ta_type_registry){ 0 };
    if (params == NULL || params->atoms == NULL || (params->builtins.items == NULL && params->builtins.count != 0)) {
        return TA_ERR_INVALID;
    }
    registry->atoms = params->atoms;
    ta_vec_init(&registry->entries, allocator, TA_VEC_LAYOUT(ta_type_entry));
    ta_result result = ta_table_init(&registry->index, allocator,
                                     &(ta_table_params){ .mode = TA_TABLE_GROWING, .page_slots = TA_TYPE_PAGE_SLOTS, .first_slots = TA_TYPE_FIRST_SLOTS, .grow_sixteenths = 6 });
    for (uint32_t i = 0; result == TA_OK && i < params->builtins.count; i += 1) {
        const ta_type_builtin *builtin = &params->builtins.items[i];
        ta_atom canonical = TA_ATOM_NONE;
        if (builtin->aliases == NULL && builtin->alias_count != 0) {
            result = TA_ERR_INVALID;
            break;
        }
        result = ta_type_add_text(registry, builtin->name, TA_ATOM_NONE, &canonical);
        for (uint32_t j = 0; result == TA_OK && j < builtin->alias_count; j += 1) {
            result = ta_type_add_text(registry, builtin->aliases[j], canonical, NULL);
        }
    }
    if (result != TA_OK) {
        ta_type_registry_destroy(registry);
    }
    return result;
}

void ta_type_registry_destroy(ta_type_registry *registry) {
    ta_vec_destroy(&registry->entries);
    ta_table_destroy(&registry->index);
    *registry = (ta_type_registry){ 0 };
}

ta_result ta_type_registry_declare(ta_type_registry *registry, ta_str name, ta_atom *out) {
    return ta_type_add_text(registry, name, TA_ATOM_NONE, out);
}

bool ta_type_registry_declared_atom(const ta_type_registry *registry, ta_atom name, ta_atom *out) {
    uint32_t index;
    if (out != NULL) {
        *out = TA_ATOM_NONE;
    }
    if (registry->atoms == NULL || !ta_atom_valid(registry->atoms, name) || !ta_type_find(registry, name, &index)) {
        return false;
    }
    if (out != NULL) {
        *out = ((const ta_type_entry *)ta_vec_at(&registry->entries, index))->canonical;
    }
    return true;
}

bool ta_type_registry_declared(const ta_type_registry *registry, ta_str name, ta_atom *out) {
    ta_atom atom;
    if (registry->atoms == NULL || name.len == 0 || ta_atom_find(registry->atoms, name, &atom) != TA_OK) {
        if (out != NULL) {
            *out = TA_ATOM_NONE;
        }
        return false;
    }
    return ta_type_registry_declared_atom(registry, atom, out);
}

uint32_t ta_type_registry_count(const ta_type_registry *registry) {
    return registry->entries.count;
}
