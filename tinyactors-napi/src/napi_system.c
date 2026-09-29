#include "napi_system.h"

#include "napi_children.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enumerate.h"
#include "events.h"
#include "gen/ta_napi.h"
#include "import.h"
#include "napi_codecs.h"
#include "napi_inputs.h"
#include "napi_services.h"
#include "napi_loads.h"
#include "napi_trace.h"
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
    /* Closing from a finalizer: exit handlers core runs cannot call JavaScript. */
    wrapper->dm.js_usable = env_usable;
    tna_system_close(wrapper->system);
    tna_children_close(wrapper);
    wrapper->system->unreachable_data = NULL;
    wrapper->system->quarantine_data = NULL;
    wrapper->system->fault_data = NULL;
    tna_js_datamodel_close(&wrapper->dm, !wrapper->env_data->torn_down);
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

/*
 * Core tells of each definition it registers, retires and frees (core ADR
 * 0079), during a turn, a cascade or its teardown. Every event is counted
 * for the metrics and written as a churn log when a trace sink wants logs;
 * on FREED, what the addon keeps per definition goes then, not at a later
 * sweep.
 */
static void tna_definition_notify(void *self, const ta_definition_event *event) {
    tna_napi_system *wrapper = self;
    tna_definition_origin origin = tna_definitions_note(&wrapper->system->definitions, event);
    if (wrapper->system->tracing) {
        tna_otlp_definition_log(&wrapper->system->otlp, &(tna_otlp_definition_log_params){
                                                            .event = event,
                                                            .origin = { .ptr = tna_definition_origin_name(origin),
                                                                        .len = (uint32_t)strlen(tna_definition_origin_name(origin)) },
                                                        });
    }
    if (event->kind != TA_DEFINITION_EVENT_FREED) {
        return;
    }
    tna_children_forget(wrapper, event->definition);
    tna_trace_watch_forget_definition(&wrapper->system->trace_watch, event->definition);
    if (wrapper->system->tracing) {
        tna_otlp_forget_definition(&wrapper->system->otlp, event->definition);
    }
}

static const ta_definition_sink_vtable tna_definition_sink_vtable = { .notify = tna_definition_notify };

/* Core is gone: every value it held has been released (ADR 0011 point 7). */
static void tna_after_core(void *context) {
    tna_napi_system *wrapper = context;
    tna_js_datamodel_after_core(&wrapper->dm);
    tna_values_close(&wrapper->values);
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
tna_napi_system *tna_open_this(napi_env env, napi_callback_info info) {
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
    /* lib/system.js always passes the incarnation, as a bigint (napi ADR 0032). */
    uint64_t incarnation = 0;
    napi_value given = NULL;
    napi_valuetype options_type = napi_undefined;
    bool lossless = true;
    if (argv[1] != NULL) {
        napi_typeof(env, argv[1], &options_type);
    }
    if (options_type == napi_object && napi_get_named_property(env, argv[1], "incarnation", &given) == napi_ok) {
        napi_valuetype given_type = napi_undefined;
        napi_typeof(env, given, &given_type);
        if (given_type != napi_undefined &&
            (given_type != napi_bigint || napi_get_value_bigint_uint64(env, given, &incarnation, &lossless) != napi_ok ||
             !lossless)) {
            free(wrapper);
            return tna_throw(env, "invalid", "createSystem(): incarnation must fit in 64 bits");
        }
    }
    ta_result result = tna_system_create(incarnation, &wrapper->system);
    if (result != TA_OK) {
        free(wrapper);
        return tna_throw(env, tna_result_code(result), "cannot create a system");
    }
    wrapper->env_data = data;
    bool values_made = tna_values_init(&wrapper->values, env, &wrapper->system->allocator);
    wrapper->values.torn_down = &data->torn_down;
    wrapper->values.env_live = &data->values_live;
    wrapper->system->after_core = tna_after_core;
    wrapper->system->after_core_context = wrapper;
    if (!values_made || !tna_js_datamodel_init(&wrapper->dm, wrapper, env)) {
        tna_js_datamodel_close(&wrapper->dm, true);
        tna_system_free(wrapper->system);
        free(wrapper);
        return tna_throw(env, "out-of-memory", "cannot create the data model");
    }
    if (!tna_pump_init(env, wrapper, facade, argv[1])) {
        tna_pump_close(wrapper, true);
        tna_system_free(wrapper->system);
        tna_js_datamodel_close(&wrapper->dm, true);
        free(wrapper);
        return NULL;
    }
    tna_services_init(wrapper);
    (void)ta_system_set_definition_sink(wrapper->system->core,
                                        (ta_definition_sink){ .self = wrapper, .v = &tna_definition_sink_vtable });
    if (napi_wrap(env, self, wrapper, tna_finalize, NULL, NULL) != napi_ok) {
        tna_pump_close(wrapper, true);
        tna_system_free(wrapper->system);
        tna_js_datamodel_close(&wrapper->dm, true);
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

/* advanceTo(time): manual clock only (ADR 0007 point 3). */
static napi_value tna_advance_to_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    napi_value self = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    if (argc < 1) {
        return tna_throw(env, "invalid", "advanceTo(): expected a time");
    }
    return tna_pump_advance_to(env, wrapper, argv[0]);
}

/*
 * refuseAllocationsFrom(bytes): tests only. From now on the system's
 * allocator refuses every allocation of at least `bytes`, as a full pool
 * would; 0 refuses none again. It is not part of the public API: lib/
 * never calls it, and tests reach it through nativeOf(system).
 */
static napi_value tna_refuse_allocations_from_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    double bytes = -1;
    if (argc < 1 || napi_get_value_double(env, argv[0], &bytes) != napi_ok || !(bytes >= 0) ||
        bytes > 9007199254740991.0) {
        return tna_throw(env, "invalid", "refuseAllocationsFrom(): expected a byte count");
    }
    tna_allocator_refuse_from(&wrapper->system->allocator, (size_t)bytes);
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

/* clock: "realtime" or "manual". */
static napi_value tna_clock_getter(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    napi_value clock = NULL;
    const char *text = wrapper->system->clock.kind == TNA_CLOCK_MANUAL ? "manual" : "realtime";
    napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &clock);
    return clock;
}

/* time: core's logical milliseconds; the realtime clock is read first. */
static napi_value tna_time_getter(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system_sync_time(wrapper->system);
    napi_value time = NULL;
    napi_create_double(env, (double)ta_system_time(wrapper->system->core), &time);
    return time;
}

/* release(slot, generation): puts a quarantined actor back into scheduling (ADR 0008). */
/* quarantine(slot, generation): true when the actor was not quarantined before (napi ADR 0032 section 6). */
static napi_value tna_quarantine_method(napi_env env, napi_callback_info info) {
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
        return tna_throw(env, "invalid", "quarantine: expected (slot, generation)");
    }
    napi_value out = NULL;
    napi_get_boolean(env, tna_sched_quarantine(&wrapper->system->sched, ta_actor_id_make(slot, generation)), &out);
    return out;
}

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

/* (slot, generation) from the first two arguments; throws and returns false otherwise. */
static bool tna_actor_arguments(napi_env env, const napi_value *argv, size_t argc, const char *what, ta_actor_id *out) {
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 2 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        char message[96];
        snprintf(message, sizeof message, "%s: expected (slot, generation)", what);
        tna_throw(env, "invalid", message);
        return false;
    }
    *out = ta_actor_id_make(slot, generation);
    return true;
}

static napi_status tna_put_value(napi_env env, napi_value object, const char *name, napi_value value) {
    return value == NULL ? napi_ok : napi_set_named_property(env, object, name, value);
}

/* { source, targets }: an operator transition waiting in a mailbox, by state ID. */
static napi_value tna_jump_object(napi_env env, const tna_jump *jump) {
    napi_value out = NULL;
    napi_value source = NULL;
    napi_value targets = NULL;
    if (napi_create_object(env, &out) != napi_ok || napi_create_uint32(env, jump->source, &source) != napi_ok ||
        napi_create_array_with_length(env, jump->count, &targets) != napi_ok) {
        return NULL;
    }
    for (uint32_t at = 0; at < jump->count; at += 1u) {
        napi_value target = NULL;
        if (napi_create_uint32(env, jump->targets[at], &target) != napi_ok ||
            napi_set_element(env, targets, at, target) != napi_ok) {
            return NULL;
        }
    }
    return tna_put_value(env, out, "source", source) == napi_ok && tna_put_value(env, out, "targets", targets) == napi_ok
               ? out
               : NULL;
}

/*
 * mailbox(slot, generation, limit): the first `limit` messages waiting for
 * the actor, oldest first, as [{ event, data?, awaited, scope?, transition? }]:
 * `awaited` when a send() waits for it, `scope` ({ index, generation }) when
 * its origin is an openScope() scope, `transition` ({ source, targets } by
 * state ID) when it carries an operator transition (ADR 0026). O(limit).
 */
static napi_value tna_mailbox_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    ta_actor_id actor = 0;
    uint32_t limit = 0;
    if (wrapper == NULL || !tna_actor_arguments(env, argv, argc, "mailbox", &actor)) {
        return NULL;
    }
    if (argc < 3 || napi_get_value_uint32(env, argv[2], &limit) != napi_ok) {
        return tna_throw(env, "invalid", "mailbox: expected (slot, generation, limit)");
    }
    tna_system *system = wrapper->system;
    const tna_sched_entry *entry = tna_sched_lookup(&system->sched, actor);
    napi_value out = NULL;
    if (napi_create_array(env, &out) != napi_ok) {
        return tna_throw(env, "out-of-memory", "mailbox: cannot build the list");
    }
    uint32_t count = 0;
    for (uint32_t index = entry == NULL ? 0 : entry->first; index != 0 && count < limit;
         index = tna_sched_envelope(&system->sched, index)->next) {
        const tna_envelope *envelope = tna_sched_envelope(&system->sched, index);
        napi_value item = NULL;
        napi_value awaited = NULL;
        napi_value data = NULL;
        napi_status status = napi_create_object(env, &item);
        if (status == napi_ok) status = tna_put_value(env, item, "event", tna_event_text(env, system, envelope->event.name));
        if (status == napi_ok && !tna_value_absent(&envelope->event.data) && tna_value_get(env, &envelope->event.data, &data)) {
            status = tna_put_value(env, item, "data", data);
        }
        if (status == napi_ok) status = napi_get_boolean(env, envelope->waiter != 0, &awaited);
        if (status == napi_ok) status = tna_put_value(env, item, "awaited", awaited);
        const ta_origin *origin = &envelope->event.origin;
        if (status == napi_ok && origin->kind == TA_ORIGIN_IO && origin->as.io.origin_class == TA_IO_ORIGIN_SCOPED) {
            napi_value scope = NULL;
            napi_value number = NULL;
            status = napi_create_object(env, &scope);
            if (status == napi_ok) status = napi_create_uint32(env, origin->as.io.as.scope.index, &number);
            if (status == napi_ok) status = tna_put_value(env, scope, "index", number);
            if (status == napi_ok) status = napi_create_uint32(env, origin->as.io.as.scope.generation, &number);
            if (status == napi_ok) status = tna_put_value(env, scope, "generation", number);
            if (status == napi_ok) status = tna_put_value(env, item, "scope", scope);
        }
        if (status == napi_ok && envelope->has_jump) {
            napi_value jump = tna_jump_object(env, envelope->jump);
            status = jump == NULL ? napi_generic_failure : tna_put_value(env, item, "transition", jump);
        }
        if (status == napi_ok) status = napi_set_element(env, out, count, item);
        if (status != napi_ok) {
            return tna_throw(env, "out-of-memory", "mailbox: cannot build the list");
        }
        count += 1u;
    }
    return out;
}

/*
 * selectDatamodel(name): the data model the next compile uses, "null" (core's
 * null data model, the default), "javascript", or a registered one (napi ADR
 * 0027). System.define() calls it just before compiling; the compile resets
 * it to the default. Any other name throws `invalid`.
 */
static napi_value tna_select_datamodel_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    /* One byte more than any data model's name, so a longer one is never
     * cut down to match. */
    char name[TNA_DM_NAME_MAX + 1u];
    size_t length = 0;
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], name, sizeof name, &length) != napi_ok ||
        length >= TNA_DM_NAME_MAX || tna_js_datamodel_select(&wrapper->dm, (ta_str){ name, (uint32_t)length }) != TA_OK) {
        return tna_throw(env, "invalid", "selectDatamodel: expected \"javascript\", \"null\" or a registered data model");
    }
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

/*
 * registerDatamodel(name, compile): a data model named `name` whose source
 * expressions `compile(source, role)` turns into functions and locations
 * (ADR 0019). lib/ checks the arguments and normalizes compile's results.
 */
static napi_value tna_register_datamodel_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    if (wrapper->system->inside_core) {
        return tna_throw(env, "reentrant", "registerDatamodel: not from inside a core call (ADR 0013)");
    }
    char name[TNA_DM_NAME_MAX];
    size_t length = 0;
    napi_valuetype type = napi_undefined;
    if (argc < 2 || napi_get_value_string_utf8(env, argv[0], name, sizeof name, &length) != napi_ok ||
        napi_typeof(env, argv[1], &type) != napi_ok || type != napi_function) {
        return tna_throw(env, "invalid", "registerDatamodel: expected a name and a compile function");
    }
    if (tna_js_datamodel_register(&wrapper->dm, (ta_str){ name, (uint32_t)length }, argv[1]) != TA_OK) {
        return tna_throw(env, "invalid",
                         "registerDatamodel: the name is taken, reserved (javascript, null), too long, "
                         "or the system has no room for another data model");
    }
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

/*
 * registerFunction(name, run) and registerClass(name, constructor): what
 * source text `name()` calls (napi ADR 0029) and `new Name(...)` builds
 * (napi ADR 0031), in one name space. The method's data says which. lib/
 * checks that `name` is an identifier other than In.
 */
static napi_value tna_register_name_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    void *data = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, &data) != napi_ok) {
        return NULL;
    }
    bool constructor = data != NULL;
    const char *method = constructor ? "registerClass" : "registerFunction";
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    char message[128];
    if (wrapper->system->inside_core) {
        snprintf(message, sizeof message, "%s: not from inside a core call (ADR 0013)", method);
        return tna_throw(env, "reentrant", message);
    }
    char name[256];
    size_t length = 0;
    napi_valuetype type = napi_undefined;
    if (argc < 2 || napi_get_value_string_utf8(env, argv[0], name, sizeof name, &length) != napi_ok ||
        length == 0 || length >= sizeof name - 1u || napi_typeof(env, argv[1], &type) != napi_ok || type != napi_function) {
        snprintf(message, sizeof message, "%s: expected a name of at most 254 bytes and a %s", method,
                 constructor ? "constructor" : "function");
        return tna_throw(env, "invalid", message);
    }
    ta_result result = tna_js_datamodel_register_name(
        &wrapper->dm,
        &(tna_register_name_params){ .name = { name, (uint32_t)length }, .value = argv[1], .constructor = constructor });
    if (result == TA_ERR_INVALID) {
        snprintf(message, sizeof message, "%s: a function or class is already registered under this name", method);
        return tna_throw(env, "invalid", message);
    }
    if (result != TA_OK) {
        snprintf(message, sizeof message, "%s: could not keep it", method);
        return tna_throw(env, "out-of-memory", message);
    }
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    return undefined;
}

static napi_status tna_set_uint(napi_env env, napi_value object, const char *name, double value) {
    napi_value number = NULL;
    napi_status status = napi_create_double(env, value, &number);
    return status == napi_ok ? napi_set_named_property(env, object, name, number) : status;
}

/* A refused import, as the plain object lib/ turns into a TinyactorsError. */
static napi_value tna_import_refusal(napi_env env, ta_result result, const tna_import_failure *failure) {
    napi_value refusal = NULL;
    napi_value text = NULL;
    napi_status status = napi_create_object(env, &refusal);
    if (status == napi_ok) status = napi_create_string_utf8(env, result == TA_ERR_UNSUPPORTED ? "unsupported" : "invalid",
                                                            NAPI_AUTO_LENGTH, &text);
    if (status == napi_ok) status = napi_set_named_property(env, refusal, "code", text);
    if (status == napi_ok) status = napi_create_string_utf8(env, failure->stage == TA_IMPORT_STAGE_DECODE ? "decode" : "compile",
                                                            NAPI_AUTO_LENGTH, &text);
    if (status == napi_ok) status = napi_set_named_property(env, refusal, "stage", text);
    if (status == napi_ok) status = napi_create_string_utf8(env, failure->message, NAPI_AUTO_LENGTH, &text);
    if (status == napi_ok) status = napi_set_named_property(env, refusal, "message", text);
    if (status == napi_ok && failure->located) {
        status = tna_set_uint(env, refusal, "line", failure->line);
        if (status == napi_ok) status = tna_set_uint(env, refusal, "column", failure->column);
        if (status == napi_ok) status = tna_set_uint(env, refusal, "offset", (double)failure->offset);
    }
    /* A codec's refusal (napi ADR 0027): its code, and what it is about. */
    const char *chart = failure->stage == TA_IMPORT_STAGE_DECODE ? tna_chart_error_name(failure->code) : NULL;
    if (status == napi_ok && chart != NULL) {
        status = napi_create_string_utf8(env, chart, NAPI_AUTO_LENGTH, &text);
        if (status == napi_ok) status = napi_set_named_property(env, refusal, "chart", text);
        if (status == napi_ok && failure->construct[0] != '\0') {
            status = napi_create_string_utf8(env, failure->construct, NAPI_AUTO_LENGTH, &text);
            if (status == napi_ok) status = napi_set_named_property(env, refusal, "construct", text);
        }
        if (status == napi_ok && failure->attribute[0] != '\0') {
            status = napi_create_string_utf8(env, failure->attribute, NAPI_AUTO_LENGTH, &text);
            if (status == napi_ok) status = napi_set_named_property(env, refusal, "attribute", text);
        }
    }
    return status == napi_ok ? refusal : tna_throw(env, "out-of-memory", "define: cannot describe the refusal");
}


/*
 * After a definition is registered by either define() form: its inline
 * child charts are compiled now and nested (napi ADR 0024), and it keeps
 * `resolve` for the children it compiles at invoke time from srcexpr. A
 * refused child retires the definition, which frees what it nested.
 */
static ta_result tna_adopt(tna_napi_system *wrapper, ta_definition_id id, napi_value resolve,
                           tna_import_failure *failure) {
    tna_system *system = wrapper->system;
    /* resolve(src) and the compilers run inside this core call (ADR 0019). */
    system->inside_core = true;
    ta_result result = tna_children_nest(wrapper, id, resolve, failure);
    if (result != TA_OK) {
        /* Retiring drops the hold, which frees the chart and what it nested. */
        (void)ta_system_retire_definition(system->core, id);
    }
    system->inside_core = false;
    tna_scratch_reset_call(&system->scratch);
    if (result == TA_OK && resolve != NULL && tna_children_keep_resolver(wrapper, id, resolve) != TA_OK) {
        (void)ta_system_retire_definition(system->core, id);
        result = TA_ERR_NOMEM;
    }
    return result;
}

/* What a define() method answers: the ID, a refusal object, or a throw. */
static napi_value tna_define_result(napi_env env, ta_result result, const tna_import_failure *failure,
                                    ta_definition_id id, const char *method) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        return NULL;
    }
    if (result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) {
        return tna_import_refusal(env, result, failure);
    }
    char message[96];
    if (result != TA_OK) {
        snprintf(message, sizeof message, "%s: %s", method,
                 result == TA_ERR_DEFINITION_MISMATCH ? "another chart is registered under this identity"
                                                      : "the definition failed");
        return tna_throw(env, tna_result_code(result), message);
    }
    napi_value value = NULL;
    napi_create_bigint_uint64(env, id, &value);
    return value;
}

/*
 * The codec a define() or an encode() uses (napi ADR 0027): the SCXML codec
 * when `value` is undefined, or a JavaScript codec { name, decode?, encode? }
 * whose functions lib/ wrapped (lib/system.js, #codecCall). False with an
 * exception pending when `value` is neither.
 */
typedef struct {
    ta_scxml_codec scxml;
    tna_js_codec js;
    ta_chart_codec adapter;
} tna_codec_choice;

static bool tna_codec_argument(tna_call *call, napi_value value, tna_codec_choice *choice) {
    napi_env env = call->env;
    napi_valuetype type = napi_undefined;
    if (value == NULL || (napi_typeof(env, value, &type) == napi_ok && type == napi_undefined)) {
        if (tna_scxml_codec(&choice->scxml, &choice->adapter) != TA_OK) {
            tna_throw(env, "out-of-memory", "define: no memory for the SCXML codec");
            return false;
        }
        return true;
    }
    napi_value functions[2] = { NULL, NULL };
    static const char *const names[2] = { "decode", "encode" };
    for (uint32_t index = 0; index < 2u; index += 1u) {
        napi_valuetype kind = napi_undefined;
        if (napi_get_named_property(env, value, names[index], &functions[index]) != napi_ok ||
            napi_typeof(env, functions[index], &kind) != napi_ok) {
            return false;
        }
        if (kind != napi_function) {
            functions[index] = NULL;
        }
    }
    char name[TNA_CODEC_NAME_MAX];
    size_t length = 0;
    napi_value text = NULL;
    if (napi_get_named_property(env, value, "name", &text) != napi_ok ||
        napi_get_value_string_utf8(env, text, name, sizeof name, &length) != napi_ok ||
        tna_js_codec_init(&choice->js, call, (ta_str){ name, (uint32_t)length }, functions[0], functions[1],
                          &choice->adapter) != TA_OK) {
        tna_throw(env, "invalid", "define: expected a codec { name, decode?, encode? }");
        return false;
    }
    return true;
}

/*
 * importDefinition(bytes, name?, resolve?, identity?, codec?): decodes a
 * document with the SCXML codec, or with `codec`, a JavaScript codec (napi
 * ADR 0027); compiles it against the data model it names; and registers it
 * (ADR 0019, core ADR 0076). `name` is what the document is called, the
 * source its spans name (core ADR 0085); `resolve(src)` fetches <data src>
 * and <script src>; `identity` ({ family: { name }, revision }) is the
 * identity to register it under, which no chart syntax carries. The
 * definition ID as a bigint, or a refusal object { code, stage, message,
 * line, column, offset, chart, construct, attribute }.
 */
static napi_value tna_import_definition_method(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = { NULL, NULL, NULL, NULL, NULL };
    napi_value self = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "define: not from inside a core call (ADR 0013)");
    }
    bool typed = false;
    napi_typedarray_type kind = napi_uint8_array;
    size_t length = 0;
    void *data = NULL;
    if (argc < 1 || napi_is_typedarray(env, argv[0], &typed) != napi_ok || !typed ||
        napi_get_typedarray_info(env, argv[0], &kind, &length, &data, NULL, NULL) != napi_ok ||
        kind != napi_uint8_array || length > UINT32_MAX) {
        return tna_throw(env, "invalid", "importDefinition: expected the document as a Uint8Array");
    }
    /* The call carries a JavaScript codec's spec, and what it retains, until
     * core has compiled it. */
    tna_call call;
    tna_call_begin(&call, env, "define");
    if (!tna_call_receiver(&call, self)) {
        return tna_call_end(&call, NULL);
    }
    ta_str name = { 0 };
    napi_valuetype name_type = napi_undefined;
    if (argc >= 2 && napi_typeof(env, argv[1], &name_type) == napi_ok && name_type != napi_undefined &&
        !tna_text_argument(env, system, argv[1], &name)) {
        return tna_call_end(&call, tna_throw(env, "invalid", "importDefinition: expected the name as a string"));
    }
    tna_src_resolver fetcher = { .env = env, .resolve = NULL };
    napi_valuetype resolve_type = napi_undefined;
    if (argc >= 3 && napi_typeof(env, argv[2], &resolve_type) == napi_ok && resolve_type == napi_function) {
        fetcher.resolve = argv[2];
    }
    ta_definition_identity *identity = NULL;
    napi_valuetype identity_type = napi_undefined;
    if (argc >= 4 && napi_typeof(env, argv[3], &identity_type) == napi_ok && identity_type != napi_undefined) {
        identity = tna_call_alloc(&call, sizeof *identity, _Alignof(ta_definition_identity));
        if (identity == NULL || !tna_gen_decode_identity(&call, argv[3], identity)) {
            return tna_call_end(&call, NULL);
        }
    }
    tna_codec_choice codec;
    if (!tna_codec_argument(&call, argc >= 5 ? argv[4] : NULL, &codec)) {
        return tna_call_end(&call, NULL);
    }
    ta_datamodel choices[2u + TNA_DM_REGISTERED_MAX];
    uint32_t count = tna_js_datamodel_choices(&wrapper->dm, choices);
    ta_definition_id id = 0;
    tna_import_failure failure;
    /* The codec and the compilers run inside this core call: one that calls
     * back into the system gets `reentrant` (ADR 0019 section 3). */
    system->inside_core = true;
    ta_result result = tna_import(system, &(tna_import_params){
                                              .codec = codec.adapter,
                                              .input = { .ptr = data, .len = (uint32_t)length },
                                              .name = name,
                                              .datamodels = choices,
                                              .datamodel_count = count,
                                              .out = &id,
                                              .failure = &failure,
                                              .resolver = tna_src_resolver_adapter(&fetcher),
                                              .identity = identity,
                                          });
    system->inside_core = false;
    tna_scratch_reset_call(&system->scratch);
    if (result == TA_OK) {
        result = tna_adopt(wrapper, id, fetcher.resolve, &failure);
    }
    return tna_call_end(&call, tna_define_result(env, result, &failure, id, "importDefinition"));
}

/*
 * adoptDefinition(id, resolve?): what define(spec) does after registering
 * (napi ADR 0027), as importDefinition does after its import: nests the
 * definition's inline SCXML children and keeps `resolve`. The ID back as a
 * bigint, or a refusal object as importDefinition's; a refused definition
 * is retired.
 */
static napi_value tna_adopt_definition_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    if (wrapper->system->inside_core) {
        return tna_throw(env, "reentrant", "define: not from inside a core call (ADR 0013)");
    }
    uint64_t id = 0;
    bool lossless = false;
    if (argc < 1 || napi_get_value_bigint_uint64(env, argv[0], &id, &lossless) != napi_ok || !lossless) {
        return tna_throw(env, "invalid", "adoptDefinition: expected a definition ID");
    }
    napi_value resolve = NULL;
    napi_valuetype resolve_type = napi_undefined;
    if (argc >= 2 && napi_typeof(env, argv[1], &resolve_type) == napi_ok && resolve_type == napi_function) {
        resolve = argv[1];
    }
    tna_import_failure failure;
    ta_result result = tna_adopt(wrapper, (ta_definition_id)id, resolve, &failure);
    return tna_define_result(env, result, &failure, (ta_definition_id)id, "adoptDefinition");
}
/* One kind's spans as an array by ID: { line, column, endLine, endColumn,
 * offset, length }, or null where the encode wrote nothing for the element. */
static napi_status tna_encoded_spans(napi_env env, napi_value into, const char *kind, const ta_source_span *spans,
                                     uint32_t count) {
    napi_value list = NULL;
    napi_status status = napi_create_array_with_length(env, count, &list);
    for (uint32_t id = 0; status == napi_ok && id < count; id += 1u) {
        const ta_source_span *span = &spans[id];
        napi_value entry = NULL;
        if (span->line == 0) {
            status = napi_get_null(env, &entry);
        } else {
            status = napi_create_object(env, &entry);
            if (status == napi_ok) status = tna_set_uint(env, entry, "line", span->line);
            if (status == napi_ok) status = tna_set_uint(env, entry, "column", span->column);
            if (status == napi_ok) status = tna_set_uint(env, entry, "endLine", span->end_line);
            if (status == napi_ok) status = tna_set_uint(env, entry, "endColumn", span->end_column);
            if (status == napi_ok) status = tna_set_uint(env, entry, "offset", (double)span->offset);
            if (status == napi_ok) status = tna_set_uint(env, entry, "length", span->length);
        }
        if (status == napi_ok) status = napi_set_element(env, list, id, entry);
    }
    return status == napi_ok ? napi_set_named_property(env, into, kind, list) : status;
}

/*
 * encodeDefinition(id, codec, print, datamodel?): the definition written
 * with the SCXML codec (`codec` undefined), or with `codec`, a JavaScript
 * codec (napi ADR 0027); its host expressions printed by `print`, lib's
 * (kind, value) => text or { message }, and naming `datamodel` in place of
 * its own when that is a string (napi ADR 0031); as { bytes, identity?,
 * spans: { state, transition, action, expression } }: the bytes, its
 * explicit identity ({ family: { name }, revision }), and where each element
 * landed (core ADR 0085), each kind an array by ID. Throws `not-found` for
 * no such definition; a codec that cannot write the chart (SCXML and a
 * JavaScript function) or only decodes answers a refusal object, as
 * importDefinition's.
 */
static napi_value tna_encode_definition_method(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4] = { NULL, NULL, NULL, NULL };
    napi_value self = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "encodeDefinition: not from inside a core call (ADR 0013)");
    }
    uint64_t definition = 0;
    bool lossless = false;
    if (argc < 1 || napi_get_value_bigint_uint64(env, argv[0], &definition, &lossless) != napi_ok || !lossless) {
        return tna_throw(env, "invalid", "encodeDefinition: expected a definition ID as a bigint");
    }
    tna_call call;
    tna_call_begin(&call, env, "encode");
    if (!tna_call_receiver(&call, self)) {
        return tna_call_end(&call, NULL);
    }
    tna_codec_choice codec;
    if (!tna_codec_argument(&call, argc >= 2 ? argv[1] : NULL, &codec)) {
        return tna_call_end(&call, NULL);
    }
    napi_valuetype print_type = napi_undefined;
    napi_valuetype datamodel_type = napi_undefined;
    char datamodel[TNA_DM_NAME_MAX];
    size_t datamodel_length = 0;
    if (argc < 3 || napi_typeof(env, argv[2], &print_type) != napi_ok || print_type != napi_function ||
        (argc >= 4 && napi_typeof(env, argv[3], &datamodel_type) != napi_ok)) {
        return tna_call_end(&call, tna_throw(env, "invalid", "encodeDefinition: expected a print function"));
    }
    if (datamodel_type == napi_string &&
        (napi_get_value_string_utf8(env, argv[3], datamodel, sizeof datamodel, &datamodel_length) != napi_ok ||
         datamodel_length >= sizeof datamodel - 1u)) {
        return tna_call_end(&call, tna_throw(env, "invalid", "encodeDefinition: the data model name is too long"));
    }
    tna_printer printer = { .dm = &wrapper->dm, .print = argv[2] };
    ta_chart_expression_printer expressions = tna_printer_adapter(&printer);
    ta_arena arena;
    if (tna_encode_arena(system, &arena) != TA_OK) {
        return tna_call_end(&call, tna_throw(env, "out-of-memory", "encodeDefinition: no memory for the encode"));
    }
    ta_bytes text = { 0 };
    ta_source_map map = { 0 };
    const ta_definition_identity *identity = NULL;
    tna_import_failure failure;
    /* A JavaScript codec runs inside this core call. */
    system->inside_core = true;
    ta_result result = tna_encode(system, &(tna_encode_params){
                                              .definition = definition,
                                              .codec = codec.adapter,
                                              .expressions = expressions,
                                              .datamodel = { datamodel, (uint32_t)datamodel_length },
                                              .arena = &arena,
                                              .out = &text,
                                              .spans = &map,
                                              .identity = &identity,
                                              .failure = &failure,
                                          });
    system->inside_core = false;
    napi_value out = NULL;
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        out = NULL;
    } else if (result == TA_OK) {
        napi_value spans = NULL;
        napi_value value = NULL;
        void *data = NULL;
        napi_value buffer = NULL;
        napi_status status = napi_create_object(env, &out);
        if (status == napi_ok) status = napi_create_arraybuffer(env, text.len, &data, &buffer);
        if (status == napi_ok && text.len != 0) memcpy(data, text.ptr, text.len);
        if (status == napi_ok) status = napi_create_typedarray(env, napi_uint8_array, text.len, buffer, 0, &value);
        if (status == napi_ok) status = napi_set_named_property(env, out, "bytes", value);
        if (status == napi_ok && identity != NULL) {
            value = tna_gen_encode_identity(&call, identity);
            status = value != NULL ? napi_set_named_property(env, out, "identity", value) : napi_generic_failure;
        }
        if (status == napi_ok) status = napi_create_object(env, &spans);
        if (status == napi_ok) status = tna_encoded_spans(env, spans, "state", map.states, map.state_count);
        if (status == napi_ok) status = tna_encoded_spans(env, spans, "transition", map.transitions, map.transition_count);
        if (status == napi_ok) status = tna_encoded_spans(env, spans, "action", map.actions, map.action_count);
        if (status == napi_ok) status = tna_encoded_spans(env, spans, "expression", map.expressions, map.expression_count);
        if (status == napi_ok) status = napi_set_named_property(env, out, "spans", spans);
        if (status != napi_ok) {
            out = tna_throw(env, "out-of-memory", "encodeDefinition: cannot build the result");
        }
    } else if (result == TA_ERR_NOT_FOUND) {
        out = tna_throw(env, "not-found", "encodeDefinition: no such definition");
    } else if (result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) {
        if (result == TA_ERR_UNSUPPORTED) {
            failure = (tna_import_failure){ .stage = TA_IMPORT_STAGE_DECODE };
            snprintf(failure.message, sizeof failure.message, "the codec cannot encode");
        } else if (printer.message[0] != '\0') {
            /* The data model's reason, not the codec's phrase (napi ADR 0031). */
            snprintf(failure.message, sizeof failure.message, "%s", printer.message);
        }
        out = tna_import_refusal(env, result, &failure);
    } else {
        out = tna_throw(env, "out-of-memory", "encodeDefinition: the encode failed");
    }
    ta_arena_destroy(&arena);
    return tna_call_end(&call, out);
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
 * The event a post queues: `name`, and `data` boxed when it is not
 * undefined (ADR 0011). The caller owns the data's reference.
 */
static bool tna_post_event(napi_env env, tna_napi_system *wrapper, uint32_t name, napi_value data, ta_event *out) {
    *out = (ta_event){ .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    napi_valuetype type = napi_undefined;
    if (data == NULL || napi_typeof(env, data, &type) != napi_ok || type == napi_undefined) {
        return true;
    }
    if (!tna_value_make(&wrapper->values, data, &out->data)) {
        tna_throw(env, "out-of-memory", "post: cannot keep the event data");
        return false;
    }
    return true;
}

/* The retained-bytes estimate argument (ADR 0005 point 9): 0 when absent. */
static bool tna_bytes_argument(napi_env env, napi_value value, uint32_t *out) {
    napi_valuetype type = napi_undefined;
    *out = 0;
    if (value == NULL || napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) {
        return true;
    }
    double bytes = 0;
    if (type != napi_number || napi_get_value_double(env, value, &bytes) != napi_ok || !(bytes >= 0)) {
        return false;
    }
    *out = bytes > (double)UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
    return true;
}

/*
 * A send() in flight: its waiter and promise. A post has none. `failed`
 * settles a refused message: a post throws, a send rejects (ADR 0008 point
 * 3: a full queue is not misuse, so a send reports it through its promise).
 */
typedef struct {
    bool send;
    uint32_t waiter;
    napi_deferred deferred;
    napi_value promise;
} tna_accepting;

static bool tna_accept_begin(napi_env env, tna_napi_system *wrapper, tna_accepting *accepting) {
    if (!accepting->send) {
        return true;
    }
    if (napi_create_promise(env, &accepting->deferred, &accepting->promise) != napi_ok) {
        tna_throw(env, "out-of-memory", "send: cannot create a promise");
        return false;
    }
    accepting->waiter = tna_sched_waiter_take(&wrapper->system->sched);
    if (accepting->waiter != 0) {
        tna_sched_waiter(&wrapper->system->sched, accepting->waiter)->host = accepting->deferred;
    }
    return true;
}

static napi_value tna_accept_failed(napi_env env, tna_napi_system *wrapper, tna_accepting *accepting, ta_result result) {
    const char *code = result == TA_ERR_BUSY ? "queue-full" : tna_result_code(result);
    const char *message = result == TA_ERR_BUSY
                              ? "the system holds maxQueuedMessages messages or maxRetainedBytes of data"
                              : "the message was not accepted";
    if (!accepting->send) {
        return tna_throw(env, code, message);
    }
    tna_sched_waiter_give(&wrapper->system->sched, accepting->waiter);
    napi_reject_deferred(env, accepting->deferred, tna_error(env, code, message));
    return accepting->promise;
}

static napi_value tna_accept_done(napi_env env, tna_napi_system *wrapper, tna_accepting *accepting) {
    if (!accepting->send) {
        napi_value undefined = NULL;
        napi_get_undefined(env, &undefined);
        return undefined;
    }
    const tna_waiter *waiter = tna_sched_waiter(&wrapper->system->sched, accepting->waiter);
    wrapper->last_send_token = (double)waiter->generation * 4294967296.0 + (double)accepting->waiter;
    return accepting->promise;
}

/*
 * The operator transition argument of post() and send() (ADR 0026, core ADR
 * 0082): undefined, or { definition, source, targets } with the definition
 * ID (a bigint) and state IDs, the targets read into the call arena. Throws
 * and returns false when it is malformed.
 */
static bool tna_jump_argument(napi_env env, tna_system *system, napi_value value, tna_message_jump *out, bool *present) {
    napi_valuetype type = napi_undefined;
    *present = false;
    if (value == NULL || napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) {
        return true;
    }
    napi_value definition = NULL;
    napi_value source = NULL;
    napi_value targets = NULL;
    bool lossless = true;
    bool is_array = false;
    uint32_t count = 0;
    if (type != napi_object || napi_get_named_property(env, value, "definition", &definition) != napi_ok ||
        napi_get_value_bigint_uint64(env, definition, &out->definition, &lossless) != napi_ok || !lossless ||
        napi_get_named_property(env, value, "source", &source) != napi_ok ||
        napi_get_value_uint32(env, source, &out->transition.source) != napi_ok ||
        napi_get_named_property(env, value, "targets", &targets) != napi_ok ||
        napi_is_array(env, targets, &is_array) != napi_ok || !is_array ||
        napi_get_array_length(env, targets, &count) != napi_ok || count == 0) {
        tna_throw(env, "invalid", "post: transition must be { definition, source, targets } with at least one target");
        return false;
    }
    ta_state_id *ids = ta_arena_alloc(&system->scratch.call, (size_t)count * sizeof(ta_state_id), _Alignof(ta_state_id));
    if (ids == NULL) {
        tna_throw(env, "out-of-memory", "post: cannot read the transition");
        return false;
    }
    for (uint32_t at = 0; at < count; at += 1u) {
        napi_value target = NULL;
        if (napi_get_element(env, targets, at, &target) != napi_ok || napi_get_value_uint32(env, target, &ids[at]) != napi_ok) {
            tna_throw(env, "invalid", "post: transition targets must be state IDs");
            return false;
        }
    }
    out->transition.targets = ids;
    out->transition.target_count = count;
    *present = true;
    return true;
}

/*
 * post(slot, generation, name, data?, bytes?, origin?, trace?, transition?)
 * and send(...): queue an event for an actor (ADR 0005). `name` is a ta_event_name_id from internEvent();
 * `data` is any JavaScript value, shared, not copied (ADR 0011); `bytes` is
 * its retained-bytes estimate. Mail for a stale ID is accepted as a dead
 * letter. post() throws "queue-full" when a limit is reached; send() returns
 * a promise that settles when the message's macrostep ends (ADR 0005 point
 * 11) and rejects with "queue-full" instead. Neither calls core, so both are
 * allowed inside a core call (a data-model callback may post; ADR 0013).
 */
static napi_value tna_post_common(napi_env env, napi_callback_info info, bool send) {
    size_t argc = 8;
    napi_value argv[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t name = 0;
    uint32_t bytes = 0;
    if (wrapper == NULL) {
        return NULL;
    }
    if (argc < 3 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &name) != napi_ok || !tna_bytes_argument(env, argv[4], &bytes)) {
        return tna_throw(env, "invalid", "post: expected (slot, generation, name, data, bytes)");
    }
    ta_trace_parent trace;
    if (!tna_trace_argument(env, argv[6], &trace)) {
        return NULL;
    }
    tna_system_start_trace(wrapper->system, &trace.context);
    ta_event event;
    if (!tna_post_event(env, wrapper, name, argv[3], &event)) {
        return NULL;
    }
    if (!tna_origin_argument(env, wrapper->system, argv[5], &event.origin)) {
        ta_event_release(&event);
        return NULL;
    }
    tna_message_jump jump;
    bool has_jump = false;
    if (!tna_jump_argument(env, wrapper->system, argc < 8 ? NULL : argv[7], &jump, &has_jump)) {
        tna_scratch_reset_call(&wrapper->system->scratch);
        ta_event_release(&event);
        return NULL;
    }
    tna_accepting accepting = { .send = send };
    if (!tna_accept_begin(env, wrapper, &accepting)) {
        tna_scratch_reset_call(&wrapper->system->scratch);
        ta_event_release(&event);
        return NULL;
    }
    ta_result result = send && accepting.waiter == 0
                           ? TA_ERR_NOMEM
                           : tna_system_post(wrapper->system, &(tna_message){
                                                                  .target = ta_actor_id_make(slot, generation),
                                                                  .event = &event,
                                                                  .bytes = bytes,
                                                                  .waiter = accepting.waiter,
                                                                  .trace = &trace,
                                                                  .jump = has_jump ? &jump : NULL,
                                                              });
    if (has_jump) {
        tna_scratch_reset_call(&wrapper->system->scratch); /* the scheduler copied the targets */
    }
    if (result != TA_OK) {
        ta_event_release(&event);
        return tna_accept_failed(env, wrapper, &accepting, result);
    }
    return tna_accept_done(env, wrapper, &accepting);
}

static napi_value tna_post_method(napi_env env, napi_callback_info info) {
    return tna_post_common(env, info, false);
}

static napi_value tna_send_method(napi_env env, napi_callback_info info) {
    return tna_post_common(env, info, true);
}

/*
 * cancelSend(token, code): a send()'s signal was aborted or its timeout
 * passed (code "aborted" or "timeout"). A message still waiting in a mailbox
 * is withdrawn and never delivered; one core already accepted is not undone.
 * The promise rejects with `code`. Returns false when the send had settled.
 */
static napi_value tna_cancel_send_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    double token = 0;
    char code[16] = { 0 };
    size_t length = 0;
    if (argc < 2 || napi_get_value_double(env, argv[0], &token) != napi_ok ||
        napi_get_value_string_utf8(env, argv[1], code, sizeof code, &length) != napi_ok) {
        return tna_throw(env, "invalid", "cancelSend: expected (token, code)");
    }
    tna_sched *sched = &wrapper->system->sched;
    uint32_t index = (uint32_t)(token - (double)(uint32_t)(token / 4294967296.0) * 4294967296.0);
    uint32_t generation = (uint32_t)(token / 4294967296.0);
    tna_waiter *waiter = tna_sched_waiter(sched, index);
    bool cancelled = false;
    if (waiter != NULL && waiter->generation == generation && waiter->host != NULL) {
        napi_deferred deferred = waiter->host;
        cancelled = tna_system_cancel(wrapper->system, index);
        if (cancelled) {
            const char *message = strcmp(code, "timeout") == 0 ? "send: the timeout passed before the message completed"
                                                                : "send: the signal was aborted";
            napi_reject_deferred(env, deferred, tna_error(env, code, message));
            tna_sched_waiter_give(sched, index);
        }
    }
    napi_value out = NULL;
    napi_get_boolean(env, cancelled, &out);
    return out;
}

/* The token of the last send() this system accepted (see cancelSend). */
static napi_value tna_last_send_token_getter(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    napi_value out = NULL;
    napi_create_double(env, wrapper->last_send_token, &out);
    return out;
}

bool tna_text_argument(napi_env env, tna_system *system, napi_value value, ta_str *out) {
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

/* Largest page idPage() accepts: 2^16 actors, 512 KiB of IDs. */
#define TNA_MAX_PAGE_SIZE 65536u

/*
 * idPage(from, limit): one page of actor IDs from core's walk (ADR 0003,
 * core ADR 0062). Returns { ids, next }: `ids` is a Uint32Array of
 * interleaved [slot, generation] pairs, whose backing store core's visitor
 * filled directly, and `next` is the position to pass for the next page, or
 * null when the walk is over. `from` must be 0 or a `next` this method gave.
 * The walk is a core call, so it is not allowed inside one.
 */
static napi_value tna_id_page_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    uint32_t from = 0;
    uint32_t limit = 0;
    if (argc < 2 || napi_get_value_uint32(env, argv[0], &from) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &limit) != napi_ok || limit == 0 || limit > TNA_MAX_PAGE_SIZE) {
        return tna_throw(env, "invalid", "idPage: expected (from, limit) with 1 <= limit <= 65536");
    }
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "actors cannot be enumerated from inside a core call (ADR 0013)");
    }
    void *data = NULL;
    napi_value buffer = NULL;
    if (napi_create_arraybuffer(env, (size_t)limit * 2u * sizeof(uint32_t), &data, &buffer) != napi_ok) {
        return tna_throw(env, "out-of-memory", "idPage: cannot allocate the page");
    }
    tna_id_page page = { .from = from, .limit = limit, .pairs = data };
    ta_result result = tna_enumerate_ids(system, &page);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "idPage: the walk failed");
    }
    napi_value ids = NULL;
    napi_value next = NULL;
    napi_value out = NULL;
    napi_status status = napi_create_typedarray(env, napi_uint32_array, (size_t)page.count * 2u, buffer, 0, &ids);
    if (status == napi_ok) {
        status = page.next == TA_ACTOR_VISIT_END ? napi_get_null(env, &next) : napi_create_uint32(env, page.next, &next);
    }
    if (status == napi_ok) {
        status = napi_create_object(env, &out);
    }
    if (status == napi_ok) {
        status = napi_set_named_property(env, out, "ids", ids);
    }
    if (status == napi_ok) {
        status = napi_set_named_property(env, out, "next", next);
    }
    if (status != napi_ok) {
        return tna_throw(env, "out-of-memory", "idPage: cannot build the page");
    }
    return out;
}

/*
 * postNamed(family, name, event, data?, bytes?) and sendNamed(...): as post
 * and send, for the actor the directory has at that address (ADR 0006), or a
 * dead letter when it has none. Interning the address is a core call, so
 * these are not allowed inside a core call.
 */
static napi_value tna_post_named_common(napi_env env, napi_callback_info info, bool send) {
    size_t argc = 7;
    napi_value argv[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
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
    ta_trace_parent trace;
    if (!tna_trace_argument(env, argv[6], &trace)) {
        return NULL;
    }
    tna_system_start_trace(wrapper->system, &trace.context);
    ta_actor_address text = {0};
    uint32_t name = 0;
    uint32_t bytes = 0;
    bool ok = argc >= 3 && tna_text_argument(env, system, argv[0], &text.family.name) &&
              tna_text_argument(env, system, argv[1], &text.name) &&
              napi_get_value_uint32(env, argv[2], &name) == napi_ok && tna_bytes_argument(env, argv[4], &bytes);
    ta_address *address = NULL;
    ta_result result = ok ? ta_address_intern(system->core, &text, &address) : TA_ERR_INVALID;
    tna_scratch_reset_call(&system->scratch);
    if (result == TA_ERR_INVALID) {
        return tna_throw(env, "invalid", "post: expected (family, name, event) with non-empty text");
    }
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "post: cannot intern the address");
    }
    ta_event event;
    if (!tna_post_event(env, wrapper, name, argv[3], &event)) {
        ta_address_release(address);
        return NULL;
    }
    if (!tna_origin_argument(env, system, argv[5], &event.origin)) {
        ta_address_release(address);
        ta_event_release(&event);
        return NULL;
    }
    tna_accepting accepting = { .send = send };
    if (!tna_accept_begin(env, wrapper, &accepting)) {
        ta_address_release(address);
        ta_event_release(&event);
        return NULL;
    }
    result = send && accepting.waiter == 0
                 ? TA_ERR_NOMEM
                 : tna_router_post_named(system, address,
                                         &(tna_message){ .event = &event, .bytes = bytes, .waiter = accepting.waiter,
                                                         .trace = &trace });
    ta_address_release(address);
    if (result != TA_OK) {
        ta_event_release(&event);
        return tna_accept_failed(env, wrapper, &accepting, result);
    }
    tna_system_want_turn(system);
    return tna_accept_done(env, wrapper, &accepting);
}

/*
 * The origin of a message the input log recorded (napi ADR 0032 sections 6
 * and 7): `{ actor: <URI> }`, parsed back by core, for the SCXML processor,
 * or an I/O origin as post() takes one. A URI core cannot parse leaves the
 * message without an origin. Throws and returns false for a malformed I/O
 * origin.
 */
static bool tna_logged_origin(napi_env env, tna_system *system, napi_value value, ta_origin *out) {
    napi_valuetype type = napi_undefined;
    napi_value actor = NULL;
    bool has_actor = false;
    if (value != NULL && napi_typeof(env, value, &type) == napi_ok && type == napi_object &&
        napi_has_named_property(env, value, "actor", &has_actor) == napi_ok && has_actor &&
        napi_get_named_property(env, value, "actor", &actor) == napi_ok) {
        ta_str uri = { 0 };
        ta_scxml_target target = { 0 };
        if (tna_text_argument(env, system, actor, &uri) && ta_scxml_target_parse(system->core, uri, &target) == TA_OK) {
            if (target.kind == TA_SCXML_TARGET_ACTOR) {
                *out = (ta_origin){ .kind = TA_ORIGIN_ACTOR, .as.actor = target.as.actor };
            } else {
                ta_scxml_target_release(&target);
            }
        }
        tna_scratch_reset_call(&system->scratch);
        return true;
    }
    return tna_origin_argument(env, system, value, out);
}

/* A logged send ID as this system names it; 0 for none (napi ADR 0032
 * section 6). A literal ID this system never saw is interned first (ADR
 * 0033 section 5), so it reaches the chart as it was written. */
static ta_send_id tna_logged_send_id(napi_env env, tna_system *system, napi_value value) {
    napi_valuetype type = napi_undefined;
    ta_send_id id = 0;
    if (value != NULL && napi_typeof(env, value, &type) == napi_ok && type == napi_string) {
        ta_str send = { 0 };
        if (tna_text_argument(env, system, value, &send)) {
            ta_result parsed = ta_send_id_parse(system->core, send, &id);
            ta_atom atom = 0;
            if (parsed == TA_ERR_NOT_FOUND && ta_system_intern_atom(system->core, send, &atom) == TA_OK) {
                parsed = ta_send_id_parse(system->core, send, &id);
            }
            if (parsed != TA_OK) {
                id = 0;
            }
        }
        tna_scratch_reset_call(&system->scratch);
    }
    return id;
}

/*
 * redeliver(slot, generation, event, data?, origin?, sendID?, platform?, since):
 * puts a message a replay found queued and never delivered back in the
 * published actor's mailbox (napi ADR 0033 section 5), with its origin
 * (`{ actor: <URI> }` or an I/O origin), send ID and platform flag. The
 * log has its queued record already, so none is made: it answers the seq
 * the message now has, which lib maps back to the record's, or 0 when the
 * actor is not logged. `since` is when it was first accepted.
 */
static napi_value tna_redeliver_method(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value argv[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "redeliver: cannot be called from inside a core call (ADR 0013)");
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t name = 0;
    double since = 0;
    if (argc < 8 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &name) != napi_ok || napi_get_value_double(env, argv[7], &since) != napi_ok ||
        !(since >= 0)) {
        return tna_throw(env, "invalid", "redeliver: expected (slot, generation, event, data, origin, sendID, platform, since)");
    }
    ta_event event;
    if (!tna_post_event(env, wrapper, name, argv[3], &event)) {
        return NULL;
    }
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, argv[6], &type) == napi_ok && type == napi_boolean) {
        bool platform = false;
        napi_get_value_bool(env, argv[6], &platform);
        event.type = platform ? TA_EVENT_PLATFORM : TA_EVENT_EXTERNAL;
    }
    if (!tna_logged_origin(env, system, argv[4], &event.origin)) {
        ta_event_release(&event);
        return NULL;
    }
    event.send_id = tna_logged_send_id(env, system, argv[5]);
    ta_trace_parent trace = { 0 };
    tna_inputs_redeliver(&system->inputs, (ta_time)since);
    ta_result result = tna_system_post(system, &(tna_message){ .target = ta_actor_id_make(slot, generation),
                                                               .event = &event,
                                                               .trace = &trace });
    uint64_t seq = tna_inputs_redelivered(&system->inputs);
    if (result != TA_OK) {
        ta_event_release(&event);
        return tna_throw(env, result == TA_ERR_BUSY ? "queue-full" : tna_result_code(result),
                         "redeliver: the message was not accepted");
    }
    napi_value out = NULL;
    napi_create_double(env, (double)seq, &out);
    return out;
}

/* The replaying actor's active invocation whose ID reads `text`; TA_INVOKE_ID_NONE when it has none. */
static ta_invoke_id tna_replay_invocation(tna_system *system, ta_actor_id actor, ta_str text) {
    ta_actor_inspection view;
    if (text.len == 0 || ta_actor_inspect(system->core, actor, &view) != TA_OK) {
        return TA_INVOKE_ID_NONE;
    }
    for (uint32_t index = 0; index < view.invoke_count; index += 1u) {
        ta_str id = { 0 };
        if (view.invokes[index].active && ta_system_invoke_id_text(system->core, view.invokes[index].id, &id) == TA_OK &&
            id.len == text.len && memcmp(id.ptr, text.ptr, text.len) == 0) {
            return view.invokes[index].id;
        }
    }
    return TA_INVOKE_ID_NONE;
}

/*
 * replaySend(slot, generation, name, data, origin, sendID, platform, invokeID, report, time, transition?):
 * one input record a replay gives a replaying actor (napi ADR 0032 section
 * 7), queued in its mailbox like a send(), whose promise settles when the
 * macrostep it starts ends. Just before the actor takes it, its clock moves
 * to `time`, the record's, so a replay queues many records at once. An `event` record carries its name, data,
 * origin (as requeue() parses it), send ID, platform flag and the text of
 * the invocation it came from, matched to the actor's active invocation. A
 * `report` record (`report` 1: unavailable, 2: quarantined, 3: failed)
 * carries the invocation and its data; core takes it as
 * ta_actor_report_invoke() when the actor is idle.
 */
static napi_value tna_replay_send_method(napi_env env, napi_callback_info info) {
    size_t argc = 11;
    napi_value argv[11] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "replay: cannot be called from inside a core call (ADR 0013)");
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t name = 0;
    uint32_t report = 0;
    double at = -1;
    if (argc < 10 || napi_get_value_double(env, argv[9], &at) != napi_ok || !(at >= 0) || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &name) != napi_ok || napi_get_value_uint32(env, argv[8], &report) != napi_ok ||
        report > TNA_REPORT_FAILED) {
        return tna_throw(env, "invalid", "replaySend: expected (slot, generation, name, data, origin, sendID, platform, "
                                         "invokeID, report, time)");
    }
    ta_actor_id actor = ta_actor_id_make(slot, generation);
    ta_event event;
    if (!tna_post_event(env, wrapper, name, argv[3], &event)) {
        return NULL;
    }
    bool platform = false;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, argv[6], &type) == napi_ok && type == napi_boolean) {
        napi_get_value_bool(env, argv[6], &platform);
    }
    event.type = platform ? TA_EVENT_PLATFORM : TA_EVENT_EXTERNAL;
    if (report == TNA_REPORT_NONE && !tna_logged_origin(env, system, argv[4], &event.origin)) {
        ta_event_release(&event);
        return NULL;
    }
    event.send_id = report == TNA_REPORT_NONE ? tna_logged_send_id(env, system, argv[5]) : 0;
    if (napi_typeof(env, argv[7], &type) == napi_ok && type == napi_string) {
        ta_str text = { 0 };
        if (tna_text_argument(env, system, argv[7], &text)) {
            event.invoke_id = tna_replay_invocation(system, actor, text);
        }
        tna_scratch_reset_call(&system->scratch);
    }
    if (report != TNA_REPORT_NONE && event.invoke_id == TA_INVOKE_ID_NONE) {
        ta_event_release(&event);
        return tna_throw(env, "not-found", "replay: the replaying actor has no active invocation the report names");
    }
    /* A `jump` record's operator transition (napi ADR 0033 section 3). */
    tna_message_jump jump;
    bool has_jump = false;
    if (!tna_jump_argument(env, system, argc < 11 ? NULL : argv[10], &jump, &has_jump)) {
        tna_scratch_reset_call(&system->scratch);
        ta_event_release(&event);
        return NULL;
    }
    tna_accepting accepting = { .send = true };
    if (!tna_accept_begin(env, wrapper, &accepting)) {
        tna_scratch_reset_call(&system->scratch);
        ta_event_release(&event);
        return NULL;
    }
    ta_trace_parent trace = { 0 };
    ta_time time = (ta_time)at;
    ta_result result = accepting.waiter == 0
                           ? TA_ERR_NOMEM
                           : tna_system_post(system, &(tna_message){ .target = actor,
                                                                     .event = &event,
                                                                     .waiter = accepting.waiter,
                                                                     .trace = &trace,
                                                                     .report = (uint8_t)report,
                                                                     .jump = has_jump ? &jump : NULL,
                                                                     .replay_time = &time });
    if (has_jump) {
        tna_scratch_reset_call(&system->scratch); /* the scheduler copied the targets */
    }
    if (result != TA_OK) {
        ta_event_release(&event);
        return tna_accept_failed(env, wrapper, &accepting, result);
    }
    return tna_accept_done(env, wrapper, &accepting);
}

/*
 * dispatchDue(): dispatches the sends due now, as a turn does after its
 * deliveries; true when some are still due (a budget or a full queue left
 * them). evict() calls it before it removes an actor (napi ADR 0032
 * section 8).
 */
static napi_value tna_dispatch_due_method(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "dispatchDue: cannot be called from inside a core call (ADR 0013)");
    }
    bool more_due = tna_system_dispatch_due(system);
    tna_system_want_turn(system);
    napi_value result = NULL;
    napi_get_boolean(env, more_due, &result);
    return result;
}

static napi_value tna_post_named_method(napi_env env, napi_callback_info info) {
    return tna_post_named_common(env, info, false);
}

static napi_value tna_send_named_method(napi_env env, napi_callback_info info) {
    return tna_post_named_common(env, info, true);
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
    if (status == napi_ok) status = tna_set_number(env, *out, "largestBlock", (double)counters.largest_block);
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

/* Lowercase hex of `bytes` into `out` (2 * count + 1 bytes). */
static void tna_hex(const uint8_t *bytes, size_t count, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t at = 0; at < count; at += 1u) {
        out[2u * at] = digits[bytes[at] >> 4];
        out[2u * at + 1u] = digits[bytes[at] & 15u];
    }
    out[2u * count] = '\0';
}

static napi_status tna_set_text(napi_env env, napi_value object, const char *name, const char *text) {
    napi_value value = NULL;
    napi_status status = napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &value);
    return status == napi_ok ? napi_set_named_property(env, object, name, value) : status;
}

/* { count, sum, min, max, bounds, buckets, exemplars: [{ bucket, value, time, traceID, spanID }] } */
static napi_status tna_histogram_object(napi_env env, const tna_histogram *histogram, napi_value *out) {
    napi_value bounds = NULL;
    napi_value buckets = NULL;
    napi_value exemplars = NULL;
    napi_status status = napi_create_object(env, out);
    if (status == napi_ok) status = tna_set_number(env, *out, "count", (double)histogram->count);
    if (status == napi_ok) status = tna_set_number(env, *out, "sum", histogram->sum);
    if (status == napi_ok) status = tna_set_number(env, *out, "min", histogram->min);
    if (status == napi_ok) status = tna_set_number(env, *out, "max", histogram->max);
    if (status == napi_ok) status = napi_create_array_with_length(env, histogram->bound_count, &bounds);
    if (status == napi_ok) status = napi_create_array_with_length(env, histogram->bound_count + 1u, &buckets);
    if (status == napi_ok) status = napi_create_array(env, &exemplars);
    uint32_t exemplar_count = 0;
    for (uint32_t at = 0; status == napi_ok && at <= histogram->bound_count; at += 1u) {
        napi_value number = NULL;
        if (at < histogram->bound_count) {
            status = napi_create_double(env, histogram->bounds[at], &number);
            if (status == napi_ok) status = napi_set_element(env, bounds, at, number);
        }
        if (status == napi_ok) status = napi_create_double(env, (double)histogram->buckets[at], &number);
        if (status == napi_ok) status = napi_set_element(env, buckets, at, number);
        const tna_exemplar *exemplar = &histogram->exemplars[at];
        if (status == napi_ok && exemplar->set) {
            char trace[33];
            char span[17];
            tna_hex(exemplar->trace_id, 16, trace);
            tna_hex(exemplar->span_id, 8, span);
            napi_value entry = NULL;
            status = napi_create_object(env, &entry);
            if (status == napi_ok) status = tna_set_number(env, entry, "bucket", (double)at);
            if (status == napi_ok) status = tna_set_number(env, entry, "value", exemplar->value);
            if (status == napi_ok) status = tna_set_number(env, entry, "time", (double)exemplar->time_ns / 1e6);
            if (status == napi_ok) status = tna_set_text(env, entry, "traceID", trace);
            if (status == napi_ok) status = tna_set_text(env, entry, "spanID", span);
            if (status == napi_ok) status = napi_set_element(env, exemplars, exemplar_count++, entry);
        }
    }
    if (status == napi_ok) status = napi_set_named_property(env, *out, "bounds", bounds);
    if (status == napi_ok) status = napi_set_named_property(env, *out, "buckets", buckets);
    if (status == napi_ok) status = napi_set_named_property(env, *out, "exemplars", exemplars);
    return status;
}

/*
 * hostMetrics(): what only the host knows, for system.metrics() (napi ADR
 * 0017 section 7): { turn, lateness (histograms, in seconds), runnable,
 * ready (queue lengths, including entries a later turn skips),
 * quarantined, overdue (seconds), faults: { code: count }, deadLetters:
 * { reason: count }, and for loads of unloaded actors (napi ADR 0020)
 * load (a histogram, in seconds), loads: { outcome: count } and waiting,
 * and the definition lifecycle (core ADR 0079) as definitions: {
 * registered, retired, freed, alive: [{ origin, retired, count }] } }.
 */
static napi_value tna_host_metrics_method(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    const tna_system *system = wrapper->system;
    const tna_sched *sched = &system->sched;
    napi_value out = NULL;
    napi_value turn = NULL;
    napi_value lateness = NULL;
    napi_value faults = NULL;
    napi_value dead = NULL;
    napi_value load = NULL;
    napi_value loads = NULL;
    napi_status status = napi_create_object(env, &out);
    if (status == napi_ok) status = tna_histogram_object(env, &system->metrics.turn_seconds, &turn);
    if (status == napi_ok) status = napi_set_named_property(env, out, "turn", turn);
    if (status == napi_ok) status = tna_histogram_object(env, &system->metrics.lateness_seconds, &lateness);
    if (status == napi_ok) status = napi_set_named_property(env, out, "lateness", lateness);
    if (status == napi_ok) status = tna_set_number(env, out, "runnable", (double)tna_ring_count(&sched->runnable));
    if (status == napi_ok) status = tna_set_number(env, out, "ready", (double)tna_ring_count(&sched->ready));
    if (status == napi_ok) status = tna_set_number(env, out, "quarantined", (double)sched->quarantined);
    if (status == napi_ok) status = tna_set_number(env, out, "overdue", tna_system_overdue_seconds(system));
    if (status == napi_ok) status = napi_create_object(env, &faults);
    for (uint32_t code = 0; status == napi_ok && code < TNA_FAULT_CODES; code += 1u) {
        if (sched->faults[code] != 0) {
            status = tna_set_number(env, faults, tna_result_code(-(ta_result)code), (double)sched->faults[code]);
        }
    }
    if (status == napi_ok) status = napi_set_named_property(env, out, "faults", faults);
    if (status == napi_ok) status = napi_create_object(env, &dead);
    for (uint32_t reason = 0; status == napi_ok && reason < TNA_DEAD_REASONS; reason += 1u) {
        status = tna_set_number(env, dead, tna_dead_reason_name(reason), (double)sched->dead_by_reason[reason]);
    }
    if (status == napi_ok) status = napi_set_named_property(env, out, "deadLetters", dead);
    /* Loads of unloaded actors (napi ADR 0020): outcomes, duration, waiting mail. */
    if (status == napi_ok) status = tna_histogram_object(env, &system->metrics.load_seconds, &load);
    if (status == napi_ok) status = napi_set_named_property(env, out, "load", load);
    if (status == napi_ok) status = napi_create_object(env, &loads);
    for (uint32_t outcome = 0; status == napi_ok && outcome < TNA_LOAD_OUTCOMES; outcome += 1u) {
        status = tna_set_number(env, loads, tna_load_outcome_name(outcome), (double)system->loads.outcomes[outcome]);
    }
    if (status == napi_ok) status = napi_set_named_property(env, out, "loads", loads);
    if (status == napi_ok) status = tna_set_number(env, out, "waiting", (double)system->loads.waiting);
    /* The definition lifecycle (core ADR 0079): events, and what is alive by origin. */
    napi_value definitions = NULL;
    napi_value alive = NULL;
    uint64_t counts[TNA_ORIGINS][2];
    tna_definitions_alive(&system->definitions, system->core, counts);
    if (status == napi_ok) status = napi_create_object(env, &definitions);
    if (status == napi_ok) status = tna_set_number(env, definitions, "registered", (double)system->definitions.registered);
    if (status == napi_ok) status = tna_set_number(env, definitions, "retired", (double)system->definitions.retired);
    if (status == napi_ok) status = tna_set_number(env, definitions, "freed", (double)system->definitions.freed);
    if (status == napi_ok) status = napi_create_array_with_length(env, TNA_ORIGINS * 2u, &alive);
    for (uint32_t origin = 0, at = 0; status == napi_ok && origin < TNA_ORIGINS; origin += 1u) {
        for (uint32_t retired = 0; status == napi_ok && retired < 2u; retired += 1u, at += 1u) {
            napi_value entry = NULL;
            napi_value name = NULL;
            napi_value flag = NULL;
            status = napi_create_object(env, &entry);
            if (status == napi_ok) status = napi_create_string_utf8(env, tna_definition_origin_name(origin), NAPI_AUTO_LENGTH, &name);
            if (status == napi_ok) status = napi_set_named_property(env, entry, "origin", name);
            if (status == napi_ok) status = napi_get_boolean(env, retired != 0, &flag);
            if (status == napi_ok) status = napi_set_named_property(env, entry, "retired", flag);
            if (status == napi_ok) status = tna_set_number(env, entry, "count", (double)counts[origin][retired]);
            if (status == napi_ok) status = napi_set_element(env, alive, at, entry);
        }
    }
    if (status == napi_ok) status = napi_set_named_property(env, definitions, "alive", alive);
    if (status == napi_ok) status = napi_set_named_property(env, out, "definitions", definitions);
    if (status != napi_ok) {
        return tna_throw(env, "out-of-memory", "cannot build the host metrics object");
    }
    return out;
}

/*
 * stats(): { memory: { total, core, envelopes, ... }, allocatorCalls,
 * messages: { queued, delivered, deadLetters }, values: { live, made } }
 * (ADRs 0008, 0009, 0011).
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
    napi_value values = NULL;
    if (status == napi_ok) status = napi_create_object(env, &values);
    if (status == napi_ok) status = tna_set_number(env, values, "live", (double)wrapper->values.live);
    if (status == napi_ok) status = tna_set_number(env, values, "made", (double)wrapper->values.made);
    if (status == napi_ok) status = napi_set_named_property(env, stats, "values", values);
    if (status != napi_ok) {
        return tna_throw(env, "out-of-memory", "cannot build the stats object");
    }
    return stats;
}

napi_status tna_napi_system_define(napi_env env, napi_value exports) {
    napi_property_descriptor properties[] = {
        { "close", NULL, tna_close_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "stats", NULL, tna_stats_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "hostMetrics", NULL, tna_host_metrics_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "refuseAllocationsFrom", NULL, tna_refuse_allocations_from_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "flush", NULL, tna_flush_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "advanceTo", NULL, tna_advance_to_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "clock", NULL, NULL, tna_clock_getter, NULL, NULL, napi_default_jsproperty, NULL },
        { "time", NULL, NULL, tna_time_getter, NULL, NULL, napi_default_jsproperty, NULL },
        { "release", NULL, tna_release_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "quarantine", NULL, tna_quarantine_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "scheduling", NULL, tna_scheduling_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "mailbox", NULL, tna_mailbox_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "selectDatamodel", NULL, tna_select_datamodel_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "registerDatamodel", NULL, tna_register_datamodel_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "registerFunction", NULL, tna_register_name_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "registerClass", NULL, tna_register_name_method, NULL, NULL, NULL, napi_default_method, (void *)"class" },
        { "importDefinition", NULL, tna_import_definition_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "adoptDefinition", NULL, tna_adopt_definition_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "encodeDefinition", NULL, tna_encode_definition_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "internEvent", NULL, tna_intern_event_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "post", NULL, tna_post_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "postNamed", NULL, tna_post_named_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "send", NULL, tna_send_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "sendNamed", NULL, tna_send_named_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "cancelSend", NULL, tna_cancel_send_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "lastSendToken", NULL, NULL, tna_last_send_token_getter, NULL, NULL, napi_default_jsproperty, NULL },
        { "idPage", NULL, tna_id_page_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "registerIO", NULL, tna_register_io_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "openScope", NULL, tna_open_scope_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "closeScope", NULL, tna_close_scope_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "registerInvoker", NULL, tna_register_invoker_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setLogSink", NULL, tna_set_log_sink_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setTraceSink", NULL, tna_set_trace_sink_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "spawnTrace", NULL, tna_spawn_trace_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setLoaderFamily", NULL, tna_set_loader_family_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "completeLoad", NULL, tna_complete_load_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "loadTrace", NULL, tna_load_trace_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "wakeAt", NULL, tna_wake_at_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "cancelWake", NULL, tna_cancel_wake_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setOffloadValues", NULL, tna_set_offload_values_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setDurableCodec", NULL, tna_set_durable_codec_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "durableBytes", NULL, tna_durable_bytes_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "durableValue", NULL, tna_durable_value_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "offload", NULL, tna_offload_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "hasOffloaded", NULL, tna_has_offloaded_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "dropOffloaded", NULL, tna_drop_offloaded_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "loadOffloaded", NULL, tna_load_offloaded_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "configureTraces", NULL, tna_configure_traces_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setTraceWatch", NULL, tna_set_trace_watch_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "traceWatchSizes", NULL, tna_trace_watch_sizes_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "reportInvoke", NULL, tna_report_invoke_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "setInputLog", NULL, tna_set_input_log_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "inputHost", NULL, tna_input_host_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "redeliver", NULL, tna_redeliver_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "replaySend", NULL, tna_replay_send_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "dispatchDue", NULL, tna_dispatch_due_method, NULL, NULL, NULL, napi_default_method, NULL },
        { "closed", NULL, NULL, tna_closed_getter, NULL, NULL, napi_default_jsproperty, NULL },
    };
    napi_value constructor = NULL;
    napi_status status = napi_define_class(env, "System", NAPI_AUTO_LENGTH, tna_construct, NULL,
                                           sizeof properties / sizeof properties[0], properties, &constructor);
    return status == napi_ok ? napi_set_named_property(env, exports, "System", constructor) : status;
}
