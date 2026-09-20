#include "actor.h"
#include "interpreter.h"

ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t generation = (uint32_t)(id >> 32);
    if (slot >= system->actors.count) {
        return NULL;
    }
    const ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
    return entry->generation == generation ? entry->actor : NULL;
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

static ta_result ta_actor_claim_slot(ta_system *system, ta_actor *actor, ta_actor_id *out) {
    uint32_t slot = system->actors.count;
    for (uint32_t index = 0; index < system->actors.count; index += 1) {
        const ta_actor_slot *entry = ta_vec_at(&system->actors, index);
        if (entry->actor == NULL) {
            slot = index;
            break;
        }
    }
    if (slot == system->actors.count) {
        ta_actor_slot fresh = { .generation = 1 };
        ta_result result = ta_vec_push(&system->actors, &fresh);
        if (result != TA_OK) {
            return result;
        }
    }
    ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
    entry->actor = actor;
    *out = ((ta_actor_id)entry->generation << 32) | slot;
    return TA_OK;
}

/*
 * Tears the actor down completely, then uncounts it. The last actor of a
 * retired definition frees the definition, so nothing below may touch it.
 */
static void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    if (actor->datamodel_instance != NULL) {
        ta_mem_free(system->allocator, actor->datamodel_instance,
                    &(ta_allocator_free_params){ .size = ta_definition_instance_bytes(definition),
                                                 .alignment = definition->datamodel_instance_alignment });
    }
    ta_mem_free(system->allocator, actor->session_storage,
                &(ta_allocator_free_params){ .size = actor->session_bytes, .alignment = _Alignof(uint64_t) });
    /* A named actor owns the text of its address; a runtime ref owns nothing. */
    ta_address_release(&actor->record.self, system->allocator);
    ta_interpreter_release_actor_state(system, actor);
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor), .alignment = _Alignof(ta_actor) });
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
    ta_actor_ref self = { .kind = TA_ACTOR_REF_NAMED, .as.named = *address };
    ta_result result = ta_address_copy(&self, system->allocator);
    if (result != TA_OK) {
        return result;
    }
    result = system->directory.v->claim(
        system->directory.self,
        &(ta_actor_directory_claim_params){
            .address = &self.as.named,
            .entry = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
    if (result != TA_OK) {
        ta_address_release(&self, system->allocator);
        return result;
    }
    /* From here the actor answers to its address: every origin core writes
     * for it is the address, so a reply outlives this runtime ID. */
    actor->record.self = self;
    return TA_OK;
}

/* Frees the slot an actor was given, so a failed spawn reuses it. */
static void ta_actor_release_slot(ta_system *system, ta_actor_id id) {
    ta_actor_slot *entry = ta_vec_at(&system->actors, (uint32_t)(id & 0xffffffffu));
    entry->actor = NULL;
    entry->generation += 1u;
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
    (void)system->directory.v->remove(
        system->directory.self,
        &(ta_actor_directory_remove_params){
            .address = &actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
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
    ta_actor_directory_entry entry = { .kind = TA_ACTOR_DIRECTORY_NONE };
    ta_result result = system->directory.v->resolve(
        system->directory.self,
        &(ta_actor_directory_resolve_params){ .address = params->address, .out = &entry });
    if (result != TA_OK) {
        return result;
    }
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        *params->out = entry.actor;
        return TA_OK;
    }
    return entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TA_ERR_UNLOADED : TA_ERR_NOT_FOUND;
}

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor *actor = ta_mem_alloc(system->allocator, sizeof(ta_actor), _Alignof(ta_actor));
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    /* Counted from here so every ta_actor_free() path can uncount it. */
    definition->actor_count += 1u;
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    actor->session_bytes = ta_definition_session_bytes(definition);
    actor->session_storage = ta_mem_alloc(system->allocator, actor->session_bytes, _Alignof(uint64_t));
    if (actor->session_storage == NULL) {
        ta_actor_free(system, actor);
        return TA_ERR_NOMEM;
    }
    ta_mem_zero(actor->session_storage, actor->session_bytes);
    unsigned char *storage = actor->session_storage;
    actor->configuration = (ta_configuration){ .storage = storage, .layout = &definition->configuration };
    actor->late_initialized = (uint64_t *)(storage + config_bytes);
    actor->history = (ta_history_state){ .storage = storage + config_bytes * 2u, .layout = &definition->history };

    uint32_t instance_size = definition->datamodel_instance_size;
    actor->datamodel_instance = ta_mem_alloc(system->allocator, ta_definition_instance_bytes(definition),
                                             definition->datamodel_instance_alignment);
    if (actor->datamodel_instance == NULL) {
        ta_actor_free(system, actor);
        return TA_ERR_NOMEM;
    }
    ta_mem_zero(actor->datamodel_instance, instance_size);

    if (params->parent != NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            ta_actor_free(system, actor);
            return TA_ERR_NOMEM;
        }
        *actor->sparse = (ta_actor_sparse_state){ .parent = *params->parent, .has_parent = true };
        ta_vec_init(&actor->sparse->internal_events, system->allocator, TA_VEC_LAYOUT(ta_queued_event));
    }

    ta_actor_id id = 0;
    ta_result result = ta_actor_claim_slot(system, actor, &id);
    if (result != TA_OK) {
        ta_actor_free(system, actor);
        return result;
    }
    actor->record = (ta_actor_record){
        .id = id,
        .definition = params->definition,
        .self = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = id },
        .residency = TA_ACTOR_RUNNING,
    };
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
    if (actor->finished) {
        return TA_ERR_INVALID;
    }
    if (actor->macrostep || actor->has_external_event) {
        return TA_ERR_BUSY;
    }
    ta_event copy;
    ta_event_retain(event, &copy);
    actor->external_event = copy;
    actor->has_external_event = true;
    actor->macrostep = true;
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
    /* Sends already submitted stay queued for their receivers. */
    ta_timer_cancel_unsubmitted(&system->timers, id);
    ta_actor_release_address(system, actor);
    ta_actor_release_slot(system, id);
    ta_actor_free(system, actor);
    return TA_OK;
}

static const ta_event *ta_actor_paused_event(const ta_actor *actor) {
    if (!actor->macrostep) {
        return NULL;
    }
    if (actor->has_external_event) {
        return &actor->external_event;
    }
    if (actor->has_current_event && actor->current_event.type == TA_EVENT_EXTERNAL) {
        return &actor->current_event;
    }
    return NULL;
}

static size_t ta_vec_bytes(const ta_vec *vec) {
    return (size_t)vec->capacity * vec->element_size;
}

/* Everything the core allocates for the actor now; see ta_actor_inspection. */
static size_t ta_actor_allocated_bytes(const ta_actor *actor) {
    size_t total = sizeof(ta_actor) + actor->session_bytes;
    if (actor->datamodel_instance != NULL) {
        total += ta_definition_instance_bytes(actor->definition);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL) {
        total += sizeof(ta_actor_sparse_state);
        total += ta_vec_bytes(&sparse->internal_events);
        total += ta_vec_bytes(&sparse->pending_invokes);
        total += (size_t)sparse->invoke_capacity * (sizeof(ta_active_invoke) + sizeof(ta_invoke_meta));
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
        .self = actor->record.self,
        .definition_id = definition->id,
        .definition = definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .active_state_count = active,
        .macrostep_in_progress = actor->macrostep,
        .current_event = ta_actor_paused_event(actor),
        .delayed_send_count = ta_timer_count_owner(&system->timers, id),
        .allocated_bytes = ta_actor_allocated_bytes(actor),
    };
    if (actor->sparse != NULL) {
        out->internal_events = actor->sparse->internal_events.items;
        out->internal_count = actor->sparse->internal_events.count;
        out->invokes = actor->sparse->invokes;
        out->invoke_count = actor->sparse->invoke_count;
        out->has_completion = actor->finished;
        out->completion = actor->finished ? &actor->sparse->completion : NULL;
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

ta_result ta_system_visit_actors(ta_system *system, ta_actor_inspector inspector) {
    if (system == NULL || inspector.visit == NULL) {
        return TA_ERR_INVALID;
    }
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        const ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
        if (entry->actor == NULL) {
            continue;
        }
        ta_actor_inspection inspection;
        ta_result result = ta_actor_inspect(system, entry->actor->record.id, &inspection);
        if (result == TA_OK) {
            result = inspector.visit(inspector.self, &inspection);
        }
        if (result != TA_OK) {
            return result;
        }
    }
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

ta_result ta_actor_delayed_sends(ta_system *system, const ta_actor_delayed_sends_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    if (ta_actor_lookup(system, params->actor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t count = ta_timer_count_owner(&system->timers, params->actor);
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->sends == NULL)) {
        return TA_ERR_NOMEM;
    }
    /* Selection by (due, id) keeps the documented order without sorting the heap. */
    ta_time last_due = 0;
    ta_timer_id last_id = 0;
    for (uint32_t at = 0; at < count; at += 1) {
        const ta_delayed_send *best = NULL;
        for (uint32_t index = 0; index < system->timers.count; index += 1) {
            const ta_delayed_send *send = &system->timers.items[index];
            bool after = at == 0 || send->due > last_due || (send->due == last_due && send->id > last_id);
            bool better = best == NULL || send->due < best->due || (send->due == best->due && send->id < best->id);
            if (send->owner == params->actor && after && better) {
                best = send;
            }
        }
        last_due = best->due;
        last_id = best->id;
        params->sends[at] = (ta_delayed_send_info){
            .send_id = best->send_id,
            .due = best->due,
            .event = best->event,
            .io_type = best->io_type,
            .target = best->target,
            .data = best->data,
        };
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
