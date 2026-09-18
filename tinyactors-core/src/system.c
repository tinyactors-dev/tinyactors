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
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
        if (entry->actor != NULL) {
            (void)ta_actor_destroy(system, ((ta_actor_id)entry->generation << 32) | slot);
        }
    }
    for (uint32_t index = 0; index < system->definition_count; index += 1) {
        ta_definition_destroy(system->definitions[index]);
    }
    ta_mem_free(system->allocator, system->definitions,
                &(ta_allocator_free_params){ .size = system->definition_capacity * sizeof(ta_definition *),
                                             .alignment = _Alignof(ta_definition *) });
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
    if (system->definition_count == system->definition_capacity) {
        uint32_t capacity = system->definition_capacity == 0 ? 8u : system->definition_capacity * 2u;
        ta_definition **grown = ta_mem_alloc(system->allocator, capacity * sizeof(ta_definition *),
                                             _Alignof(ta_definition *));
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        if (system->definitions != NULL) {
            ta_mem_copy(grown, system->definitions, system->definition_count * sizeof(ta_definition *));
            ta_mem_free(system->allocator, system->definitions,
                        &(ta_allocator_free_params){ .size = system->definition_capacity * sizeof(ta_definition *),
                                                     .alignment = _Alignof(ta_definition *) });
        }
        system->definitions = grown;
        system->definition_capacity = capacity;
    }
    definition->id = system->definition_count;
    definition->registered = true;
    system->definitions[system->definition_count] = definition;
    system->definition_count += 1;
    *out = definition->id;
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
    const ta_delayed_send *next = ta_timer_peek(&system->timers);
    if (next == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    *out = next->due;
    return TA_OK;
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
        const ta_delayed_send *next = ta_timer_peek(&system->timers);
        if (next == NULL || next->due > now) {
            break;
        }
        ta_delayed_send send;
        (void)ta_timer_pop(&system->timers, &send);
        if (ta_interpreter_dispatch_delayed(system, &send) == TA_EXTERNAL_EVENT_RETRY) {
            /* Rescheduling keeps its ID, and so its place in deadline order. */
            ta_result result = ta_timer_schedule(&system->timers, &send);
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
    const ta_delayed_send *next = ta_timer_peek(&system->timers);
    *params->dispatched = dispatched;
    *params->more_due = retry || (next != NULL && next->due <= now);
    return TA_OK;
}
