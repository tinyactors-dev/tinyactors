#include "napi_system.h"

#include <stdlib.h>

#include "events.h"
#include "pump.h"
#include "router.h"

static void tna_unlist(tna_napi_system *wrapper) {
    if (!wrapper->listed) {
        return;
    }
    tna_env *data = wrapper->env_data;
    if (wrapper->previous != NULL) {
        wrapper->previous->next = wrapper->next;
    } else {
        data->open_systems = wrapper->next;
    }
    if (wrapper->next != NULL) {
        wrapper->next->previous = wrapper->previous;
    }
    wrapper->previous = NULL;
    wrapper->next = NULL;
    wrapper->listed = false;
}

const char tna_napi_system_tag = 0;

static void tna_free_wrapper(tna_napi_system *wrapper) {
    tna_system_free(wrapper->system);
    tna_env *data = wrapper->env_data;
    free(wrapper);
    tna_env_release(data);
}

static void tna_unlink_handle(tna_napi_handle *handle) {
    tna_napi_system *owner = handle->owner;
    if (handle->previous != NULL) {
        handle->previous->next = handle->next;
    } else {
        owner->handles = handle->next;
    }
    if (handle->next != NULL) {
        handle->next->previous = handle->previous;
    }
    handle->previous = NULL;
    handle->next = NULL;
}

void tna_napi_system_adopt(tna_napi_system *wrapper, tna_napi_handle *handle) {
    handle->owner = wrapper;
    handle->previous = NULL;
    handle->next = wrapper->handles;
    if (wrapper->handles != NULL) {
        wrapper->handles->previous = handle;
    }
    wrapper->handles = handle;
    wrapper->holders += 1u;
}

void tna_napi_system_unhold(tna_napi_system *wrapper) {
    wrapper->holders -= 1u;
    if (wrapper->finalized && wrapper->holders == 0) {
        tna_free_wrapper(wrapper);
    }
}

void tna_napi_system_drop(tna_napi_handle *handle) {
    tna_napi_system *owner = handle->owner;
    tna_unlink_handle(handle);
    free(handle);
    tna_napi_system_unhold(owner);
}

void tna_napi_system_flush(tna_napi_system *wrapper) {
    if (wrapper->system->inside_core) {
        return;
    }
    tna_napi_handle *handle = wrapper->handles;
    while (handle != NULL) {
        tna_napi_handle *next = handle->next;
        if (handle->finalized) {
            if (handle->pointer != NULL && !tna_system_closed(wrapper->system)) {
                handle->kind->release(handle->pointer);
            }
            tna_napi_system_drop(handle);
        }
        handle = next;
    }
}

/*
 * Close releases every handle core does not reclaim itself, before core goes
 * away (ADR 0012). Handle objects that are still reachable keep working as
 * "closed" objects; those already finalized are freed.
 */
static void tna_close_handles(tna_napi_system *wrapper) {
    tna_napi_handle *handle = wrapper->handles;
    while (handle != NULL) {
        tna_napi_handle *next = handle->next;
        if (handle->pointer != NULL && !handle->kind->reclaimed_by_core) {
            handle->kind->release(handle->pointer);
        }
        handle->pointer = NULL;
        if (handle->finalized) {
            tna_napi_system_drop(handle);
        }
        handle = next;
    }
}

/*
 * Closes the native system once and records what it leaked. `env_usable` is
 * false in the finalizer and the cleanup hook, where no JavaScript can run.
 */
static void tna_close(tna_napi_system *wrapper, bool env_usable) {
    if (tna_system_closed(wrapper->system)) {
        return;
    }
    tna_pump_close(wrapper, env_usable);
    tna_close_handles(wrapper);
    tna_system_close(wrapper->system);
    tna_unlist(wrapper);
    wrapper->env_data->systems_closed += 1u;
    wrapper->env_data->leaked_bytes += tna_system_leaked_bytes(wrapper->system);
}

void tna_napi_system_close_open(tna_env *data) {
    while (data->open_systems != NULL) {
        tna_close(data->open_systems, false);
    }
}

static void tna_finalize(napi_env env, void *finalize_data, void *hint) {
    (void)env;
    (void)hint;
    tna_napi_system *wrapper = finalize_data;
    tna_close(wrapper, false);
    wrapper->finalized = true;
    if (wrapper->holders == 0) {
        tna_free_wrapper(wrapper);
    }
}

tna_napi_system *tna_napi_system_unwrap(napi_env env, napi_value value) {
    void *wrapped = NULL;
    if (napi_unwrap(env, value, &wrapped) != napi_ok || wrapped == NULL ||
        *(const void *const *)wrapped != &tna_napi_system_tag) {
        return NULL;
    }
    return wrapped;
}

/* Unwraps `this`; throws and returns NULL when it is not a System. */
static tna_napi_system *tna_this(napi_env env, napi_callback_info info) {
    napi_value self = NULL;
    tna_napi_system *wrapped = NULL;
    if (napi_get_cb_info(env, info, NULL, NULL, &self, NULL) != napi_ok ||
        (wrapped = tna_napi_system_unwrap(env, self)) == NULL) {
        tna_throw(env, "invalid", "not a System");
        return NULL;
    }
    return wrapped;
}

/* Like tna_this, but also throws "closed" for a closed system. */
static tna_napi_system *tna_open_this(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_this(env, info);
    if (wrapper != NULL && tna_system_closed(wrapper->system)) {
        tna_throw(env, "closed", "the system is closed");
        return NULL;
    }
    return wrapper;
}

/* new System(facade?, options?): facade is the public System (lib/system.js). */
static napi_value tna_construct(napi_env env, napi_callback_info info) {
    napi_value self = NULL;
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) {
        return NULL;
    }
    napi_valuetype facade_type = napi_undefined;
    if (argv[0] != NULL) {
        napi_typeof(env, argv[0], &facade_type);
    }
    napi_value facade = facade_type == napi_object ? argv[0] : NULL;
    tna_env *data = tna_env_get(env);
    if (data == NULL || data->torn_down) {
        return tna_throw(env, "closed", "the environment is shutting down");
    }
    tna_napi_system *wrapper = calloc(1, sizeof *wrapper);
    if (wrapper == NULL) {
        return tna_throw(env, "out-of-memory", "cannot allocate a system");
    }
    wrapper->tag = &tna_napi_system_tag;
    ta_result result = tna_system_create(&wrapper->system);
    if (result != TA_OK) {
        free(wrapper);
        return tna_throw(env, tna_result_code(result), "cannot create a system");
    }
    wrapper->env_data = data;
    if (!tna_pump_init(env, wrapper, facade, argv[1])) {
        tna_pump_close(wrapper, true);
        tna_system_free(wrapper->system);
        free(wrapper);
        return NULL;
    }
    if (napi_wrap(env, self, wrapper, tna_finalize, NULL, NULL) != napi_ok) {
        tna_pump_close(wrapper, true);
        tna_system_free(wrapper->system);
        free(wrapper);
        return tna_throw(env, "invalid", "cannot wrap a system");
    }
    tna_env_retain(data);
    wrapper->next = data->open_systems;
    if (data->open_systems != NULL) {
        data->open_systems->previous = wrapper;
    }
    data->open_systems = wrapper;
    wrapper->listed = true;
    data->systems_created += 1u;
    return self;
}

static napi_value tna_close_method(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_close(wrapper, true);
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

static napi_value tna_closed_getter(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    napi_value closed = NULL;
    napi_get_boolean(env, tna_system_closed(wrapper->system), &closed);
    return closed;
}

static napi_value tna_flush_method(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    return wrapper == NULL ? NULL : tna_pump_flush(env, wrapper);
}

/* release(slot, generation): puts a quarantined actor back into scheduling (ADR 0008). */
static napi_value tna_release_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    napi_value self = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (wrapper == NULL) {
        return NULL;
    }
    if (napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "release: expected (slot, generation)");
    }
    ta_result result = tna_sched_release(&wrapper->system->sched, ta_actor_id_make(slot, generation));
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "release: the actor is not known");
    }
    tna_system_want_turn(wrapper->system);
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

/* scheduling(slot, generation): the actor's scheduling state, for tests and inspection. */
static napi_value tna_scheduling_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (wrapper == NULL) {
        return NULL;
    }
    if (napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "scheduling: expected (slot, generation)");
    }
    static const char *const names[] = { "none", "idle", "ready", "runnable", "quarantined" };
    const tna_sched_entry *entry = tna_sched_lookup(&wrapper->system->sched, ta_actor_id_make(slot, generation));
    napi_value state = NULL;
    napi_create_string_utf8(env, entry == NULL ? "none" : names[entry->state], NAPI_AUTO_LENGTH, &state);
    return state;
}

/*
 * post(slot, generation, name): queues an event without data for an actor
 * (ADR 0005). `name` is a ta_event_name_id from internEvent(). Mail for a
 * stale ID is accepted as a dead letter. Throws "queue-full" at
 * maxQueuedMessages. It never calls core, so it is allowed inside a core
 * call (a data-model callback may post; ADR 0013).
 */
static napi_value tna_post_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t name = 0;
    if (wrapper == NULL) {
        return NULL;
    }
    if (argc < 3 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &name) != napi_ok) {
        return tna_throw(env, "invalid", "post: expected (slot, generation, name)");
    }
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_result result = tna_system_post(wrapper->system, ta_actor_id_make(slot, generation), &event);
    if (result == TA_ERR_BUSY) {
        return tna_throw(env, "queue-full", "post: the system holds maxQueuedMessages messages");
    }
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "post: the message was not accepted");
    }
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

/* A string argument read into the call arena; false when it is not a string. */
static bool tna_text_argument(napi_env env, tna_system *system, napi_value value, ta_str *out) {
    napi_valuetype type = napi_undefined;
    size_t length = 0;
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, value, NULL, 0, &length) != napi_ok) {
        return false;
    }
    char *text = ta_arena_alloc(&system->scratch.call, length + 1u, 1);
    if (text == NULL) {
        return false;
    }
    napi_get_value_string_utf8(env, value, text, length + 1u, &length);
    *out = (ta_str){ .ptr = text, .len = (uint32_t)length };
    return true;
}

/*
 * postNamed(family, name, event): queues an event without data for the actor
 * the directory has at that address (ADR 0006), or a dead letter when it has
 * none. Interning the address is a core call, so this one is not allowed
 * inside a core call.
 */
static napi_value tna_post_named_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "post: a named target cannot be resolved from inside a core call (ADR 0013)");
    }
    ta_actor_address text = {0};
    uint32_t name = 0;
    bool ok = argc == 3 && tna_text_argument(env, system, argv[0], &text.family.name) &&
              tna_text_argument(env, system, argv[1], &text.name) &&
              napi_get_value_uint32(env, argv[2], &name) == napi_ok;
    ta_address *address = NULL;
    ta_result result = ok ? ta_address_intern(system->core, &text, &address) : TA_ERR_INVALID;
    tna_scratch_reset_call(&system->scratch);
    if (result == TA_ERR_INVALID) {
        return tna_throw(env, "invalid", "post: expected (family, name, event) with non-empty text");
    }
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "post: cannot intern the address");
    }
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    result = tna_router_post_named(system, address, &event);
    ta_address_release(address);
    if (result == TA_ERR_BUSY) {
        return tna_throw(env, "queue-full", "post: the system holds maxQueuedMessages messages");
    }
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "post: the message was not accepted");
    }
    tna_system_want_turn(system);
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

/*
 * internEvent(text): the ta_event_name_id of a dotted event name (ADR 0004).
 * lib/events.js calls it once per distinct name and caches the answer.
 */
static napi_value tna_intern_event_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "an event name cannot be interned from inside a core call (ADR 0013)");
    }
    napi_valuetype type = napi_undefined;
    size_t length = 0;
    if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, argv[0], NULL, 0, &length) != napi_ok) {
        return tna_throw(env, "invalid", "an event name must be a string");
    }
    char stack[256];
    char *text = stack;
    if (length + 1u > sizeof stack) {
        text = ta_arena_alloc(&system->scratch.call, length + 1u, 1);
        if (text == NULL) {
            return tna_throw(env, "out-of-memory", "cannot read the event name");
        }
    }
    napi_get_value_string_utf8(env, argv[0], text, length + 1u, &length);
    ta_event_name_id id = 0;
    system->inside_core = true;
    ta_result result = tna_event_name_intern(system->core, (ta_str){ .ptr = text, .len = (uint32_t)length }, &id);
    system->inside_core = false;
    tna_scratch_reset_call(&system->scratch);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result),
                         result == TA_ERR_INVALID
                             ? "invalid event name: it must be non-empty dotted text with no empty segment and at most 64 segments"
                             : "cannot intern the event name");
    }
    napi_value out = NULL;
    napi_create_uint32(env, id, &out);
    return out;
}

static napi_status tna_set_number(napi_env env, napi_value object, const char *name, double value) {
    napi_value number = NULL;
    napi_status status = napi_create_double(env, value, &number);
    return status == napi_ok ? napi_set_named_property(env, object, name, number) : status;
}

static napi_status tna_counters_object(napi_env env, tna_memory_counters counters, napi_value *out) {
    napi_status status = napi_create_object(env, out);
    if (status == napi_ok) status = tna_set_number(env, *out, "bytesInUse", (double)counters.bytes_in_use);
    if (status == napi_ok) status = tna_set_number(env, *out, "peakBytes", (double)counters.peak_bytes);
    if (status == napi_ok) status = tna_set_number(env, *out, "allocations", (double)counters.allocations);
    if (status == napi_ok) status = tna_set_number(env, *out, "frees", (double)counters.frees);
    return status;
}

static const char *const tna_category_names[TNA_MEMORY_CATEGORY_COUNT] = {
    [TNA_MEMORY_CORE] = "core",
    [TNA_MEMORY_ENVELOPES] = "envelopes",
    [TNA_MEMORY_SCHEDULING] = "scheduling",
    [TNA_MEMORY_VALUES] = "values",
    [TNA_MEMORY_DIRECTORY] = "directory",
    [TNA_MEMORY_SCRATCH] = "scratch",
    [TNA_MEMORY_OTHER] = "other",
};

/*
 * stats(): { memory: { total, core, envelopes, ... }, allocatorCalls,
 * messages: { queued, delivered, deadLetters } } (ADRs 0008, 0009).
 */
static napi_value tna_stats_method(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    const tna_allocator *allocator = &wrapper->system->allocator;
    napi_value stats = NULL;
    napi_value memory = NULL;
    napi_value counters = NULL;
    napi_status status = napi_create_object(env, &stats);
    if (status == napi_ok) status = napi_create_object(env, &memory);
    if (status == napi_ok) status = tna_counters_object(env, tna_allocator_total(allocator), &counters);
    if (status == napi_ok) status = napi_set_named_property(env, memory, "total", counters);
    for (int category = 0; status == napi_ok && category < TNA_MEMORY_CATEGORY_COUNT; category += 1) {
        status = tna_counters_object(env, tna_allocator_counters(allocator, (tna_memory_category)category), &counters);
        if (status == napi_ok) status = napi_set_named_property(env, memory, tna_category_names[category], counters);
    }
    if (status == napi_ok) status = napi_set_named_property(env, stats, "memory", memory);
    if (status == napi_ok) status = tna_set_number(env, stats, "allocatorCalls", (double)tna_allocator_calls(allocator));
    const tna_sched *sched = &wrapper->system->sched;
    napi_value messages = NULL;
    if (status == napi_ok) status = napi_create_object(env, &messages);
    if (status == napi_ok) status = tna_set_number(env, messages, "queued", (double)sched->queued);
    if (status == napi_ok) status = tna_set_number(env, messages, "delivered", (double)sched->delivered);
    if (status == napi_ok) status = tna_set_number(env, messages, "deadLetters", (double)sched->dead_letters);
    if (status == napi_ok) status = napi_set_named_property(env, stats, "messages", messages);
    if (status != napi_ok) {
        return tna_throw(env, "out-of-memory", "cannot build the stats object");
    }
    return stats;
}

napi_status tna_napi_system_define(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        { "close", NULL, tna_close_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "stats", NULL, tna_stats_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "flush", NULL, tna_flush_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "release", NULL, tna_release_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "scheduling", NULL, tna_scheduling_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "internEvent", NULL, tna_intern_event_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "post", NULL, tna_post_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "postNamed", NULL, tna_post_named_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "closed", NULL, NULL, tna_closed_getter, NULL, NULL, napi_default_jsproperty, NULL },
    };
    napi_value constructor = NULL;
    napi_status status = napi_define_class(env, "System", NAPI_AUTO_LENGTH, tna_construct, NULL,
                                           sizeof properties / sizeof properties[0], properties, &constructor);
    return status == napi_ok ? napi_set_named_property(env, exports, "System", constructor) : status;
}
