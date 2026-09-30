#include "hosted.h"

#include <stdlib.h>

#include <tinyactors_host.h>

#include "allocator.h"
#include "rpc.h"
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
    if (result == TA_OK) {
        result = tah_system_post(hosted->system,
                                 &(tah_message){.target = (ta_actor_id)tat_hosted_u64(params, "actor"), .event = &event});
    }
    return tat_encode_ta_result(bind, &result);
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
