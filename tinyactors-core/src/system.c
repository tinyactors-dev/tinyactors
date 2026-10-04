#include "system.h"
#include "trace.h"
#include "interpreter.h"

#define TA_SCXML_IO_URI "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

/* ------------------------------------------------------------------------
 * System parameters (ADR 0092)
 */

typedef struct {
    const char *name;
    const char *unit;
    uint32_t default_value;
    uint32_t min;
    uint32_t max;
} ta_param_row;

/* One row per ta_param, in its order. The bounds are the only ones: no
 * other code checks a parameter again. */
static const ta_param_row ta_param_rows[TA_PARAM_COUNT] = {
    [TA_PARAM_MICROSTEP_LIMIT] = { "microstepLimit", "microsteps", TA_PARAM_DEFAULT_MICROSTEP_LIMIT, 0, 2147483647u },
    [TA_PARAM_EVENT_BLOCK_CACHE] = { "eventBlockCache", "blocks", TA_PARAM_DEFAULT_EVENT_BLOCK_CACHE, 0, 65536u },
    [TA_PARAM_EVENT_NAME_DEPTH] = { "eventNameDepth", "segments", TA_PARAM_DEFAULT_EVENT_NAME_DEPTH, 8,
                                    TA_EVENT_NAME_MAX_DEPTH },
    [TA_PARAM_COMPILE_MESSAGE_BYTES] = { "compileMessageBytes", "bytes", TA_PARAM_DEFAULT_COMPILE_MESSAGE_BYTES, 16,
                                         65536u },
    [TA_PARAM_COMPILE_TEXT_BYTES] = { "compileTextBytes", "bytes", TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES, 256, 65536u },
    [TA_PARAM_COMPILE_NEAREST_BYTES] = { "compileNearestBytes", "bytes", TA_PARAM_DEFAULT_COMPILE_NEAREST_BYTES, 0,
                                         65536u },
    [TA_PARAM_INTERPRETER_BYTES] = { "interpreterBytes", "bytes", TA_PARAM_DEFAULT_INTERPRETER_BYTES, 1024,
                                     1073741824u },
    [TA_PARAM_RENDER_BYTES] = { "renderBytes", "bytes", TA_PARAM_DEFAULT_RENDER_BYTES, 256, 1073741824u },
    [TA_PARAM_INTERPRETER_NESTING] = { "interpreterNesting", "interpreters", TA_PARAM_DEFAULT_INTERPRETER_NESTING, 1,
                                       64 },
    [TA_PARAM_MAX_ACTORS] = { "maxActors", "actors", TA_PARAM_DEFAULT_MAX_ACTORS, 1, 2147483647u },
    [TA_PARAM_MAX_PENDING_SENDS] = { "maxPendingSends", "sends", TA_PARAM_DEFAULT_MAX_PENDING_SENDS, 1, 2147483647u },
    [TA_PARAM_MAX_DEFINITIONS] = { "maxDefinitions", "definitions", TA_PARAM_DEFAULT_MAX_DEFINITIONS, 1, 2147483647u },
};

ta_result ta_param_info_of(ta_param param, ta_param_info *out) {
    if (out == NULL || (uint32_t)param >= (uint32_t)TA_PARAM_COUNT) {
        return TA_ERR_INVALID;
    }
    const ta_param_row *row = &ta_param_rows[param];
    *out = (ta_param_info){ .name = ta_str_from_cstr(row->name),
                            .default_value = row->default_value,
                            .min = row->min,
                            .max = row->max,
                            .unit = ta_str_from_cstr(row->unit) };
    return TA_OK;
}

ta_result ta_system_param(const ta_system *system, ta_param param, uint64_t *out) {
    if (system == NULL || out == NULL || (uint32_t)param >= (uint32_t)TA_PARAM_COUNT) {
        return TA_ERR_INVALID;
    }
    *out = system->params[param];
    return TA_OK;
}

/* Every parameter's value: the defaults, then the config's, each checked
 * against its bounds once. TA_ERR_INVALID names the parameter at fault. */
static ta_result ta_params_resolve(const ta_system_config *config, uint32_t values[TA_PARAM_COUNT]) {
    bool given[TA_PARAM_COUNT] = { false };
    for (uint32_t index = 0; index < (uint32_t)TA_PARAM_COUNT; index += 1u) {
        values[index] = ta_param_rows[index].default_value;
    }
    if (config->param_count != 0 && config->params == NULL) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < config->param_count; index += 1u) {
        const ta_param_value *value = &config->params[index];
        uint32_t param = (uint32_t)value->param;
        if (param >= (uint32_t)TA_PARAM_COUNT) {
            return TA_ERR_INVALID;
        }
        const ta_param_row *row = &ta_param_rows[param];
        if (given[param] || value->value < row->min || value->value > row->max) {
            if (config->rejected != NULL) {
                *config->rejected = value->param;
            }
            return TA_ERR_INVALID;
        }
        given[param] = true;
        values[param] = (uint32_t)value->value;
    }
    return TA_OK;
}

/* Bytes of one interpreter: its structural region and its render area,
 * each a fixed arena, the render area starting aligned. */
static size_t ta_interpreter_region_bytes(const uint32_t params[TA_PARAM_COUNT]) {
    size_t structural = TA_ALIGN_UP(ta_arena_fixed_bytes(params[TA_PARAM_INTERPRETER_BYTES]),
                                    (size_t)_Alignof(ta_arena_block));
    return structural + ta_arena_fixed_bytes(params[TA_PARAM_RENDER_BYTES]);
}

/* Lays a region's two fixed arenas over its memory. */
static void ta_interpreter_region_open(const ta_system *system, void *memory, ta_arena *scratch, ta_arena *render) {
    size_t structural = TA_ALIGN_UP(ta_arena_fixed_bytes(system->params[TA_PARAM_INTERPRETER_BYTES]),
                                    (size_t)_Alignof(ta_arena_block));
    ta_arena_init_fixed(scratch, memory, structural);
    ta_arena_init_fixed(render, (unsigned char *)memory + structural,
                        ta_arena_fixed_bytes(system->params[TA_PARAM_RENDER_BYTES]));
}

ta_result ta_system_nested_region(ta_system *system, uint32_t depth, ta_arena *scratch, ta_arena *render) {
    if (depth == 0 || depth >= system->params[TA_PARAM_INTERPRETER_NESTING]) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_nested_region *region = &system->nested[depth - 1u];
    if (region->memory == NULL) {
        size_t bytes = ta_interpreter_region_bytes(system->params);
        region->memory = ta_mem_alloc(system->allocator, bytes, _Alignof(ta_arena_block));
        if (region->memory == NULL) {
            return TA_ERR_NOMEM;
        }
        region->bytes = bytes;
    }
    ta_interpreter_region_open(system, region->memory, scratch, render);
    return TA_OK;
}

/*
 * The memory every run needs, allocated once (ADR 0092 section 3):
 * interpreter region 0 with its render area, then the table of nested
 * regions; and the event-block cache, filled to its parameter.
 */
static ta_result ta_system_arena_open(ta_system *system) {
    size_t region = ta_interpreter_region_bytes(system->params);
    size_t nested = (size_t)(system->params[TA_PARAM_INTERPRETER_NESTING] - 1u) * sizeof(ta_nested_region);
    size_t bytes = TA_ALIGN_UP(region, (size_t)_Alignof(ta_nested_region)) + nested;
    system->arena_memory = ta_mem_alloc(system->allocator, bytes, _Alignof(ta_arena_block));
    if (system->arena_memory == NULL) {
        return TA_ERR_NOMEM;
    }
    system->arena_bytes = bytes;
    ta_interpreter_region_open(system, system->arena_memory, &system->scratch, &system->render);
    system->nested = (ta_nested_region *)((unsigned char *)system->arena_memory +
                                          TA_ALIGN_UP(region, (size_t)_Alignof(ta_nested_region)));
    for (uint32_t index = 0; index + 1u < system->params[TA_PARAM_INTERPRETER_NESTING]; index += 1u) {
        system->nested[index] = (ta_nested_region){ 0 };
    }
    return TA_OK;
}

static void ta_system_arena_close(ta_system *system) {
    if (system->arena_memory == NULL) {
        return;
    }
    for (uint32_t index = 0; index + 1u < system->params[TA_PARAM_INTERPRETER_NESTING]; index += 1u) {
        ta_nested_region *region = &system->nested[index];
        if (region->memory != NULL) {
            ta_mem_free(system->allocator, region->memory,
                        &(ta_allocator_free_params){ .size = region->bytes, .alignment = _Alignof(ta_arena_block) });
        }
    }
    ta_mem_free(system->allocator, system->arena_memory,
                &(ta_allocator_free_params){ .size = system->arena_bytes, .alignment = _Alignof(ta_arena_block) });
    system->arena_memory = NULL;
    system->scratch = (ta_arena){ 0 };
    system->render = (ta_arena){ 0 };
}

bool ta_system_quota_actor(ta_system *system) {
    if (system->live_actors >= system->params[TA_PARAM_MAX_ACTORS]) {
        system->counters.quota_refusals += 1u;
        return false;
    }
    return true;
}

ta_result ta_system_create(const ta_system_config *config, ta_system **out) {
    if (config == NULL || out == NULL || config->allocator.v == NULL ||
        config->allocator.v->alloc == NULL || config->allocator.v->free == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t params[TA_PARAM_COUNT];
    ta_result resolved = ta_params_resolve(config, params);
    if (resolved != TA_OK) {
        return resolved;
    }
    ta_system *system = ta_mem_alloc(config->allocator, sizeof(ta_system), _Alignof(ta_system));
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    *system = (ta_system){ .allocator = config->allocator,
                           .incarnation = config->incarnation,
                           .block_cache_limit = params[TA_PARAM_EVENT_BLOCK_CACHE],
                           .trace = ta_null_trace_sink(),
                           .definition_sink = ta_null_definition_sink() };
    ta_mem_copy(system->params, params, sizeof params);
    ta_system_seed_trace_ids(system, 0);
    ta_clock_set(&system->clock, config->initial_time);
    ta_vec_init(&system->actors.pages, config->allocator, TA_VEC_LAYOUT(ta_actor_slot *));
    system->free_actor_slot = UINT32_MAX;
    ta_vec_init(&system->definitions, config->allocator, TA_VEC_LAYOUT(ta_definition_slot));
    system->free_definition_slot = UINT32_MAX;
    ta_vec_init(&system->io_processors, config->allocator, TA_VEC_LAYOUT(ta_io_registration));
    ta_vec_init(&system->invokers, config->allocator, TA_VEC_LAYOUT(ta_invoker_registration));
    ta_address_table_init(&system->addresses, config->allocator);
    ta_io_origin_table_init(&system->io_scopes, config->allocator, true);
    ta_io_origin_table_init(&system->io_messages, config->allocator, false);
    ta_result result = ta_session_table_init(&system->sessions, config->allocator);
    if (result == TA_OK) {
        result = ta_atom_table_init(&system->atoms, config->allocator);
    }
    if (result != TA_OK) {
        ta_session_table_destroy(&system->sessions);
        ta_mem_free(config->allocator, system,
                    &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
        return result;
    }
    result = ta_event_name_table_init(&system->event_names, config->allocator, &system->atoms);
    if (result == TA_OK) {
        system->event_names.max_depth = params[TA_PARAM_EVENT_NAME_DEPTH];
        result = ta_timer_heap_init(&system->timers, config->allocator);
    }
    if (result == TA_OK) {
        result = ta_system_arena_open(system);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_str_from_cstr(TA_SCXML_IO_URI), &system->scxml_io_type);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_str_from_cstr("scxml"), &system->scxml_io_alias);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_str_from_cstr("http://www.w3.org/TR/scxml/"),
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
        ta_actor_slot *entry = ta_actor_slot_at(system, slot);
        if (entry->actor != NULL) {
            (void)ta_actor_destroy(system, ta_actor_id_make(slot, entry->generation));
        }
    }
    while (system->unregistered_definitions != NULL) {
        ta_definition_destroy(system->unregistered_definitions);
    }
    /* Every actor is gone, so every definition goes, retired or not, and
     * the host hears of each while it is still intact (ADR 0079). */
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition != NULL) {
            ta_system_tell_definition(system, entry->definition, TA_DEFINITION_EVENT_FREED);
            ta_definition_destroy(entry->definition);
            entry->definition = NULL;
        }
    }
    ta_vec_destroy(&system->definitions);
    ta_timer_heap_destroy(&system->timers);
    ta_actor_table_destroy(&system->actors, system->allocator);
    /* After the actors: their held events released their MESSAGE origins. */
    ta_io_origin_table_destroy(&system->io_scopes);
    ta_io_origin_table_destroy(&system->io_messages);
    ta_vec_destroy(&system->io_processors);
    ta_vec_destroy(&system->invokers);
    system->block_cache_limit = 0;
    ta_interpreter_block_cache_trim(system);
    ta_system_arena_close(system);
    ta_interpreter_release_spares(system);
    /* Unloaded sessions hold addresses; give them back before the table goes. */
    ta_session_table_destroy(&system->sessions);
    /* Last: everything above gave its addresses back. */
    ta_address_table_destroy(&system->addresses);
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

/* ------------------------------------------------------------------------
 * The definition sink (ADR 0079)
 */

static void ta_null_definition_notify(void *self, const ta_definition_event *event) {
    (void)self;
    (void)event;
}

static const ta_definition_sink_vtable ta_null_definition_vtable = { .notify = ta_null_definition_notify };

ta_definition_sink ta_null_definition_sink(void) {
    return (ta_definition_sink){ .self = NULL, .v = &ta_null_definition_vtable };
}

ta_result ta_system_set_definition_sink(ta_system *system, ta_definition_sink sink) {
    if (system == NULL || sink.v == NULL || sink.v->notify == NULL) {
        return TA_ERR_INVALID;
    }
    system->definition_sink = sink;
    return TA_OK;
}

/*
 * One definition event, told twice (ADR 0080): first as a lifecycle record
 * to the trace sink, when its mask asks for the kind, then to the definition
 * sink (ADR 0079). The record goes first, so a trace sink reading the
 * definition finds it as intact as the definition sink does, and before the
 * host's cleanup of it.
 */
void ta_system_tell_definition(ta_system *system, const ta_definition *definition, ta_definition_event_kind kind) {
    static const ta_trace_kind trace_kinds[] = {
        [TA_DEFINITION_EVENT_REGISTERED] = TA_TRACE_DEFINITION_REGISTERED,
        [TA_DEFINITION_EVENT_RETIRED] = TA_TRACE_DEFINITION_RETIRED,
        [TA_DEFINITION_EVENT_FREED] = TA_TRACE_DEFINITION_FREED,
    };
    ta_trace_kind trace_kind = trace_kinds[kind];
    ta_trace_context none = { 0 };
    if (ta_trace_lifecycle_wanted(&system->trace, trace_kind, &none)) {
        ta_trace_record record = ta_trace_record_blank(trace_kind);
        /* One key per definition and event: a definition ID is never issued
         * twice (ADR 0043), and each event happens at most once to it. */
        record.key = (ta_step_key){ .session = TA_TRACE_DEFINITIONS_SESSION,
                                    .macrostep = definition->id,
                                    .microstep = (uint32_t)kind + 1u };
        record.definition = definition->id;
        record.identity = definition->identity;
        record.now = ta_clock_now(&system->clock);
        ta_trace_write(&system->trace, &record);
    }
    ta_definition_event event = { .kind = kind, .definition = definition->id, .identity = definition->identity };
    system->definition_sink.v->notify(system->definition_sink.self, &event);
}

ta_result ta_system_set_external_event_router(ta_system *system, ta_external_event_router router) {
    if (system == NULL || (router.v != NULL && router.v->submit == NULL)) {
        return TA_ERR_INVALID;
    }
    system->external_events = router;
    return TA_OK;
}

static bool ta_bytes_same(ta_bytes left, ta_bytes right);

/* The newest registered, non-retired definition with this identity, or NULL. */
static ta_definition *ta_system_live_definition(const ta_system *system, const ta_definition_identity *identity) {
    /* Slots are reused, so registration order decides: the newest match wins. */
    ta_definition *best = NULL;
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        ta_definition *definition = entry->definition;
        if (definition == NULL || definition->retired ||
            (best != NULL && best->registration_serial > definition->registration_serial)) {
            continue;
        }
        if (ta_str_equal(definition->identity.family.name, identity->family.name) &&
            ta_bytes_same(definition->identity.revision, identity->revision)) {
            best = definition;
        }
    }
    return best;
}

/*
 * Whether two definitions of the same content hold the same handles
 * (ADR 0087): every host expression's (v, self, value), the value being
 * what `retain` returned, is equal as pointers. Equal content hashes mean
 * the host expressions sit in the same places, so the tables line up.
 */
static bool ta_definition_same_handles(const ta_definition *left, const ta_definition *right) {
    if (left->expression_count != right->expression_count) {
        return false;
    }
    for (uint32_t index = 0; index < left->expression_count; index += 1u) {
        const ta_expression *a = &left->expressions[index];
        const ta_expression *b = &right->expressions[index];
        if (a->kind != b->kind) {
            return false;
        }
        if (a->kind == TA_EXPRESSION_HOST &&
            (a->as.host.v != b->as.host.v || a->as.host.self != b->as.host.self || a->as.host.value != b->as.host.value)) {
            return false;
        }
    }
    return true;
}

/*
 * The live definition that answers `definition`'s registration, or NULL to
 * register it beside the others (ADR 0077, ADR 0087). Every live
 * definition with its identity is looked at: other content under it is
 * TA_ERR_DEFINITION_MISMATCH, whatever the data-model adapter; a match has
 * the same adapter and the same handles, and the newest match answers.
 */
static ta_result ta_system_match_definition(const ta_system *system, const ta_definition *definition,
                                            ta_definition **out) {
    ta_definition *best = NULL;
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1u) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        ta_definition *live = entry->definition;
        if (live == NULL || live->retired || !ta_str_equal(live->identity.family.name, definition->identity.family.name) ||
            !ta_bytes_same(live->identity.revision, definition->identity.revision)) {
            continue;
        }
        if (live->content_hash != definition->content_hash) {
            return TA_ERR_DEFINITION_MISMATCH;
        }
        /* The same chart compiled against another data-model adapter is
         * another definition: it runs differently, so it registers beside. */
        if (live->datamodel.self != definition->datamodel.self || live->datamodel.v != definition->datamodel.v) {
            continue;
        }
        if (live->host_expressions && !ta_definition_same_handles(live, definition)) {
            continue;
        }
        if (best == NULL || live->registration_serial > best->registration_serial) {
            best = live;
        }
    }
    *out = best;
    return TA_OK;
}

ta_result ta_system_register_definition(ta_system *system, ta_definition *definition, ta_definition_id *out) {
    if (system == NULL || definition == NULL || out == NULL || definition->system != system ||
        definition->registered) {
        return TA_ERR_INVALID;
    }
    /*
     * Idempotent by identity (ADR 0077): a live definition with this
     * identity, the same content and the same handles is the one registered,
     * and it gains a hold. The same identity for other content is refused
     * rather than silently answered with the old chart.
     */
    ta_definition *live = NULL;
    ta_result result = ta_system_match_definition(system, definition, &live);
    if (result != TA_OK) {
        return result;
    }
    if (live != NULL) {
        /* Where the chart is written now (ADR 0085): the spans of the
         * registration that asked, when it brought any. */
        ta_definition_take_spans(live, definition);
        live->holds += 1u;
        *out = live->id;
        ta_definition_destroy(definition);
        return TA_OK;
    }
    if (system->live_definitions >= system->params[TA_PARAM_MAX_DEFINITIONS]) {
        /* A quota, answered as an allocator that ran out (ADR 0092 section 4). */
        system->counters.quota_refusals += 1u;
        return TA_ERR_NOMEM;
    }
    uint32_t slot = system->free_definition_slot;
    if (slot == UINT32_MAX) {
        ta_definition_slot fresh = { .generation = 1, .next_free = UINT32_MAX };
        result = ta_vec_push(&system->definitions, &fresh);
        if (result != TA_OK) {
            return result;
        }
        slot = system->definitions.count - 1u;
    }
    ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    if (slot == system->free_definition_slot) {
        system->free_definition_slot = entry->next_free;
    }
    if (definition->occupancy == NULL && definition->state_count != 0) {
        definition->occupancy = ta_arena_alloc(&definition->arena, (size_t)definition->state_count * sizeof(uint32_t),
                                               _Alignof(uint32_t));
        if (definition->occupancy == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_zero(definition->occupancy, (size_t)definition->state_count * sizeof(uint32_t));
    }
    entry->definition = definition;
    entry->next_free = UINT32_MAX;
    system->live_definitions += 1u;
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
    definition->holds = 1u;
    definition->registration_serial = system->next_definition_serial++;
    *out = definition->id;
    ta_system_tell_definition(system, definition, TA_DEFINITION_EVENT_REGISTERED);
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

static bool ta_definition_unused(const ta_definition *definition) {
    return definition->holds == 0 && definition->holders == 0 && definition->actor_count == 0;
}

/*
 * Frees `definition` if nothing uses it any more (ADR 0077): no hold, no
 * definition nesting it, and no resident actor. Freeing a definition drops
 * its hold on each definition it nests, which may free those in turn; the
 * cascade runs as a worklist, not by recursion (ADR 0060). It runs in two
 * passes (ADR 0079): the first collects the cascade, the second tells the
 * host and frees in reverse order of discovery. A nested definition is
 * discovered after the one nesting it, so leaves go first, and each is
 * intact when the host hears of it.
 */
void ta_system_settle_definition(ta_system *system, ta_definition *definition) {
    ta_definition *pending = ta_definition_unused(definition) ? definition : NULL;
    if (pending != NULL) {
        pending->free_next = NULL;
    }
    ta_definition *doomed = NULL;
    while (pending != NULL) {
        ta_definition *freeing = pending;
        pending = freeing->free_next;
        for (uint32_t index = 0; freeing->nested != NULL && index < freeing->invoke_decl_count; index += 1u) {
            ta_definition *child = ta_system_definition(system, freeing->nested[index]);
            if (child == NULL) {
                continue;
            }
            child->holders -= 1u;
            if (ta_definition_unused(child)) {
                child->free_next = pending;
                pending = child;
            }
        }
        /* Out of the worklist, so its link can thread the second pass. */
        freeing->free_next = doomed;
        doomed = freeing;
    }
    while (doomed != NULL) {
        ta_definition *freeing = doomed;
        doomed = freeing->free_next;
        ta_system_tell_definition(system, freeing, TA_DEFINITION_EVENT_FREED);
        uint32_t slot = (uint32_t)(freeing->id & 0xffffffffu);
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        /* The slot's next occupant gets a new generation, so this ID goes stale. */
        entry->definition = NULL;
        system->live_definitions -= 1u;
        entry->generation += 1u;
        entry->next_free = system->free_definition_slot;
        system->free_definition_slot = slot;
        ta_definition_destroy(freeing);
    }
}

ta_result ta_system_release_definition(ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, id);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (definition->holds == 0) {
        return TA_ERR_INVALID;
    }
    definition->holds -= 1u;
    ta_system_settle_definition(system, definition);
    return TA_OK;
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
    if (!definition->retired) {
        definition->retired = true;
        ta_system_tell_definition(system, definition, TA_DEFINITION_EVENT_RETIRED);
    }
    definition->holds = 0;
    ta_system_settle_definition(system, definition);
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
    const ta_definition *live = ta_system_live_definition(system, identity);
    if (live == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    *out = live->id;
    return TA_OK;
}

ta_result ta_system_definitions(const ta_system *system, const ta_system_definitions_params *params) {
    if (system == NULL || params == NULL || params->count == NULL) {
        if (params != NULL && params->count != NULL) {
            *params->count = 0;
        }
        return TA_ERR_INVALID;
    }
    /* Slots in increasing order: a definition held throughout a walk keeps
     * its slot, so it is listed exactly once (ADR 0078). */
    uint32_t count = 0;
    for (uint32_t slot = params->from; slot < system->definitions.count; slot += 1u) {
        if (params->limit != 0 && count == params->limit) {
            break;
        }
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition == NULL) {
            continue;
        }
        if (params->definitions != NULL && count < params->capacity) {
            params->definitions[count] = entry->definition->id;
        }
        count += 1u;
    }
    *params->count = count;
    return count > params->capacity ? TA_ERR_NOMEM : TA_OK;
}

bool ta_system_is_scxml_io(const ta_system *system, ta_atom type) {
    return type == system->scxml_io_type || type == system->scxml_io_alias;
}

const ta_io *ta_system_io_of(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->io_processors.count; index += 1) {
        const ta_io_registration *entry = ta_vec_at(&system->io_processors, index);
        if (entry->type == type) {
            return &entry->io;
        }
    }
    return NULL;
}

const ta_io_registration *ta_system_io_at(const ta_system *system, uint32_t index) {
    if (index == 0 || index > system->io_processors.count) {
        return NULL;
    }
    return ta_vec_at(&system->io_processors, index - 1u);
}

ta_result ta_system_find_io(const ta_system *system, ta_atom type, uint32_t *index) {
    if (system == NULL || index == NULL) {
        return TA_ERR_INVALID;
    }
    if (ta_system_is_scxml_io(system, type)) {
        *index = 0;
        return TA_OK;
    }
    for (uint32_t at = 0; at < system->io_processors.count; at += 1) {
        const ta_io_registration *entry = ta_vec_at(&system->io_processors, at);
        if (entry->type == type) {
            *index = at + 1u;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/* Registering a type again replaces its adapter at the same index (ADR 0068). */
ta_result ta_system_register_io(ta_system *system, const ta_io_registration_params *params) {
    if (system == NULL || params == NULL || params->io.v == NULL || params->io.v->send == NULL ||
        !ta_atom_valid(&system->atoms, params->type) || ta_system_is_scxml_io(system, params->type)) {
        return TA_ERR_INVALID;
    }
    uint32_t index = 0;
    if (ta_system_find_io(system, params->type, &index) == TA_OK) {
        ta_io_registration *entry = ta_vec_at(&system->io_processors, index - 1u);
        entry->io = params->io;
    } else {
        ta_io_registration entry = { .type = params->type, .io = params->io };
        ta_result result = ta_vec_push(&system->io_processors, &entry);
        if (result != TA_OK) {
            return result;
        }
        index = system->io_processors.count;
    }
    if (params->index != NULL) {
        *params->index = index;
    }
    return TA_OK;
}

uint32_t ta_system_invoker_index(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            return index;
        }
    }
    return UINT32_MAX;
}

ta_atom ta_system_invoker_type(const ta_system *system, uint32_t index) {
    if (index >= system->invokers.count) {
        return TA_ATOM_NONE;
    }
    const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
    return entry->type;
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

uint64_t ta_system_incarnation(const ta_system *system) {
    return system == NULL ? 0 : system->incarnation;
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
        ta_actor *owner = ta_actor_lookup(system, send.owner);
        /*
         * A pending send leaves its owner's list as it fires, and an image
         * captures that list, so an image taken before no longer describes
         * the owner (ADR 0014). A submitted send is in no image.
         */
        if (owner != NULL && !send.submitted) {
            ta_actor_touch(owner);
        }
        const ta_definition *owner_definition = owner != NULL ? owner->definition : NULL;
        ta_external_event_submit_status fired = ta_interpreter_dispatch_delayed(system, &send, false);
        if (fired != TA_EXTERNAL_EVENT_RETRY) {
            if (owner_definition != NULL) {
                TA_COUNT(system, owner_definition, delayed_fired);
            } else {
                system->counters.delayed_fired += 1u;
            }
        }
        if (fired == TA_EXTERNAL_EVENT_RETRY) {
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

/* ------------------------------------------------------------------------
 * Counters and occupancy (ADR 0072)
 */

ta_result ta_definition_counters(const ta_system *system, ta_definition_id definition, ta_counters *out) {
    const ta_definition *found = ta_system_definition(system, definition);
    if (found == NULL || out == NULL) {
        return out == NULL ? TA_ERR_INVALID : TA_ERR_NOT_FOUND;
    }
    *out = found->counters;
    return TA_OK;
}

ta_result ta_definition_set_microstep_limit(ta_system *system, ta_definition_id definition, uint32_t limit) {
    ta_definition *found = system == NULL ? NULL : ta_system_definition(system, definition);
    if (found == NULL) {
        return system == NULL ? TA_ERR_INVALID : TA_ERR_NOT_FOUND;
    }
    found->microstep_limit = limit;
    return TA_OK;
}

void ta_system_counters(const ta_system *system, ta_counters *out) {
    if (system != NULL && out != NULL) {
        *out = system->counters;
    }
}

ta_result ta_definition_occupancy(const ta_system *system, const ta_definition_occupancy_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL) {
        *params->count = 0;
        return TA_ERR_NOT_FOUND;
    }
    *params->count = definition->state_count;
    if (params->states == NULL || params->capacity < definition->state_count) {
        return TA_ERR_NOMEM;
    }
    if (definition->state_count != 0) {
        ta_mem_copy(params->states, definition->occupancy, (size_t)definition->state_count * sizeof(uint32_t));
    }
    return TA_OK;
}
