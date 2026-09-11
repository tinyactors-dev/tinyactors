#include "internal.h"

static size_t aligned_size(size_t size, size_t alignment)
{
    size_t padding = (alignment - size % alignment) % alignment;
    return size > (size_t)-1 - padding ? (size_t)-1 : size + padding;
}

void ta_definition_footprint(const ta_definition_t *definition, ta_actor_footprint_t *out)
{
    size_t alignment = TAI_ALIGNMENT, bytes = 0, control;
    if (out == NULL) return;
    *out = (ta_actor_footprint_t){0};
    if (definition == NULL) return;
    if (definition->description.data_layout.storage == TA_DATA_BYTES) {
        bytes = definition->description.data_layout.bytes_layout.size;
        if (definition->description.data_layout.bytes_layout.alignment > alignment)
            alignment = definition->description.data_layout.bytes_layout.alignment;
    }
    /* The allocator promises arena-header alignment. Reserve worst-case extra
     * padding for the stronger payload alignment in that same first chunk. */
    control = tai_arena_control_bytes();
    if (control > (size_t)-1 - alignment + 1) control = (size_t)-1;
    else control += alignment - 1;
    if (control > (size_t)-1 - aligned_size(sizeof(tai_actor_t), alignment)) control = (size_t)-1;
    else control += aligned_size(sizeof(tai_actor_t), alignment);
    out->control_bytes = control;
    out->state_bytes = bytes;
    out->minimum_chunk_bytes = bytes > (size_t)-1 - control ? (size_t)-1 : control + bytes;
}

ta_status_t tai_actor_scope(tai_actor_t *actor, ta_lifetime_t lifetime, ta_arena_t **out)
{
    if (actor == NULL || (unsigned)lifetime >= TA_LIFETIME_COUNT) return TA_INVALID;
    return ta_arena_open(actor->arena, &actor->system->config.arenas[lifetime], out);
}

ta_status_t tai_extra(tai_actor_t *actor)
{
    ta_status_t status;
    ta_arena_t *arena;
    if (actor->extra != NULL) return TA_OK;
    status = tai_actor_scope(actor, TA_LIFETIME_ACTOR, &arena);
    if (status != TA_OK) return status;
    status = tai_alloc(arena, sizeof(*actor->extra), (void **)&actor->extra);
    if (status == TA_OK) {
        *actor->extra = (tai_actor_extra_t){0};
        actor->extra->arena = arena;
    } else ta_arena_close(arena);
    return status;
}

ta_text_t tai_session(tai_actor_t *actor, uint8_t location)
{
    ta_system_t *system = actor->system;
    char *buffer = system->identity_buffer;
    char digits[20];
    size_t length = 0, count = 0;
    uint64_t id = actor->id;
    if (location) { tai_copy(buffer, "#_scxml_", 8); length = 8; }
    tai_copy(buffer + length, system->config.session_prefix.data, system->config.session_prefix.size);
    length += system->config.session_prefix.size;
    buffer[length++] = '.';
    do { digits[count++] = (char)('0' + id % 10); id /= 10; } while (id != 0);
    while (count != 0) buffer[length++] = digits[--count];
    return (ta_text_t){buffer, length};
}

static void drop_actor_data(void *argument)
{
    tai_actor_t *actor = (tai_actor_t *)argument;
    if (actor->data.storage == TA_DATA_NATIVE && actor->data.as.native.handle != 0)
        actor->system->config.values.release(actor->system->config.host_user, actor->data.as.native);
    ta_definition_release(actor->definition);
    ta_definition_release(actor->identity_definition);
}

static ta_status_t spawn_input(tai_actor_t *actor, const ta_spawn_options_t *options)
{
    ta_arena_t *arena;
    tai_initial_input_t *input;
    tai_value_scope_t scope;
    ta_status_t status;
    ta_body_t parameters = {.kind=TA_BODY_PARAMETERS,
        .parameters=options->parameters,.parameter_count=options->parameter_count};
    if (options->seed.handle == 0 && options->parameter_count == 0 && options->trace == NULL)
        return TA_OK;
    status = tai_actor_scope(actor, TA_LIFETIME_MESSAGE, &arena);
    if (status != TA_OK) return status;
    status = tai_alloc(arena, sizeof(*input), (void **)&input);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    *input = (tai_initial_input_t){0};
    input->arena = arena;
    scope = (tai_value_scope_t){actor->system, arena};
    status = tai_value_snapshot(&scope, options->seed, &input->seed);
    if (status == TA_OK) status = tai_body_snapshot(&scope, &parameters, &input->parameters);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    if (tai_trace_copy(&scope, options->trace, &input->trace) != TA_OK)
        tai_saturate(&actor->system->stats.dropped_trace_records);
    actor->initial = input;
    return TA_OK;
}

typedef struct actor_admission {
    const ta_spawn_options_t *options;
    ta_definition_t *definition;
    const ta_actor_limits_t *limits;
} actor_admission_t;

static ta_status_t allocate_actor(ta_system_t *system, const actor_admission_t *admit,
                                  tai_actor_t **out)
{
    const ta_data_layout_t *data = &admit->definition->description.data_layout;
    ta_arena_options_t options = system->config.arenas[TA_LIFETIME_ACTOR];
    ta_actor_footprint_t footprint;
    ta_layout_t layout = {sizeof(tai_actor_t), TAI_ALIGNMENT};
    ta_arena_t *arena;
    tai_actor_t *actor;
    ta_status_t status;
    size_t offset;
    ta_definition_footprint(admit->definition, &footprint);
    if (footprint.minimum_chunk_bytes == (size_t)-1 ||
        footprint.minimum_chunk_bytes > options.maximum_chunk_bytes ||
        footprint.minimum_chunk_bytes > admit->limits->maximum_bytes) return TA_LIMIT;
    if (options.maximum_bytes > admit->limits->maximum_bytes) options.maximum_bytes = admit->limits->maximum_bytes;
    if (options.initial_chunk_bytes < footprint.minimum_chunk_bytes)
        options.initial_chunk_bytes = footprint.minimum_chunk_bytes;
    if (data->storage == TA_DATA_BYTES && data->bytes_layout.alignment > layout.alignment)
        layout.alignment = data->bytes_layout.alignment;
    offset = aligned_size(sizeof(*actor), layout.alignment);
    layout.size = offset + footprint.state_bytes;
    status = ta_arena_open(system->arena, &options, &arena);
    if (status != TA_OK) return status;
    status = ta_arena_alloc(arena, &layout, (void **)&actor);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    *actor = (tai_actor_t){0};
    actor->arena = arena;
    actor->system = system;
    actor->id = system->next_actor;
    actor->limits = &system->config.actor_defaults;
    actor->leaf = TA_INDEX_NONE;
    actor->status = TA_ACTOR_STARTING;
    actor->data.storage = data->storage;
    if (data->storage == TA_DATA_BYTES) actor->data.as.bytes = (ta_bytes_t){NULL,0};
    if (data->storage == TA_DATA_BYTES && data->bytes_layout.size != 0) {
        ta_bytes_t image = admit->options->initial_bytes.data != NULL ? admit->options->initial_bytes : data->initial_bytes;
        void *bytes = (unsigned char *)actor + offset;
        actor->data.as.bytes = (ta_bytes_t){bytes, data->bytes_layout.size};
        if (image.data != NULL) tai_copy(bytes, image.data, image.size);
        else tai_zero(bytes, data->bytes_layout.size);
    }
    actor->definition = actor->identity_definition = admit->definition;
    ta_definition_retain(actor->definition);
    ta_definition_retain(actor->identity_definition);
    if (admit->options->limits != NULL) {
        status = tai_extra(actor);
        if (status == TA_OK) {
            actor->extra->custom_limits = *admit->limits;
            actor->limits = &actor->extra->custom_limits;
        }
    }
    if (status == TA_OK) status = spawn_input(actor, admit->options);
    if (status != TA_OK) { drop_actor_data(actor); ta_arena_close(arena); return status; }
    *out = actor;
    return TA_OK;
}

ta_status_t tai_spawn_actor(ta_system_t *system, const ta_spawn_options_t *options,
                           ta_actor_ref_t *out)
{
    actor_admission_t admit;
    tai_actor_t *actor;
    tai_registry_t *entry;
    ta_status_t status;
    const ta_data_layout_t *layout;
    if (out == NULL) return TA_INVALID;
    *out = (ta_actor_ref_t){0};
    if (system == NULL || options == NULL) return TA_INVALID;
    if (system->shutdown) return TA_CANCELLED;
    if (system->stats.actors >= system->config.limits.actors || system->next_actor == 0) return TA_LIMIT;
    if ((options->definition == NULL) == (options->definition_key.data == NULL)) return TA_INVALID;
    admit.options = options;
    admit.definition = options->definition;
    if (admit.definition == NULL) {
        if (!tai_text_valid(options->definition_key, 0)) return TA_INVALID;
        entry = tai_registry_find(system->definition_registry, options->definition_key);
        if (entry == NULL) return TA_NOT_FOUND;
        admit.definition = entry->definition;
    }
    if (admit.definition->system != system ||
        (options->parameter_count != 0 && options->parameters == NULL) ||
        (options->initial_bytes.data == NULL && options->initial_bytes.size != 0)) return TA_INVALID;
    layout = &admit.definition->description.data_layout;
    if (layout->storage == TA_DATA_BYTES) {
        if (options->seed.handle != 0 ||
            (options->initial_bytes.data != NULL && options->initial_bytes.size != layout->bytes_layout.size)) return TA_INVALID;
    } else if (options->initial_bytes.data != NULL || options->initial_bytes.size != 0) return TA_INVALID;
    if (!tai_text_valid(options->name, 1)) return TA_INVALID;
    if (options->name.data != NULL && options->name.size == 0) return TA_INVALID;
    if (options->name.data != NULL && tai_registry_find(system->names, options->name) != NULL) return TA_CONFLICT;
    admit.limits = options->limits != NULL ? options->limits : &system->config.actor_defaults;
    status = allocate_actor(system, &admit, &actor);
    if (status != TA_OK) return status;
    tai_actor_insert(system, actor);
    if (options->name.data != NULL) {
        ta_name_binding_t binding = {options->name, {0}};
        status = ta_name_bind(system, &binding, (ta_actor_ref_t){actor->id});
        if (status != TA_OK) { tai_actor_remove(system, actor); drop_actor_data(actor); ta_arena_close(actor->arena); return status; }
    }
    ++system->stats.actors;
    if (system->next_actor == UINT64_MAX) system->next_actor = 0; else ++system->next_actor;
    tai_ready(actor);
    tai_mutated(system);
    *out = (ta_actor_ref_t){actor->id};
    return TA_OK;
}

ta_status_t ta_actor_spawn(ta_system_t *system, const ta_spawn_options_t *options, ta_actor_ref_t *out)
{
    if (system != NULL && system->running) { if (out != NULL) *out = (ta_actor_ref_t){0}; return TA_BUSY; }
    return tai_spawn_actor(system, options, out);
}

void tai_release_child(tai_actor_t *actor)
{
    ta_system_t *system = actor->system;
    tai_unready(actor);
    tai_actor_remove(system, actor);
    --system->stats.actors;
    system->stats.queued_events -= actor->external_count + actor->internal_count;
    drop_actor_data(actor);
    ta_arena_close(actor->arena);
    tai_mutated(system);
}

void tai_actor_info(tai_actor_t *actor, ta_actor_info_t *out)
{
    *out = (ta_actor_info_t){0};
    out->status = actor->status;
    out->reason = actor->reason;
    out->revision = actor->definition->revision;
    out->sessionid = tai_session(actor, 0);
    if (actor->name != NULL) out->registered_name = actor->name->key;
    out->state = actor->data;
    out->stable = actor->stable;
    out->external_events = actor->external_count;
    out->internal_events = actor->internal_count;
    if (actor->extra != NULL) {
        out->delayed_sends = actor->extra->delayed_count;
        out->invocations = actor->extra->invocation_count;
    }
    ta_arena_stats(actor->arena, &out->memory);
}

ta_status_t ta_actor_inspect(ta_system_t *system, ta_actor_ref_t ref, ta_actor_info_t *out)
{
    tai_actor_t *actor;
    if (system == NULL || out == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    actor = tai_actor_find(system, ref);
    if (actor == NULL) return TA_STALE;
    tai_actor_info(actor, out);
    return TA_OK;
}

ta_status_t ta_actor_set_limits(ta_system_t *system, ta_actor_ref_t ref,
                                const ta_actor_limits_t *limits)
{
    tai_actor_t *actor;
    tai_actor_extra_t *extra;
    ta_status_t status;
    ta_memory_stats_t memory;
    if (system == NULL || limits == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    actor = tai_actor_find(system, ref);
    if (actor == NULL) return TA_STALE;
    extra = actor->extra;
    ta_arena_stats(actor->arena, &memory);
    if (limits->maximum_bytes < memory.reserved_bytes ||
        limits->external_events < actor->external_count ||
        limits->internal_events < actor->internal_count - (actor->error_slot != 0) ||
        limits->queued_event_bytes < actor->queued_bytes ||
        limits->microsteps_per_macrostep < actor->microsteps ||
        limits->actions_per_macrostep < actor->actions ||
        (extra != NULL && (limits->delayed_sends < extra->delayed_count ||
          limits->invocations < extra->invocation_count || limits->io_requests < extra->request_count ||
          limits->io_request_bytes < extra->request_bytes))) return TA_LIMIT;
    status = tai_extra(actor);
    if (status != TA_OK) return status;
    status = ta_arena_set_limit(actor->arena, limits->maximum_bytes);
    if (status != TA_OK) {
        if (extra == NULL) { ta_arena_close(actor->extra->arena); actor->extra = NULL; }
        return status;
    }
    actor->extra->custom_limits = *limits;
    actor->limits = &actor->extra->custom_limits;
    tai_mutated(system);
    return TA_OK;
}

ta_status_t ta_actor_stop(ta_system_t *system, ta_actor_ref_t ref)
{
    tai_actor_t *actor;
    if (system == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    actor = tai_actor_find(system, ref);
    if (actor == NULL) return TA_STALE;
    if (actor->status >= TA_ACTOR_DONE || actor->stop) return TA_OK;
    actor->stop = 1;
    actor->status = TA_ACTOR_STOPPING;
    actor->stable = 0;
    tai_ready(actor);
    tai_mutated(system);
    return TA_OK;
}

ta_status_t ta_actor_release(ta_system_t *system, ta_actor_ref_t ref)
{
    tai_actor_t *actor;
    if (system == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    actor = tai_actor_find(system, ref);
    if (actor == NULL) return TA_STALE;
    if (actor->status < TA_ACTOR_DONE ||
        (actor->extra != NULL && actor->extra->parent_invocation != NULL)) return TA_BUSY;
    if (actor->name != NULL) {
        ta_name_binding_t binding = {actor->name->key, ref};
        ta_status_t status = ta_name_unbind(system, &binding);
        if (status != TA_OK) return status;
    }
    tai_unready(actor);
    tai_actor_remove(system, actor);
    --system->stats.actors;
    system->stats.queued_events -= actor->external_count + actor->internal_count;
    drop_actor_data(actor);
    ta_arena_close(actor->arena);
    tai_mutated(system);
    return TA_OK;
}

ta_status_t ta_system_shutdown(ta_system_t *system)
{
    tai_actor_t *actor;
    if (system == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (system->shutdown) return TA_OK;
    system->shutdown = 1;
    for (actor = system->actors; actor != NULL; actor = actor->next)
        if (actor->status < TA_ACTOR_DONE) {
            actor->stop = 1;
            actor->status = TA_ACTOR_STOPPING;
            actor->stable = 0;
            tai_ready(actor);
        }
    tai_mutated(system);
    return TA_OK;
}
