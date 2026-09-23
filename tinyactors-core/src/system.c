#include "system.h"
#include "interpreter.h"

#define TA_SCXML_IO_URI "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

static ta_str ta_cstr(const char *text) {
    ta_str out = { .ptr = text, .len = 0 };
    while (text[out.len] != '\0') {
        out.len += 1;
    }
    return out;
}

ta_result ta_system_create(const ta_system_config *config, ta_system **out) {
    if (config == NULL || out == NULL || config->allocator.v == NULL ||
        config->allocator.v->alloc == NULL || config->allocator.v->free == NULL) {
        return TA_ERR_INVALID;
    }
    ta_system *system = ta_mem_alloc(config->allocator, sizeof(ta_system), _Alignof(ta_system));
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    *system = (ta_system){ .allocator = config->allocator };
    ta_clock_set(&system->clock, config->initial_time);
    ta_vec_init(&system->actors, config->allocator, TA_VEC_LAYOUT(ta_actor_slot));
    system->free_actor_slot = UINT32_MAX;
    ta_vec_init(&system->definitions, config->allocator, TA_VEC_LAYOUT(ta_definition_slot));
    system->free_definition_slot = UINT32_MAX;
    ta_vec_init(&system->io_processors, config->allocator, TA_VEC_LAYOUT(ta_io_registration));
    ta_vec_init(&system->invokers, config->allocator, TA_VEC_LAYOUT(ta_invoker_registration));
    ta_result result = ta_atom_table_init(&system->atoms, config->allocator);
    if (result != TA_OK) {
        ta_mem_free(config->allocator, system,
                    &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
        return result;
    }
    result = ta_event_name_table_init(&system->event_names, config->allocator, &system->atoms);
    if (result == TA_OK) {
        result = ta_timer_heap_init(&system->timers, config->allocator);
    }
    if (result == TA_OK) {
        result = ta_arena_init(&system->scratch, config->allocator, 4096);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_cstr(TA_SCXML_IO_URI), &system->scxml_io_type);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_cstr("scxml"), &system->scxml_io_alias);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_cstr("http://www.w3.org/TR/scxml/"),
                                &system->scxml_invoke_type);
    }
    if (result != TA_OK) {
        ta_system_destroy(system);
        return result;
    }
    *out = system;
    return TA_OK;
}

void ta_system_destroy(ta_system *system) {
    if (system == NULL) {
        return;
    }
    /* Teardown releases submitted sends instead of keeping them as orphans. */
    system->timers.closing = true;
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
        if (entry->actor != NULL) {
            (void)ta_actor_destroy(system, ta_actor_id_make(slot, entry->generation));
        }
    }
    while (system->unregistered_definitions != NULL) {
        ta_definition_destroy(system->unregistered_definitions);
    }
    /* Every actor is gone, so every definition goes, retired or not. */
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition != NULL) {
            ta_definition_destroy(entry->definition);
            entry->definition = NULL;
        }
    }
    ta_vec_destroy(&system->definitions);
    ta_timer_heap_destroy(&system->timers);
    ta_vec_destroy(&system->actors);
    ta_vec_destroy(&system->io_processors);
    ta_vec_destroy(&system->invokers);
    ta_arena_destroy(&system->scratch);
    ta_event_name_table_destroy(&system->event_names);
    ta_atom_table_destroy(&system->atoms);
    ta_allocator allocator = system->allocator;
    ta_mem_free(allocator, system,
                &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
}

ta_result ta_system_set_actor_directory(ta_system *system, ta_actor_directory directory) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    system->directory = directory;
    return TA_OK;
}

ta_result ta_system_set_log_sink(ta_system *system, ta_log_sink sink) {
    if (system == NULL || (sink.v != NULL && sink.v->write == NULL)) {
        return TA_ERR_INVALID;
    }
    system->log_sink = sink;
    return TA_OK;
}

ta_result ta_system_set_external_event_router(ta_system *system, ta_external_event_router router) {
    if (system == NULL || (router.v != NULL && router.v->submit == NULL)) {
        return TA_ERR_INVALID;
    }
    system->external_events = router;
    return TA_OK;
}

ta_result ta_system_register_definition(ta_system *system, ta_definition *definition, ta_definition_id *out) {
    if (system == NULL || definition == NULL || out == NULL || definition->system != system ||
        definition->registered) {
        return TA_ERR_INVALID;
    }
    uint32_t slot = system->free_definition_slot;
    if (slot == UINT32_MAX) {
        ta_definition_slot fresh = { .generation = 1, .next_free = UINT32_MAX };
        ta_result result = ta_vec_push(&system->definitions, &fresh);
        if (result != TA_OK) {
            return result;
        }
        slot = system->definitions.count - 1u;
    }
    ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    if (slot == system->free_definition_slot) {
        system->free_definition_slot = entry->next_free;
    }
    entry->definition = definition;
    entry->next_free = UINT32_MAX;
    if (definition->unregistered_previous != NULL) {
        definition->unregistered_previous->unregistered_next = definition->unregistered_next;
    } else {
        system->unregistered_definitions = definition->unregistered_next;
    }
    if (definition->unregistered_next != NULL) {
        definition->unregistered_next->unregistered_previous = definition->unregistered_previous;
    }
    definition->unregistered_previous = NULL;
    definition->unregistered_next = NULL;
    definition->id = ((ta_definition_id)entry->generation << 32) | slot;
    definition->registered = true;
    definition->registration_serial = system->next_definition_serial++;
    *out = definition->id;
    return TA_OK;
}

ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t generation = (uint32_t)(id >> 32);
    if (slot >= system->definitions.count) {
        return NULL;
    }
    const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    return entry->generation == generation ? entry->definition : NULL;
}

void ta_system_free_definition(ta_system *system, ta_definition *definition) {
    uint32_t slot = (uint32_t)(definition->id & 0xffffffffu);
    ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    /* The slot's next occupant gets a new generation, so this ID goes stale. */
    entry->definition = NULL;
    entry->generation += 1u;
    entry->next_free = system->free_definition_slot;
    system->free_definition_slot = slot;
    ta_definition_destroy(definition);
}

ta_result ta_system_retire_definition(ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, id);
    if (definition == NULL) {
        /*
         * A slot's generation advances only when its definition is freed, so
         * an older generation of an existing slot was retired and freed.
         */
        uint32_t slot = (uint32_t)(id & 0xffffffffu);
        uint32_t generation = (uint32_t)(id >> 32);
        bool freed = slot < system->definitions.count && generation != 0 &&
                     generation < ((const ta_definition_slot *)ta_vec_at(&system->definitions, slot))->generation;
        return freed ? TA_OK : TA_ERR_NOT_FOUND;
    }
    definition->retired = true;
    if (definition->actor_count == 0) {
        ta_system_free_definition(system, definition);
    }
    return TA_OK;
}

static bool ta_bytes_same(ta_bytes left, ta_bytes right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

ta_result ta_system_find_definition(const ta_system *system,
                                    const ta_definition_identity *identity,
                                    ta_definition_id *out) {
    if (system == NULL || identity == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    /* Slots are reused, so registration order decides: the newest match wins. */
    const ta_definition *best = NULL;
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        const ta_definition *definition = entry->definition;
        if (definition == NULL || definition->retired ||
            (best != NULL && best->registration_serial > definition->registration_serial)) {
            continue;
        }
        if (ta_str_equal(definition->identity.family.name, identity->family.name) &&
            ta_bytes_same(definition->identity.revision, identity->revision)) {
            best = definition;
        }
    }
    if (best == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    *out = best->id;
    return TA_OK;
}

bool ta_system_is_scxml_io(const ta_system *system, ta_atom type) {
    return type == system->scxml_io_type || type == system->scxml_io_alias;
}

const ta_io *ta_system_find_io(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->io_processors.count; index += 1) {
        const ta_io_registration *entry = ta_vec_at(&system->io_processors, index);
        if (entry->type == type) {
            return &entry->io;
        }
    }
    return NULL;
}

ta_result ta_system_register_io(ta_system *system, ta_atom type, ta_io io) {
    if (system == NULL || io.v == NULL || io.v->send == NULL || !ta_atom_valid(&system->atoms, type) ||
        ta_system_is_scxml_io(system, type)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < system->io_processors.count; index += 1) {
        ta_io_registration *entry = ta_vec_at(&system->io_processors, index);
        if (entry->type == type) {
            entry->io = io;
            return TA_OK;
        }
    }
    ta_io_registration entry = { .type = type, .io = io };
    return ta_vec_push(&system->io_processors, &entry);
}

ta_result ta_system_register_invoker(ta_system *system, ta_atom type, ta_invoker invoker) {
    if (system == NULL || invoker.v == NULL || !ta_atom_valid(&system->atoms, type)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            entry->invoker = invoker;
            return TA_OK;
        }
    }
    ta_invoker_registration entry = { .type = type, .invoker = invoker };
    return ta_vec_push(&system->invokers, &entry);
}

ta_result ta_system_intern_atom(ta_system *system, ta_str text, ta_atom *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    return ta_atom_intern(&system->atoms, text, out);
}

ta_result ta_system_atom_text(const ta_system *system, ta_atom atom, ta_str *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (!ta_atom_valid(&system->atoms, atom)) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&system->atoms, atom);
    return TA_OK;
}

ta_result ta_event_name_intern(ta_system *system, const ta_atom_list *segments, ta_event_name_id *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_event_name_intern_atoms_internal(&system->event_names, segments, out);
}

bool ta_event_name_matches(const ta_system *system, ta_event_name_id event, ta_event_name_id prefix) {
    return system != NULL && ta_event_name_matches_internal(&system->event_names, event, prefix);
}

ta_result ta_event_name_format(const ta_system *system, const ta_event_name_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL ||
        !ta_event_name_valid(&system->event_names, params->name)) {
        return TA_ERR_INVALID;
    }
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&system->event_names, params->name, segments);
    uint32_t length = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        length += ta_atom_text(&system->atoms, segments[index]).len + (index == 0 ? 0u : 1u);
    }
    *params->needed = length + 1u;
    if (params->buffer == NULL || params->capacity < length + 1u) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        if (index != 0) {
            params->buffer[at++] = '.';
        }
        ta_str text = ta_atom_text(&system->atoms, segments[index]);
        ta_mem_copy(params->buffer + at, text.ptr, text.len);
        at += text.len;
    }
    params->buffer[at] = '\0';
    return TA_OK;
}

ta_result ta_system_set_time(ta_system *system, ta_time now) {
    if (system == NULL || now < ta_clock_now(&system->clock)) {
        return TA_ERR_INVALID;
    }
    ta_clock_set(&system->clock, now);
    return TA_OK;
}

ta_time ta_system_time(const ta_system *system) {
    return system == NULL ? 0 : ta_clock_now(&system->clock);
}

ta_atom ta_system_scxml_io_type(const ta_system *system) {
    return system == NULL ? TA_ATOM_NONE : system->scxml_io_type;
}

ta_result ta_system_next_deadline(const ta_system *system, ta_time *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_timer_next_due(&system->timers, out) ? TA_OK : TA_ERR_NOT_FOUND;
}

ta_result ta_system_dispatch_due(ta_system *system, const ta_due_dispatch_params *params) {
    if (system == NULL || params == NULL || params->budget == 0 || params->dispatched == NULL ||
        params->more_due == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t dispatched = 0;
    bool retry = false;
    ta_time now = ta_clock_now(&system->clock);
    while (dispatched < params->budget) {
        ta_time due = 0;
        if (!ta_timer_next_due(&system->timers, &due) || due > now) {
            break;
        }
        ta_delayed_send send;
        (void)ta_timer_pop(&system->timers, &send);
        if (ta_interpreter_dispatch_delayed(system, &send) == TA_EXTERNAL_EVENT_RETRY) {
            /*
             * Rescheduling keeps its ID, and so its place in deadline order.
             * The owner may be gone by now; its submitted sends are orphans.
             */
            ta_timer_list *home = NULL;
            ta_result result = ta_actor_timer_home(system, send.owner, &home);
            if (result == TA_OK) {
                result = ta_timer_schedule(&system->timers, home, &send);
            }
            if (result != TA_OK) {
                ta_value_release(&send.target);
                ta_value_release(&send.data);
                return result;
            }
            retry = true;
            break;
        }
        dispatched += 1;
    }
    ta_time next = 0;
    *params->dispatched = dispatched;
    *params->more_due = retry || (ta_timer_next_due(&system->timers, &next) && next <= now);
    return TA_OK;
}
