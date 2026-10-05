#include "hosted.h"

#include <stdlib.h>
#include <string.h>

#include <tinyactors_host.h>

#include "allocator.h"
#include "instruments.h"
#include "json_datamodel.h"
#include "rpc.h"
#include "value_codec.h"
#include "ta_rpc.h"

/*
 * The test host's platform for the kernel: libc memory, and a clock the
 * scenario owns. It starts at zero and moves only when a scenario says so,
 * so a hosted run is reproducible; the random seed is fixed for the same
 * reason.
 */
typedef struct {
    uint64_t now_ns;
} tat_hosted_clock;

typedef struct {
    tah_system *system;
    tat_hosted_clock clock;
    /* The recorder's records since the last hosted.records, in turn order:
     * [{"family", "name", "at", "event": {...}}], and the wakes they gave,
     * {"family/name": at or null}, the latest for each address (kernel ADR
     * 0006 section 2). */
    jv records;
    jv wakes;
} tat_hosted;

static uint64_t tat_hosted_monotonic(void *self) {
    return ((tat_hosted_clock *)self)->now_ns;
}

static uint64_t tat_hosted_wall(void *self) {
    return ((tat_hosted_clock *)self)->now_ns;
}

static uint64_t tat_hosted_seed(void *self) {
    (void)self;
    return 0x74696e7961637473ull;
}

/* Turns run when a scenario asks (hosted.turn), so a wake needs no loop. */
/* Turns run when a scenario asks for them: the request stands until then. */
static bool tat_hosted_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
    return true;
}

static bool tat_hosted_resolve(tat_bind *bind, jv params, tat_hosted **out) {
    jv handle = jv_object_get(jv_copy(params), jv_string("system"));
    void *resolved = NULL;
    bool ok = tat_handle_resolve(bind, handle, "hosted", &resolved);
    jv_free(handle);
    *out = resolved;
    return ok;
}

static uint64_t tat_hosted_u64(jv params, const char *name) {
    jv value = jv_object_get(jv_copy(params), jv_string(name));
    uint64_t out = jv_is_valid(value) && jv_get_kind(value) == JV_KIND_NUMBER ? (uint64_t)jv_number_value(value) : 0;
    jv_free(value);
    return out;
}

static const tah_platform_vtable tat_hosted_platform_vtable = {
    .monotonic_ns = tat_hosted_monotonic,
    .wall_ns = tat_hosted_wall,
    .random_seed = tat_hosted_seed,
};

jv tat_hosted_create_method(tat_bind *bind, jv params, int *error_code) {
    jv incarnation = jv_object_get(jv_copy(params), jv_string("incarnation"));
    uint64_t chosen = 0;
    if (jv_is_valid(incarnation) && jv_get_kind(incarnation) == JV_KIND_NUMBER) {
        chosen = (uint64_t)jv_number_value(incarnation);
    }
    jv_free(incarnation);
    /* The platform's memory: libc, or an allocator adapter such as the
     * scenario's allocator.counting, whose budget and faults then reach the
     * kernel and its core (core ADR 0023). */
    ta_allocator allocator = tat_libc_allocator();
    jv chosen_allocator = jv_object_get(jv_copy(params), jv_string("allocator"));
    bool decoded = !jv_is_valid(chosen_allocator) || jv_get_kind(chosen_allocator) == JV_KIND_NULL ||
                   tat_adapter_decode(bind, chosen_allocator, "ta_allocator", &allocator, sizeof allocator);
    jv_free(chosen_allocator);
    if (!decoded) {
        return tat_bind_take_error(bind, error_code);
    }
    tat_hosted *hosted = calloc(1, sizeof *hosted);
    if (hosted == NULL) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.create: out of memory");
    }
    tah_platform platform = {
        .self = &hosted->clock, .v = &tat_hosted_platform_vtable, .allocator = allocator};
    /* The kernel's limit on queued messages (napi ADR 0035), when given. */
    tah_params hosting;
    tah_params_defaults(&hosting);
    hosting.values[TAH_PARAM_MAX_QUEUED_MESSAGES] = tat_hosted_u64(params, "max_queued_messages");
    ta_result result = tah_system_create(
        &(tah_system_create_params){.platform = platform, .incarnation = chosen, .hosting = &hosting}, &hosted->system);
    if (result != TA_OK) {
        free(hosted);
        return tat_encode_ta_result(bind, &result);
    }
    hosted->records = jv_array();
    hosted->wakes = jv_object();
    tah_system_set_driver(hosted->system, &(tah_pump_driver){ .wake = tat_hosted_wake });
    (void)tah_system_set_clock(hosted->system, TAH_CLOCK_MANUAL, 0);
    jv out = jv_object();
    out = jv_object_set(out, jv_string("hosted"), tat_handle_register(bind, "hosted", hosted));
    /* The kernel's core system, for the generated core methods (defining
     * charts, inspecting actors); retired when the hosted system closes. */
    out = jv_object_set(out, jv_string("core"), tat_handle_register(bind, "system", tah_system_core(hosted->system)));
    return out;
}

jv tat_hosted_close_method(tat_bind *bind, jv params, int *error_code) {
    jv handle = jv_object_get(jv_copy(params), jv_string("system"));
    void *resolved = NULL;
    bool ok = tat_handle_resolve(bind, handle, "hosted", &resolved);
    jv_free(handle);
    if (!ok) {
        return tat_bind_take_error(bind, error_code);
    }
    tat_hosted *hosted = resolved;
    tat_handle_retire(bind, tah_system_core(hosted->system));
    tah_system_close(hosted->system);
    size_t leaked = tah_system_leaked_bytes(hosted->system);
    tah_system_free(hosted->system);
    jv_free(hosted->records);
    jv_free(hosted->wakes);
    tat_handle_retire(bind, hosted);
    free(hosted);
    return jv_object_set(jv_object(), jv_string("leaked"), jv_number((double)leaked));
}

/* Spawns an actor of `definition`, at `address` when it is not NULL,
 * through core, and hands it to the kernel's scheduler. */
static ta_result tat_hosted_spawn_at(tah_system *system, ta_definition_id definition, const ta_actor_address *address,
                                     ta_actor_id *actor) {
    tah_system_enter_core(system);
    ta_result result = ta_actor_spawn(tah_system_core(system), &(ta_actor_spawn_params){
                                                                   .definition = definition, .address = address, .out = actor});
    tah_system_leave_core(system);
    return result == TA_OK ? tah_hook_actor_created(system, *actor) : result;
}

/* Spawns an actor through core and hands it to the kernel's scheduler. */
jv tat_hosted_spawn_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv family = jv_object_get(jv_copy(params), jv_string("family"));
    jv name = jv_object_get(jv_copy(params), jv_string("name"));
    bool named = jv_get_kind(family) == JV_KIND_STRING && jv_get_kind(name) == JV_KIND_STRING;
    ta_actor_address address = {0};
    if (named) {
        address.family.name = (ta_str){jv_string_value(family), (uint32_t)jv_string_length_bytes(jv_copy(family))};
        address.name = (ta_str){jv_string_value(name), (uint32_t)jv_string_length_bytes(jv_copy(name))};
    }
    ta_actor_id actor = 0;
    ta_result result = tat_hosted_spawn_at(hosted->system, (ta_definition_id)tat_hosted_u64(params, "definition"),
                                           named ? &address : NULL, &actor);
    jv_free(family);
    jv_free(name);
    if (result != TA_OK) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.spawn: the actor could not be spawned");
    }
    return jv_number((double)actor);
}

/* Posts an event into an actor's mailbox, or to a family and name. */
jv tat_hosted_post_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv text = jv_object_get(jv_copy(params), jv_string("event"));
    if (jv_get_kind(text) != JV_KIND_STRING) {
        jv_free(text);
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("hosted.post: params.event must be an event name");
    }
    ta_event_name_id name = 0;
    ta_result result = tah_event_name_intern(
        tah_system_core(hosted->system), (ta_str){jv_string_value(text), (uint32_t)jv_string_length_bytes(jv_copy(text))},
        &name);
    jv_free(text);
    ta_event event = {.name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE};
    jv data = jv_object_get(jv_copy(params), jv_string("data"));
    if (result == TA_OK && jv_is_valid(data) && jv_get_kind(data) != JV_KIND_NULL) {
        /* Hosted charts use the jq data model. */
        result = tat_value_decode(tat_json_datamodel(&bind->json), jv_copy(data), &event.data);
    }
    jv_free(data);
    jv family = jv_object_get(jv_copy(params), jv_string("family"));
    jv actor_name = jv_object_get(jv_copy(params), jv_string("name"));
    if (result == TA_OK && jv_get_kind(family) == JV_KIND_STRING && jv_get_kind(actor_name) == JV_KIND_STRING) {
        /* By name, as an application posts: through the directory, or into a
         * load when the address has no actor and its family has a loader. */
        ta_actor_address text = {
            .family = {.name = {jv_string_value(family), (uint32_t)jv_string_length_bytes(jv_copy(family))}},
            .name = {jv_string_value(actor_name), (uint32_t)jv_string_length_bytes(jv_copy(actor_name))}};
        ta_address *address = NULL;
        result = ta_address_intern(tah_system_core(hosted->system), &text, &address);
        if (result == TA_OK) {
            result = tah_system_post_named(hosted->system, address, &(tah_message){.event = &event});
            ta_address_release(address);
        }
    } else if (result == TA_OK) {
        result = tah_system_post(hosted->system,
                                 &(tah_message){.target = (ta_actor_id)tat_hosted_u64(params, "actor"), .event = &event});
    }
    if (result != TA_OK) {
        ta_value_release(&event.data);
    }
    jv_free(family);
    jv_free(actor_name);
    return tat_encode_ta_result(bind, &result);
}

static jv tat_hosted_text(ta_str text) {
    return jv_string_sized(text.ptr, (int)text.len);
}

static const char *tat_hosted_event_type(ta_event_type type) {
    return type == TA_EVENT_PLATFORM ? "platform" : type == TA_EVENT_INTERNAL ? "internal" : "external";
}

/* The recorder's records and wakes, as the application would journal them (kernel ADR 0006 section 2). */
static void tat_hosted_take_records(tat_hosted *hosted) {
    tah_system *system = hosted->system;
    ta_arena arena;
    bool arena_ready = ta_arena_init(&arena, tat_libc_allocator(), 1024) == TA_OK;
    for (uint32_t index = 0; arena_ready && index < tah_recorder_count(system); index += 1u) {
        tah_record record = tah_recorder_record(system, index);
        tah_record_texts texts;
        if (tah_recorder_texts(system, index, ta_arena_allocator(&arena), &texts) != TA_OK) {
            continue;
        }
        jv event = jv_object_set(jv_object(), jv_string("name"), tat_hosted_text(texts.name));
        event = jv_object_set(event, jv_string("type"), jv_string(tat_hosted_event_type(texts.type)));
        if (texts.send_id.len != 0) {
            event = jv_object_set(event, jv_string("send_id"), tat_hosted_text(texts.send_id));
        }
        if (texts.origin.len != 0) {
            event = jv_object_set(event, jv_string("origin"), tat_hosted_text(texts.origin));
            event = jv_object_set(event, jv_string("origin_type"), tat_hosted_text(texts.origin_type));
        }
        if (texts.invoke_id.len != 0) {
            event = jv_object_set(event, jv_string("invoke_id"), tat_hosted_text(texts.invoke_id));
        }
        if (record.event->data.v != NULL) {
            event = jv_object_set(event, jv_string("data"), tat_value_encode(&record.event->data));
        }
        jv item = jv_object_set(jv_object(), jv_string("family"), tat_hosted_text(record.address.family.name));
        item = jv_object_set(item, jv_string("name"), tat_hosted_text(record.address.name));
        item = jv_object_set(item, jv_string("at"), jv_number((double)record.at));
        hosted->records = jv_array_append(hosted->records, jv_object_set(item, jv_string("event"), event));
        ta_arena_reset(&arena);
    }
    if (arena_ready) {
        ta_arena_destroy(&arena);
    }
    uint32_t count = 0;
    const tah_record_wake *wakes = tah_recorder_wakes(system, &count);
    for (uint32_t index = 0; index < count; index += 1u) {
        jv key = jv_string_fmt("%.*s/%.*s", (int)wakes[index].address.family.name.len,
                               wakes[index].address.family.name.ptr, (int)wakes[index].address.name.len,
                               wakes[index].address.name.ptr);
        hosted->wakes = jv_object_set(hosted->wakes, key,
                                      wakes[index].has_at ? jv_number((double)wakes[index].at) : jv_null());
    }
    tah_recorder_release(system);
}

/* The turn's dead letters, from its records (kernel ADR 0001 section 5):
 * {"actor", "family"?, "name"?, "event", "reason", "data"?} each. */
static jv tat_hosted_dead_letters(tah_system *system, jv list) {
    ta_system *core = tah_system_core(system);
    for (const tah_turn_record *record = tah_sched_records(tah_system_sched(system)); record != NULL;
         record = record->next) {
        if (record->kind != TAH_RECORD_DEAD_LETTER) {
            continue;
        }
        jv item = jv_object_set(jv_object(), jv_string("actor"), jv_number((double)record->actor));
        if (record->has_address) {
            item = jv_object_set(item, jv_string("family"), tat_hosted_text(record->address.family.name));
            item = jv_object_set(item, jv_string("name"), tat_hosted_text(record->address.name));
        }
        item = jv_object_set(item, jv_string("event"), tat_event_name_text(core, record->event));
        item = jv_object_set(item, jv_string("reason"), jv_string(tah_dead_reason_name(record->reason)));
        if (record->data.v != NULL) {
            item = jv_object_set(item, jv_string("data"), tat_value_encode(&record->data));
        }
        list = jv_array_append(list, item);
    }
    return list;
}

/* The loads started since the last call, which the scenario answers with
 * hosted.load: {"index", "generation", "family", "name", "why"} each. */
static jv tat_hosted_load_requests(tah_system *system, jv list) {
    tah_load_request request;
    while (tah_loads_next_request(tah_system_loads(system), &request)) {
        ta_actor_address address = ta_address_view(request.address);
        jv item = jv_object_set(jv_object(), jv_string("index"), jv_number(request.index));
        item = jv_object_set(item, jv_string("generation"), jv_number(request.generation));
        item = jv_object_set(item, jv_string("family"), tat_hosted_text(address.family.name));
        item = jv_object_set(item, jv_string("name"), tat_hosted_text(address.name));
        item = jv_object_set(item, jv_string("why"), jv_string(request.why == TAH_LOAD_WHY_TIMER     ? "timer"
                                                               : request.why == TAH_LOAD_WHY_SESSION ? "session"
                                                                                                     : "mail"));
        list = jv_array_append(list, item);
    }
    return list;
}

/* Runs turns until the system is quiet, at most `limit` (default 1). */
jv tat_hosted_turn_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    uint64_t limit = tat_hosted_u64(params, "limit");
    uint64_t turns = 0;
    jv dead = jv_array();
    jv loads = tat_hosted_load_requests(hosted->system, jv_array());
    for (; turns < (limit == 0 ? 1 : limit); turns += 1) {
        tah_system_turn(hosted->system);
        dead = tat_hosted_dead_letters(hosted->system, dead);
        tat_hosted_take_records(hosted);
        tah_system_end_turn(hosted->system);
        loads = tat_hosted_load_requests(hosted->system, loads);
        if (tah_system_quiet(hosted->system)) {
            turns += 1;
            break;
        }
    }
    jv out = jv_object_set(jv_object(), jv_string("turns"), jv_number((double)turns));
    out = jv_object_set(out, jv_string("quiet"), jv_bool(tah_system_quiet(hosted->system)));
    out = jv_object_set(out, jv_string("dead_letters"), dead);
    return jv_object_set(out, jv_string("loads"), loads);
}

static const char *tat_hosted_state_name(tah_sched_state state) {
    switch (state) {
    case TAH_SCHED_IDLE:
        return "idle";
    case TAH_SCHED_READY:
        return "ready";
    case TAH_SCHED_RUNNABLE:
        return "runnable";
    case TAH_SCHED_QUARANTINED:
        return "quarantined";
    case TAH_SCHED_NONE:
        break;
    }
    return "none";
}

/*
 * The mailboxes of `actors` (kernel ADR 0001 section 5), each
 * {"actor", "state", "depth", "events"}, with the queued events' names in
 * delivery order, and the scheduler's counters: messages queued,
 * delivered, and the dead letters reported so far, by reason.
 */
jv tat_hosted_mailboxes_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    tah_sched *sched = tah_system_sched(hosted->system);
    ta_system *core = tah_system_core(hosted->system);
    jv actors = jv_object_get(jv_copy(params), jv_string("actors"));
    int count = jv_get_kind(actors) == JV_KIND_ARRAY ? jv_array_length(jv_copy(actors)) : 0;
    jv boxes = jv_array();
    for (int index = 0; index < count; index += 1) {
        jv id = jv_array_get(jv_copy(actors), index);
        ta_actor_id actor = jv_get_kind(id) == JV_KIND_NUMBER ? (ta_actor_id)jv_number_value(id) : 0;
        jv_free(id);
        jv events = jv_array();
        for (uint32_t envelope = tah_sched_mailbox_first(sched, actor); envelope != 0;) {
            tah_queued_message message = tah_sched_queued_message(sched, envelope);
            events = jv_array_append(events, tat_event_name_text(core, message.event->name));
            envelope = message.next;
        }
        jv box = jv_object_set(jv_object(), jv_string("actor"), jv_number((double)actor));
        box = jv_object_set(box, jv_string("state"), jv_string(tat_hosted_state_name(tah_sched_state_of(sched, actor))));
        box = jv_object_set(box, jv_string("depth"), jv_number((double)tah_sched_mailbox_count(sched, actor)));
        boxes = jv_array_append(boxes, jv_object_set(box, jv_string("events"), events));
    }
    jv_free(actors);
    tah_sched_stats stats;
    tah_sched_read_stats(sched, &stats);
    jv reasons = jv_object();
    for (uint32_t reason = 0; reason < TAH_DEAD_REASONS; reason += 1u) {
        if (stats.dead_by_reason[reason] != 0) {
            reasons = jv_object_set(reasons, jv_string(tah_dead_reason_name((tah_dead_reason)reason)),
                                    jv_number((double)stats.dead_by_reason[reason]));
        }
    }
    jv dead = jv_object_set(jv_object(), jv_string("total"), jv_number((double)stats.dead_letters));
    jv out = jv_object_set(jv_object(), jv_string("mailboxes"), boxes);
    out = jv_object_set(out, jv_string("queued"), jv_number((double)stats.queued));
    out = jv_object_set(out, jv_string("delivered"), jv_number((double)stats.delivered));
    return jv_object_set(out, jv_string("dead_letters"), jv_object_set(dead, jv_string("by_reason"), reasons));
}

/* Moves the scenario's clock, and core's time with it, to `time` (ms). */
jv tat_hosted_advance_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    uint64_t time = tat_hosted_u64(params, "time");
    hosted->clock.now_ns = time * 1000000u;
    ta_result result = tah_system_advance_to(hosted->system, (ta_time)time);
    return tat_encode_ta_result(bind, &result);
}

/* Records the named actors of `families` (kernel ADR 0006 section 2). */
jv tat_hosted_record_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv list = jv_object_get(jv_copy(params), jv_string("families"));
    int count = jv_get_kind(list) == JV_KIND_ARRAY ? jv_array_length(jv_copy(list)) : 0;
    ta_str families[16];
    jv held[16];
    if (count > 16) {
        count = 16;
    }
    for (int index = 0; index < count; index += 1) {
        held[index] = jv_array_get(jv_copy(list), index);
        families[index] = (ta_str){jv_string_value(held[index]),
                                   (uint32_t)jv_string_length_bytes(jv_copy(held[index]))};
    }
    ta_result result = tah_recorder_configure(hosted->system, families, (uint32_t)count);
    for (int index = 0; index < count; index += 1) {
        jv_free(held[index]);
    }
    jv_free(list);
    return tat_encode_ta_result(bind, &result);
}

/* The records and wakes since the last call, and forgets them: {"records", "wakes"}. */
jv tat_hosted_records_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    tat_hosted_take_records(hosted);
    jv out = jv_object_set(jv_object(), jv_string("records"), hosted->records);
    out = jv_object_set(out, jv_string("wakes"), hosted->wakes);
    hosted->records = jv_array();
    hosted->wakes = jv_object();
    return out;
}

/* Evicts an idle named actor: its journal becomes its only state, and its wake its first pending send. */
jv tat_hosted_evict_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    ta_actor_id actor = (ta_actor_id)tat_hosted_u64(params, "actor");
    tah_system *system = hosted->system;
    ta_actor_inspection view;
    ta_result result = ta_actor_inspect(tah_system_core(system), actor, &view);
    if (result == TA_OK) {
        tah_recorder_evicted(system, actor, view.delayed_send_count != 0,
                             view.delayed_send_count != 0 ? view.delayed_sends[0].due : 0);
        tah_system_enter_core(system);
        result = ta_actor_destroy(tah_system_core(system), actor);
        tah_system_leave_core(system);
        if (result == TA_OK) {
            (void)tah_hook_actor_removed(system, actor);
        }
    }
    tat_hosted_take_records(hosted);
    return tat_encode_ta_result(bind, &result);
}

typedef struct {
    ta_definition_id definition;
    const ta_actor_address *address;
    ta_time at;     /* when the actor's life began */
    jv journal;     /* [{"event", "at"}], borrowed */
    ta_actor_id actor; /* out */
} tat_hosted_replay_params;

/* One journaled event, replayed into `actor` (kernel ADR 0006 section 3). */
static ta_result tat_hosted_replay_one(tat_bind *bind, tah_system *system, ta_actor_id actor, jv entry, ta_time *last) {
    jv event = jv_object_get(jv_copy(entry), jv_string("event"));
    jv at = jv_object_get(jv_copy(entry), jv_string("at"));
    jv texts[6] = {jv_object_get(jv_copy(event), jv_string("name")), jv_object_get(jv_copy(event), jv_string("type")),
                   jv_object_get(jv_copy(event), jv_string("send_id")), jv_object_get(jv_copy(event), jv_string("origin")),
                   jv_object_get(jv_copy(event), jv_string("origin_type")),
                   jv_object_get(jv_copy(event), jv_string("invoke_id"))};
    ta_str views[6];
    for (int index = 0; index < 6; index += 1) {
        views[index] = jv_get_kind(texts[index]) == JV_KIND_STRING
                           ? (ta_str){jv_string_value(texts[index]), (uint32_t)jv_string_length_bytes(jv_copy(texts[index]))}
                           : (ta_str){0};
    }
    ta_event_type type = TA_EVENT_EXTERNAL;
    if (views[1].len == 8 && memcmp(views[1].ptr, "platform", 8) == 0) {
        type = TA_EVENT_PLATFORM;
    } else if (views[1].len == 8 && memcmp(views[1].ptr, "internal", 8) == 0) {
        type = TA_EVENT_INTERNAL;
    }
    ta_value value = {0};
    jv data = jv_object_get(jv_copy(event), jv_string("data"));
    ta_result result = TA_OK;
    if (jv_is_valid(data) && jv_get_kind(data) != JV_KIND_NULL) {
        result = tat_value_decode(tat_json_datamodel(&bind->json), jv_copy(data), &value);
    }
    ta_time time = jv_get_kind(at) == JV_KIND_NUMBER ? (ta_time)jv_number_value(at) : 0;
    if (result == TA_OK) {
        result = tah_replay_event(system, &(tah_replay_event_params){
                                              .actor = actor, .name = views[0], .type = type, .send_id = views[2],
                                              .origin = views[3], .origin_type = views[4], .invoke_id = views[5],
                                              .data = value.v != NULL ? &value : NULL, .time = time});
        *last = time;
    }
    ta_value_release(&value);
    jv_free(data);
    for (int index = 0; index < 6; index += 1) {
        jv_free(texts[index]);
    }
    jv_free(at);
    jv_free(event);
    return result;
}

/*
 * Rebuilds a named actor from its journal (kernel ADR 0006 section 3):
 * prepared at `at`, each event replayed at its time, published at the last
 * one's. TA_ERR_DONE when the journal ends in a final state: nothing is
 * published, and the prepared actor is gone.
 */
static ta_result tat_hosted_replay(tat_bind *bind, tah_system *system, tat_hosted_replay_params *params) {
    ta_time at = params->at;
    tah_system_enter_core(system);
    ta_result result = ta_actor_prepare(tah_system_core(system), &(ta_actor_prepare_params){
                                                                     .address = params->address,
                                                                     .definition = params->definition,
                                                                     .at = &at,
                                                                     .out = &params->actor});
    tah_system_leave_core(system);
    if (result == TA_OK) {
        result = tah_hook_actor_created(system, params->actor);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_time last = at;
    int count = jv_get_kind(params->journal) == JV_KIND_ARRAY ? jv_array_length(jv_copy(params->journal)) : 0;
    for (int index = 0; result == TA_OK && index < count; index += 1) {
        jv entry = jv_array_get(jv_copy(params->journal), index);
        result = tat_hosted_replay_one(bind, system, params->actor, entry, &last);
        jv_free(entry);
    }
    if (result == TA_OK) {
        result = tah_replay_events_done(system, params->actor);
    }
    if (result == TA_OK) {
        ta_actor_publish_params publish = {.actor = params->actor, .at = &last};
        tah_system_enter_core(system);
        result = ta_actor_publish(tah_system_core(system), &publish);
        tah_system_leave_core(system);
        if (result == TA_OK) {
            result = tah_hook_actor_published(system, &publish);
        }
    }
    if (result != TA_OK) {
        tah_system_enter_core(system);
        if (ta_actor_destroy(tah_system_core(system), params->actor) == TA_OK) {
            tah_system_leave_core(system);
            (void)tah_hook_actor_removed(system, params->actor);
        } else {
            tah_system_leave_core(system);
        }
        params->actor = 0;
    }
    return result;
}

/* Rebuilds `family/name` from its journal; answers the actor, or the ta_result name. */
jv tat_hosted_restore_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv journal = jv_object_get(jv_copy(params), jv_string("journal"));
    jv family = jv_object_get(jv_copy(params), jv_string("family"));
    jv name = jv_object_get(jv_copy(params), jv_string("name"));
    ta_actor_address address = {
        .family = {.name = {jv_string_value(family), (uint32_t)jv_string_length_bytes(jv_copy(family))}},
        .name = {jv_string_value(name), (uint32_t)jv_string_length_bytes(jv_copy(name))}};
    tat_hosted_replay_params replay = {.definition = (ta_definition_id)tat_hosted_u64(params, "definition"),
                                       .address = &address, .at = (ta_time)tat_hosted_u64(params, "at"),
                                       .journal = journal};
    ta_result result = tat_hosted_replay(bind, hosted->system, &replay);
    jv_free(journal);
    jv_free(family);
    jv_free(name);
    if (result != TA_OK) {
        return tat_encode_ta_result(bind, &result);
    }
    return jv_number((double)replay.actor);
}

/* Gives `family` a loader that lets `max_waiting` messages wait (0 removes it). */
jv tat_hosted_set_loader_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv family = jv_object_get(jv_copy(params), jv_string("family"));
    ta_result result = TA_ERR_INVALID;
    if (jv_get_kind(family) == JV_KIND_STRING && jv_string_length_bytes(jv_copy(family)) > 0) {
        result = tah_loads_set_family(
            tah_system_loads(hosted->system),
            (ta_str){jv_string_value(family), (uint32_t)jv_string_length_bytes(jv_copy(family))},
            (uint32_t)tat_hosted_u64(params, "max_waiting"));
    }
    jv_free(family);
    return tat_encode_ta_result(bind, &result);
}

static const struct {
    const char *name;
    tah_load_outcome outcome;
} tat_hosted_load_outcomes[] = {
    {"loaded", TAH_LOAD_LOADED},   {"spawned", TAH_LOAD_SPAWNED}, {"not-found", TAH_LOAD_NOT_FOUND},
    {"rejected", TAH_LOAD_REJECTED}, {"failed", TAH_LOAD_FAILED}, {"timed-out", TAH_LOAD_TIMED_OUT},
};

/*
 * Answers a load request hosted.turn handed out, as an application's loader
 * would, from the log the scenario stored for the address: "loaded" replays
 * `log` (hex) with the calls hosted.restore makes, or spawns a fresh actor
 * of `definition` when the log is empty; "spawned" spawns one; the other
 * outcomes turn the waiting mail into dead letters. Then the kernel finishes
 * the load, which moves the waiting mail into the actor's mailbox.
 * Answers {"result", "outcome", "actor"?}.
 */
jv tat_hosted_load_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    tah_system *system = hosted->system;
    jv text = jv_object_get(jv_copy(params), jv_string("outcome"));
    const char *wanted = jv_get_kind(text) == JV_KIND_STRING ? jv_string_value(text) : "loaded";
    tah_load_outcome outcome = TAH_LOAD_OUTCOMES;
    for (size_t at = 0; at < sizeof tat_hosted_load_outcomes / sizeof tat_hosted_load_outcomes[0]; at += 1u) {
        if (strcmp(tat_hosted_load_outcomes[at].name, wanted) == 0) {
            outcome = tat_hosted_load_outcomes[at].outcome;
        }
    }
    jv_free(text);
    if (outcome == TAH_LOAD_OUTCOMES) {
        *error_code = RPC_INVALID_PARAMS;
        return jv_string("hosted.load: params.outcome must be loaded, spawned, not-found, rejected, failed or timed-out");
    }
    uint32_t index = (uint32_t)tat_hosted_u64(params, "index");
    uint32_t generation = (uint32_t)tat_hosted_u64(params, "generation");
    tah_load_info load = tah_loads_info(tah_system_loads(system), index, generation);
    ta_result result = load.address == NULL ? TA_ERR_STALE : TA_OK;
    ta_actor_id actor = 0;
    if (result == TA_OK && (outcome == TAH_LOAD_LOADED || outcome == TAH_LOAD_SPAWNED)) {
        /* Borrowed from the load, which holds it until it finishes. */
        ta_actor_address address = ta_address_view(load.address);
        ta_definition_id definition = (ta_definition_id)tat_hosted_u64(params, "definition");
        jv journal = jv_object_get(jv_copy(params), jv_string("journal"));
        bool replaying = outcome == TAH_LOAD_LOADED && jv_get_kind(journal) == JV_KIND_ARRAY &&
                         jv_array_length(jv_copy(journal)) > 0;
        if (replaying) {
            tat_hosted_replay_params replay = {.definition = definition, .address = &address,
                                               .at = (ta_time)tat_hosted_u64(params, "at"), .journal = journal};
            result = tat_hosted_replay(bind, system, &replay);
            actor = replay.actor;
            if (result == TA_ERR_DONE) {
                outcome = TAH_LOAD_NOT_FOUND; /* the journal ends in a final state */
                result = TA_OK;
            }
        } else {
            /* No journal (or none asked for): a new actor at the address. */
            outcome = TAH_LOAD_SPAWNED;
            result = tat_hosted_spawn_at(system, definition, &address, &actor);
        }
        jv_free(journal);
        if (result != TA_OK) {
            outcome = TAH_LOAD_FAILED;
            actor = 0;
        }
    }
    ta_result finished = result == TA_ERR_STALE
                             ? result
                             : tah_loads_finish(tah_system_loads(system), &(tah_load_finish){
                                                                          .core = tah_system_core(system),
                                                                          .sched = tah_system_sched(system),
                                                                          .directory = tah_system_directory(system),
                                                                          .metrics = tah_system_metrics(system),
                                                                          .index = index,
                                                                          .generation = generation,
                                                                          .outcome = outcome,
                                                                          .actor = actor,
                                                                          .now_ns = tah_system_monotonic_ns(system),
                                                                          .wall_ns = tah_system_wall_ns(system)});
    tah_system_want_turn(system);
    if (result == TA_OK) {
        result = finished;
    }
    jv out = jv_object_set(jv_object(), jv_string("result"), tat_encode_ta_result(bind, &result));
    for (size_t at = 0; at < sizeof tat_hosted_load_outcomes / sizeof tat_hosted_load_outcomes[0]; at += 1u) {
        if (tat_hosted_load_outcomes[at].outcome == outcome) {
            out = jv_object_set(out, jv_string("outcome"), jv_string(tat_hosted_load_outcomes[at].name));
        }
    }
    if (actor != 0) {
        out = jv_object_set(out, jv_string("actor"), jv_number((double)actor));
    }
    return out;
}

static jv tat_hosted_memory(tah_memory_counters counters) {
    jv out = jv_object_set(jv_object(), jv_string("bytes_in_use"), jv_number((double)counters.bytes_in_use));
    out = jv_object_set(out, jv_string("peak_bytes"), jv_number((double)counters.peak_bytes));
    return jv_object_set(out, jv_string("largest_block"), jv_number((double)counters.largest_block));
}

/* The kernel's memory counters for what core allocates and for everything: {"memory": {"core", "total"}}. */
jv tat_hosted_stats_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    tah_allocator *allocator = tah_system_allocator(hosted->system);
    jv memory = jv_object_set(jv_object(), jv_string("core"),
                              tat_hosted_memory(tah_allocator_counters(allocator, TAH_MEMORY_CORE)));
    memory = jv_object_set(memory, jv_string("total"), tat_hosted_memory(tah_allocator_total(allocator)));
    return jv_object_set(jv_object(), jv_string("memory"), memory);
}
