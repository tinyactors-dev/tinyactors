#include "internal.h"

typedef struct alias_node { struct alias_node *next; const char *text; } alias_node_t;
typedef struct processor_node {
    struct processor_node *next;
    ta_ioprocessor_desc_t value;
} processor_node_t;
typedef struct service_node {
    struct service_node *next;
    ta_service_desc_t value;
} service_node_t;
typedef struct registration_build { ta_system_t *system; ta_arena_t *scratch; } registration_build_t;

static const char *const scxml_aliases[] = { "scxml", NULL };
static const ta_ioprocessor_desc_t scxml_processor = {
    .type = TA_SCXML_IOPROCESSOR, .aliases = scxml_aliases
};
static const ta_service_desc_t scxml_service = {
    .type = TA_SCXML_INVOKE_TYPE, .aliases = scxml_aliases,
    .event_processor = TA_SCXML_IOPROCESSOR
};

void ta_system_config_init(ta_system_config_t *config)
{
    size_t i;
    if (config == NULL) return;
    *config = (ta_system_config_t){0};
    config->abi_version = TA_ABI_VERSION;
    config->struct_size = sizeof(*config);
    config->limits = (ta_system_limits_t){
        .maximum_bytes = (size_t)-1, .actors = (size_t)-1, .definitions = (size_t)-1,
        .names = (size_t)-1, .queued_events = (size_t)-1, .delayed_sends = (size_t)-1,
        .invocations = (size_t)-1, .io_requests = (size_t)-1,
        .definition_states = (size_t)-1, .definition_transitions = (size_t)-1,
        .definition_actions = (size_t)-1, .state_depth = 64, .action_depth = 64,
        .event_name_bytes = 1024, .trace_metadata_bytes = 4096
    };
    config->actor_defaults = (ta_actor_limits_t){
        .maximum_bytes = (size_t)-1, .external_events = (size_t)-1,
        .internal_events = (size_t)-1, .queued_event_bytes = (size_t)-1,
        .delayed_sends = (size_t)-1, .invocations = (size_t)-1,
        .io_requests = (size_t)-1, .io_request_bytes = (size_t)-1,
        .microsteps_per_macrostep = 100000, .actions_per_macrostep = 1000000
    };
    for (i = 0; i < TA_LIFETIME_COUNT; ++i)
        config->arenas[i] = (ta_arena_options_t){256, 65536, (size_t)-1, 65536};
}

static uint8_t type_matches(const char *type, const char *const *aliases, ta_text_t query)
{
    if (tai_text_compare(ta_text_from_cstr(type), query) == 0) return 1;
    if (aliases != NULL) {
        while (*aliases != NULL) {
            if (tai_text_compare(ta_text_from_cstr(*aliases), query) == 0) return 1;
            ++aliases;
        }
    }
    return 0;
}

const ta_ioprocessor_desc_t *tai_processor(const ta_system_t *system, ta_text_t type)
{
    size_t i;
    if (type.data == NULL && type.size == 0) return &system->config.ioprocessors[0];
    for (i = 0; i < system->processor_count; ++i) {
        const ta_ioprocessor_desc_t *p = &system->config.ioprocessors[i];
        if (type_matches(p->type, p->aliases, type)) return p;
    }
    return NULL;
}

const ta_service_desc_t *tai_service(const ta_system_t *system, ta_text_t type)
{
    size_t i;
    if (type.data == NULL && type.size == 0) return &system->config.services[0];
    for (i = 0; i < system->service_count; ++i) {
        const ta_service_desc_t *p = &system->config.services[i];
        if (type_matches(p->type, p->aliases, type)) return p;
    }
    return NULL;
}

static ta_status_t copy_aliases(registration_build_t *build, const char *const *source,
                               const char *const **out)
{
    alias_node_t *head = NULL, **tail = &head, *node;
    size_t count = 0, i;
    ta_status_t status;
    void *memory;
    const char **array;
    tai_array_layout_t layout;
    *out = NULL;
    if (source == NULL) return TA_OK;
    while (*source != NULL) {
        if (**source == '\0') return TA_INVALID;
        if (count == (size_t)-1) return TA_LIMIT;
        status = tai_alloc(build->scratch, sizeof(*node), &memory);
        if (status != TA_OK) return status;
        node = (alias_node_t *)memory;
        *node = (alias_node_t){0};
        status = tai_cstr_copy(build->system->arena, *source++, &node->text);
        if (status != TA_OK) return status;
        *tail = node;
        tail = &node->next;
        ++count;
    }
    if (count == (size_t)-1) return TA_LIMIT;
    layout = (tai_array_layout_t){count + 1, sizeof(*array), TAI_ALIGNMENT};
    status = tai_array(build->system->arena, &layout, &memory);
    if (status != TA_OK) return status;
    array = (const char **)memory;
    for (i = 0, node = head; node != NULL; node = node->next) array[i++] = node->text;
    array[count] = NULL;
    *out = array;
    return TA_OK;
}

static uint8_t unique_names(const char *type, const char *const *aliases)
{
    size_t i, j;
    if (aliases == NULL) return 1;
    for (i = 0; aliases[i] != NULL; ++i) {
        ta_text_t alias = ta_text_from_cstr(aliases[i]);
        if (tai_text_compare(ta_text_from_cstr(type), alias) == 0) return 0;
        for (j = 0; j < i; ++j)
            if (tai_text_compare(ta_text_from_cstr(aliases[j]), alias) == 0) return 0;
    }
    return 1;
}

static ta_status_t copy_processors(registration_build_t *build,
                                   const ta_ioprocessor_desc_t *source)
{
    processor_node_t *head = NULL, **tail = &head, *node, *previous;
    const ta_ioprocessor_desc_t *input = &scxml_processor;
    ta_system_t *system = build->system;
    size_t count = 0, i;
    void *memory;
    ta_status_t status;
    tai_array_layout_t layout;
    ta_ioprocessor_desc_t *array;
    while (input != NULL) {
        if (input->type == NULL) {
            if (input->aliases != NULL || input->user != NULL || input->location != NULL)
                return TA_INVALID;
            break;
        }
        if (*input->type == '\0' || count == (size_t)-1) return TA_INVALID;
        status = tai_alloc(build->scratch, sizeof(*node), &memory);
        if (status != TA_OK) return status;
        node = (processor_node_t *)memory;
        *node = (processor_node_t){0};
        node->value.user = input->user;
        node->value.location = input->location;
        status = tai_cstr_copy(system->arena, input->type, &node->value.type);
        if (status == TA_OK) status = copy_aliases(build, input->aliases, &node->value.aliases);
        if (status != TA_OK) return status;
        if (!unique_names(node->value.type, node->value.aliases)) return TA_INVALID;
        for (previous = head; previous != NULL; previous = previous->next) {
            if (type_matches(previous->value.type, previous->value.aliases,
                             ta_text_from_cstr(node->value.type))) return TA_INVALID;
            if (node->value.aliases != NULL)
                for (i = 0; node->value.aliases[i] != NULL; ++i)
                    if (type_matches(previous->value.type, previous->value.aliases,
                                     ta_text_from_cstr(node->value.aliases[i]))) return TA_INVALID;
        }
        *tail = node;
        tail = &node->next;
        ++count;
        input = input == &scxml_processor ? source : input + 1;
    }
    if (count == (size_t)-1) return TA_LIMIT;
    layout = (tai_array_layout_t){count + 1, sizeof(*array), TAI_ALIGNMENT};
    status = tai_array(system->arena, &layout, &memory);
    if (status != TA_OK) return status;
    array = (ta_ioprocessor_desc_t *)memory;
    for (i = 0, node = head; node != NULL; node = node->next) array[i++] = node->value;
    array[count] = (ta_ioprocessor_desc_t){0};
    system->config.ioprocessors = array;
    system->processor_count = count;
    return TA_OK;
}

static ta_status_t copy_services(registration_build_t *build, const ta_service_desc_t *source)
{
    service_node_t *head = NULL, **tail = &head, *node, *previous;
    const ta_service_desc_t *input = &scxml_service;
    ta_system_t *system = build->system;
    ta_service_desc_t *array;
    tai_array_layout_t layout;
    void *memory;
    ta_status_t status;
    size_t count = 0, i;
    while (input != NULL) {
        if (input->type == NULL) {
            if (input->aliases != NULL || input->event_processor != NULL) return TA_INVALID;
            break;
        }
        if (*input->type == '\0' || input->event_processor == NULL ||
            *input->event_processor == '\0') return TA_INVALID;
        if (count == (size_t)-1) return TA_LIMIT;
        status = tai_alloc(build->scratch, sizeof(*node), &memory);
        if (status != TA_OK) return status;
        node = (service_node_t *)memory;
        *node = (service_node_t){0};
        status = tai_cstr_copy(system->arena, input->type, &node->value.type);
        if (status == TA_OK) status = copy_aliases(build, input->aliases, &node->value.aliases);
        if (status == TA_OK) status = tai_cstr_copy(system->arena, input->event_processor,
                                                  &node->value.event_processor);
        if (status != TA_OK) return status;
        if (!unique_names(node->value.type, node->value.aliases) ||
            tai_processor(system, ta_text_from_cstr(node->value.event_processor)) == NULL)
            return TA_INVALID;
        for (previous = head; previous != NULL; previous = previous->next) {
            if (type_matches(previous->value.type, previous->value.aliases,
                             ta_text_from_cstr(node->value.type))) return TA_INVALID;
            if (node->value.aliases != NULL)
                for (i = 0; node->value.aliases[i] != NULL; ++i)
                    if (type_matches(previous->value.type, previous->value.aliases,
                                     ta_text_from_cstr(node->value.aliases[i]))) return TA_INVALID;
        }
        *tail = node;
        tail = &node->next;
        ++count;
        input = input == &scxml_service ? source : input + 1;
    }
    if (count == (size_t)-1) return TA_LIMIT;
    layout = (tai_array_layout_t){count + 1, sizeof(*array), TAI_ALIGNMENT};
    status = tai_array(system->arena, &layout, &memory);
    if (status != TA_OK) return status;
    array = (ta_service_desc_t *)memory;
    for (i = 0, node = head; node != NULL; node = node->next) array[i++] = node->value;
    array[count] = (ta_service_desc_t){0};
    system->config.services = array;
    system->service_count = count;
    return TA_OK;
}

static uint8_t valid_config(const ta_system_config_t *config)
{
    size_t i;
    unsigned hooks;
    if (config->abi_version != TA_ABI_VERSION || config->struct_size != sizeof(*config) ||
        !tai_text_valid(config->session_prefix, 0) || !tai_text_valid(config->data_model, 0))
        return 0;
    for (i = 0; i < config->session_prefix.size; ++i) {
        char c = config->session_prefix.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return 0;
    }
    hooks = (config->values.retain != NULL) + (config->values.release != NULL) +
            (config->values.snapshot != NULL);
    if (hooks != 0 && hooks != 3) return 0;
    if ((config->trace.start == NULL) != (config->trace.end == NULL)) return 0;
    for (i = 0; i < TA_LIFETIME_COUNT; ++i) {
        const ta_arena_options_t *a = &config->arenas[i];
        if (a->initial_chunk_bytes > a->maximum_chunk_bytes ||
            a->maximum_allocation > a->maximum_chunk_bytes) return 0;
    }
    return 1;
}

ta_status_t ta_system_create(ta_arena_t *parent, const ta_system_config_t *config,
                             ta_system_t **out_system)
{
    ta_arena_options_t options;
    ta_arena_t *arena;
    registration_build_t build;
    ta_system_t *system;
    ta_status_t status;
    void *memory;
    if (parent == NULL || config == NULL || out_system == NULL) return TA_INVALID;
    *out_system = NULL;
    if (!valid_config(config)) return TA_INVALID;
    options = config->arenas[TA_LIFETIME_SYSTEM];
    if (config->limits.maximum_bytes < options.maximum_bytes)
        options.maximum_bytes = config->limits.maximum_bytes;
    status = ta_arena_open(parent, &options, &arena);
    if (status != TA_OK) return status;
    status = tai_alloc(arena, sizeof(*system), &memory);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    system = (ta_system_t *)memory;
    *system = (ta_system_t){0};
    system->arena = arena;
    system->config = *config;
    system->next_revision = system->next_actor = system->next_message = 1;
    system->next_request = system->next_invocation = 1;
    system->next_sendid = 1;
    system->mutation = 1;
    status = tai_text_copy(arena, config->session_prefix, &system->config.session_prefix);
    if (status == TA_OK) status = tai_text_copy(arena, config->data_model, &system->config.data_model);
    if (status == TA_OK) {
        if (config->session_prefix.size > (size_t)-1 - 30) status = TA_LIMIT;
        else {
            system->identity_capacity = config->session_prefix.size + 30;
            status = tai_alloc(arena, system->identity_capacity, (void **)&system->identity_buffer);
        }
    }
    if (status == TA_OK) status = tai_scope(system, TA_LIFETIME_SYSTEM, &build.scratch);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    build.system = system;
    status = copy_processors(&build, config->ioprocessors);
    if (status == TA_OK) status = copy_services(&build, config->services);
    ta_arena_close(build.scratch);
    if (status != TA_OK) { ta_arena_close(arena); return status; }
    *out_system = system;
    return TA_OK;
}

ta_status_t ta_system_set_limits(ta_system_t *system, const ta_system_limits_t *limits)
{
    ta_status_t status;
    if (system == NULL || limits == NULL) return TA_INVALID;
    if (system->running) return TA_BUSY;
    if (limits->actors < system->stats.actors || limits->definitions < system->stats.definitions ||
        limits->names < system->stats.names || limits->queued_events < system->stats.queued_events ||
        limits->delayed_sends < system->stats.delayed_sends ||
        limits->invocations < system->stats.invocations || limits->io_requests < system->stats.io_requests)
        return TA_LIMIT;
    status = ta_arena_set_limit(system->arena, limits->maximum_bytes);
    if (status != TA_OK) return status;
    system->config.limits = *limits;
    tai_mutated(system);
    return TA_OK;
}

void ta_system_stats(const ta_system_t *system, ta_system_stats_t *out_stats)
{
    if (system == NULL || out_stats == NULL) return;
    *out_stats = system->stats;
    ta_arena_stats(system->arena, &out_stats->memory);
}

ta_status_t ta_system_destroy(ta_system_t *system)
{
    ta_definition_t *definition;
    tai_registry_t *entry;
    if (system == NULL) return TA_INVALID;
    if (system->running || system->stats.actors != 0 || system->stats.io_requests != 0 ||
        system->stats.invocations != 0) return TA_BUSY;
    for (definition = system->definitions; definition != NULL; definition = definition->next)
        if (definition->references != definition->held_by_definitions + definition->held_by_registry)
            return TA_BUSY;
    while ((entry = tai_registry_first(system->definition_registry)) != NULL) {
        ta_definition_binding_t binding = { entry->key, UINT64_MAX };
        ta_status_t status = ta_definition_unpublish(system, &binding);
        if (status != TA_OK) return status;
    }
    if (system->definitions != NULL) return TA_BUSY;
    ta_arena_close(system->arena);
    return TA_OK;
}
