#include "type_registry.h"
#include "atom.h"
#include "hash.h"
#include "str.h"
#include "table.h"
#include "vec.h"
#include "../include/tinyactors.h"

/* A literal as a ta_str, for the built-in sets. */
#define TA_TYPE_REGISTRY_TEXT(literal) { .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) }

static const ta_type_registry_builtin ta_type_registry_io_names[] = {
    { .name = TA_TYPE_REGISTRY_TEXT("http://www.w3.org/TR/scxml/#SCXMLEventProcessor"), .canonical = 0 },
    { .name = TA_TYPE_REGISTRY_TEXT("scxml"), .canonical = 1 },
};

static const ta_type_registry_builtin ta_type_registry_invoker_names[] = {
    { .name = TA_TYPE_REGISTRY_TEXT("http://www.w3.org/TR/scxml/"), .canonical = 0 },
    { .name = TA_TYPE_REGISTRY_TEXT("scxml"), .canonical = 1 },
    { .name = TA_TYPE_REGISTRY_TEXT("http://www.w3.org/TR/scxml"), .canonical = 1 },
};

const ta_type_registry_builtins TA_TYPE_REGISTRY_IO_BUILTINS = {
    .names = ta_type_registry_io_names,
    .count = (uint32_t)(sizeof(ta_type_registry_io_names) / sizeof(ta_type_registry_io_names[0])),
};

const ta_type_registry_builtins TA_TYPE_REGISTRY_INVOKER_BUILTINS = {
    .names = ta_type_registry_invoker_names,
    .count = (uint32_t)(sizeof(ta_type_registry_invoker_names) / sizeof(ta_type_registry_invoker_names[0])),
};

/* What the hash table's comparison needs: the entries and the atom looked for. */
typedef struct {
    const ta_type_registry *registry;
    ta_atom name;
} ta_type_registry_key;

/* The hash of an atom: its value mixed, so every bit reaches the table's mask. */
static uint32_t ta_type_registry_hash(ta_atom name) {
    return (uint32_t)ta_hash_mix64((uint64_t)name + 1u);
}

/* Whether entry `value - 1` is the name in `context`, a ta_type_registry_key. */
static bool ta_type_registry_key_equal(const void *context, uint32_t value) {
    const ta_type_registry_key *key = context;
    const ta_type_registry_entry *entry = ta_vec_at(&key->registry->entries, value - 1u);
    return entry->name == key->name;
}

/* The position of `name`'s entry, or TA_ERR_NOT_FOUND. */
static ta_result ta_type_registry_find(const ta_type_registry *registry, ta_atom name, uint32_t *position) {
    ta_type_registry_key key = { .registry = registry, .name = name };
    uint32_t value = 0;
    ta_result result = ta_table_find(
        &registry->index,
        &(ta_table_key){ .hash = ta_type_registry_hash(name), .equal = ta_type_registry_key_equal, .context = &key },
        &value);
    if (result == TA_OK) {
        *position = value - 1u;
    }
    return result;
}

/* Whether a name may be an entry's: not empty, and readable. */
static bool ta_type_registry_name_valid(ta_str name) {
    return name.len != 0 && name.ptr != NULL;
}

/* Whether the built-in set is well formed; see ta_type_registry_init. */
static bool ta_type_registry_builtins_valid(const ta_type_registry_builtins *builtins) {
    if (builtins->count != 0 && builtins->names == NULL) {
        return false;
    }
    for (uint32_t at = 0; at < builtins->count; at += 1u) {
        const ta_type_registry_builtin *builtin = &builtins->names[at];
        if (!ta_type_registry_name_valid(builtin->name)) {
            return false;
        }
        if (builtin->canonical != 0 &&
            (builtin->canonical > at || builtins->names[builtin->canonical - 1u].canonical != 0)) {
            return false;
        }
        for (uint32_t earlier = 0; earlier < at; earlier += 1u) {
            if (ta_str_equal(builtins->names[earlier].name, builtin->name)) {
                return false;
            }
        }
    }
    return true;
}

/* Adds an entry; the entries and the hash table have room for it. */
static void ta_type_registry_add(ta_type_registry *registry, ta_type_registry_entry entry) {
    uint32_t position = registry->entries.count;
    (void)ta_vec_push(&registry->entries, &entry);
    (void)ta_table_insert(&registry->index, ta_type_registry_hash(entry.name), position + 1u);
}

ta_result ta_type_registry_init(ta_type_registry *registry, const ta_type_registry_params *params) {
    *registry = (ta_type_registry){ .atoms = params->atoms };
    ta_vec_init(&registry->entries, params->allocator, TA_VEC_LAYOUT(ta_type_registry_entry));
    const ta_type_registry_builtins *builtins = params->builtins;
    ta_result result = ta_table_init(&registry->index,
                                     &(ta_table_params){ .allocator = params->allocator,
                                                         .mode = TA_TABLE_GROWING,
                                                         .capacity = builtins->count });
    if (result == TA_OK && !ta_type_registry_builtins_valid(builtins)) {
        result = TA_ERR_INVALID;
    }
    if (result == TA_OK) {
        result = ta_vec_reserve(&registry->entries, builtins->count);
    }
    for (uint32_t at = 0; result == TA_OK && at < builtins->count; at += 1u) {
        const ta_type_registry_builtin *builtin = &builtins->names[at];
        ta_atom name = TA_ATOM_NONE;
        result = ta_atom_intern(registry->atoms, builtin->name, &name);
        if (result == TA_OK) {
            ta_atom canonical = name;
            if (builtin->canonical != 0) {
                canonical = ((const ta_type_registry_entry *)ta_vec_at(&registry->entries, builtin->canonical - 1u))->name;
            }
            ta_type_registry_add(registry, (ta_type_registry_entry){ .name = name, .canonical = canonical });
        }
    }
    if (result != TA_OK) {
        ta_type_registry_destroy(registry);
        return result;
    }
    registry->builtin_count = builtins->count;
    return TA_OK;
}

void ta_type_registry_destroy(ta_type_registry *registry) {
    ta_vec_destroy(&registry->entries);
    ta_table_destroy(&registry->index);
    registry->builtin_count = 0;
}

ta_result ta_type_registry_declare(ta_type_registry *registry, ta_str name) {
    if (!ta_type_registry_name_valid(name)) {
        return TA_ERR_INVALID;
    }
    ta_atom atom = TA_ATOM_NONE;
    uint32_t position = 0;
    if (ta_atom_find(registry->atoms, name, &atom) == TA_OK &&
        ta_type_registry_find(registry, atom, &position) == TA_OK) {
        return position < registry->builtin_count ? TA_ERR_UNSUPPORTED : TA_OK;
    }
    /* Room first, so that nothing after the name's atom can fail. */
    ta_result result = ta_table_reserve(&registry->index, 1u);
    if (result == TA_OK) {
        result = ta_vec_reserve(&registry->entries, registry->entries.count + 1u);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(registry->atoms, name, &atom);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_type_registry_add(registry, (ta_type_registry_entry){ .name = atom, .canonical = atom });
    return TA_OK;
}

ta_result ta_type_registry_declared(const ta_type_registry *registry, ta_atom type, ta_atom *canonical) {
    uint32_t position = 0;
    ta_result result = ta_type_registry_find(registry, type, &position);
    if (result == TA_OK && canonical != NULL) {
        *canonical = ((const ta_type_registry_entry *)ta_vec_at(&registry->entries, position))->canonical;
    }
    return result;
}
