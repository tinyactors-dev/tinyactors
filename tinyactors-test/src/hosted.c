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
    /* Batches the kernel's input log handed over since the last
     * hosted.batches: [{"family", "name", "bytes": hex}], one per address
     * per turn, in turn order. */
    jv batches;
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
    ta_result result = tah_system_create(
        &(tah_system_create_params){.platform = platform, .incarnation = chosen}, &hosted->system);
    if (result != TA_OK) {
        free(hosted);
        return tat_encode_ta_result(bind, &result);
    }
    hosted->batches = jv_array();
    /* The input log writes values with the jq data model's codec, which
     * streams both ways (kernel ADR 0002; core ADR 0090's notes). Hosted
     * charts use the jq data model. */
    tah_inputs_set_datamodel(tah_system_inputs(hosted->system), tat_json_datamodel(&bind->json));
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
    jv_free(hosted->batches);
    tat_handle_retire(bind, hosted);
    free(hosted);
    return jv_object_set(jv_object(), jv_string("leaked"), jv_number((double)leaked));
}

/* Spawns an actor of `definition`, at `address` when it is not NULL,
 * through core, and hands it to the kernel's scheduler (and input log). */
static ta_result tat_hosted_spawn_at(tah_system *system, ta_definition_id definition, const ta_actor_address *address,
                                     ta_actor_id *actor) {
    tah_system_enter_core(system);
    ta_result result = ta_actor_spawn(tah_system_core(system), &(ta_actor_spawn_params){
                                                                   .definition = definition, .address = address, .out = actor});
    tah_system_leave_core(system);
    return result == TA_OK ? tah_hook_actor_spawned(system, *actor) : result;
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
        /* Hosted charts use the jq data model (hosted.log). */
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
            result = tah_router_post_named(hosted->system, address, &(tah_message){.event = &event});
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

/* The turn's input-log batch, as the application would store it. */
static void tat_hosted_take_batch(tat_hosted *hosted) {
    tah_inputs *inputs = tah_system_inputs(hosted->system);
    uint32_t wake_count = 0;
    (void)tah_inputs_wakes(hosted->system, &wake_count);
    static const char digits[] = "0123456789abcdef";
    for (uint32_t index = 0; index < tah_inputs_entry_count(inputs); index += 1u) {
        tah_input_entry entry = tah_inputs_entry_at(inputs, index);
        ta_actor_address address = ta_address_view(entry.address);
        size_t length = entry.length;
        char *hex = malloc(length * 2u + 1u);
        if (hex == NULL) {
            continue;
        }
        for (size_t at = 0; at < length; at += 1u) {
            uint8_t byte = entry.bytes[at];
            hex[at * 2u] = digits[byte >> 4];
            hex[at * 2u + 1u] = digits[byte & 15u];
        }
        hex[length * 2u] = 0;
        jv item = jv_object();
        item = jv_object_set(item, jv_string("family"), tat_hosted_text(address.family.name));
        item = jv_object_set(item, jv_string("name"), tat_hosted_text(address.name));
        item = jv_object_set(item, jv_string("bytes"), jv_string(hex));
        free(hex);
        hosted->batches = jv_array_append(hosted->batches, item);
    }
    tah_inputs_release(inputs);
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
        tat_hosted_take_batch(hosted);
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
 * delivery order, and the scheduler's counters: messages queued, delivered,
 * and the dead letters reported so far, by reason.
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

/* Logs the named actors of `families` (kernel ADR 0002). */
jv tat_hosted_log_method(tat_bind *bind, jv params, int *error_code) {
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
    ta_result result = tah_inputs_configure(hosted->system, families, (uint32_t)count);
    for (int index = 0; index < count; index += 1) {
        jv_free(held[index]);
    }
    jv_free(list);
    return tat_encode_ta_result(bind, &result);
}

/* The batches since the last call, and forgets them. */
jv tat_hosted_batches_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv out = hosted->batches;
    hosted->batches = jv_array();
    return out;
}

/* Evicts an idle named actor: its log becomes its only state. */
jv tat_hosted_evict_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    ta_actor_id actor = (ta_actor_id)tat_hosted_u64(params, "actor");
    tah_system *system = hosted->system;
    ta_result result = tah_inputs_evicted(system, actor, false, 0);
    if (result == TA_OK) {
        tah_system_enter_core(system);
        result = ta_actor_destroy(tah_system_core(system), actor);
        tah_system_leave_core(system);
        if (result == TA_OK) {
            (void)tah_hook_actor_removed(system, actor);
        }
    }
    tat_hosted_take_batch(hosted);
    return tat_encode_ta_result(bind, &result);
}

static uint8_t tat_hosted_nibble(char c) {
    return (uint8_t)(c <= '9' ? c - '0' : c - 'a' + 10);
}

/* The bytes of lowercase hex text, malloc'd; NULL when out of memory. */
static uint8_t *tat_hosted_unhex(jv text, size_t *length) {
    const char *hex = jv_get_kind(text) == JV_KIND_STRING ? jv_string_value(text) : "";
    *length = jv_get_kind(text) == JV_KIND_STRING ? (size_t)jv_string_length_bytes(jv_copy(text)) / 2u : 0;
    uint8_t *bytes = malloc(*length == 0 ? 1 : *length);
    for (size_t at = 0; bytes != NULL && at < *length; at += 1u) {
        bytes[at] = (uint8_t)(tat_hosted_nibble(hex[at * 2u]) << 4 | tat_hosted_nibble(hex[at * 2u + 1u]));
    }
    return bytes;
}

typedef struct {
    ta_definition_id definition;
    const ta_actor_address *address;
    const uint8_t *bytes;
    size_t length;
    ta_actor_id actor; /* out */
    bool found;        /* out: the log held a life to rebuild */
} tat_hosted_replay_params;

/*
 * Rebuilds a named actor from its log (one address's batches back to back)
 * with the kernel's replay: prepare, run, publish at the last record's time,
 * then redeliver the messages still owed. `found` is false for a log with no
 * life in it, and nothing is rebuilt.
 */
static ta_result tat_hosted_replay(tah_system *system, tat_hosted_replay_params *params) {
    params->found = false;
    tah_replay *replay = tah_replay_open(system);
    ta_result result = replay == NULL ? TA_ERR_NOMEM : tah_replay_push(replay, params->bytes, params->length);
    if (result == TA_OK) {
        result = tah_replay_prepare(replay, &(tah_replay_prepare_params){.definition = params->definition,
                                                                         .address = params->address,
                                                                         .out = &params->actor,
                                                                         .found = &params->found});
    }
    tah_replay_state state = TAH_REPLAY_MORE;
    while (result == TA_OK && params->found && state == TAH_REPLAY_MORE) {
        uint32_t records = 0;
        result = tah_replay_run(replay, &(tah_replay_run_params){
                                            .actor = params->actor, .most = 256, .state = &state, .records = &records});
    }
    if (result == TA_OK && params->found) {
        ta_time at = 0;
        (void)tah_replay_last_time(replay, &at);
        tah_replay_finish(replay, params->actor);
        ta_actor_publish_params publish = {.actor = params->actor, .at = &at};
        tah_system_enter_core(system);
        result = ta_actor_publish(tah_system_core(system), &publish);
        tah_system_leave_core(system);
        if (result == TA_OK) {
            result = tah_hook_actor_published(system, &publish);
        }
        uint32_t owed = 0;
        if (result == TA_OK) {
            result = tah_replay_redeliver(replay, params->actor, &owed);
        }
    }
    if (replay != NULL) {
        tah_replay_free(replay);
    }
    return result;
}

/* Rebuilds a named actor from its log (hex) with the kernel's replay; answers the actor. */
jv tat_hosted_restore_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv log = jv_object_get(jv_copy(params), jv_string("log"));
    jv family = jv_object_get(jv_copy(params), jv_string("family"));
    jv name = jv_object_get(jv_copy(params), jv_string("name"));
    size_t length = 0;
    uint8_t *bytes = tat_hosted_unhex(log, &length);
    ta_actor_address address = {
        .family = {.name = {jv_string_value(family), (uint32_t)jv_string_length_bytes(jv_copy(family))}},
        .name = {jv_string_value(name), (uint32_t)jv_string_length_bytes(jv_copy(name))}};
    tat_hosted_replay_params replay = {.definition = (ta_definition_id)tat_hosted_u64(params, "definition"),
                                       .address = &address, .bytes = bytes, .length = length};
    ta_result result = bytes == NULL ? TA_ERR_NOMEM : tat_hosted_replay(hosted->system, &replay);
    free(bytes);
    jv_free(log);
    jv_free(family);
    jv_free(name);
    if (result != TA_OK || !replay.found) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.restore: the log did not rebuild an actor");
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
        jv log = jv_object_get(jv_copy(params), jv_string("log"));
        size_t length = 0;
        uint8_t *bytes = outcome == TAH_LOAD_LOADED ? tat_hosted_unhex(log, &length) : NULL;
        jv_free(log);
        tat_hosted_replay_params replay = {.definition = definition, .address = &address, .bytes = bytes, .length = length};
        if (outcome == TAH_LOAD_LOADED && bytes == NULL) {
            result = TA_ERR_NOMEM;
        } else if (length != 0) {
            result = tat_hosted_replay(system, &replay);
            actor = replay.actor;
        }
        free(bytes);
        if (result == TA_OK && !replay.found) {
            /* No life in the log (or none asked for): a new actor at the address. */
            outcome = TAH_LOAD_SPAWNED;
            result = tat_hosted_spawn_at(system, definition, &address, &actor);
        }
        if (result != TA_OK) {
            outcome = TAH_LOAD_FAILED;
            actor = 0;
        }
    }
    ta_result finished = result == TA_ERR_STALE
                             ? result
                             : tah_loads_finish(tah_system_loads(system), &(tah_load_finish){
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

/* A byte sink that keeps what it is given: a checkpoint, for hosted.checkpoint. */
typedef struct {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    size_t unit_start;
    bool failed;
} tat_hosted_bytes;

static ta_op_status tat_hosted_bytes_begin(void *self) {
    tat_hosted_bytes *sink = self;
    sink->unit_start = sink->length;
    return TA_OP_OK;
}

static ta_op_status tat_hosted_bytes_write(void *self, ta_bytes bytes) {
    tat_hosted_bytes *sink = self;
    if (sink->length + bytes.len > sink->capacity) {
        size_t capacity = (sink->length + bytes.len) * 2u + 64u;
        uint8_t *grown = realloc(sink->bytes, capacity);
        if (grown == NULL) {
            sink->failed = true;
            return TA_OP_NOMEM;
        }
        sink->bytes = grown;
        sink->capacity = capacity;
    }
    for (uint32_t at = 0; at < bytes.len; at += 1u) {
        sink->bytes[sink->length + at] = bytes.ptr[at];
    }
    sink->length += bytes.len;
    return TA_OP_OK;
}

static void tat_hosted_bytes_end(void *self, bool keep) {
    tat_hosted_bytes *sink = self;
    if (!keep) {
        sink->length = sink->unit_start;
    }
}

static const ta_byte_sink_vtable tat_hosted_bytes_vtable = {
    .begin = tat_hosted_bytes_begin, .write = tat_hosted_bytes_write, .end = tat_hosted_bytes_end};

/* A checkpoint of an idle logged actor (kernel ADR 0002 section 7), in hex. */
jv tat_hosted_checkpoint_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    tat_hosted_bytes sink = {0};
    ta_result result = tah_checkpoint_write(hosted->system, (ta_actor_id)tat_hosted_u64(params, "actor"),
                                            (ta_byte_sink){.self = &sink, .v = &tat_hosted_bytes_vtable});
    jv out = jv_object_set(jv_object(), jv_string("result"), tat_encode_ta_result(bind, &result));
    if (result == TA_OK) {
        static const char digits[] = "0123456789abcdef";
        char *hex = malloc(sink.length * 2u + 1u);
        if (hex != NULL) {
            for (size_t at = 0; at < sink.length; at += 1u) {
                hex[at * 2u] = digits[sink.bytes[at] >> 4];
                hex[at * 2u + 1u] = digits[sink.bytes[at] & 15u];
            }
            hex[sink.length * 2u] = 0;
            out = jv_object_set(out, jv_string("bytes"), jv_string(hex));
            free(hex);
        }
    }
    free(sink.bytes);
    return out;
}
