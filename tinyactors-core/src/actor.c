#include "actor.h"
#include "interpreter.h"

ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = ta_actor_id_slot(id);
    uint32_t generation = ta_actor_id_generation(id);
    const ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    return entry != NULL && entry->generation == generation ? entry->actor : NULL;
}

ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot) {
    if (slot >= system->actors.count) {
        return NULL;
    }
    ta_actor_slot *const *pages = system->actors.pages.items;
    return &pages[slot / TA_ACTOR_PAGE_SLOTS][slot % TA_ACTOR_PAGE_SLOTS];
}

void ta_actor_table_destroy(ta_actor_table *table, ta_allocator allocator) {
    ta_actor_slot **pages = table->pages.items;
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_mem_free(allocator, pages[page],
                    &(ta_allocator_free_params){ .size = TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                                 .alignment = _Alignof(ta_actor_slot) });
    }
    ta_vec_destroy(&table->pages);
    table->count = 0;
}

void ta_actor_layout(ta_definition *definition) {
    size_t instance_alignment = definition->datamodel_instance_alignment;
    size_t alignment = _Alignof(ta_actor);
    if (alignment < _Alignof(uint64_t)) {
        alignment = _Alignof(uint64_t);
    }
    if (alignment < instance_alignment) {
        alignment = instance_alignment;
    }
    definition->actor_session_offset = ta_align_up(sizeof(ta_actor), _Alignof(uint64_t));
    definition->actor_instance_offset =
        ta_align_up(definition->actor_session_offset + ta_definition_session_bytes(definition), instance_alignment);
    definition->actor_block_bytes = definition->actor_instance_offset + definition->datamodel_instance_size;
    definition->actor_block_alignment = alignment;
}

/*
 * Issues the next slot, allocating a page when the last one is full. Pages
 * never move, so this never copies the table (ADR 0063).
 */
static ta_result ta_actor_table_grow(ta_system *system, uint32_t *out) {
    ta_actor_table *table = &system->actors;
    if (table->count == UINT32_MAX) {
        return TA_ERR_NOMEM; /* UINT32_MAX is no slot: it ends the free list and a walk */
    }
    if ((uint64_t)table->count == (uint64_t)table->pages.count * TA_ACTOR_PAGE_SLOTS) {
        ta_actor_slot *page = ta_mem_alloc(system->allocator, TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                           _Alignof(ta_actor_slot));
        if (page == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_result result = ta_vec_push(&table->pages, &page);
        if (result != TA_OK) {
            ta_mem_free(system->allocator, page,
                        &(ta_allocator_free_params){ .size = TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                                     .alignment = _Alignof(ta_actor_slot) });
            return result;
        }
    }
    *out = table->count;
    table->count += 1u;
    *ta_actor_slot_at(system, *out) = (ta_actor_slot){ .generation = 1, .next_free = UINT32_MAX };
    return TA_OK;
}

uint32_t ta_actor_id_slot(ta_actor_id id) {
    return (uint32_t)(id & 0xffffffffu);
}

uint32_t ta_actor_id_generation(ta_actor_id id) {
    return (uint32_t)(id >> 32);
}

ta_actor_id ta_actor_id_make(uint32_t slot, uint32_t generation) {
    return ((ta_actor_id)generation << 32) | slot;
}

ta_timer_list *ta_actor_timers(const ta_actor *actor) {
    return actor->sparse == NULL ? NULL : &actor->sparse->timers;
}

ta_result ta_actor_schedule_send(ta_system *system, ta_actor *actor, const ta_delayed_send *send) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_timer_schedule(&system->timers, &sparse->timers, send);
}

ta_result ta_actor_timer_home(ta_system *system, ta_actor_id owner, ta_timer_list **out) {
    ta_actor *actor = ta_actor_lookup(system, owner);
    if (actor == NULL) {
        *out = &system->timers.orphans;
        return TA_OK;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    *out = &sparse->timers;
    return TA_OK;
}

static void ta_activation_effect_release(ta_activation_effect *effect) {
    if (effect->kind == TA_ACTIVATION_SEND) {
        ta_value_release(&effect->as.send.send.target);
        ta_value_release(&effect->as.send.send.data);
        ta_actor_ref_release(&effect->as.send.send.destination);
        ta_actor_ref_release(&effect->as.send.send.origin);
    } else if (effect->kind == TA_ACTIVATION_LOG) {
        ta_value_release(&effect->as.log.value);
    }
}

static void ta_actor_discard_activation_effects(ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return;
    }
    ta_activation_effects *effects = sparse->activation_effects;
    for (uint32_t index = 0; index < effects->entries.count; index += 1u) {
        ta_activation_effect_release(ta_vec_at(&effects->entries, index));
    }
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

static ta_result ta_actor_hold_effect(ta_system *system, ta_actor *actor, const ta_activation_effect *effect) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    if (sparse->activation_effects == NULL) {
        sparse->activation_effects = ta_mem_alloc(system->allocator, sizeof(ta_activation_effects),
                                                  _Alignof(ta_activation_effects));
        if (sparse->activation_effects == NULL) {
            return TA_ERR_NOMEM;
        }
        *sparse->activation_effects = (ta_activation_effects){0};
        ta_vec_init(&sparse->activation_effects->entries, system->allocator, TA_VEC_LAYOUT(ta_activation_effect));
    }
    return ta_vec_push(&sparse->activation_effects->entries, effect);
}

ta_result ta_actor_hold_send(ta_system *system, ta_actor *actor, const ta_activation_send *send) {
    return ta_actor_hold_effect(system, actor,
                                &(ta_activation_effect){ .kind = TA_ACTIVATION_SEND, .as.send = *send });
}

ta_result ta_actor_hold_log(ta_system *system, ta_actor *actor, const ta_log_record *record) {
    ta_activation_effect effect = { .kind = TA_ACTIVATION_LOG, .as.log = *record };
    ta_value_retain(&effect.as.log.value);
    ta_result result = ta_actor_hold_effect(system, actor, &effect);
    if (result != TA_OK) {
        ta_value_release(&effect.as.log.value);
    }
    return result;
}

bool ta_actor_cancel_held_send(ta_actor *actor, ta_send_id send_id) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return false;
    }
    ta_vec *entries = &sparse->activation_effects->entries;
    bool found = false;
    for (uint32_t index = entries->count; index > 0; index -= 1u) {
        ta_activation_effect *effect = ta_vec_at(entries, index - 1u);
        if (effect->kind != TA_ACTIVATION_SEND || effect->as.send.send.send_id != send_id) {
            continue;
        }
        ta_activation_effect_release(effect);
        for (uint32_t at = index; at < entries->count; at += 1u) {
            *(ta_activation_effect *)ta_vec_at(entries, at - 1u) = *(ta_activation_effect *)ta_vec_at(entries, at);
        }
        entries->count -= 1u;
        found = true;
    }
    return found;
}

bool ta_actor_state_active(const ta_actor *actor, ta_state_id state) {
    const ta_definition *definition = actor->definition;
    if (state >= definition->state_count) {
        return false;
    }
    if (actor->live_active != NULL) {
        return ta_bits_get(actor->live_active, state);
    }
    if (ta_definition_is_atomic(definition, state)) {
        return ta_configuration_contains_atomic(&actor->configuration, state);
    }
    for (ta_state_id leaf = 0; leaf < definition->state_count; leaf += 1) {
        if (ta_configuration_contains_atomic(&actor->configuration, leaf) &&
            ta_definition_is_descendant(definition, leaf, state)) {
            return true;
        }
    }
    return false;
}

void ta_actor_exit_invoked(ta_system *system, ta_actor_id id) {
    ta_interpreter_exit_invoked(system, id);
}

/*
 * Takes the head of the free list, or grows the table when it is empty.
 * Scanning the table for a hole instead would make every spawn cost a walk
 * over the live population, which is what invariant 17 forbids: an ephemeral
 * child of a four-million-actor system must not pay for its siblings.
 */
static ta_result ta_actor_claim_slot(ta_system *system, ta_actor *actor, ta_actor_id *out) {
    uint32_t slot = system->free_actor_slot;
    if (slot == UINT32_MAX) {
        ta_result result = ta_actor_table_grow(system, &slot);
        if (result != TA_OK) {
            return result;
        }
    } else {
        system->free_actor_slot = ta_actor_slot_at(system, slot)->next_free;
    }
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = actor;
    entry->next_free = UINT32_MAX;
    *out = ta_actor_id_make(slot, entry->generation);
    return TA_OK;
}

/*
 * Tears the actor down completely, then uncounts it. The last actor of a
 * retired definition frees the definition, so nothing below may touch it.
 */
void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    ta_actor_discard_activation_effects(actor);
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    /* A named actor holds a reference to its address; a runtime ref holds nothing. */
    ta_actor_ref_release(&actor->record.self);
    ta_interpreter_release_actor_state(system, actor);
    if (actor->session_node != NULL) {
        ta_session_remove(&system->sessions, actor->session_node);
        ta_session_node_free(&system->sessions, actor->session_node);
    }
    /* The record, the session storage and the instance are one block (ADR 0063). */
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = definition->actor_block_bytes,
                                             .alignment = definition->actor_block_alignment });
    ta_definition *owner = ta_system_definition(system, definition->id);
    owner->actor_count -= 1u;
    if (owner->retired && owner->actor_count == 0) {
        ta_system_free_definition(system, owner);
    }
}

/*
 * Takes the address for the actor: copies its text into actor-owned storage
 * and claims it in the host directory. claim is a compare-and-swap against
 * "no entry", so an address another actor holds — resident or unloaded —
 * answers TA_ERR_STALE and this spawn leaves nothing behind.
 */
static ta_result ta_actor_claim_address(ta_system *system,
                                        ta_actor *actor,
                                        const ta_actor_address *address) {
    if (system->directory.v == NULL || system->directory.v->claim == NULL) {
        /* A logical address is only meaningful against a host directory. */
        return TA_ERR_UNSUPPORTED;
    }
    if (address->family.name.ptr == NULL || address->family.name.len == 0 ||
        address->name.ptr == NULL || address->name.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor_ref self = { .kind = TA_ACTOR_REF_NAMED };
    ta_result result = ta_address_intern(system, address, &self.as.named);
    if (result != TA_OK) {
        return result;
    }
    result = system->directory.v->claim(
        system->directory.self,
        &(ta_actor_directory_claim_params){
            .address = self.as.named,
            .entry = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
    if (result != TA_OK) {
        ta_actor_ref_release(&self);
        return result;
    }
    /* From here the actor answers to its address: every origin core writes
     * for it is the address, so a reply outlives this runtime ID. */
    actor->record.self = self;
    return TA_OK;
}

/*
 * Frees the slot an actor was given, so a failed spawn reuses it. The slot's
 * next occupant gets a new generation, so this ID goes stale. Generation 0
 * is never issued (ADR 0059): a wrap from UINT32_MAX goes to 1.
 */
void ta_actor_release_slot(ta_system *system, ta_actor_id id) {
    uint32_t slot = ta_actor_id_slot(id);
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = NULL;
    entry->generation = entry->generation == UINT32_MAX ? 1u : entry->generation + 1u;
    entry->next_free = system->free_actor_slot;
    system->free_actor_slot = slot;
}

/*
 * Gives up the route of a named actor that is being destroyed. remove
 * compares against this actor's own entry, so an incumbent whose address has
 * already been taken over by an activated replacement removes nothing: the
 * successor keeps the route. A failed comparison is that case, not an error.
 */
static void ta_actor_release_address(ta_system *system, const ta_actor *actor) {
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED || !actor->published) {
        return;
    }
    if (system->directory.v == NULL || system->directory.v->remove == NULL) {
        return;
    }
    if (system->directory.v->resolve != NULL) {
        ta_actor_directory_entry current = {0};
        ta_result result = system->directory.v->resolve(
            system->directory.self,
            &(ta_actor_directory_resolve_params){ .address = actor->record.self.as.named, .out = &current });
        if (result == TA_OK &&
            (current.kind != TA_ACTOR_DIRECTORY_RESIDENT || current.actor != actor->record.id)) {
            return;
        }
    }
    (void)system->directory.v->remove(
        system->directory.self,
        &(ta_actor_directory_remove_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
}

/*
 * The host directory's compare-and-swap. Every residency change is one of
 * these: the caller states the entry it believes is there, and a directory
 * that disagrees answers TA_ERR_STALE and changes nothing. That is what
 * keeps a residency change retryable — a refused swap has done nothing, so
 * the caller may simply try again (ADR 0014).
 */
ta_result ta_actor_directory_swap(ta_system *system, const ta_actor_directory_replace_params *params) {
    if (system->directory.v == NULL || system->directory.v->replace == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return system->directory.v->replace(system->directory.self, params);
}

/* The same comparison, for an entry that should end up gone. */
ta_result ta_actor_directory_drop(ta_system *system, const ta_actor_directory_remove_params *params) {
    if (system->directory.v == NULL || system->directory.v->remove == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return system->directory.v->remove(system->directory.self, params);
}

/*
 * Releases the name of an actor that is not resident. The comparison is
 * against UNLOADED, so this cannot remove a name some live actor answers to:
 * a resident entry, or none at all, is TA_ERR_STALE. Core owns nothing of
 * the actor at this point — the host deletes its checkpoint afterwards.
 */
ta_result ta_actor_remove_unloaded(ta_system *system, const ta_actor_remove_unloaded_params *params) {
    if (system == NULL || params == NULL || params->address == NULL) {
        return TA_ERR_INVALID;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system, params->address, &address);
    if (result != TA_OK) {
        return result;
    }
    result = ta_actor_directory_drop(
        system,
        &(ta_actor_directory_remove_params){
            .address = address,
            .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    /* The unloaded session ends with its name (ADR 0066). */
    if (result == TA_OK && address->unloaded != NULL) {
        ta_session_node *parked = address->unloaded;
        ta_session_remove(&system->sessions, parked);
        ta_session_node_free(&system->sessions, parked);
    }
    ta_address_release(address);
    return result;
}

/*
 * Resolves a logical address through the host directory. An unloaded actor
 * exists but has no runtime ID, which is TA_ERR_UNLOADED and leaves *out
 * alone; only a resident entry writes one.
 */
ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params) {
    if (system == NULL || params == NULL || params->address == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->directory.v == NULL || system->directory.v->resolve == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system, params->address, &address);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_directory_entry entry = { .kind = TA_ACTOR_DIRECTORY_NONE };
    result = system->directory.v->resolve(
        system->directory.self,
        &(ta_actor_directory_resolve_params){ .address = address, .out = &entry });
    ta_address_release(address);
    if (result != TA_OK) {
        return result;
    }
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        *params->out = entry.actor;
        return TA_OK;
    }
    return entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TA_ERR_UNLOADED : TA_ERR_NOT_FOUND;
}

/*
 * A blank actor of `definition`: session storage zeroed, a data-model
 * instance allocated but not initialized, a slot claimed, and a record that
 * answers to its runtime ID. Spawn bootstraps it; reconstruction fills it
 * from an image instead, so both start here.
 */
ta_result ta_actor_new(ta_system *system, ta_definition *definition, ta_actor **out) {
    /* One allocation: the record, the session storage, the instance (ADR 0063). */
    ta_actor *actor = ta_mem_alloc(system->allocator, definition->actor_block_bytes, definition->actor_block_alignment);
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    unsigned char *block = (unsigned char *)actor;
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    /* Counted from here so every ta_actor_free() path can uncount it. */
    definition->actor_count += 1u;
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    actor->session_bytes = ta_definition_session_bytes(definition);
    actor->session_storage = block + definition->actor_session_offset;
    ta_mem_zero(actor->session_storage, actor->session_bytes);
    unsigned char *storage = actor->session_storage;
    actor->configuration = (ta_configuration){ .storage = storage, .layout = &definition->configuration };
    actor->late_initialized = (uint64_t *)(storage + config_bytes);
    actor->history = (ta_history_state){ .storage = storage + config_bytes * 2u, .layout = &definition->history };

    /* An instance of size 0 (the null data model) points at the block's end. */
    actor->datamodel_instance = block + definition->actor_instance_offset;
    ta_mem_zero(actor->datamodel_instance, definition->datamodel_instance_size);

    ta_actor_id id = 0;
    ta_result result = ta_actor_claim_slot(system, actor, &id);
    if (result != TA_OK) {
        ta_actor_free(system, actor);
        return result;
    }
    actor->record = (ta_actor_record){
        .id = id,
        .definition = definition->id,
        .self = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = id },
        .residency = TA_ACTOR_RUNNING,
    };
    /* A new session is named by the runtime ID it starts with (ADR 0066). */
    actor->session_id = id;
    *out = actor;
    return TA_OK;
}

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor *actor = NULL;
    ta_result result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;

    if (params->parent != NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return TA_ERR_NOMEM;
        }
        *actor->sparse = (ta_actor_sparse_state){ .parent = *params->parent, .has_parent = true };
    }

    result = definition->datamodel.v->init_instance(
        definition->datamodel.self,
        &(ta_datamodel_init_instance_params){
            .shared = definition->datamodel_shared,
            .instance = actor->datamodel_instance,
            .binding = params->datamodel_binding,
            .host = &actor->host,
        });
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    actor->datamodel_initialized = true;
    /* The claim is the last fallible step: bootstrap has not run yet, so a
     * refused address leaves no trace of this spawn anywhere. */
    if (params->address != NULL) {
        result = ta_actor_claim_address(system, actor, params->address);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
    }
    if (params->parent != NULL) {
        /* Invoke: the invoking parent routes #_<invokeid> and autoforwarding to this child. */
        ta_invoke_attach_child(system, params->parent, id);
    }
    *params->out = id;
    return TA_OK;
}

ta_result ta_actor_prepare_replacement(ta_system *system, const ta_actor_prepare_replacement_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *incumbent = ta_actor_lookup(system, params->incumbent);
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (incumbent == NULL || definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    if (!incumbent->published || incumbent->record.self.kind != TA_ACTOR_REF_NAMED ||
        !ta_str_equal(ta_address_view(incumbent->record.self.as.named).family.name,
                      definition->identity.family.name)) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = NULL;
    ta_result result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;
    actor->published = false;
    actor->replacement_of = incumbent->record.id;
    actor->record.self = incumbent->record.self;
    ta_actor_ref_retain(&actor->record.self);
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = params->datamodel_binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    *params->out = id;
    return TA_OK;
}

static uint32_t ta_actor_activation_send_count(const ta_actor *actor) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return 0;
    }
    uint32_t count = 0;
    const ta_vec *entries = &sparse->activation_effects->entries;
    for (uint32_t index = 0; index < entries->count; index += 1u) {
        const ta_activation_effect *effect = ta_vec_at(entries, index);
        count += effect->kind == TA_ACTIVATION_SEND ? 1u : 0u;
    }
    return count;
}

static void ta_actor_release_activation_effects(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return;
    }
    ta_activation_effects *effects = sparse->activation_effects;
    for (uint32_t index = 0; index < effects->entries.count; index += 1u) {
        ta_activation_effect *effect = ta_vec_at(&effects->entries, index);
        if (effect->kind == TA_ACTIVATION_LOG) {
            if (system->log_sink.v != NULL) {
                system->log_sink.v->write(system->log_sink.self, &effect->as.log);
            }
            ta_value_release(&effect->as.log.value);
            continue;
        }
        ta_delayed_send *send = &effect->as.send.send;
        if (effect->as.send.immediate) {
            ta_external_event_submit_status status = ta_interpreter_dispatch_delayed(system, send);
            if (status != TA_EXTERNAL_EVENT_RETRY) {
                continue;
            }
            /* Dispatch marked it submitted and took its origin and invoke ID. */
        }
        /* Reserved by ta_actor_activate_replacement(), so this cannot fail. */
        (void)ta_timer_schedule(&system->timers, &sparse->timers, send);
    }
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

ta_result ta_actor_activate_replacement(ta_system *system, const ta_actor_activate_replacement_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *incumbent = ta_actor_lookup(system, params->incumbent);
    ta_actor *replacement = ta_actor_lookup(system, params->replacement);
    if (incumbent == NULL || replacement == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!incumbent->published || replacement->published || replacement->replacement_of != incumbent->record.id ||
        incumbent->record.self.kind != TA_ACTOR_REF_NAMED || replacement->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    if (replacement->macrostep || replacement->record.residency != TA_ACTOR_IDLE) {
        return TA_ERR_BUSY;
    }
    uint32_t sends = ta_actor_activation_send_count(replacement);
    ta_result result = sends == 0 ? TA_OK : ta_timer_reserve(&system->timers, &replacement->sparse->timers, sends);
    if (result != TA_OK) {
        return result;
    }
    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = replacement->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = incumbent->record.id },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = replacement->record.id },
        });
    if (result != TA_OK) {
        return result;
    }
    replacement->published = true;
    replacement->replacement_of = 0;
    ta_actor_touch(replacement);
    ta_actor_release_activation_effects(system, replacement);
    if (replacement->sparse != NULL && replacement->sparse->pending_invokes.count != 0) {
        replacement->macrostep = true;
        replacement->record.residency = TA_ACTOR_RUNNING;
    }
    return TA_OK;
}

/*
 * Checks an I/O origin against the registry, and gives an event from one of
 * the actor's invocations an invoke origin when it has none (ADR 0068).
 * That covers hosts that post invoker events through their own mailbox.
 */
static ta_result ta_actor_accept_origin(const ta_system *system, const ta_actor *actor, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        const ta_io_origin *io = &event->origin.as.io;
        if (ta_system_io_at(system, io->processor) == NULL ||
            (io->origin_class == TA_IO_ORIGIN_MESSAGE && io->as.entry == NULL) ||
            (uint32_t)io->origin_class > (uint32_t)TA_IO_ORIGIN_MESSAGE) {
            return TA_ERR_INVALID;
        }
        return TA_OK;
    }
    if (event->origin.kind != TA_ORIGIN_NONE || event->invoke_id == TA_INVOKE_ID_NONE) {
        return TA_OK;
    }
    (void)ta_invoke_origin_for(system, actor, event->invoke_id, &event->origin);
    return TA_OK;
}

ta_result ta_actor_deliver(ta_system *system, ta_actor_id id, const ta_event *event) {
    if (event == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!ta_event_name_valid(&system->event_names, event->name) || event->name == TA_EVENT_NAME_ROOT) {
        return TA_ERR_INVALID;
    }
    /* Before busy: a finished actor never becomes free again (ADR 0055). */
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || actor->slots.external != TA_EXTERNAL_NONE) {
        return TA_ERR_BUSY;
    }
    ta_event accepted = *event;
    ta_result result = ta_actor_accept_origin(system, actor, &accepted);
    if (result != TA_OK) {
        return result;
    }
    ta_event_retain(&accepted, &actor->slots.external_event);
    actor->slots.external = TA_EXTERNAL_WAITING;
    actor->macrostep = true;
    ta_actor_touch(actor);
    actor->record.residency = TA_ACTOR_RUNNING;
    return TA_OK;
}

ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params) {
    if (params == NULL || params->status == NULL || params->budget == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_interpreter_resume(system, actor, params);
}

ta_result ta_actor_destroy(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL && sparse->completion_pending && sparse->has_parent) {
        ta_actor_ref parent = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = sparse->parent.actor };
        if (ta_invoke_parent_accepts(system, actor, &parent)) {
            return TA_ERR_BUSY;
        }
    }
    /* Sends already submitted stay queued for their receivers (ADR 0054). */
    ta_result result = ta_timer_reserve_orphans(&system->timers, ta_actor_timers(actor));
    if (result != TA_OK) {
        return result;
    }
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_actor_release_address(system, actor);
    ta_actor_release_slot(system, id);
    ta_actor_free(system, actor);
    return TA_OK;
}

static const ta_event *ta_actor_paused_event(const ta_actor *actor) {
    if (!actor->macrostep) {
        return NULL;
    }
    return actor->slots.external == TA_EXTERNAL_NONE ? NULL : &actor->slots.external_event;
}

const ta_event *ta_actor_bound_event(const ta_actor *actor) {
    switch ((ta_event_binding)actor->slots.bound) {
    case TA_BOUND_EXTERNAL:
        return &actor->slots.external_event;
    case TA_BOUND_INTERNAL:
        return &actor->internal_queue->events[0].event;
    case TA_BOUND_KEPT:
        return &actor->slots.kept_event;
    case TA_BOUND_NONE:
    default:
        return NULL;
    }
}

uint32_t ta_actor_waiting_internal(const ta_actor *actor) {
    if (actor->internal_queue == NULL) {
        return 0;
    }
    return actor->internal_queue->count - (actor->slots.bound == TA_BOUND_INTERNAL ? 1u : 0u);
}

static size_t ta_vec_bytes(const ta_vec *vec) {
    return (size_t)vec->capacity * vec->element_size;
}

/* Everything the core allocates for the actor now; see ta_actor_inspection. */
static size_t ta_actor_allocated_bytes(const ta_actor *actor) {
    size_t total = actor->definition->actor_block_bytes;
    if (actor->internal_queue != NULL) {
        total += ta_internal_queue_bytes(actor->internal_queue->capacity);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL) {
        total += sizeof(ta_actor_sparse_state);
        total += ta_vec_bytes(&sparse->pending_invokes);
        total += (size_t)sparse->invoke_capacity * (sizeof(ta_active_invoke) + sizeof(ta_invoke_meta));
        if (sparse->activation_effects != NULL) {
            total += sizeof(ta_activation_effects);
            total += ta_vec_bytes(&sparse->activation_effects->entries);
        }
        total += ta_timer_list_bytes(&sparse->timers);
        if (sparse->paused != NULL) {
            total += sparse->paused->bytes;
        }
    }
    return total;
}

ta_result ta_actor_inspect(ta_system *system, ta_actor_id id, ta_actor_inspection *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    uint32_t active = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        active += ta_actor_state_active(actor, state) ? 1u : 0u;
    }
    *out = (ta_actor_inspection){
        .actor = id,
        .session_id = actor->session_id,
        .self = actor->record.self,
        .definition_id = definition->id,
        .definition = definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .active_state_count = active,
        .macrostep_in_progress = actor->macrostep,
        .microstep_in_progress = actor->sparse != NULL && actor->sparse->paused != NULL,
        .current_event = ta_actor_paused_event(actor),
        .allocated_bytes = ta_actor_allocated_bytes(actor),
        .final_state = TA_STATE_NONE,
    };
    /* A taken head is `_event`, not work that waits. */
    uint32_t waiting = ta_actor_waiting_internal(actor);
    if (waiting != 0) {
        out->internal_events = actor->internal_queue->events + (actor->internal_queue->count - waiting);
        out->internal_count = waiting;
    }
    if (actor->sparse != NULL) {
        out->invokes = actor->sparse->invokes;
        out->invoke_count = actor->sparse->invoke_count;
        out->delayed_sends = actor->sparse->timers.count == 0 ? NULL : actor->sparse->timers.sends;
        out->delayed_send_count = actor->sparse->timers.count;
        out->has_completion = actor->finished;
        out->completion = actor->finished ? &actor->sparse->completion : NULL;
        out->final_state = actor->finished ? actor->sparse->final_state : TA_STATE_NONE;
    }
    return TA_OK;
}

ta_result ta_actor_datamodel_root(ta_system *system, ta_actor_id id, ta_value *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    *out = (ta_value){0};
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->inspect == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_result result = v->inspect(definition->datamodel.self,
                                  &(ta_datamodel_inspect_params){
                                      .shared = definition->datamodel_shared,
                                      .instance = actor->datamodel_instance,
                                      .host = &actor->host,
                                      .out = out,
                                  });
    if (result != TA_OK) {
        *out = (ta_value){0};
    }
    return result;
}

ta_result ta_system_visit_actors(ta_system *system, const ta_actor_visit_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    /* A walk of everything may leave `next` null. */
    uint32_t unused = 0;
    uint32_t *next = params->next != NULL ? params->next : &unused;
    *next = params->from;
    if (system == NULL || params->inspector.visit == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t visited = 0;
    uint32_t slot = params->from;
    for (; slot < system->actors.count; slot += 1u) {
        if (params->limit != 0 && visited == params->limit) {
            break;
        }
        /* Read the entry each time: a visit may spawn and grow the table. */
        const ta_actor_slot *entry = ta_actor_slot_at(system, slot);
        if (entry->actor == NULL) {
            continue;
        }
        ta_actor_inspection inspection;
        ta_result result = ta_actor_inspect(system, entry->actor->record.id, &inspection);
        if (result == TA_OK) {
            result = params->inspector.visit(params->inspector.self, &inspection);
        }
        if (result != TA_OK) {
            *next = slot; /* not taken: the next call offers it again */
            return result;
        }
        visited += 1u;
    }
    *next = slot >= system->actors.count ? TA_ACTOR_VISIT_END : slot;
    return TA_OK;
}

ta_result ta_actor_active_states(ta_system *system, const ta_actor_active_states_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += ta_actor_state_active(actor, state) ? 1u : 0u;
    }
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->states == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_actor_state_active(actor, state)) {
            params->states[at++] = state;
        }
    }
    return TA_OK;
}

ta_result ta_actor_history(ta_system *system, const ta_actor_history_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    if (params->history >= definition->state_count ||
        definition->states[params->history].history_slot == UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    const uint64_t *words = ta_history_slot_words(&actor->history, definition->states[params->history].history_slot);
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += ta_bits_get(words, state) ? 1u : 0u;
    }
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->states == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(words, state)) {
            params->states[at++] = state;
        }
    }
    return TA_OK;
}

ta_result ta_actor_get_completion(ta_system *system, ta_actor_id id, ta_actor_completion *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!actor->finished) {
        return TA_ERR_BUSY;
    }
    *out = (ta_actor_completion){ .has_data = false };
    if (actor->sparse == NULL || !actor->sparse->completion.has_data) {
        return TA_OK;
    }
    out->data = actor->sparse->completion.data;
    ta_value_retain(&out->data);
    out->has_data = true;
    return TA_OK;
}
