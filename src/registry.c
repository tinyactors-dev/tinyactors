#include "internal.h"

tai_registry_t *tai_registry_find(tai_registry_t *root, ta_text_t key)
{
    while (root != NULL) {
        int order = tai_text_compare(key, root->key);
        if (order == 0) return root;
        root = order < 0 ? root->left : root->right;
    }
    return NULL;
}

tai_registry_t *tai_registry_first(tai_registry_t *root)
{
    if (root != NULL) while (root->left != NULL) root = root->left;
    return root;
}

tai_registry_t *tai_registry_next(tai_registry_t *entry)
{
    if (entry->right != NULL) return tai_registry_first(entry->right);
    while (entry->parent != NULL && entry == entry->parent->right) entry = entry->parent;
    return entry->parent;
}

static void rotate_left(tai_registry_t **root, tai_registry_t *entry)
{
    tai_registry_t *child = entry->right;
    entry->right = child->left;
    if (child->left != NULL) child->left->parent = entry;
    child->parent = entry->parent;
    if (entry->parent == NULL) *root = child;
    else if (entry == entry->parent->left) entry->parent->left = child;
    else entry->parent->right = child;
    child->left = entry;
    entry->parent = child;
}

static void rotate_right(tai_registry_t **root, tai_registry_t *entry)
{
    tai_registry_t *child = entry->left;
    entry->left = child->right;
    if (child->right != NULL) child->right->parent = entry;
    child->parent = entry->parent;
    if (entry->parent == NULL) *root = child;
    else if (entry == entry->parent->left) entry->parent->left = child;
    else entry->parent->right = child;
    child->right = entry;
    entry->parent = child;
}

void tai_registry_insert(tai_registry_t **root, tai_registry_t *entry)
{
    tai_registry_t *at = *root, *parent = NULL;
    int order = 0;
    while (at != NULL) {
        parent = at;
        order = tai_text_compare(entry->key, at->key);
        at = order < 0 ? at->left : at->right;
    }
    entry->left = NULL;
    entry->right = NULL;
    entry->parent = parent;
    entry->priority = tai_hash(entry->key);
    if (parent == NULL) *root = entry;
    else if (order < 0) parent->left = entry;
    else parent->right = entry;
    while (entry->parent != NULL && entry->priority < entry->parent->priority) {
        if (entry == entry->parent->left) rotate_right(root, entry->parent);
        else rotate_left(root, entry->parent);
    }
}

void tai_registry_remove(tai_registry_t **root, tai_registry_t *entry)
{
    while (entry->left != NULL || entry->right != NULL) {
        if (entry->right == NULL ||
            (entry->left != NULL && entry->left->priority < entry->right->priority))
            rotate_right(root, entry);
        else rotate_left(root, entry);
    }
    if (entry->parent == NULL) *root = NULL;
    else if (entry->parent->left == entry) entry->parent->left = NULL;
    else entry->parent->right = NULL;
    entry->parent = NULL;
}

ta_status_t ta_definition_publish(ta_system_t *system,
                                 const ta_definition_binding_t *binding,
                                 ta_definition_t *definition)
{
    tai_registry_t *entry;
    ta_definition_t *old;
    ta_arena_t *arena;
    ta_status_t status;
    void *memory;
    if (system == NULL || binding == NULL || definition == NULL ||
        definition->system != system || !tai_text_valid(binding->key, 0)) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (system->shutdown) return TA_CANCELLED;
    entry = tai_registry_find(system->definition_registry, binding->key);
    if (binding->expected_revision != TA_REVISION_ANY &&
        binding->expected_revision != (entry == NULL ? 0 : entry->definition->revision))
        return TA_CONFLICT;
    if (entry == NULL) {
        status = tai_scope(system, TA_LIFETIME_DEFINITION, &arena);
        if (status != TA_OK) return status;
        status = tai_alloc(arena, sizeof(*entry), &memory);
        if (status != TA_OK) { ta_arena_close(arena); return status; }
        entry = (tai_registry_t *)memory;
        *entry = (tai_registry_t){0};
        entry->arena = arena;
        status = tai_text_copy(arena, binding->key, &entry->key);
        if (status != TA_OK) { ta_arena_close(arena); return status; }
        tai_registry_insert(&system->definition_registry, entry);
    }
    old = entry->definition;
    ta_definition_retain(definition);
    ++definition->held_by_registry;
    entry->definition = definition;
    if (old != NULL) { --old->held_by_registry; ta_definition_release(old); }
    tai_mutated(system);
    return TA_OK;
}

ta_status_t ta_definition_unpublish(ta_system_t *system,
                                   const ta_definition_binding_t *binding)
{
    tai_registry_t *entry;
    ta_definition_t *definition;
    ta_arena_t *arena;
    if (system == NULL || binding == NULL || !tai_text_valid(binding->key, 0)) return TA_INVALID;
    if (system->running) return TA_BUSY;
    entry = tai_registry_find(system->definition_registry, binding->key);
    if (entry == NULL) return TA_NOT_FOUND;
    if (binding->expected_revision != TA_REVISION_ANY &&
        binding->expected_revision != entry->definition->revision) return TA_CONFLICT;
    definition = entry->definition;
    arena = entry->arena;
    tai_registry_remove(&system->definition_registry, entry);
    --definition->held_by_registry;
    ta_definition_release(definition);
    ta_arena_close(arena);
    tai_mutated(system);
    return TA_OK;
}

ta_status_t ta_definition_lookup(ta_system_t *system, ta_text_t key,
                                ta_definition_t **out_definition)
{
    tai_registry_t *entry;
    if (system == NULL || out_definition == NULL || !tai_text_valid(key, 0)) return TA_INVALID;
    *out_definition = NULL;
    entry = tai_registry_find(system->definition_registry, key);
    if (entry == NULL) return TA_NOT_FOUND;
    ta_definition_retain(entry->definition);
    *out_definition = entry->definition;
    return TA_OK;
}
