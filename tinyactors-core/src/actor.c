#include "handle.h"
#include "actor.h"
#include "interpreter.h"
#include "mail.h"

ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return NULL;
    }
    const ta_actor_slot *entry = ta_actor_slot_at(system, ta_handle_slot(id));
    return entry != NULL && ta_handle_matches(id, entry->generation) ? entry->actor : NULL;
}

/*
 * ta_actor_slot_at returns the slot numbered `slot` (0-based, the number an
 * actor ID carries), or NULL when that slot was never issued. The pool counts
 * from 1, so this is the one place the actor table adds the 1; UINT32_MAX
 * wraps to 0, which the pool answers with NULL. It never allocates.
 */
ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot) {
    return ta_pool_at(&system->actors, slot + 1u);
}

/* A slot that has never been issued: generation 1 (0 is never issued, ADR 0059) and no actor. */
static const ta_actor_slot ta_actor_slot_fresh = { .generation = 1 };

/*
 * ta_actor_table_init prepares the actor table (lifetime: system) over
 * `allocator`. It allocates nothing: the first page comes from the first
 * spawn's refill. Slots are recycled with a new generation and never freed
 * before the system's destroy, so a pointer to a slot stays valid.
 */
ta_result ta_actor_table_init(ta_pool *table, ta_allocator allocator) {
    return ta_pool_init(table, allocator,
                        &(ta_pool_params){ .item_size = sizeof(ta_actor_slot),
                                           .item_alignment = _Alignof(ta_actor_slot),
                                           .link_offset = offsetof(ta_actor_slot, next_free),
                                           .page_items = TA_ACTOR_PAGE_SLOTS,
                                           .fresh = &ta_actor_slot_fresh });
}

ta_result ta_actor_layout(ta_definition *definition) {
    ta_layout layout;
    size_t session_bytes = 0;
    if (ta_definition_session_bytes(definition, &session_bytes) != TA_OK) {
        return TA_ERR_NOMEM; /* the definition keeps its old (zero) layout */
    }
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(ta_actor), .alignment = _Alignof(ta_actor), .count = 1u });
    size_t session = ta_layout_add(&layout, &(ta_layout_entry){ .size = session_bytes,
                                                                .alignment = _Alignof(uint64_t),
                                                                .count = 1u });
    size_t instance = ta_layout_add(&layout, &(ta_layout_entry){ .size = definition->datamodel_instance_size,
                                                                 .alignment = definition->datamodel_instance_alignment,
                                                                 .count = 1u });
    size_t bytes;
    size_t alignment;
    ta_result result = ta_layout_finish(&layout, &bytes, &alignment);
    if (result != TA_OK) {
        return result; /* the definition keeps its old (zero) layout */
    }
    definition->actor_session_offset = session;
    definition->actor_session_bytes = session_bytes;
    definition->actor_instance_offset = instance;
    definition->actor_block_bytes = bytes;
    definition->actor_block_alignment = alignment;
    return TA_OK;
}

/* ta_actor_id_slot returns the slot index an actor ID names, in the actor table (system lifetime). Pure; see ta_handle_slot. */
uint32_t ta_actor_id_slot(ta_actor_id id) {
    return ta_handle_slot(id);
}

/* ta_actor_id_generation returns the generation the slot had when the ID was issued; a destroyed actor's ID no longer matches the slot. Pure; see ta_handle_generation. */
uint32_t ta_actor_id_generation(ta_actor_id id) {
    return ta_handle_generation(id);
}

/* ta_actor_id_make packs a slot and the generation that slot holds into an actor ID (a call-lifetime value). Never allocates; see ta_handle_make. */
ta_actor_id ta_actor_id_make(uint32_t slot, uint32_t generation) {
    return ta_handle_make(slot, generation);
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

ta_result ta_actor_reserve_send(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_timer_reserve(&system->timers, &sparse->timers, 1u);
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

static ta_activation_effects *ta_actor_activation(ta_system *system, ta_actor *actor);

static ta_result ta_actor_hold_effect(ta_system *system, ta_actor *actor, const ta_activation_effect *effect) {
    ta_activation_effects *effects = ta_actor_activation(system, actor);
    if (effects == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_vec_push(&effects->entries, effect);
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
    uint32_t index = ta_pool_take(&system->actors);
    if (index == 0) {
        /* Full: this is a spawn, a safe point, so the table may grow. */
        ta_result result = ta_pool_refill(&system->actors, 1u);
        if (result != TA_OK) {
            return result;
        }
        index = ta_pool_take(&system->actors);
    }
    ta_actor_slot *entry = ta_pool_at(&system->actors, index);
    entry->actor = actor;
    *out = ta_handle_make(index - 1u, entry->generation);
    return TA_OK;
}

/*
 * Tears the actor down completely, then uncounts it. The last actor of a
 * definition nothing holds frees it (ADR 0077), so nothing below may touch it.
 */
void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    ta_run_actor_gone(system, actor);
    ta_actor_discard_activation_effects(actor);
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    /* A named actor holds a reference to its address; a runtime ref holds nothing. */
    ta_actor_ref_release(&actor->record.self);
    ta_interpreter_release_actor_state(system, actor);
    /* A reloaded session has a table entry; a session named by its runtime ID has none. */
    if (actor->session_id != actor->record.id) {
        ta_session_drop_resident(&system->sessions, actor->session_id, actor);
    }
    /* The record, the session storage and the instance are one block (ADR 0063). */
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = definition->actor_block_bytes,
                                             .alignment = definition->actor_block_alignment });
    ta_definition *owner = ta_system_definition(system, definition->id);
    owner->actor_count -= 1u;
    system->live_actors -= 1u;
    ta_system_settle_definition(system, owner);
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
    uint32_t slot = ta_handle_slot(id);
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = NULL;
    entry->generation = ta_handle_next_generation(entry->generation);
    ta_pool_give(&system->actors, slot + 1u);
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
    if (result == TA_OK) {
        ta_session_drop_unloaded(&system->sessions, address);
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
    /* maxActors is answered as an allocator that ran out (ADR 0092 section 4). */
    if (!ta_system_quota_actor(system)) {
        return TA_ERR_NOMEM;
    }
    /* One allocation: the record, the session storage, the instance (ADR 0063). */
    ta_actor *actor = ta_mem_alloc(system->allocator, definition->actor_block_bytes, definition->actor_block_alignment);
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    unsigned char *block = (unsigned char *)actor;
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    /* Counted from here so every ta_actor_free() path can uncount it. */
    definition->actor_count += 1u;
    system->live_actors += 1u;
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    actor->session_bytes = definition->actor_session_bytes;
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
        .self = { .kind = TA_ACTOR_REF_RUNTIME, .family = definition->family, .as.runtime = id },
        .residency = TA_ACTOR_RUNNING,
    };
    /* A new session is named by the runtime ID it starts with (ADR 0066). */
    actor->session_id = id;
    *out = actor;
    return TA_OK;
}

/*
 * Re-arms the delayed sends a host captured from an earlier session of this
 * actor (ADR 0074 section 8). Everything is checked and the timer list
 * reserved first, so scheduling cannot fail halfway; a failure leaves the
 * actor to be freed by the caller, which frees whatever was armed.
 */
static ta_result ta_actor_rearm_sends(ta_system *system, ta_actor *actor, const ta_actor_spawn_params *params) {
    uint32_t count = params->pending_send_count;
    if (count == 0) {
        return TA_OK;
    }
    if (params->pending_sends == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = actor->definition;
    for (uint32_t index = 0; index < count; index += 1) {
        const ta_delayed_send_info *info = &params->pending_sends[index];
        if (!ta_trie_valid(&system->event_names.trie, info->event) ||
            (info->has_content && info->content >= definition->content_count) ||
            (info->has_target_text && info->target_text >= definition->content_count) ||
            (info->destination.kind == TA_ACTOR_REF_RUNTIME &&
             !ta_atom_valid(&system->atoms, info->destination.family))) {
            return TA_ERR_INVALID;
        }
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_timer_reserve(&system->timers, &sparse->timers, count);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        const ta_delayed_send_info *info = &params->pending_sends[index];
        ta_delayed_send send = {
            .owner = actor->record.id,
            .send_id = info->send_id,
            .due = info->due,
            .event = info->event,
            .io_type = info->io_type,
            .destination = info->destination,
            .target = info->target,
            .data = info->data,
            .has_content = info->has_content,
            .content = info->content,
            .has_target_text = info->has_target_text,
            .target_text = info->target_text,
            .invoke_id = TA_INVOKE_ID_NONE,
            .trace = info->trace,
        };
        /* Runtime ID 0 names no actor, so as an SCXML send's destination it
         * means the session being spawned: a send to itself, stored by a
         * host that cannot know the new session's ID in advance. */
        if (send.destination.kind == TA_ACTOR_REF_RUNTIME && send.destination.as.runtime == 0 &&
            ta_system_is_scxml_io(system, send.io_type)) {
            send.destination.as.runtime = actor->record.id;
            send.destination.family = definition->family;
        }
        ta_actor_ref_retain(&send.destination);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        result = ta_timer_schedule(&system->timers, &sparse->timers, &send);
        if (result != TA_OK) {
            ta_actor_ref_release(&send.destination);
            ta_value_release(&send.target);
            ta_value_release(&send.data);
            break;
        }
        /* A later <send> never reuses a re-armed generated ID. */
        if ((send.send_id & TA_GENERATED_SEND_ID_BIT) != 0) {
            uint64_t counter = send.send_id & ~TA_GENERATED_SEND_ID_BIT;
            if (counter > actor->next_send_id) {
                actor->next_send_id = counter;
            }
        }
    }
    return result;
}

/* How many invoking parents stand above an actor: 0 for one nobody invoked. */
static uint32_t ta_actor_invoke_depth(ta_system *system, ta_actor_id id) {
    uint32_t depth = 0;
    const ta_actor *actor = ta_actor_lookup(system, id);
    while (actor != NULL && actor->sparse != NULL && actor->sparse->has_parent && depth < UINT32_MAX - 1u) {
        depth += 1u;
        actor = ta_actor_lookup(system, actor->sparse->parent.actor);
    }
    return depth;
}

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->parent != NULL && ta_actor_invoke_depth(system, params->parent->actor) + 1u >=
                                      system->params[TA_PARAM_INTERPRETER_NESTING]) {
        /* Exiting it would need an interpreter region past interpreterNesting
         * (ADR 0092 section 3). */
        return TA_ERR_UNSUPPORTED;
    }
    /* The bootstrap is a started macrostep (run.h): room for it comes first, so queueing it cannot fail. */
    ta_result result = ta_run_reserve_start(system);
    if (result != TA_OK) {
        return result;
    }
    ta_actor *actor = NULL;
    result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;

    /* A traced spawn's parent waits in an event block for the bootstrap
     * macrostep (ADR 0067); an untraced one needs no block. */
    if (!ta_trace_parent_empty(&params->trace)) {
        result = ta_interpreter_hold_block(system, actor);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
        actor->event_block->slots.trace = params->trace;
    }

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
    result = ta_actor_rearm_sends(system, actor, params);
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees what was re-armed, too */
        return result;
    }
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
    ta_trace_lifecycle(system, actor, TA_TRACE_SPAWNED, &params->trace);
    ta_run_start(system, actor);
    if (params->address != NULL) {
        /* Inputs that waited for this address follow the bootstrap, in arrival order (mail.h). */
        ta_mail_address_resident(system, actor->record.self.as.named, id);
    }
    *params->out = id;
    return TA_OK;
}

bool ta_actor_replaying(const ta_actor *actor) {
    return !actor->published && actor->sparse != NULL && actor->sparse->activation_effects != NULL &&
           actor->sparse->activation_effects->replaying;
}

ta_time ta_actor_now(const ta_system *system, const ta_actor *actor) {
    return actor != NULL && ta_actor_replaying(actor) ? actor->sparse->activation_effects->clock
                                                      : ta_clock_now(&system->clock);
}

/* The held-effects record a prepared actor keeps; a replaying one needs it
 * from the start, for its clock. */
static ta_activation_effects *ta_actor_activation(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return NULL;
    }
    if (sparse->activation_effects == NULL) {
        sparse->activation_effects = ta_mem_alloc(system->allocator, sizeof(ta_activation_effects),
                                                  _Alignof(ta_activation_effects));
        if (sparse->activation_effects == NULL) {
            return NULL;
        }
        *sparse->activation_effects = (ta_activation_effects){0};
        ta_vec_init(&sparse->activation_effects->entries, system->allocator, TA_VEC_LAYOUT(ta_activation_effect));
    }
    return sparse->activation_effects;
}

/* The named self of a prepared actor: the incumbent's, or `address` interned. */
static ta_result ta_actor_prepared_self(ta_system *system,
                                        const ta_actor_prepare_params *params,
                                        const ta_definition *definition,
                                        ta_actor_ref *out) {
    if (params->incumbent != 0) {
        ta_actor *incumbent = ta_actor_lookup(system, params->incumbent);
        if (incumbent == NULL) {
            return TA_ERR_NOT_FOUND;
        }
        if (!incumbent->published || incumbent->record.self.kind != TA_ACTOR_REF_NAMED ||
            !ta_str_equal(ta_address_view(incumbent->record.self.as.named).family.name,
                          definition->identity.family.name)) {
            return TA_ERR_INVALID;
        }
        *out = incumbent->record.self;
        ta_actor_ref_retain(out);
        return TA_OK;
    }
    const ta_actor_address *address = params->address;
    if (address == NULL || address->name.ptr == NULL || address->name.len == 0 ||
        !ta_str_equal(address->family.name, definition->identity.family.name)) {
        return TA_ERR_INVALID;
    }
    if (system->directory.v == NULL || system->directory.v->claim == NULL || system->directory.v->replace == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_NAMED };
    return ta_address_intern(system, address, &out->as.named);
}

/* ------------------------------------------------------------------------
 * An actor's state, exported and restored (ADR 0094 section 3)
 */


/* Everything a state names must exist in `definition`, before anything is
 * created. */
static ta_result ta_actor_state_check(const ta_system *system, const ta_definition *definition,
                                      const ta_actor_state *state) {
    const ta_definition_identity *have = &definition->identity;
    const ta_definition_identity *want = &state->definition;
    ta_str have_revision = { .ptr = (const char *)have->revision.ptr, .len = have->revision.len };
    ta_str want_revision = { .ptr = (const char *)want->revision.ptr, .len = want->revision.len };
    if (!ta_str_equal(have->family.name, want->family.name) || !ta_str_equal(have_revision, want_revision)) {
        return TA_ERR_DEFINITION_MISMATCH;
    }
    if ((state->active_count != 0 && state->active == NULL) || (state->late_count != 0 && state->late == NULL) ||
        (state->history_count != 0 && state->history == NULL) || (state->send_count != 0 && state->sends == NULL) ||
        (state->invoke_count != 0 && state->invokes == NULL)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < state->active_count; index += 1u) {
        if (state->active[index] >= definition->state_count) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->late_count; index += 1u) {
        if (state->late[index] >= definition->state_count) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->history_count; index += 1u) {
        const ta_history_entry *entry = &state->history[index];
        if (entry->history >= definition->state_count || entry->state >= definition->state_count ||
            definition->states[entry->history].history_slot == UINT32_MAX) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->invoke_count; index += 1u) {
        if (state->invokes[index].decl >= definition->invoke_decl_count) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->send_count; index += 1u) {
        const ta_delayed_send_info *info = &state->sends[index];
        if (info->submitted) {
            continue;
        }
        if (!ta_trie_valid(&system->event_names.trie, info->event) || !ta_atom_valid(&system->atoms, info->io_type) ||
            (info->has_content && info->content >= definition->content_count) ||
            (info->has_target_text && info->target_text >= definition->content_count)) {
            return TA_ERR_INVALID;
        }
    }
    return TA_OK;
}

/* Gives a replaying actor, prepared and not bootstrapped, a checked state. */
static ta_result ta_actor_state_restore(ta_system *system, ta_actor *actor, const ta_actor_state *state) {
    const ta_definition *definition = actor->definition;
    ta_configuration_clear(&actor->configuration);
    for (uint32_t index = 0; index < state->active_count; index += 1u) {
        ta_configuration_set_atomic(&actor->configuration, state->active[index], true);
    }
    for (uint32_t index = 0; index < state->late_count && actor->late_initialized != NULL; index += 1u) {
        ta_bits_put(actor->late_initialized, state->late[index], true);
    }
    for (uint32_t index = 0; index < state->history_count; index += 1u) {
        const ta_history_entry *entry = &state->history[index];
        uint32_t slot = definition->states[entry->history].history_slot;
        ta_bits_put(ta_history_slot_words(&actor->history, slot), entry->state, true);
    }
    /* It starts idle: the state it restores is past its bootstrap. */
    actor->bootstrapped = true;
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    actor->next_send_id = state->generated_ids;
    actor->trace_macrostep = state->macrostep;
    actor->trace_microstep = state->microstep;
    actor->trace_lifecycle = state->lifecycle;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < state->send_count; index += 1u) {
        const ta_delayed_send_info *info = &state->sends[index];
        if (info->submitted) {
            continue; /* it left the actor before the state was taken */
        }
        ta_delayed_send send = { .owner = actor->record.id,
                                 .send_id = info->send_id,
                                 .due = info->due,
                                 .event = info->event,
                                 .io_type = info->io_type,
                                 .destination = info->destination,
                                 .target = info->target,
                                 .data = info->data,
                                 .has_content = info->has_content,
                                 .content = info->content,
                                 .has_target_text = info->has_target_text,
                                 .target_text = info->target_text,
                                 .invoke_id = TA_INVOKE_ID_NONE,
                                 .trace = info->trace };
        /* Runtime ID 0, as for ta_actor_spawn()'s pending sends, is the
         * actor itself: a host cannot know its new session in advance. */
        if (send.destination.kind == TA_ACTOR_REF_RUNTIME && send.destination.as.runtime == 0 &&
            ta_system_is_scxml_io(system, send.io_type)) {
            send.destination.as.runtime = actor->record.id;
            send.destination.family = definition->family;
        }
        ta_actor_ref_retain(&send.destination);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        result = ta_actor_hold_send(system, actor, &(ta_activation_send){ .send = send, .immediate = false });
        if (result != TA_OK) {
            ta_actor_ref_release(&send.destination);
            ta_value_release(&send.target);
            ta_value_release(&send.data);
        }
    }
    for (uint32_t index = 0; result == TA_OK && index < state->invoke_count; index += 1u) {
        const ta_invoke_state *invoke = &state->invokes[index];
        result = ta_invoke_restore(system, actor, &(ta_invoke_restore_params){ .id = invoke->id,
                                                                              .type = invoke->type,
                                                                              .decl = invoke->decl });
    }
    return result;
}

ta_result ta_actor_export_state(ta_system *system, const ta_actor_export_params *params) {
    if (system == NULL || params == NULL || params->state_count == NULL || params->history_count == NULL ||
        params->invoke_count == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->state_count = 0;
    *params->history_count = 0;
    *params->invoke_count = 0;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!actor->published) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || actor->queued != 0 || ta_actor_external_event(actor) != NULL ||
        ta_interpreter_internal_pending(actor)) {
        return TA_ERR_BUSY;
    }
    const ta_definition *definition = actor->definition;
    const ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t active = 0;
    uint32_t late = 0;
    uint32_t history = 0;
    uint32_t invokes = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        active += ta_configuration_contains_atomic(&actor->configuration, state) ? 1u : 0u;
        late += actor->late_initialized != NULL && ta_bits_get(actor->late_initialized, state) ? 1u : 0u;
        uint32_t slot = definition->states[state].history_slot;
        for (ta_state_id recorded = 0; slot != UINT32_MAX && recorded < definition->state_count; recorded += 1u) {
            history += ta_bits_get(ta_history_slot_words(&actor->history, slot), recorded) ? 1u : 0u;
        }
    }
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        invokes += sparse->invokes[index].active ? 1u : 0u;
    }
    *params->state_count = active + late;
    *params->history_count = history;
    *params->invoke_count = invokes;
    if (active + late > params->state_capacity || history > params->history_capacity ||
        invokes > params->invoke_capacity || (active + late != 0 && params->states == NULL) ||
        (history != 0 && params->history == NULL) || (invokes != 0 && params->invokes == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t next_active = 0;
    uint32_t next_late = active;
    uint32_t next_history = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        if (ta_configuration_contains_atomic(&actor->configuration, state)) {
            params->states[next_active++] = state;
        }
        if (actor->late_initialized != NULL && ta_bits_get(actor->late_initialized, state)) {
            params->states[next_late++] = state;
        }
        uint32_t slot = definition->states[state].history_slot;
        for (ta_state_id recorded = 0; slot != UINT32_MAX && recorded < definition->state_count; recorded += 1u) {
            if (ta_bits_get(ta_history_slot_words(&actor->history, slot), recorded)) {
                params->history[next_history++] = (ta_history_entry){ .history = state, .state = recorded };
            }
        }
    }
    uint32_t next_invoke = 0;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        if (sparse->invokes[index].active) {
            params->invokes[next_invoke++] = (ta_invoke_state){ .id = sparse->invokes[index].id,
                                                                .type = sparse->invokes[index].type,
                                                                .decl = ta_invoke_table_decl(&sparse->invoke_table, index) };
        }
    }
    ta_value root = { 0 };
    if (definition->datamodel.v->inspect != NULL) {
        if (definition->datamodel.v->inspect(definition->datamodel.self,
                                             &(ta_datamodel_inspect_params){ .shared = definition->datamodel_shared,
                                                                             .instance = actor->datamodel_instance,
                                                                             .host = &actor->host,
                                                                             .out = &root }) != TA_OK) {
            root = (ta_value){ 0 };
        }
    }
    *params->out = (ta_actor_state){
        .definition = definition->identity,
        .generated_ids = actor->next_send_id,
        .clock = ta_actor_now(system, actor),
        .macrostep = actor->trace_macrostep,
        .microstep = actor->trace_microstep,
        .lifecycle = actor->trace_lifecycle,
        .active = active != 0 ? params->states : NULL,
        .active_count = active,
        .late = late != 0 ? params->states + active : NULL,
        .late_count = late,
        .history = history != 0 ? params->history : NULL,
        .history_count = history,
        .sends = sparse != NULL && sparse->timers.count != 0 ? sparse->timers.sends : NULL,
        .send_count = sparse != NULL ? sparse->timers.count : 0,
        .invokes = invokes != 0 ? params->invokes : NULL,
        .invoke_count = invokes,
        .root = root,
    };
    return TA_OK;
}

ta_result ta_actor_prepare(ta_system *system, const ta_actor_prepare_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_time *at = params->at;
    const ta_value *binding = params->datamodel_binding;
    ta_time spawn_at = 0;
    /* A state gives the clock, the binding and everything else (ADR 0094). */
    const ta_actor_state *state = params->state;
    if (state != NULL) {
        if (params->at != NULL) {
            return TA_ERR_INVALID;
        }
        ta_result checked = ta_actor_state_check(system, definition, state);
        if (checked != TA_OK) {
            return checked;
        }
        spawn_at = state->clock;
        at = &spawn_at;
        if (binding == NULL && state->root.v != NULL) {
            binding = &state->root;
        }
    }
    if (at != NULL && *at > ta_clock_now(&system->clock)) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_run_reserve_start(system);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_ref self = {0};
    result = ta_actor_prepared_self(system, params, definition, &self);
    if (result != TA_OK) {
        return result;
    }
    ta_actor *actor = NULL;
    result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        ta_actor_ref_release(&self);
        return result;
    }
    ta_actor_id id = actor->record.id;
    actor->published = false;
    actor->replacement_of = params->incumbent;
    actor->record.self = self;
    if (at != NULL) {
        ta_activation_effects *effects = ta_actor_activation(system, actor);
        if (effects == NULL) {
            result = TA_ERR_NOMEM;
        } else {
            effects->replaying = true;
            effects->clock = *at;
        }
    }
    /* A traced bootstrap's parent waits in an event block (ADR 0067). */
    if (result == TA_OK && !ta_trace_parent_empty(&params->trace)) {
        result = ta_interpreter_hold_block(system, actor);
        if (result == TA_OK) {
            actor->event_block->slots.trace = params->trace;
        }
    }
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result == TA_OK && state != NULL) {
        result = ta_actor_state_restore(system, actor, state);
    }
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    if (actor->macrostep) {
        ta_run_start(system, actor); /* a state-restored actor starts idle: its bootstrap is past */
    }
    *params->out = id;
    return TA_OK;
}

/* Held sends that publishing will schedule: all of them, or, with a clock
 * move to `*at`, those due after it. */
static uint32_t ta_actor_activation_send_count(const ta_actor *actor, const ta_time *at) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return 0;
    }
    uint32_t count = 0;
    const ta_vec *entries = &sparse->activation_effects->entries;
    for (uint32_t index = 0; index < entries->count; index += 1u) {
        const ta_activation_effect *effect = ta_vec_at(entries, index);
        count += effect->kind == TA_ACTIVATION_SEND && (at == NULL || effect->as.send.send.due > *at) ? 1u : 0u;
    }
    return count;
}

static void ta_actor_free_activation(ta_actor_sparse_state *sparse) {
    ta_activation_effects *effects = sparse->activation_effects;
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

/*
 * Releases what an actor prepared without `at` held: logs to the sink,
 * immediate sends dispatched, delayed sends scheduled. A replaying actor
 * holds only delayed sends, and they are scheduled: it dropped everything
 * else as it ran (ADR 0089 section 3).
 */
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
            ta_external_event_submit_status status = ta_interpreter_dispatch_delayed(system, send, false);
            if (status != TA_EXTERNAL_EVENT_RETRY) {
                continue;
            }
            /* Dispatch marked it submitted and took its origin and invoke ID. */
        }
        /* Reserved by ta_actor_publish(), so this cannot fail. */
        (void)ta_timer_schedule(&system->timers, &sparse->timers, send);
    }
    ta_actor_free_activation(sparse);
}

/*
 * Puts a prepared actor on the route, as one directory call. With no
 * incumbent, an address with no entry is claimed and an UNLOADED one is
 * replaced, and the unloaded session ends with it (ADR 0066); a resident
 * one is BUSY.
 */
static ta_result ta_actor_publish_route(ta_system *system, ta_actor *actor, ta_actor_id incumbent) {
    ta_address *address = actor->record.self.as.named;
    ta_actor_directory_entry resident = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id };
    if (incumbent != 0) {
        return ta_actor_directory_swap(
            system, &(ta_actor_directory_replace_params){
                        .address = address,
                        .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = incumbent },
                        .replacement = resident,
                    });
    }
    ta_result result = system->directory.v->claim(
        system->directory.self, &(ta_actor_directory_claim_params){ .address = address, .entry = resident });
    if (result != TA_ERR_STALE) {
        return result;
    }
    result = ta_actor_directory_swap(system, &(ta_actor_directory_replace_params){
                                                 .address = address,
                                                 .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
                                                 .replacement = resident,
                                             });
    if (result == TA_ERR_STALE) {
        return TA_ERR_BUSY;
    }
    if (result == TA_OK) {
        ta_session_drop_unloaded(&system->sessions, address);
    }
    return result;
}

ta_result ta_actor_publish(ta_system *system, const ta_actor_publish_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    ta_actor *incumbent = params->incumbent == 0 ? NULL : ta_actor_lookup(system, params->incumbent);
    if (actor == NULL || (params->incumbent != 0 && incumbent == NULL)) {
        return TA_ERR_NOT_FOUND;
    }
    if (actor->published || actor->replacement_of != params->incumbent ||
        actor->record.self.kind != TA_ACTOR_REF_NAMED ||
        (incumbent != NULL && (!incumbent->published || incumbent->record.self.kind != TA_ACTOR_REF_NAMED))) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || actor->queued != 0 || actor->record.residency != TA_ACTOR_IDLE) {
        return TA_ERR_BUSY;
    }
    bool replaying = ta_actor_replaying(actor);
    /* A last clock move is only for a replaying actor (ADR 0094 section 1). */
    const ta_time *at = params->at;
    if (at != NULL && (!replaying || !ta_actor_input_time_valid(system, actor, *at))) {
        return TA_ERR_INVALID;
    }
    /* Everything that can fail comes first, so a refusal changes nothing. */
    uint32_t sends = ta_actor_activation_send_count(actor, at);
    ta_result result = ta_run_reserve_start(system); /* a lost invocation or a pending start may begin a macrostep */
    if (result == TA_OK && sends != 0) {
        result = ta_timer_reserve(&system->timers, &actor->sparse->timers, sends);
    }
    if (result == TA_OK && replaying) {
        result = ta_invoke_prepare_retire(system, actor);
    }
    if (result == TA_OK) {
        result = ta_actor_publish_route(system, actor, params->incumbent);
    }
    if (result != TA_OK) {
        return result;
    }
    if (at != NULL) {
        ta_actor_move_clock(actor, *at);
    }
    actor->published = true;
    actor->replacement_of = 0;
    ta_actor_touch(actor);
    ta_actor_release_activation_effects(system, actor);
    if (incumbent != NULL) {
        ta_mail_retarget(system, incumbent->record.id, actor->record.id); /* its queued inputs follow the address */
    }
    ta_mail_address_resident(system, actor->record.self.as.named, actor->record.id);
    if (replaying) {
        /* Its services never started, and the world moved on while it was
         * away: each active invocation is reported lost, as after a load. */
        if (ta_invoke_retire_all(system, actor)) {
            ta_interpreter_start_macrostep(system, actor, NULL);
        }
    } else if (actor->sparse != NULL && actor->sparse->pending_invokes.count != 0) {
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
        ta_run_start(system, actor);
    }
    return TA_OK;
}

/*
 * Whether `time` may be an input's time for `actor` (ADR 0094 section 1): a
 * replaying actor's is no earlier than its clock and no later than the
 * system's; every other actor's is the system's own.
 */
bool ta_actor_input_time_valid(const ta_system *system, const ta_actor *actor, ta_time time) {
    ta_time now = ta_clock_now(&system->clock);
    if (!ta_actor_replaying(actor)) {
        return time == now;
    }
    return time >= actor->sparse->activation_effects->clock && time <= now;
}

/*
 * Moves a replaying actor's clock to `time`, dropping every held delayed
 * send due by then: time reached it in the run being replayed, so it fired
 * then (ADR 0089 section 2). Nothing to do for any other actor. The caller
 * has checked the time.
 */
void ta_actor_move_clock(ta_actor *actor, ta_time time) {
    if (!ta_actor_replaying(actor)) {
        return;
    }
    ta_activation_effects *effects = actor->sparse->activation_effects;
    ta_vec *entries = &effects->entries;
    uint32_t kept = 0;
    for (uint32_t index = 0; index < entries->count; index += 1u) {
        ta_activation_effect *effect = ta_vec_at(entries, index);
        if (effect->kind == TA_ACTIVATION_SEND && effect->as.send.send.due <= time) {
            ta_activation_effect_release(effect);
            continue;
        }
        *(ta_activation_effect *)ta_vec_at(entries, kept) = *effect;
        kept += 1u;
    }
    if (kept != entries->count) {
        entries->count = kept;
        ta_actor_touch(actor);
    }
    effects->clock = time;
}

/*
 * Checks an I/O origin against the registry, and gives an event from one of
 * the actor's invocations an invoke origin when it has none (ADR 0068).
 * That covers hosts that post invoker events through their own mailbox.
 */
ta_result ta_actor_accept_origin(const ta_system *system, const ta_actor *actor, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        const ta_io_origin *io = &event->origin.as.io;
        if (ta_system_io_at(system, io->processor) == NULL ||
            (io->origin_class == TA_IO_ORIGIN_MESSAGE && io->as.entry == NULL) ||
            (uint32_t)io->origin_class > (uint32_t)TA_IO_ORIGIN_MESSAGE) {
            return TA_ERR_INVALID;
        }
        return TA_OK;
    }
    if (event->origin.kind != TA_ORIGIN_NONE || event->invoke_id == TA_INVOKE_ID_NONE || actor == NULL) {
        return TA_OK; /* no session yet (an input that waits for an address) has no invocations to name */
    }
    (void)ta_invoke_origin_for(system, actor, event->invoke_id, &event->origin);
    return TA_OK;
}

ta_result ta_operator_jump_bytes(uint32_t count, uint32_t *bytes) {
    ta_layout layout;
    size_t total = 0;
    ta_layout_init(&layout, offsetof(ta_operator_jump, targets));
    ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(ta_state_id), .alignment = _Alignof(ta_state_id),
                                               .count = count });
    if (ta_layout_finish(&layout, &total, NULL) != TA_OK || total > UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    *bytes = (uint32_t)total;
    return TA_OK;
}

ta_result ta_actor_abandon(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_result result = ta_interpreter_abandon(system, actor);
    if (result == TA_OK) {
        ta_run_abandoned(system, actor);
    }
    return result;
}

/*
 * An operator's withdrawal (ADR 0084): what a <cancel> of `send` in the
 * actor does to its pending sends, from outside any macrostep. The actor
 * changes, so an image captured before no longer describes it, and the
 * record is lifecycle-like: its key is the session's next lifecycle key.
 */
static ta_result ta_actor_withdraw(ta_system *system, ta_actor *actor, ta_send_id send);

ta_result ta_actor_withdraw_send(ta_system *system, ta_actor_id id, ta_send_id send) {
    if (send == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL || ta_actor_timers(actor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_actor_withdraw(system, actor, send);
}

static ta_result ta_actor_withdraw(ta_system *system, ta_actor *actor, ta_send_id send) {
    ta_timer_list *list = ta_actor_timers(actor);
    /* A replaying actor's delayed sends are held, not pending (ADR 0089). */
    bool held = ta_actor_replaying(actor) && ta_actor_cancel_held_send(actor, send);
    if (ta_timer_cancel(&system->timers, list, send) != TA_OK && !held) {
        for (uint32_t index = 0; index < list->count; index += 1u) {
            if (list->sends[index].send_id == send) {
                return TA_ERR_DONE; /* only submitted ones are left */
            }
        }
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_touch(actor);
    TA_COUNT(system, actor->definition, sends_cancelled);
    ta_step_key key = ta_trace_lifecycle_key(actor);
    ta_trace_context none = {0};
    if (ta_trace_lifecycle_wanted(&system->trace_masks, TA_TRACE_WITHDRAWN, &none)) {
        ta_trace_record record = ta_trace_record_blank(TA_TRACE_WITHDRAWN);
        record.key = key;
        record.actor = actor->record.id;
        record.definition = actor->definition->id;
        record.now = ta_actor_now(system, actor);
        record.send = send;
        ta_tel_write_trace(system, &record, NULL);
    }
    return TA_OK;
}

/* The family fills the padding after `kind` (ADR 0088). */
_Static_assert(sizeof(ta_actor_ref) == 16, "ta_actor_ref must stay 16 bytes");
_Static_assert(sizeof(ta_step_key) == 24, "ta_step_key must stay 24 bytes: its family fills padding");

ta_atom ta_actor_family_of(const ta_system *system, ta_actor_id id) {
    const ta_actor *actor = ta_actor_lookup(system, id);
    return actor != NULL ? actor->definition->family : 0;
}

ta_actor_ref ta_actor_runtime_ref(const ta_system *system, ta_actor_id id) {
    return (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .family = ta_actor_family_of(system, id), .as.runtime = id };
}

ta_result ta_actor_destroy(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL && sparse->completion_pending && sparse->has_parent) {
        ta_actor_ref parent = ta_actor_runtime_ref(system, sparse->parent.actor);
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
    ta_trace_lifecycle(system, actor, TA_TRACE_DESTROYED, NULL);
    ta_actor_release_address(system, actor);
    ta_actor_release_slot(system, id);
    ta_actor_free(system, actor);
    return TA_OK;
}

static const ta_event *ta_actor_paused_event(const ta_actor *actor) {
    if (!actor->macrostep) {
        return NULL;
    }
    return ta_actor_external_event(actor);
}

const ta_event *ta_actor_external_event(const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (block == NULL || block->slots.external == TA_EXTERNAL_NONE) {
        return NULL;
    }
    return &block->slots.external_event;
}

const ta_event *ta_actor_bound_event(const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (block == NULL) {
        return NULL;
    }
    switch ((ta_event_binding)block->slots.bound) {
    case TA_BOUND_EXTERNAL:
        return &block->slots.external_event;
    case TA_BOUND_INTERNAL:
        return &block->events[0].event;
    case TA_BOUND_KEPT:
        return &block->slots.kept_event;
    case TA_BOUND_NONE:
    default:
        return NULL;
    }
}

uint32_t ta_actor_waiting_internal(const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (block == NULL) {
        return 0;
    }
    return block->count - (block->slots.bound == TA_BOUND_INTERNAL ? 1u : 0u);
}

/* Everything the core allocates for the actor now; see ta_actor_inspection. */
static size_t ta_actor_allocated_bytes(const ta_actor *actor) {
    size_t total = actor->definition->actor_block_bytes;
    if (actor->event_block != NULL) {
        total += ta_event_block_bytes(actor->event_block->capacity);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL) {
        total += sizeof(ta_actor_sparse_state);
        total += ta_vec_bytes(&sparse->pending_invokes);
        total += ta_invoke_table_bytes(&sparse->invoke_table);
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
        .microstep_in_progress = actor->sparse != NULL && actor->sparse->paused != NULL &&
                                 ta_run_phase_in_microstep(actor->sparse->paused->at.phase),
        .step = ta_actor_step(actor),
        .position = ta_actor_position(actor),
        .current_event = ta_actor_paused_event(actor),
        .allocated_bytes = ta_actor_allocated_bytes(actor),
        .final_state = TA_STATE_NONE,
        .trace = { .context = ta_actor_macrostep_parent(actor).context,
                   .cause = ta_actor_step(actor) },
    };
    /* A taken head is `_event`, not work that waits. */
    uint32_t waiting = ta_actor_waiting_internal(actor);
    if (waiting != 0) {
        out->internal_events = actor->event_block->events + (actor->event_block->count - waiting);
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
        out->has_parent = actor->sparse->has_parent;
        out->parent = actor->sparse->parent;
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
    for (; slot < ta_pool_extent(&system->actors); slot += 1u) {
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
    *next = slot >= ta_pool_extent(&system->actors) ? TA_ACTOR_VISIT_END : slot;
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
