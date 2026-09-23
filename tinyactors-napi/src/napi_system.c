#include "napi_system.h"

#include <stdlib.h>

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

void tna_napi_system_drop(tna_napi_handle *handle) {
    tna_napi_system *owner = handle->owner;
    tna_unlink_handle(handle);
    free(handle);
    owner->holders -= 1u;
    if (owner->finalized && owner->holders == 0) {
        tna_free_wrapper(owner);
    }
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

/* Closes the native system once and records what it leaked. */
static void tna_close(tna_napi_system *wrapper) {
    if (tna_system_closed(wrapper->system)) {
        return;
    }
    tna_close_handles(wrapper);
    tna_system_close(wrapper->system);
    tna_unlist(wrapper);
    wrapper->env_data->systems_closed += 1u;
    wrapper->env_data->leaked_bytes += tna_system_leaked_bytes(wrapper->system);
}

void tna_napi_system_close_open(tna_env *data) {
    while (data->open_systems != NULL) {
        tna_close(data->open_systems);
    }
}

static void tna_finalize(napi_env env, void *finalize_data, void *hint) {
    (void)env;
    (void)hint;
    tna_napi_system *wrapper = finalize_data;
    tna_close(wrapper);
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

static napi_value tna_construct(napi_env env, napi_callback_info info) {
    napi_value self = NULL;
    if (napi_get_cb_info(env, info, NULL, NULL, &self, NULL) != napi_ok) {
        return NULL;
    }
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
    if (napi_wrap(env, self, wrapper, tna_finalize, NULL, NULL) != napi_ok) {
        tna_system_free(wrapper->system);
        free(wrapper);
        return tna_throw(env, "invalid", "cannot wrap a system");
    }
    wrapper->env_data = data;
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
    tna_close(wrapper);
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

/* stats(): { memory: { total, core, envelopes, ... }, allocatorCalls } (ADR 0009). */
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
    if (status != napi_ok) {
        return tna_throw(env, "out-of-memory", "cannot build the stats object");
    }
    return stats;
}

napi_status tna_napi_system_define(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        { "close", NULL, tna_close_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "stats", NULL, tna_stats_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "closed", NULL, NULL, tna_closed_getter, NULL, NULL, napi_default_jsproperty, NULL },
    };
    napi_value constructor = NULL;
    napi_status status = napi_define_class(env, "System", NAPI_AUTO_LENGTH, tna_construct, NULL,
                                           sizeof properties / sizeof properties[0], properties, &constructor);
    return status == napi_ok ? napi_set_named_property(env, exports, "System", constructor) : status;
}
