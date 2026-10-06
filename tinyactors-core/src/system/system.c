/* system.c — system lifetime: one caller-owned region carved by a single
 * function in measure mode (ta_system_layout) and real mode (ta_system_init);
 * actor spawn/reclaim; relocation; snapshot/restore; inspection. */
#include "base/memory.h"
#include "system/system_internal.h"

/* ---- configuration ------------------------------------------------------------------ */

/* Slab class size an object of n bytes occupies (see slab.c). */
static uint32_t slab_class(uint32_t n) {
    for (uint32_t c = 0;; c++) {
        uint32_t sz = (c & 1u) ? 24u << (c >> 1) : 16u << (c >> 1);
        if (sz >= n) return sz;
    }
}

static void normalize_config(const ta_image_view *view, const ta_system_config *in,
                             ta_system_config *config) {
    ta_memset(config, 0, sizeof *config);
    if (in)
        ta_memcpy(config, in, in->size && in->size < sizeof *config ? in->size : sizeof *config);
    config->size = sizeof *config;
    if (!config->max_actors) config->max_actors = 64;
    if (!config->max_events) config->max_events = 256;
    if (!config->max_timers) config->max_timers = 64;
    if (!config->workspaces) config->workspaces = 4;
    if (!config->max_requests) config->max_requests = 16;
    if (!config->outbox) config->outbox = 64;
    /* clamp counts before anything is derived from them: pool handles carry a
     * 24-bit index; actor and request handles also lend bit 23 to child
     * handles (external invoke tokens) */
    if (config->max_actors > TA_SYSTEM_MAX_ACTORS) config->max_actors = TA_SYSTEM_MAX_ACTORS;
    if (config->max_requests > TA_EXTERNAL_INVOKE_BIT - 2)
        config->max_requests = TA_EXTERNAL_INVOKE_BIT - 2;
    if (config->max_events > TA_POOL_MAX_CAPACITY) config->max_events = TA_POOL_MAX_CAPACITY;
    if (config->max_timers > TA_POOL_MAX_CAPACITY) config->max_timers = TA_POOL_MAX_CAPACITY;
    if (config->workspaces > TA_POOL_MAX_CAPACITY) config->workspaces = TA_POOL_MAX_CAPACITY;
    if (config->string_bytes > TA_SLAB_MAX_BYTES) config->string_bytes = TA_SLAB_MAX_BYTES;
    if (config->instance_bytes > TA_SLAB_MAX_BYTES) config->instance_bytes = TA_SLAB_MAX_BYTES;
    if (!config->quantum) config->quantum = 1000;
    if (!config->ticks_per_second) config->ticks_per_second = 1000000000ull;
    if (!config->string_bytes) config->string_bytes = 16384 + config->max_actors * 16;
    if (!config->instance_bytes) {
        uint32_t m = 0;
        for (uint32_t i = 0; i < view->chart_count; i++) {
            uint32_t s = ta_instance_layout_of(&view->charts[i]).size;
            if (s > m) m = s;
        }
        uint64_t ib = (uint64_t)config->max_actors * slab_class(m);
        config->instance_bytes = ib > TA_SLAB_MAX_BYTES ? TA_SLAB_MAX_BYTES : (uint32_t)ib;
    }
}

typedef struct carved {
    size_t header, actor_slots, actor_generations, event_slots, event_generations, timer_slots,
        timer_generations, heap_nodes, heap_positions, workspace_slots, workspace_generations,
        request_slots, request_generations;
    size_t slab_pages, slab_metadata, effects, effect_views, sessions, chart_datamodels,
        chart_contexts;
    uint32_t workspace_size, page_count, session_capacity;
} carved;

static void carve(ta_arena *arena, const ta_image_view *view, const ta_system_config *config,
                  carved *o) {
    o->workspace_size = 0;
    for (uint32_t i = 0; i < view->chart_count; i++) {
        uint32_t s = ta_workspace_layout_of(&view->charts[i]).size;
        if (s > o->workspace_size) o->workspace_size = s;
    }
    o->page_count =
        (uint32_t)((config->instance_bytes + TA_SLAB_PAGE - 1) / TA_SLAB_PAGE +
                   (config->string_bytes + TA_SLAB_PAGE - 1) / TA_SLAB_PAGE + TA_SLAB_CLASSES);
    o->session_capacity = 16;
    while (o->session_capacity < config->max_actors * 2)
        o->session_capacity <<= 1;
    o->header = ta_arena_alloc(arena, sizeof(ta_system), TA_ALIGN_MAX);
    o->actor_slots = ta_arena_alloc(
        arena, ta_pool_slots_bytes(sizeof(ta_actor), config->max_actors), TA_ALIGN_MAX);
    o->actor_generations = ta_arena_alloc(arena, config->max_actors, 1);
    o->event_slots = ta_arena_alloc(
        arena, ta_pool_slots_bytes(sizeof(ta_event), config->max_events), TA_ALIGN_MAX);
    o->event_generations = ta_arena_alloc(arena, config->max_events, 1);
    o->timer_slots = ta_arena_alloc(
        arena, ta_pool_slots_bytes(sizeof(ta_timer), config->max_timers), TA_ALIGN_MAX);
    o->timer_generations = ta_arena_alloc(arena, config->max_timers, 1);
    o->heap_nodes = ta_arena_alloc(arena, ta_heap_nodes_bytes(config->max_timers), TA_ALIGN_MAX);
    o->heap_positions = ta_arena_alloc(arena, ta_heap_position_bytes(config->max_timers), 4);
    o->workspace_slots = ta_arena_alloc(
        arena, ta_pool_slots_bytes(o->workspace_size, config->workspaces), TA_ALIGN_MAX);
    o->workspace_generations = ta_arena_alloc(arena, config->workspaces, 1);
    o->request_slots = ta_arena_alloc(
        arena, ta_pool_slots_bytes(sizeof(ta_request), config->max_requests), TA_ALIGN_MAX);
    o->request_generations = ta_arena_alloc(arena, config->max_requests, 1);
    o->slab_pages = ta_arena_alloc(arena, ta_slab_pages_bytes(o->page_count), TA_ALIGN_MAX);
    o->slab_metadata = ta_arena_alloc(arena, ta_slab_metadata_bytes(o->page_count), TA_ALIGN_MAX);
    o->effects = ta_arena_alloc(arena, sizeof(ta_effect_record) * config->outbox, TA_ALIGN_MAX);
    o->effect_views = ta_arena_alloc(arena, sizeof(ta_effect) * config->outbox, TA_ALIGN_MAX);
    o->sessions = ta_arena_alloc(arena, sizeof(uint32_t) * o->session_capacity, 4);
    o->chart_datamodels = ta_arena_alloc(arena, view->chart_count, 1);
    o->chart_contexts = ta_arena_alloc(arena, sizeof(void *) * view->chart_count, TA_ALIGN_MAX);
}

ta_status ta_system_layout(const ta_image *image, const ta_system_config *config, size_t *bytes) {
    const ta_image_view *view = ta_image_view_of(image);
    if (!view->base) return TA_ERROR_INVALID;
    ta_system_config normalized;
    normalize_config(view, config, &normalized);
    ta_arena arena;
    carved o;
    ta_arena_init_measure(&arena);
    carve(&arena, view, &normalized, &o);
    *bytes = ta_arena_required(&arena);
    return TA_OK;
}

/* ---- binding host pointers ------------------------------------------------------------- */

static uint64_t serial_of(void *sysp, uint32_t actor) {
    ta_system *system = (ta_system *)sysp;
    uint32_t index = ta_system_resolve_actor(system, actor);
    return index == TA_NIL ? 0 : ta_actor_at(system, index)->serial;
}

static void rebind(ta_system *system, const ta_image *image) {
    ta_core *core = &system->core;
    core->image_view = *ta_image_view_of(image);
    core->image = image;
    core->events = &system->events;
    core->timers = &system->timers;
    core->workspaces = &system->workspaces;
    core->slab = &system->slab;
    core->datamodels = system->datamodels;
    core->datamodel_count = system->datamodel_count;
    core->chart_datamodel = SYSTEM_POINTER(system, chart_datamodels_relative, uint8_t);
    core->chart_context = SYSTEM_POINTER(system, chart_contexts_relative, void *);
    core->io_processors = system->config.io_processors;
    core->io_processor_count = system->config.io_processor_count;
    core->serial_of = serial_of;
    core->system = system;
    uint32_t r = system->config.max_events / 8;
    core->reserve = r > 4 ? 4 : r;
    core->overrun_max = system->config.quantum / 2 + 16;
}

static int string_equal_c_string(ta_string_view a, const char *b) {
    uint32_t n = 0;
    while (b[n])
        n++;
    return a.length == n && (n == 0 || ta_memcmp(a.pointer, b, n) == 0);
}

/* Copy datamodel bindings (+ builtin null) and map charts to them. */
static ta_status bind_datamodels(ta_system *system, const ta_image *image,
                                 const ta_datamodel_binding *datamodels, uint32_t n) {
    if (n > TA_MAX_DATAMODELS) return TA_ERROR_INVALID;
    int have_null = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!datamodels[i].vtable || !datamodels[i].vtable->name) return TA_ERROR_INVALID;
        system->datamodels[i] = datamodels[i];
        const char *nm = datamodels[i].vtable->name;
        if (nm[0] == 'n' && nm[1] == 'u' && nm[2] == 'l' && nm[3] == 'l' && nm[4] == 0)
            have_null = 1;
    }
    system->datamodel_count = n;
    if (!have_null) {
        system->datamodels[n].vtable = ta_datamodel_null();
        system->datamodels[n].datamodel = (void *)0;
        system->datamodel_count = n + 1;
    }
    uint8_t *cdm = SYSTEM_POINTER(system, chart_datamodels_relative, uint8_t);
    for (uint32_t c = 0; c < system->chart_count; c++) {
        ta_string_view want = ta_image_chart_datamodel(image, c);
        uint32_t k = 0;
        while (k < system->datamodel_count &&
               !string_equal_c_string(want, system->datamodels[k].vtable->name))
            k++;
        if (k == system->datamodel_count) return TA_ERROR_UNSUPPORTED;
        cdm[c] = (uint8_t)k;
    }
    return TA_OK;
}

static ta_status attach_charts(ta_system *system, const ta_image *image) {
    void **context = SYSTEM_POINTER(system, chart_contexts_relative, void *);
    uint8_t *cdm = SYSTEM_POINTER(system, chart_datamodels_relative, uint8_t);
    const ta_image_view *view = ta_image_view_of(image);
    for (uint32_t c = 0; c < system->chart_count; c++) {
        const ta_datamodel_binding *binding = &system->datamodels[cdm[c]];
        context[c] = (void *)0;
        if (binding->vtable->chart_attach) {
            ta_datamodel_chart chart = {sizeof chart, c, image, view->charts[c].binding, 0};
            ta_status status =
                binding->vtable->chart_attach(binding->datamodel, &chart, &context[c]);
            if (status != TA_OK) return status;
        }
    }
    return TA_OK;
}

static void detach_charts(ta_system *system) {
    void **context = SYSTEM_POINTER(system, chart_contexts_relative, void *);
    uint8_t *cdm = SYSTEM_POINTER(system, chart_datamodels_relative, uint8_t);
    for (uint32_t c = 0; c < system->chart_count; c++) {
        const ta_datamodel_binding *binding = &system->datamodels[cdm[c]];
        if (binding->vtable->chart_detach)
            binding->vtable->chart_detach(binding->datamodel, context[c]);
        context[c] = (void *)0;
    }
}

/* ---- init ------------------------------------------------------------------------------ */

static ta_system *place(void *memory, size_t length, const ta_image_view *view,
                        const ta_system_config *config, carved *o, ta_arena *arena) {
    if (memory)
        ta_memset(memory, 0, length); /* dead storage starts zero: snapshots carry no residue */
    ta_arena_init(arena, memory, length);
    carve(arena, view, config, o);
    if (arena->failed) return (ta_system *)0;
    return (ta_system *)ta_arena_pointer(arena, o->header);
}

static void set_rels(ta_system *system, ta_arena *arena, const carved *o) {
    system->effects_relative =
        (int64_t)((intptr_t)ta_arena_pointer(arena, o->effects) - (intptr_t)system);
    system->effect_views_relative =
        (int64_t)((intptr_t)ta_arena_pointer(arena, o->effect_views) - (intptr_t)system);
    system->sessions_relative =
        (int64_t)((intptr_t)ta_arena_pointer(arena, o->sessions) - (intptr_t)system);
    system->chart_datamodels_relative =
        (int64_t)((intptr_t)ta_arena_pointer(arena, o->chart_datamodels) - (intptr_t)system);
    system->chart_contexts_relative =
        (int64_t)((intptr_t)ta_arena_pointer(arena, o->chart_contexts) - (intptr_t)system);
    system->session_capacity = o->session_capacity;
}

ta_status ta_system_init(void *memory, size_t length, const ta_image *image,
                         const ta_system_config *config, const ta_datamodel_binding *datamodels,
                         uint32_t datamodel_count, ta_system **out) {
    const ta_image_view *view = ta_image_view_of(image);
    *out = (ta_system *)0;
    if (!memory || !view->base) return TA_ERROR_INVALID;
    ta_system_config normalized;
    normalize_config(view, config, &normalized);
    ta_arena arena;
    carved o;
    ta_system *system = place(memory, length, view, &normalized, &o, &arena);
    if (!system) return TA_ERROR_NO_MEMORY;
    ta_memset(system, 0, sizeof *system);
    system->magic = TA_SYSTEM_MAGIC;
    system->version = 1;
    system->region_size = arena.offset;
    system->config = normalized;
    system->chart_count = view->chart_count;
    system->next_serial = normalized.first_serial;
    ta_pool_init(&system->actors, ta_arena_pointer(&arena, o.actor_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.actor_generations), sizeof(ta_actor),
                 normalized.max_actors);
    ta_pool_init(&system->events, ta_arena_pointer(&arena, o.event_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.event_generations), sizeof(ta_event),
                 normalized.max_events);
    ta_pool_init(&system->timers, ta_arena_pointer(&arena, o.timer_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.timer_generations), sizeof(ta_timer),
                 normalized.max_timers);
    ta_heap_init(&system->heap, (ta_heap_node *)ta_arena_pointer(&arena, o.heap_nodes),
                 (uint32_t *)ta_arena_pointer(&arena, o.heap_positions), normalized.max_timers);
    ta_pool_init(&system->workspaces, ta_arena_pointer(&arena, o.workspace_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.workspace_generations),
                 o.workspace_size, normalized.workspaces);
    ta_pool_init(&system->requests, ta_arena_pointer(&arena, o.request_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.request_generations),
                 sizeof(ta_request), normalized.max_requests);
    ta_slab_init(&system->slab, ta_arena_pointer(&arena, o.slab_pages),
                 (ta_slab_page *)ta_arena_pointer(&arena, o.slab_metadata), o.page_count);
    ta_index_queue_init(&system->run_queue);
    ta_index_queue_init(&system->workspace_queue);
    ta_index_queue_init(&system->wake_queue);
    set_rels(system, &arena, &o);
    system->effects_capacity = normalized.outbox;
    uint32_t *session = SYSTEM_POINTER(system, sessions_relative, uint32_t);
    for (uint32_t i = 0; i < system->session_capacity; i++)
        session[i] = TA_NIL;
    ta_status status = bind_datamodels(system, image, datamodels, datamodel_count);
    if (status != TA_OK) return status;
    rebind(system, image);
    status = attach_charts(system, image);
    if (status != TA_OK) return status;
    system->core.out.target = system->core.out.source = system->core.out.type =
        system->core.out.sendid = TA_NIL;
    system->core.out.label = TA_NIL;
    *out = system;
    return TA_OK;
}

/* ---- sessions ---------------------------------------------------------------------------- */

#define SESSION_TOMBSTONE (TA_NIL - 1u)

static uint32_t session_hash(uint64_t serial, uint32_t capacity) {
    uint64_t h = serial * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32) & (capacity - 1);
}

void ta_system_session_insert(ta_system *system, uint32_t index) {
    uint32_t *t = SYSTEM_POINTER(system, sessions_relative, uint32_t);
    uint32_t i = session_hash(ta_actor_at(system, index)->serial, system->session_capacity);
    while (t[i] != TA_NIL && t[i] != SESSION_TOMBSTONE)
        i = (i + 1) & (system->session_capacity - 1);
    t[i] = index;
    system->session_count++;
}

uint32_t ta_system_session_lookup(const ta_system *system, uint64_t serial) {
    const uint32_t *t = SYSTEM_POINTER(system, sessions_relative, uint32_t);
    uint32_t i = session_hash(serial, system->session_capacity);
    for (uint32_t n = 0; n < system->session_capacity && t[i] != TA_NIL;
         n++, i = (i + 1) & (system->session_capacity - 1))
        if (t[i] != SESSION_TOMBSTONE && ta_pool_live(&system->actors, t[i]) &&
            ta_actor_at(system, t[i])->serial == serial)
            return t[i];
    return TA_NIL;
}

static void session_remove(ta_system *system, uint32_t index) {
    uint32_t *t = SYSTEM_POINTER(system, sessions_relative, uint32_t);
    uint32_t i = session_hash(ta_actor_at(system, index)->serial, system->session_capacity);
    for (uint32_t n = 0; n < system->session_capacity && t[i] != TA_NIL;
         n++, i = (i + 1) & (system->session_capacity - 1))
        if (t[i] == index) {
            t[i] = SESSION_TOMBSTONE;
            system->session_count--;
            return;
        }
}

/* ---- actors -------------------------------------------------------------------------------- */

uint32_t ta_system_resolve_actor(const ta_system *system, uint32_t handle) {
    if (handle & TA_EXTERNAL_INVOKE_BIT) return TA_NIL;
    return ta_pool_resolve(&system->actors, handle);
}

void ta_system_make_runnable(ta_system *system, uint32_t index) {
    ta_actor *actor = ta_actor_at(system, index);
    if (actor->state == TA_ACTOR_HIBERNATED) {
        ta_system_wake_request(system, index);
    } else if (actor->state == TA_ACTOR_IDLE) {
        actor->state = TA_ACTOR_RUNNABLE;
        ta_index_queue_push(&system->run_queue, &system->actors, 0, index);
        system->runnable_count++;
    }
}

void ta_system_mailbox_push(ta_system *system, uint32_t index, uint32_t event_index) {
    ta_actor *actor = ta_actor_at(system, index);
    system->stats_delivered++;
    ta_event *event = ta_event_at(&system->core, event_index);
    event->next = TA_NIL;
    if (actor->mailbox_tail == TA_NIL)
        actor->mailbox_head = event_index;
    else
        ta_event_at(&system->core, actor->mailbox_tail)->next = event_index;
    actor->mailbox_tail = event_index;
    actor->mailbox_length++;
    ta_system_make_runnable(system, index);
}

void ta_system_actor_variables(ta_system *system, const ta_actor *actor,
                               ta_system_variables_buffer *buffer,
                               ta_datamodel_system_variables *variables) {
    uint32_t sl = ta_format_u64(buffer->session_id, actor->serial);
    uint32_t ll = 0;
    for (uint32_t i = 0; i < system->config.io_processor_count;
         i++) /* remote-addressable base URI */
        if ((system->config.io_processors[i].flags & TA_IO_PROCESSOR_SCXML_REMOTE) &&
            system->config.io_processors[i].location.length < sizeof buffer->location - 40) {
            ll = system->config.io_processors[i].location.length;
            ta_memcpy(buffer->location, system->config.io_processors[i].location.pointer, ll);
            break;
        }
    ta_memcpy(buffer->location + ll, "#_scxml_", 8);
    ll += 8;
    ll += ta_format_u64(buffer->location + ll, actor->serial);
    uint32_t nio = 1;
    buffer->io_processors[0].type.pointer = TA_SCXML_IO_PROCESSOR;
    buffer->io_processors[0].type.length = (uint32_t)sizeof TA_SCXML_IO_PROCESSOR - 1;
    buffer->io_processors[0].location.pointer = buffer->location;
    buffer->io_processors[0].location.length = ll;
    for (uint32_t i = 0; i < system->config.io_processor_count && nio < 17; i++, nio++) {
        buffer->io_processors[nio].type = system->config.io_processors[i].type;
        buffer->io_processors[nio].location = system->config.io_processors[i].location;
    }
    variables->size = sizeof *variables;
    variables->io_processor_count = nio;
    variables->sessionid.pointer = buffer->session_id;
    variables->sessionid.length = sl;
    variables->name = ta_shared_string_get(&system->core, actor->name);
    variables->io_processors = buffer->io_processors;
}

uint32_t ta_system_spawn_actor(ta_system *system, uint32_t chart, ta_string_view name,
                               ta_payload params, uint32_t parent, uint32_t parent_slot,
                               ta_status *status) {
    ta_core *core = &system->core;
    *status = TA_OK;
    if (chart >= system->chart_count) {
        *status = TA_ERROR_INVALID;
        return TA_NIL;
    }
    uint32_t index = ta_pool_alloc(&system->actors);
    if (index == TA_NIL) {
        *status = TA_ERROR_NO_MEMORY;
        return TA_NIL;
    }
    ta_actor *actor = ta_actor_at(system, index);
    ta_memset(actor, 0, sizeof *actor);
    actor->next = TA_NIL;
    actor->mailbox_head = actor->mailbox_tail = actor->timers = TA_NIL;
    actor->parent = parent;
    actor->parent_slot = parent_slot;
    actor->serial = ++system->next_serial;
    actor->name = TA_NIL;
    actor->instance = ta_slab_alloc(&system->slab, ta_core_chart_instance_size(core, chart));
    ta_string_view cname =
        ta_image_view_string(&core->image_view, core->image_view.charts[chart].name);
    int same = name.length == cname.length &&
               (!name.length || ta_memcmp(name.pointer, cname.pointer, name.length) == 0);
    actor->name = (name.length && !same) ? ta_shared_string_new(core, name.pointer, name.length)
                                         : core->image_view.charts[chart].name;
    if (actor->instance == TA_NIL || actor->name == TA_NIL) {
        if (actor->instance != TA_NIL) ta_slab_free(&system->slab, actor->instance);
        ta_shared_string_release(core, actor->name);
        ta_pool_free(&system->actors, index);
        *status = TA_ERROR_NO_MEMORY;
        return TA_NIL;
    }
    /* system variables (copied by the datamodel during create) */
    ta_system_variables_buffer buffer;
    ta_datamodel_system_variables variables;
    ta_system_actor_variables(system, actor, &buffer, &variables);
    uint32_t handle = ta_pool_handle(&system->actors, index);
    ta_status ist = ta_core_instance_init(core, ta_actor_instance(system, actor), chart, handle,
                                          &variables, params);
    if (ist != TA_OK) {
        ta_slab_free(&system->slab, actor->instance);
        ta_shared_string_release(core, actor->name);
        ta_pool_free(&system->actors, index);
        *status = ist;
        return TA_NIL;
    }
    ta_system_session_insert(system, index);
    actor->state = TA_ACTOR_IDLE;
    actor->flags |= ACTOR_FLAG_TOUCHED;
    ta_system_make_runnable(system, index);
    return index;
}

static int unqueue(ta_system *system, ta_index_queue *queue, uint32_t index) {
    uint32_t prev = TA_NIL, cur = queue->head;
    while (cur != TA_NIL && cur != index) {
        prev = cur;
        cur = ta_actor_at(system, cur)->next;
    }
    if (cur == TA_NIL) return 0;
    uint32_t nxt = ta_actor_at(system, cur)->next;
    if (prev == TA_NIL)
        queue->head = nxt;
    else
        ta_actor_at(system, prev)->next = nxt;
    if (queue->tail == index) queue->tail = prev;
    return 1;
}

void ta_system_reclaim(ta_system *system, uint32_t index) {
    ta_core *core = &system->core;
    ta_actor *actor = ta_actor_at(system, index);
    if (actor->state == TA_ACTOR_RUNNABLE && unqueue(system, &system->run_queue, index))
        system->runnable_count--;
    if (actor->state == TA_ACTOR_WAIT_WORKSPACE) unqueue(system, &system->workspace_queue, index);
    int hibernated = actor->state == TA_ACTOR_HIBERNATED;
    if (hibernated) {
        if (actor->flags & ACTOR_FLAG_IN_WAKE_QUEUE) unqueue(system, &system->wake_queue, index);
        if (actor->flags & ACTOR_FLAG_WAKE_SENT) system->pending_wake_count--;
        system->hibernated_count--;
    }
    for (uint32_t event = actor->mailbox_head; event != TA_NIL;) {
        uint32_t nx = ta_event_at(core, event)->next;
        ta_event_release(core, event);
        event = nx;
    }
    actor->mailbox_head = actor->mailbox_tail = TA_NIL;
    actor->mailbox_length = 0;
    ta_system_cancel_timers(system, index);
    if (!hibernated) { /* a hibernated actor's state lives in the host's blob */
        ta_core_instance_deinit(core, ta_actor_instance(system, actor));
        ta_slab_free(&system->slab, actor->instance);
    }
    ta_shared_string_release(core, actor->name);
    session_remove(system, index);
    actor->state = TA_ACTOR_FREE;
    ta_pool_free(&system->actors, index);
}

/* ---- public actor API --------------------------------------------------------------------- */

#define GUARD(s)                                                                                   \
    do {                                                                                           \
        if ((s)->in_call) return TA_ERROR_STATE;                                                   \
    } while (0)

ta_status ta_system_boot(ta_system *system) {
    GUARD(system);
    const ta_image_view *view = &system->core.image_view;
    for (uint32_t i = 0; i < view->actor_count; i++) {
        ta_status status;
        ta_system_spawn_actor(system, view->actors[i].chart,
                              ta_image_view_string(view, view->actors[i].name), 0, 0, TA_NIL,
                              &status);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

ta_status ta_system_spawn(ta_system *system, uint32_t chart, ta_string_view name, ta_payload params,
                          uint32_t *actor) {
    GUARD(system);
    ta_status status;
    uint32_t index = ta_system_spawn_actor(system, chart, name, params, 0, TA_NIL, &status);
    if (actor) *actor = index == TA_NIL ? 0 : ta_pool_handle(&system->actors, index);
    return status;
}

ta_status ta_system_set_next_serial(ta_system *system, uint64_t serial) {
    GUARD(system);
    if (serial < system->next_serial) return TA_ERROR_INVALID; /* ids are never reused */
    system->next_serial = serial;
    return TA_OK;
}

ta_status ta_system_terminate(ta_system *system, uint32_t actor) {
    GUARD(system);
    uint32_t index = ta_system_resolve_actor(system, actor);
    if (index == TA_NIL) return TA_ERROR_STALE;
    ta_actor *actor_record = ta_actor_at(system, index);
    actor_record->flags |=
        ACTOR_FLAG_CANCEL | ACTOR_FLAG_ORPHAN; /* exits at its next stable point; no done.invoke */
    if (actor_record->state != TA_ACTOR_HIBERNATED) /* else: applied when it is woken */
        ta_core_cancel(&system->core, ta_actor_instance(system, actor_record));
    ta_system_make_runnable(system, index);
    return TA_OK;
}

void ta_system_stats_get(const ta_system *system, ta_system_stats *o) {
    ta_system_stats z;
    ta_memset(&z, 0, sizeof z);
    z.size = sizeof z;
    z.actors = system->actors.used;
    z.actors_capacity = system->actors.capacity;
    z.events = system->events.used;
    z.events_capacity = system->events.capacity;
    z.timers = system->timers.used;
    z.timers_capacity = system->timers.capacity;
    z.workspaces = system->workspaces.used;
    z.workspaces_capacity = system->workspaces.capacity;
    z.requests = system->requests.used;
    z.requests_capacity = system->requests.capacity;
    z.slab_pages = system->slab.used_pages;
    z.slab_pages_capacity = system->slab.page_count;
    z.slab_page_bytes = TA_SLAB_PAGE;
    z.outbox = system->effects_length - system->effects_head;
    z.outbox_capacity = system->effects_capacity;
    z.runnable = system->runnable_count;
    z.region_bytes = system->region_size;
    z.delivered = system->stats_delivered;
    z.fuel = system->stats_fuel;
    z.turns = system->stats_turns;
    z.timers_fired = system->stats_timers;
    z.spawned = system->next_serial;
    z.now = system->now;
    z.hibernated = system->hibernated_count;
    z.wakes_pending = system->pending_wake_count;
    z.hibernations = system->stats_hibernations;
    z.wakes = system->stats_wakes;
    uint32_t n = o->size && o->size < sizeof z ? o->size : (uint32_t)sizeof z;
    ta_memcpy(o, &z, n);
    o->size = n;
}

uint32_t ta_system_actor_by_session(const ta_system *system, uint64_t sessionid) {
    uint32_t index = ta_system_session_lookup(system, sessionid);
    return index == TA_NIL ? 0 : ta_pool_handle(&system->actors, index);
}

uint32_t ta_system_actors(const ta_system *system, uint32_t *cursor, uint32_t *out,
                          uint32_t capacity) {
    uint32_t n = 0, i = *cursor;
    for (; i < system->actors.high_water_mark && n < capacity; i++)
        if (ta_pool_live(&system->actors, i)) out[n++] = ta_pool_handle(&system->actors, i);
    *cursor = i;
    return n;
}

uint32_t ta_system_actor_by_name(const ta_system *system, ta_string_view name) {
    for (uint32_t i = 0; i < system->actors.high_water_mark; i++) {
        if (!ta_pool_live(&system->actors, i)) continue;
        ta_string_view n = ta_shared_string_get(&system->core, ta_actor_at(system, i)->name);
        if (n.length == name.length &&
            (n.length == 0 || ta_memcmp(n.pointer, name.pointer, n.length) == 0))
            return ta_pool_handle(&system->actors, i);
    }
    return 0;
}

void ta_system_deinit(ta_system *system) {
    if (!system || system->magic != TA_SYSTEM_MAGIC) return;
    /* the queues die with the system: drop them first so reclaiming a runnable
     * actor does not unlink it from a (singly linked) queue — O(n) each */
    ta_index_queue_init(&system->run_queue);
    ta_index_queue_init(&system->workspace_queue);
    ta_index_queue_init(&system->wake_queue);
    system->runnable_count = 0;
    for (uint32_t i = 0; i < system->actors.high_water_mark; i++) {
        if (!ta_pool_live(&system->actors, i)) continue;
        ta_actor *actor = ta_actor_at(system, i);
        if (actor->state != TA_ACTOR_HIBERNATED) actor->state = TA_ACTOR_IDLE;
        actor->flags &= (uint8_t)~ACTOR_FLAG_IN_WAKE_QUEUE;
        ta_system_reclaim(system, i);
    }
    ta_effect_record *effect = SYSTEM_POINTER(system, effects_relative, ta_effect_record);
    for (uint32_t i = system->effects_head; i < system->effects_length; i++)
        ta_system_effect_release(system, &effect[i]);
    system->effects_head = system->effects_length = 0;
    for (uint32_t i = 0; i < system->requests.high_water_mark; i++) {
        if (!ta_pool_live(&system->requests, i)) continue;
        ta_request *request = (ta_request *)ta_pool_at(&system->requests, i);
        ta_shared_string_release(&system->core, request->invokeid);
        if (request->kind != REQUEST_PENDING_SPAWN)
            ta_shared_string_release(&system->core, request->source);
        if (request->payload && request->pad < system->datamodel_count) {
            /* params of an unresolved invoke belong to the parent's datamodel */
            const ta_datamodel_binding *binding = &system->datamodels[request->pad];
            if (binding->vtable->release)
                binding->vtable->release(binding->datamodel, request->payload);
        }
        ta_pool_free(&system->requests, i);
    }
    ta_core_release_out(&system->core);
    detach_charts(system);
    system->magic = 0;
}

/* ---- inspection ---------------------------------------------------------------------------- */

ta_status ta_actor_info_get(const ta_system *system, uint32_t actor, ta_actor_info *out) {
    uint32_t index = ta_system_resolve_actor(system, actor);
    if (index == TA_NIL) return TA_ERROR_STALE;
    const ta_actor *actor_record = ta_actor_at(system, index);
    ta_actor_info r;
    ta_memset(&r, 0, sizeof r);
    r.state = actor_record->state;
    r.parent = actor_record->parent;
    r.mailbox_length = actor_record->mailbox_length;
    r.serial = actor_record->serial;
    if (actor_record->state == TA_ACTOR_HIBERNATED) {
        r.chart = actor_record->instance;
        r.current_event_name = TA_NIL;
    } else {
        const ta_instance *instance = ta_actor_instance(system, actor_record);
        r.chart = instance->chart;
        r.in_macrostep = instance->phase != PHASE_STABLE && instance->phase != PHASE_DONE;
        r.current_event_name = instance->current_event == TA_NIL
                                   ? TA_NIL
                                   : ta_event_at(&system->core, instance->current_event)->name_id;
        r.datamodel_instance = instance->datamodel;
        const ta_chart_record *chart = &system->core.image_view.charts[instance->chart];
        const uint32_t *kids =
            ta_instance_word((ta_instance *)instance, ta_instance_layout_of(chart).invoke_children);
        for (uint32_t i = 0; i < chart->invoke_count;
             i++) /* not children that vanished (faulted) */
            r.invocations += kids[i] && ((kids[i] & TA_EXTERNAL_INVOKE_BIT) ||
                                         ta_system_resolve_actor(system, kids[i]) != TA_NIL);
    }
    size_t n = out->size && out->size < sizeof r ? out->size : sizeof r; /* size-first */
    r.size = (uint32_t)n;
    ta_memcpy(out, &r, n);
    return TA_OK;
}

uint32_t ta_actor_config(const ta_system *system, uint32_t actor, uint32_t *states,
                         uint32_t capacity) {
    uint32_t index = ta_system_resolve_actor(system, actor);
    if (index == TA_NIL) return 0;
    const ta_actor *actor_record = ta_actor_at(system, index);
    if (actor_record->state == TA_ACTOR_HIBERNATED) return 0; /* its configuration is in the blob */
    ta_instance *instance = ta_actor_instance(system, actor_record);
    uint32_t n = 0, ns = system->core.image_view.charts[instance->chart].state_count;
    for (uint32_t state = 1; state < ns; state++)
        if (ta_core_in_state(&system->core, instance, state)) {
            if (n < capacity) states[n] = state;
            n++;
        }
    return n;
}

/* ---- relocation ------------------------------------------------------------------------------ */

ta_status ta_system_relocate(ta_system *system, void *new_memory, size_t new_length,
                             const ta_system_config *new_config, ta_system **out) {
    GUARD(system);
    const ta_image_view *view = &system->core.image_view;
    ta_system_config config;
    normalize_config(view, new_config ? new_config : &system->config, &config);
    config.io_processors = system->config.io_processors;
    config.io_processor_count = system->config.io_processor_count;
    if (config.max_actors < system->actors.high_water_mark ||
        config.max_events < system->events.high_water_mark ||
        config.max_timers < system->config.max_timers ||
        config.workspaces < system->workspaces.high_water_mark ||
        config.max_requests < system->requests.high_water_mark ||
        config.outbox < system->effects_length - system->effects_head)
        return TA_ERROR_INVALID;
    ta_arena arena;
    carved o;
    ta_system *new_system = place(new_memory, new_length, view, &config, &o, &arena);
    if (!new_system) return TA_ERROR_NO_MEMORY;
    if (o.page_count < system->slab.high_water_mark) return TA_ERROR_INVALID;
    *new_system = *system; /* scalar state; pools/heap/arrays re-pointed below */
    new_system->region_size = arena.offset;
    new_system->config = config;
    ta_pool_copy(&new_system->actors, ta_arena_pointer(&arena, o.actor_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.actor_generations), config.max_actors,
                 &system->actors);
    ta_pool_copy(&new_system->events, ta_arena_pointer(&arena, o.event_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.event_generations), config.max_events,
                 &system->events);
    ta_pool_copy(&new_system->timers, ta_arena_pointer(&arena, o.timer_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.timer_generations), config.max_timers,
                 &system->timers);
    ta_heap_copy(&new_system->heap, (ta_heap_node *)ta_arena_pointer(&arena, o.heap_nodes),
                 (uint32_t *)ta_arena_pointer(&arena, o.heap_positions), config.max_timers,
                 &system->heap);
    ta_pool_copy(&new_system->workspaces, ta_arena_pointer(&arena, o.workspace_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.workspace_generations),
                 config.workspaces, &system->workspaces);
    ta_pool_copy(&new_system->requests, ta_arena_pointer(&arena, o.request_slots),
                 (unsigned char *)ta_arena_pointer(&arena, o.request_generations),
                 config.max_requests, &system->requests);
    ta_slab_copy(&new_system->slab, ta_arena_pointer(&arena, o.slab_pages),
                 (ta_slab_page *)ta_arena_pointer(&arena, o.slab_metadata), o.page_count,
                 &system->slab);
    set_rels(new_system, &arena, &o);
    new_system->effects_capacity = config.outbox;
    ta_effect_record *old_effects = SYSTEM_POINTER(system, effects_relative, ta_effect_record),
                     *new_effects = SYSTEM_POINTER(new_system, effects_relative, ta_effect_record);
    new_system->effects_length = system->effects_length - system->effects_head;
    new_system->effects_head = 0;
    for (uint32_t i = 0; i < new_system->effects_length; i++)
        new_effects[i] = old_effects[system->effects_head + i];
    ta_memcpy(SYSTEM_POINTER(new_system, chart_datamodels_relative, uint8_t),
              SYSTEM_POINTER(system, chart_datamodels_relative, uint8_t), system->chart_count);
    ta_memcpy(SYSTEM_POINTER(new_system, chart_contexts_relative, void *),
              SYSTEM_POINTER(system, chart_contexts_relative, void *),
              sizeof(void *) * system->chart_count);
    uint32_t *session = SYSTEM_POINTER(new_system, sessions_relative, uint32_t);
    for (uint32_t i = 0; i < new_system->session_capacity; i++)
        session[i] = TA_NIL;
    new_system->session_count = 0;
    rebind(new_system, system->core.image);
    for (uint32_t i = 0; i < new_system->actors.high_water_mark; i++)
        if (ta_pool_live(&new_system->actors, i)) ta_system_session_insert(new_system, i);
    system->magic = 0; /* the old region is dead */
    *out = new_system;
    return TA_OK;
}

/* ---- snapshot / restore ------------------------------------------------------------------------
 */

ta_status ta_system_snapshot(const ta_system *system, void *buffer, size_t capacity,
                             size_t *length) {
    if (system->in_call) return TA_ERROR_STATE;
    if (system->effects_length != system->effects_head)
        return TA_ERROR_STATE; /* drain effects first */
    *length = (size_t)system->region_size;
    if (!buffer) return TA_OK;
    if (capacity < system->region_size) return TA_ERROR_NO_MEMORY;
    ta_memcpy(buffer, system, (size_t)system->region_size);
    /* Only real state goes into the blob. Everything rebind() recomputes from
     * the image, config and datamodel bindings is derived (host and
     * self-pointers included), the kernel's `out`/`scratch` are transient and
     * empty between calls, and the datamodel contexts are re-attached by the
     * restore. Only the image hash survives: the restore validates it. */
    ta_system *t = (ta_system *)buffer;
    ta_core *core = &t->core;
    uint64_t hash = core->image_view.hash;
    uint32_t dropped = core->dropped_errors, overrun = core->overrun;
    ta_memset(core, 0, sizeof *core);
    core->image_view.hash = hash;
    core->dropped_errors = dropped;
    core->overrun = overrun;
    ta_memset(t->datamodels, 0, sizeof t->datamodels); /* host pointers; re-bound on restore */
    t->config.io_processors = (const ta_io_processor_descriptor *)0;
    ta_memset((unsigned char *)buffer + t->chart_contexts_relative, 0,
              sizeof(void *) * t->chart_count);
    return TA_OK;
}

ta_status ta_system_restore(void *memory, size_t length, const ta_image *image, const void *snap,
                            size_t snap_length, const ta_system_config *host,
                            const ta_datamodel_binding *datamodels, uint32_t datamodel_count,
                            ta_system **out) {
    *out = (ta_system *)0;
    const ta_image_view *view = ta_image_view_of(image);
    if (((uintptr_t)memory & (TA_ALIGN_MAX - 1)) || length < snap_length ||
        snap_length < sizeof(ta_system))
        return TA_ERROR_INVALID;
    const ta_system *old = (const ta_system *)snap;
    if (old->magic != TA_SYSTEM_MAGIC || old->region_size != snap_length) return TA_ERROR_INVALID;
    if (old->core.image_view.hash != view->hash) return TA_ERROR_INVALID; /* different image */
    ta_memcpy(memory, snap, snap_length);
    ta_system *system = (ta_system *)memory;
    ta_memset(system->datamodels, 0, sizeof system->datamodels);
    system->config.io_processors =
        host ? host->io_processors : (const ta_io_processor_descriptor *)0;
    system->config.io_processor_count = host ? host->io_processor_count : 0;
    ta_status status = bind_datamodels(system, image, datamodels, datamodel_count);
    if (status != TA_OK) return status;
    rebind(system, image);
    status = attach_charts(system, image);
    if (status != TA_OK) return status;
    *out = system;
    return TA_OK;
}
