#include "hosted.h"

#include <stdlib.h>

#include <tinyactors_host.h>

#include "allocator.h"
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
static void tat_hosted_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
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
    tat_hosted *hosted = calloc(1, sizeof *hosted);
    if (hosted == NULL) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.create: out of memory");
    }
    tah_platform platform = {
        .self = &hosted->clock, .v = &tat_hosted_platform_vtable, .allocator = tat_libc_allocator()};
    ta_result result = tah_system_create(
        &(tah_system_create_params){.platform = platform, .incarnation = chosen}, &hosted->system);
    if (result != TA_OK) {
        free(hosted);
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.create: the kernel could not create a system");
    }
    hosted->batches = jv_array();
    /* The input log writes values with the jq data model's codec, which
     * streams both ways (kernel ADR 0002; core ADR 0090's notes). Hosted
     * charts use the jq data model. */
    tah_inputs_set_datamodel(&hosted->system->inputs, tat_json_datamodel(&bind->json));
    hosted->system->wake = tat_hosted_wake;
    (void)tah_system_set_clock(hosted->system, TAH_CLOCK_MANUAL, 0);
    jv out = jv_object();
    out = jv_object_set(out, jv_string("hosted"), tat_handle_register(bind, "hosted", hosted));
    /* The kernel's core system, for the generated core methods (defining
     * charts, inspecting actors); retired when the hosted system closes. */
    out = jv_object_set(out, jv_string("core"), tat_handle_register(bind, "system", hosted->system->core));
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
    tat_handle_retire(bind, hosted->system->core);
    tah_system_close(hosted->system);
    size_t leaked = tah_system_leaked_bytes(hosted->system);
    tah_system_free(hosted->system);
    jv_free(hosted->batches);
    tat_handle_retire(bind, hosted);
    free(hosted);
    return jv_object_set(jv_object(), jv_string("leaked"), jv_number((double)leaked));
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
    tah_system *system = hosted->system;
    system->inside_core = true;
    ta_result result = ta_actor_spawn(system->core, &(ta_actor_spawn_params){
                                                        .definition = (ta_definition_id)tat_hosted_u64(params, "definition"),
                                                        .address = named ? &address : NULL,
                                                        .out = &actor});
    system->inside_core = false;
    jv_free(family);
    jv_free(name);
    if (result == TA_OK) {
        result = tah_hook_actor_spawned(system, actor);
    }
    if (result != TA_OK) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.spawn: the actor could not be spawned");
    }
    return jv_number((double)actor);
}

/* Posts an event without data into an actor's mailbox. */
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
        hosted->system->core, (ta_str){jv_string_value(text), (uint32_t)jv_string_length_bytes(jv_copy(text))},
        &name);
    jv_free(text);
    ta_event event = {.name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE};
    jv data = jv_object_get(jv_copy(params), jv_string("data"));
    if (result == TA_OK && jv_is_valid(data) && jv_get_kind(data) != JV_KIND_NULL) {
        /* Hosted charts use the jq data model (hosted.log). */
        result = tat_value_decode(tat_json_datamodel(&bind->json), jv_copy(data), &event.data);
    }
    jv_free(data);
    if (result == TA_OK) {
        result = tah_system_post(hosted->system,
                                 &(tah_message){.target = (ta_actor_id)tat_hosted_u64(params, "actor"), .event = &event});
        if (result != TA_OK) {
            ta_value_release(&event.data);
        }
    }
    return tat_encode_ta_result(bind, &result);
}

static jv tat_hosted_text(ta_str text) {
    return jv_string_sized(text.ptr, (int)text.len);
}

/* The turn's input-log batch, as the application would store it. */
static void tat_hosted_take_batch(tat_hosted *hosted) {
    tah_inputs *inputs = &hosted->system->inputs;
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

/* Runs turns until the system is quiet, at most `limit` (default 1). */
jv tat_hosted_turn_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    uint64_t limit = tat_hosted_u64(params, "limit");
    uint64_t turns = 0;
    for (; turns < (limit == 0 ? 1 : limit); turns += 1) {
        tah_system_turn(hosted->system);
        tat_hosted_take_batch(hosted);
        tah_system_end_turn(hosted->system);
        if (tah_system_quiet(hosted->system)) {
            turns += 1;
            break;
        }
    }
    jv out = jv_object_set(jv_object(), jv_string("turns"), jv_number((double)turns));
    return jv_object_set(out, jv_string("quiet"), jv_bool(tah_system_quiet(hosted->system)));
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
        system->inside_core = true;
        result = ta_actor_destroy(system->core, actor);
        system->inside_core = false;
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

/*
 * Rebuilds a named actor from its log (hex bytes, one address's batches back
 * to back) with the kernel's replay: prepare, run, publish at the last
 * record's time, then redeliver the messages still owed. Answers the actor.
 */
jv tat_hosted_restore_method(tat_bind *bind, jv params, int *error_code) {
    tat_hosted *hosted = NULL;
    if (!tat_hosted_resolve(bind, params, &hosted)) {
        return tat_bind_take_error(bind, error_code);
    }
    jv log = jv_object_get(jv_copy(params), jv_string("log"));
    jv family = jv_object_get(jv_copy(params), jv_string("family"));
    jv name = jv_object_get(jv_copy(params), jv_string("name"));
    const char *hex = jv_string_value(log);
    size_t length = (size_t)jv_string_length_bytes(jv_copy(log)) / 2u;
    uint8_t *bytes = malloc(length == 0 ? 1 : length);
    for (size_t at = 0; bytes != NULL && at < length; at += 1u) {
        bytes[at] = (uint8_t)(tat_hosted_nibble(hex[at * 2u]) << 4 | tat_hosted_nibble(hex[at * 2u + 1u]));
    }
    ta_actor_address address = {
        .family = {.name = {jv_string_value(family), (uint32_t)jv_string_length_bytes(jv_copy(family))}},
        .name = {jv_string_value(name), (uint32_t)jv_string_length_bytes(jv_copy(name))}};
    tah_replay *replay = bytes == NULL ? NULL : tah_replay_open(hosted->system);
    ta_result result = replay == NULL ? TA_ERR_NOMEM : tah_replay_push(replay, bytes, length);
    ta_actor_id actor = 0;
    bool found = false;
    if (result == TA_OK) {
        result = tah_replay_prepare(replay, &(tah_replay_prepare_params){
                                                .definition = (ta_definition_id)tat_hosted_u64(params, "definition"),
                                                .address = &address, .out = &actor, .found = &found});
    }
    tah_replay_state state = TAH_REPLAY_MORE;
    while (result == TA_OK && found && state == TAH_REPLAY_MORE) {
        uint32_t records = 0;
        result = tah_replay_run(replay, &(tah_replay_run_params){
                                            .actor = actor, .most = 256, .state = &state, .records = &records});
    }
    if (result == TA_OK && found) {
        ta_time at = 0;
        (void)tah_replay_last_time(replay, &at);
        tah_replay_finish(replay, actor);
        hosted->system->inside_core = true;
        result = ta_actor_publish(hosted->system->core, &(ta_actor_publish_params){.actor = actor, .at = &at});
        hosted->system->inside_core = false;
        uint32_t owed = 0;
        if (result == TA_OK) {
            result = tah_replay_redeliver(replay, actor, &owed);
        }
    }
    if (replay != NULL) {
        tah_replay_free(replay);
    }
    free(bytes);
    jv_free(log);
    jv_free(family);
    jv_free(name);
    if (result != TA_OK || !found) {
        *error_code = RPC_INTERNAL_ERROR;
        return jv_string("hosted.restore: the log did not rebuild an actor");
    }
    return jv_number((double)actor);
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
