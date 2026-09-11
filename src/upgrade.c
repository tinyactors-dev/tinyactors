#include "internal.h"

/* Staged replacement owns three scopes; installed data is untouched until
 * structural checks, migration and protected-variable binding all succeed. */
typedef struct upgrade_stage {
    tai_actor_t *actor;
    ta_definition_t *definition;
    ta_arena_t *scratch, *state, *config;
    tai_actor_extra_t *extra;
    ta_data_view_t data;
    ta_value_t native;
    ta_migration_state_t *map;
    ta_state_id_t *oldcfg, *newcfg;
    size_t count;
    uint8_t *marks;
} upgrade_stage_t;

static uint8_t history_kind(ta_state_kind_t kind)
{
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static uint32_t named_state(const ta_definition_t *d, ta_text_t name)
{
    uint32_t i;
    for (i = 0; i < d->description.state_count; ++i)
        if (tai_text_compare(d->description.states[i].name, name) == 0) return i;
    return TA_INDEX_NONE;
}

/* Only persistent configuration/history paths must preserve topology. */
static uint32_t mapped_path(const upgrade_stage_t *st, uint32_t old)
{
    const ta_definition_t *before = st->actor->definition, *after = st->definition;
    uint32_t result = named_state(after, before->description.states[old].name), now = result;
    while (old != TA_INDEX_NONE && now != TA_INDEX_NONE) {
        const ta_state_desc_t *a = &before->description.states[old], *b = &after->description.states[now];
        if (a->kind != b->kind || tai_text_compare(a->name,b->name) != 0) return TA_INDEX_NONE;
        old = a->parent; now = b->parent;
    }
    return old == now ? result : TA_INDEX_NONE;
}

typedef struct configuration_check {
    const ta_definition_t *definition;
    uint32_t root;
    uint8_t shallow;
} configuration_check_t;

static uint8_t legal_configuration(const configuration_check_t *check, const uint8_t *marks)
{
    const ta_definition_t *d = check->definition;
    uint32_t i, j, count, children;
    for (i = check->root; i < d->subtree_end[check->root]; ++i) if (marks[i]) {
        ta_state_kind_t kind = d->description.states[i].kind;
        if (history_kind(kind)) return 0;
        if (check->shallow && i != check->root) continue;
        count = children = 0;
        for (j = i + 1; j < d->subtree_end[i]; ++j)
            if (d->description.states[j].parent == i && !history_kind(d->description.states[j].kind)) {
                ++children; if (marks[j]) ++count;
            }
        if ((kind == TA_STATE_ROOT || kind == TA_STATE_COMPOUND) && count != 1) return 0;
        if (kind == TA_STATE_PARALLEL && count != children) return 0;
    }
    return 1;
}

static ta_status_t map_configuration(upgrade_stage_t *st)
{
    tai_actor_t *a = st->actor;
    const ta_definition_desc_t *old = &a->definition->description, *next = &st->definition->description;
    uint32_t i, mapped;
    ta_status_t status;
    tai_array_layout_t layout = {next->state_count,sizeof(*st->map),TAI_ALIGNMENT};
    status = tai_array(st->scratch,&layout,(void **)&st->map); if (status != TA_OK) return status;
    layout = (tai_array_layout_t){old->state_count,sizeof(*st->oldcfg),TAI_ALIGNMENT};
    status = tai_array(st->scratch,&layout,(void **)&st->oldcfg); if (status != TA_OK) return status;
    status = tai_array(st->scratch,&layout,(void **)&st->newcfg); if (status != TA_OK) return status;
    status = tai_alloc(st->scratch,next->state_count,(void **)&st->marks); if (status != TA_OK) return status;
    tai_zero(st->marks,next->state_count);
    for (i = 0; i < next->state_count; ++i) {
        uint8_t initialized;
        mapped = named_state(a->definition,next->states[i].name);
        initialized = i == 0 || next->binding == TA_BINDING_EARLY;
        if (mapped != TA_INDEX_NONE && (old->binding == TA_BINDING_EARLY || mapped == 0 ||
            (a->extra != NULL && a->extra->late != NULL &&
             (a->extra->late[mapped / 8] & (1u << (mapped % 8)))))) initialized = 1;
        st->map[i] = (ta_migration_state_t){mapped,i,history_kind(next->states[i].kind) ? 0 : initialized};
    }
    for (i = 0; i < old->state_count; ++i) if (tai_active(a,i)) {
        mapped = mapped_path(st,i); if (mapped == TA_INDEX_NONE) return TA_INVALID;
        st->oldcfg[st->count] = i; st->newcfg[st->count++] = mapped; st->marks[mapped] = 1;
    }
    if (st->count == 0 || !st->marks[0] ||
        !legal_configuration(&(configuration_check_t){st->definition,0,0},st->marks)) return TA_INVALID;
    status = tai_alloc(st->config,((size_t)next->state_count + 7) / 8,(void **)&st->extra->parallel);
    if (status != TA_OK) return status;
    tai_zero(st->extra->parallel,((size_t)next->state_count + 7) / 8);
    for (i = 0; i < next->state_count; ++i)
        if (st->marks[i]) st->extra->parallel[i / 8] |= (uint8_t)(1u << (i % 8));
    return TA_OK;
}

static ta_status_t map_history(upgrade_stage_t *st, const tai_history_t *old)
{
    tai_history_t *next;
    ta_arena_t *arena;
    uint32_t state = mapped_path(st,old->state), target, parent;
    size_t i;
    ta_status_t status;
    const ta_definition_t *d = st->definition;
    if (state == TA_INDEX_NONE) return TA_INVALID;
    status = ta_arena_open(st->config,&st->actor->system->config.arenas[TA_LIFETIME_ACTOR],&arena);
    if (status != TA_OK) return status;
    status = tai_alloc(arena,sizeof(*next),(void **)&next); if (status != TA_OK) return status;
    *next = (tai_history_t){.arena=arena,.state=state,.count=old->count};
    status = tai_array(arena,&(tai_array_layout_t){old->count,sizeof(uint32_t),TAI_ALIGNMENT},(void **)&next->targets);
    if (status != TA_OK) return status;
    tai_zero(st->marks,d->description.state_count);
    parent = d->description.states[state].parent; st->marks[parent] = 1;
    for (i = 0; i < old->count; ++i) {
        target = mapped_path(st,old->targets[i]); if (target == TA_INDEX_NONE) return TA_INVALID;
        next->targets[i] = target;
        while (target != parent) {
            if (target == TA_INDEX_NONE) return TA_INVALID;
            st->marks[target] = 1; target = d->description.states[target].parent;
        }
    }
    if (!legal_configuration(&(configuration_check_t){d,parent,
            d->description.states[state].kind == TA_STATE_HISTORY_SHALLOW},st->marks)) return TA_INVALID;
    for (i = 1; i < next->count; ++i) {
        size_t j = i; target = next->targets[i];
        while (j != 0 && next->targets[j-1] > target) { next->targets[j] = next->targets[j-1]; --j; }
        next->targets[j] = target;
    }
    next->next = st->extra->history; st->extra->history = next;
    return TA_OK;
}

static ta_status_t stage_tables(upgrade_stage_t *st)
{
    const ta_definition_desc_t *d = &st->definition->description;
    tai_actor_extra_t *x = st->extra, *old = st->actor->extra;
    tai_history_t *history;
    uint32_t i;
    uint8_t invokes = 0;
    ta_status_t status = map_configuration(st);
    if (status != TA_OK) return status;
    if (old != NULL) {
        x->custom_limits = old->custom_limits;
        x->locations = old->locations; x->location_count = old->location_count;
        x->parent_invocation = old->parent_invocation;
        for (history = old->history; history != NULL; history = history->next) {
            status = map_history(st,history); if (status != TA_OK) return status;
        }
    }
    if (d->binding == TA_BINDING_LATE) {
        size_t bytes = ((size_t)d->state_count + 7) / 8;
        status = tai_alloc(st->config,bytes,(void **)&x->late); if (status != TA_OK) return status;
        tai_zero(x->late,bytes);
        for (i = 0; i < d->state_count; ++i)
            if (st->map[i].data_initialized) x->late[i / 8] |= (uint8_t)(1u << (i % 8));
    }
    for (i = 0; i < d->state_count; ++i) if (d->states[i].invoke_count != 0) invokes = 1;
    if (invokes) {
        status = tai_array(st->config,&(tai_array_layout_t){d->state_count,sizeof(uint32_t),TAI_ALIGNMENT},
            (void **)&x->invoke_progress); if (status != TA_OK) return status;
        for (i = 0; i < d->state_count; ++i)
            x->invoke_progress[i] = (x->parallel[i / 8] & (1u << (i % 8))) ? d->states[i].invoke_count : 0;
    }
    return TA_OK;
}

static uint8_t copy_compatible(const ta_definition_desc_t *a, const ta_definition_desc_t *b)
{
    uint32_t i;
    if (a->data_layout.storage != TA_DATA_BYTES || b->data_layout.storage != TA_DATA_BYTES ||
        a->data_layout.bytes_layout.size != b->data_layout.bytes_layout.size ||
        a->data_layout.bytes_layout.alignment != b->data_layout.bytes_layout.alignment ||
        a->data_layout.schema.size == 0 || tai_text_compare(a->data_layout.schema,b->data_layout.schema) != 0) return 0;
    for (i = 0; i < a->state_count; ++i) if (a->states[i].data_count != 0) return 0;
    for (i = 0; i < b->state_count; ++i) if (b->states[i].data_count != 0) return 0;
    return 1;
}

static ta_status_t stage_data(upgrade_stage_t *st, const ta_upgrade_options_t *options)
{
    const ta_data_layout_t *layout = &st->definition->description.data_layout;
    ta_status_t status;
    ta_context_t context;
    ta_migration_t migration;
    void *bytes = NULL;
    st->data.storage = layout->storage;
    if (layout->storage == TA_DATA_BYTES) {
        if (layout->bytes_layout.size != 0) {
            status = ta_arena_alloc(st->state,&layout->bytes_layout,&bytes); if (status != TA_OK) return status;
            if (layout->initial_bytes.data != NULL) tai_copy(bytes,layout->initial_bytes.data,layout->bytes_layout.size);
            else tai_zero(bytes,layout->bytes_layout.size);
        }
        st->data.as.bytes = (ta_bytes_t){bytes,layout->bytes_layout.size};
    }
    if (options->migrate == NULL) {
        if (!copy_compatible(&st->actor->definition->description,&st->definition->description)) return TA_INVALID;
        tai_copy(bytes,st->actor->data.as.bytes.data,layout->bytes_layout.size);
        return TA_OK;
    }
    tai_context(st->actor,TA_PHASE_MIGRATE,&context);
    migration = (ta_migration_t){.actor={st->actor->id},.from=st->actor->definition,.to=st->definition,
        .old_state=st->actor->data,.scratch=st->scratch,.old_configuration=st->oldcfg,
        .new_configuration=st->newcfg,.configuration_count=st->count,.state_map=st->map,
        .state_map_count=st->definition->description.state_count,.system=context.view.system};
    if (layout->storage == TA_DATA_BYTES) migration.new_bytes = (ta_mut_bytes_t){bytes,layout->bytes_layout.size};
    status = options->migrate(options->user,&migration,&st->native);
    if (status != TA_OK) return status;
    if (layout->storage == TA_DATA_NATIVE) {
        if (st->native.handle == 0) return TA_EXECUTION_ERROR;
        st->data.as.native = st->native;
    } else if (st->native.handle != 0) return TA_EXECUTION_ERROR;
    return TA_OK;
}

static ta_status_t stage_candidate(upgrade_stage_t *st, const ta_upgrade_options_t *options)
{
    ta_system_t *system = st->actor->system;
    ta_status_t status;
    tai_actor_t candidate;
    ta_context_t context;
    status = tai_actor_scope(st->actor,TA_LIFETIME_UPGRADE,&st->scratch); if (status != TA_OK) return status;
    status = tai_actor_scope(st->actor,TA_LIFETIME_ACTOR,&st->state); if (status != TA_OK) return status;
    status = tai_actor_scope(st->actor,TA_LIFETIME_ACTOR,&st->config); if (status != TA_OK) return status;
    status = tai_alloc(st->config,sizeof(*st->extra),(void **)&st->extra); if (status != TA_OK) return status;
    *st->extra = (tai_actor_extra_t){.arena=st->config,.configuration_arena=st->config,.state_arena=st->state};
    status = stage_tables(st); if (status != TA_OK) return status;
    status = stage_data(st,options); if (status != TA_OK) return status;
    candidate = *st->actor; candidate.definition = st->definition; candidate.extra = st->extra; candidate.data = st->data;
    candidate.leaf = TA_INDEX_NONE;
    st->extra->macrostep = st->scratch;
    tai_context(&candidate,TA_PHASE_MIGRATE,&context);
    if (system->config.data.bind_system != NULL) {
        status = system->config.data.bind_system(&context,&context.values); if (status != TA_OK) return status;
    }
    if (system->config.data.bind_event != NULL && context.values.event != NULL) {
        status = system->config.data.bind_event(&context,context.values.event); if (status != TA_OK) return status;
    }
    st->extra->macrostep = NULL;
    return TA_OK;
}

static void discard_stage(upgrade_stage_t *st)
{
    ta_system_t *system = st->actor->system;
    if (st->native.handle != 0 && system->config.values.release != NULL)
        system->config.values.release(system->config.host_user,st->native);
    if (st->config != NULL) ta_arena_close(st->config);
    if (st->state != NULL) ta_arena_close(st->state);
    if (st->scratch != NULL) ta_arena_close(st->scratch);
}

static void retire_configuration(tai_actor_t *a)
{
    tai_actor_extra_t *x = a->extra;
    ta_arena_t *extra, *state, *config;
    tai_history_t *history, *next;
    if (x == NULL) return;
    tai_activation_close(a,0);
    extra = x->arena; state = x->state_arena; config = x->configuration_arena;
    for (history = x->history; history != NULL; history = next) {
        next = history->next; ta_arena_close(history->arena);
    }
    if (state != NULL) ta_arena_close(state);
    if (config != NULL && config != state) ta_arena_close(config);
    if (extra != config && extra != state) ta_arena_close(extra);
}

ta_status_t ta_actor_upgrade(ta_system_t *system, const ta_upgrade_options_t *options)
{
    tai_actor_t *actor;
    upgrade_stage_t stage = {0};
    ta_status_t status;
    uint8_t custom_limits;
    ta_definition_t *old;
    if (system == NULL || options == NULL || options->actor.id == 0 || options->replacement == NULL ||
        options->replacement->system != system) return TA_INVALID;
    if (system->running) return TA_BUSY;
    actor = tai_actor_find(system,options->actor); if (actor == NULL) return TA_STALE;
    if (options->expected_revision != TA_REVISION_ANY && options->expected_revision != actor->definition->revision) return TA_CONFLICT;
    if (actor->status != TA_ACTOR_READY || !actor->stable || actor->stop || actor->internal_head != NULL ||
        (actor->extra != NULL && (actor->extra->delayed_count != 0 || actor->extra->invocation_count != 0 ||
                                 actor->extra->request_count != 0))) return TA_BUSY;
    stage.actor = actor; stage.definition = options->replacement;
    system->running = 1;
    status = stage_candidate(&stage,options);
    if (status != TA_OK) { discard_stage(&stage); system->running = 0; return status; }
    custom_limits = actor->extra != NULL && actor->limits == &actor->extra->custom_limits;
    old = actor->definition;
    ta_definition_retain(stage.definition);
    if (actor->data.storage == TA_DATA_NATIVE && actor->data.as.native.handle != 0)
        system->config.values.release(system->config.host_user,actor->data.as.native);
    retire_configuration(actor);
    actor->definition = stage.definition; actor->data = stage.data; actor->extra = stage.extra; actor->leaf = TA_INDEX_NONE;
    if (custom_limits) actor->limits = &stage.extra->custom_limits;
    actor->stable = 0; actor->eventless = 1;
    ta_arena_close(stage.scratch);
    ta_definition_release(old);
    tai_ready(actor); tai_mutated(system); system->running = 0;
    return TA_OK;
}
