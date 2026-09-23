#include "pump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"
#include "scheduler.h"
#include "system.h"

/* ------------------------------------------------------------------------
 * Options
 */

static bool tna_option(napi_env env, napi_value object, const char *name, napi_value *out) {
    bool has = false;
    *out = NULL;
    if (object == NULL || napi_has_named_property(env, object, name, &has) != napi_ok || !has) {
        return true;
    }
    if (napi_get_named_property(env, object, name, out) != napi_ok) {
        return false;
    }
    napi_valuetype type = napi_undefined;
    napi_typeof(env, *out, &type);
    if (type == napi_undefined) {
        *out = NULL;
    }
    return true;
}

/* A non-negative integer option, or `fallback` when absent. */
static bool tna_count_option(napi_env env, napi_value object, const char *name, uint32_t *inout) {
    napi_value value = NULL;
    if (!tna_option(env, object, name, &value)) {
        return false;
    }
    if (value == NULL) {
        return true;
    }
    double number = 0;
    if (napi_get_value_double(env, value, &number) != napi_ok || number < 0 || number > 4294967295.0 ||
        number != (double)(uint32_t)number) {
        char message[128];
        snprintf(message, sizeof message, "createSystem: scheduling.%s must be a non-negative integer", name);
        tna_throw(env, "invalid", message);
        return false;
    }
    *inout = (uint32_t)number;
    return true;
}

static bool tna_hook_option(napi_env env, napi_value object, const char *name, napi_ref *out) {
    napi_value value = NULL;
    if (!tna_option(env, object, name, &value)) {
        return false;
    }
    if (value == NULL) {
        return true;
    }
    napi_valuetype type = napi_undefined;
    napi_typeof(env, value, &type);
    if (type != napi_function) {
        char message[96];
        snprintf(message, sizeof message, "createSystem: %s must be a function", name);
        tna_throw(env, "invalid", message);
        return false;
    }
    return napi_create_reference(env, value, 1, out) == napi_ok;
}

static bool tna_finished_option(napi_env env, napi_value object, bool *destroy) {
    napi_value value = NULL;
    if (!tna_option(env, object, "finished", &value)) {
        return false;
    }
    if (value == NULL) {
        return true;
    }
    char text[16] = { 0 };
    size_t length = 0;
    if (napi_get_value_string_utf8(env, value, text, sizeof text, &length) != napi_ok ||
        (strcmp(text, "destroy") != 0 && strcmp(text, "retain") != 0)) {
        tna_throw(env, "invalid", "createSystem: finished must be \"destroy\" or \"retain\"");
        return false;
    }
    *destroy = strcmp(text, "destroy") == 0;
    return true;
}

/* ------------------------------------------------------------------------
 * Waking
 */

static napi_value tna_turn_callback(napi_env env, napi_callback_info info);

/* Releases the hold a pending turn has on the wrapper. */
static void tna_pump_unhold(tna_napi_system *wrapper) {
    tna_napi_system_unhold(wrapper);
}

/* The wake callback (system.h): setImmediate(turn). */
static void tna_pump_wake(tna_system *system, void *context) {
    tna_napi_system *wrapper = context;
    napi_env env = wrapper->env;
    napi_value global = NULL;
    napi_value set_immediate = NULL;
    napi_value turn = NULL;
    napi_value ignored = NULL;
    napi_handle_scope scope = NULL;
    bool ok = napi_open_handle_scope(env, &scope) == napi_ok;
    ok = ok && napi_get_global(env, &global) == napi_ok &&
         napi_get_named_property(env, global, "setImmediate", &set_immediate) == napi_ok &&
         napi_get_reference_value(env, wrapper->turn_function, &turn) == napi_ok && turn != NULL;
    if (ok) {
        wrapper->holders += 1u; /* the pending turn holds the wrapper */
        if (napi_call_function(env, global, set_immediate, 1, &turn, &ignored) != napi_ok) {
            wrapper->holders -= 1u;
            ok = false;
        }
    }
    if (!ok) {
        system->turn_pending = false; /* a later request tries again */
    }
    if (scope != NULL) {
        napi_close_handle_scope(env, scope);
    }
}

bool tna_pump_init(napi_env env, tna_napi_system *wrapper, napi_value facade, napi_value options) {
    wrapper->env = env;
    wrapper->destroy_finished = true;
    napi_valuetype type = napi_undefined;
    if (options != NULL) {
        napi_typeof(env, options, &type);
        if (type == napi_undefined || type == napi_null) {
            options = NULL;
        } else if (type != napi_object) {
            tna_throw(env, "invalid", "createSystem options must be an object");
            return false;
        }
    }
    napi_value scheduling = NULL;
    tna_turn_budgets *budgets = &wrapper->system->budgets;
    if (!tna_option(env, options, "scheduling", &scheduling) ||
        !tna_count_option(env, scheduling, "resumeBudget", &budgets->microsteps) ||
        !tna_count_option(env, scheduling, "actionBudget", &budgets->actions) ||
        !tna_count_option(env, scheduling, "resumesPerTurn", &budgets->resumes) ||
        !tna_finished_option(env, options, &wrapper->destroy_finished) ||
        !tna_hook_option(env, options, "done", &wrapper->done_hook) ||
        !tna_hook_option(env, options, "fault", &wrapper->fault_hook)) {
        return false;
    }
    if (budgets->microsteps == 0 || budgets->resumes == 0) {
        tna_throw(env, "invalid", "createSystem: scheduling.resumeBudget and resumesPerTurn must be at least 1");
        return false;
    }
    napi_value turn = NULL;
    if (napi_create_function(env, "tinyactorsTurn", NAPI_AUTO_LENGTH, tna_turn_callback, wrapper, &turn) != napi_ok ||
        napi_create_reference(env, turn, 1, &wrapper->turn_function) != napi_ok ||
        (facade != NULL && napi_create_reference(env, facade, 0, &wrapper->facade) != napi_ok)) {
        tna_throw(env, "out-of-memory", "cannot set up the pump");
        return false;
    }
    wrapper->system->wake = tna_pump_wake;
    wrapper->system->wake_context = wrapper;
    return true;
}

/* ------------------------------------------------------------------------
 * Hooks
 */

/* The ActorId view of `actor` (ADR 0002), or NULL without a class or facade. */
static napi_value tna_actor_view(napi_env env, tna_napi_system *wrapper, ta_actor_id actor) {
    tna_env *data = wrapper->env_data;
    napi_value actor_class = NULL;
    napi_value argv[3] = { NULL, NULL, NULL };
    napi_value view = NULL;
    if (data->actor_class == NULL || wrapper->facade == NULL ||
        napi_get_reference_value(env, data->actor_class, &actor_class) != napi_ok || actor_class == NULL ||
        napi_get_reference_value(env, wrapper->facade, &argv[0]) != napi_ok || argv[0] == NULL ||
        napi_create_uint32(env, ta_actor_id_slot(actor), &argv[1]) != napi_ok ||
        napi_create_uint32(env, ta_actor_id_generation(actor), &argv[2]) != napi_ok ||
        napi_new_instance(env, actor_class, 3, argv, &view) != napi_ok) {
        return NULL;
    }
    return view;
}

/* The author's ID of the top-level final the actor reached, or undefined. */
static napi_value tna_final_state(napi_env env, tna_system *system, ta_actor_id actor) {
    napi_value result = NULL;
    napi_get_undefined(env, &result);
    ta_actor_inspection view;
    if (ta_actor_inspect(system->core, actor, &view) != TA_OK || view.final_state == TA_STATE_NONE) {
        return result;
    }
    ta_state_info info;
    if (ta_definition_state(system->core, &(ta_definition_state_params){
                                              .definition = view.definition_id,
                                              .state = view.final_state,
                                              .out = &info,
                                          }) != TA_OK) {
        return result;
    }
    napi_create_string_utf8(env, info.id.ptr, info.id.len, &result);
    return result;
}

static napi_status tna_set(napi_env env, napi_value object, const char *name, napi_value value) {
    return value == NULL ? napi_ok : napi_set_named_property(env, object, name, value);
}

/*
 * Calls `hook` with `argument`. An exception is kept as the turn's first
 * one, if it is the first, and cleared, so the turn goes on.
 */
static void tna_call_hook(napi_env env, napi_ref hook, napi_value argument, napi_value *first_error) {
    napi_value function = NULL;
    napi_value global = NULL;
    napi_value ignored = NULL;
    if (napi_get_reference_value(env, hook, &function) != napi_ok || function == NULL ||
        napi_get_global(env, &global) != napi_ok) {
        return;
    }
    if (napi_call_function(env, global, function, 1, &argument, &ignored) != napi_ok) {
        napi_value error = NULL;
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
            napi_get_and_clear_last_exception(env, &error) == napi_ok && *first_error == NULL) {
            *first_error = error;
        }
    }
}

static void tna_destroy_finished(tna_system *system, ta_actor_id actor) {
    system->inside_core = true;
    ta_result result = ta_actor_destroy(system->core, actor);
    system->inside_core = false;
    if (result == TA_OK || result == TA_ERR_NOT_FOUND) {
        tna_sched_remove(&system->sched, actor, TNA_DEAD_DONE);
    }
}

static void tna_done_record(napi_env env, tna_napi_system *wrapper, const tna_turn_record *record, napi_value *first_error) {
    tna_system *system = wrapper->system;
    if (wrapper->done_hook != NULL && tna_sched_lookup(&system->sched, record->actor) != NULL) {
        napi_value view = tna_actor_view(env, wrapper, record->actor);
        napi_value argument = NULL;
        napi_value data = NULL;
        napi_get_undefined(env, &data); /* completion data: step 11 */
        if (view != NULL && napi_create_object(env, &argument) == napi_ok &&
            tna_set(env, argument, "actor", view) == napi_ok &&
            tna_set(env, argument, "finalState", tna_final_state(env, system, record->actor)) == napi_ok &&
            tna_set(env, argument, "data", data) == napi_ok) {
            tna_call_hook(env, wrapper->done_hook, argument, first_error);
        }
    }
    /* The hook may have closed the system or destroyed the actor itself. */
    if (wrapper->destroy_finished && !tna_system_closed(system) &&
        tna_sched_lookup(&system->sched, record->actor) != NULL) {
        tna_destroy_finished(system, record->actor);
    }
}

static void tna_fault_record(napi_env env, tna_napi_system *wrapper, const tna_turn_record *record, napi_value *first_error) {
    if (wrapper->fault_hook == NULL) {
        return;
    }
    const char *code = tna_result_code(record->code);
    char message[160];
    snprintf(message, sizeof message, "%s failed: %s", record->operation, code);
    napi_value argument = NULL;
    napi_value operation = NULL;
    napi_value code_value = NULL;
    napi_value message_value = NULL;
    napi_value view = tna_actor_view(env, wrapper, record->actor);
    if (view != NULL && napi_create_object(env, &argument) == napi_ok &&
        napi_create_string_utf8(env, record->operation, NAPI_AUTO_LENGTH, &operation) == napi_ok &&
        napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) == napi_ok &&
        napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) == napi_ok &&
        tna_set(env, argument, "actor", view) == napi_ok && tna_set(env, argument, "operation", operation) == napi_ok &&
        tna_set(env, argument, "code", code_value) == napi_ok &&
        tna_set(env, argument, "message", message_value) == napi_ok) {
        tna_call_hook(env, wrapper->fault_hook, argument, first_error);
    }
}

/* ------------------------------------------------------------------------
 * Flush waiters
 */

static bool tna_add_waiter(tna_napi_system *wrapper, napi_deferred deferred) {
    if (wrapper->waiter_count == wrapper->waiter_capacity) {
        uint32_t capacity = wrapper->waiter_capacity == 0 ? 4u : wrapper->waiter_capacity * 2u;
        napi_deferred *grown = realloc(wrapper->waiters, capacity * sizeof *grown);
        if (grown == NULL) {
            return false;
        }
        wrapper->waiters = grown;
        wrapper->waiter_capacity = capacity;
    }
    wrapper->waiters[wrapper->waiter_count] = deferred;
    wrapper->waiter_count += 1u;
    return true;
}

static void tna_resolve_waiters(napi_env env, tna_napi_system *wrapper) {
    napi_value undefined = NULL;
    napi_get_undefined(env, &undefined);
    /* A waiter's continuation runs later (a microtask), so the list is stable here. */
    uint32_t count = wrapper->waiter_count;
    wrapper->waiter_count = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        napi_resolve_deferred(env, wrapper->waiters[index], undefined);
    }
}

static bool tna_settled(const tna_system *system) {
    return tna_system_closed(system) || (tna_system_quiet(system) && !system->turn_pending);
}

napi_value tna_pump_flush(napi_env env, tna_napi_system *wrapper) {
    napi_deferred deferred = NULL;
    napi_value promise = NULL;
    if (napi_create_promise(env, &deferred, &promise) != napi_ok) {
        return tna_throw(env, "out-of-memory", "cannot create a promise");
    }
    if (tna_settled(wrapper->system)) {
        napi_value undefined = NULL;
        napi_get_undefined(env, &undefined);
        napi_resolve_deferred(env, deferred, undefined);
        return promise;
    }
    if (!tna_add_waiter(wrapper, deferred)) {
        napi_reject_deferred(env, deferred, tna_error(env, "out-of-memory", "cannot wait for the system"));
    }
    return promise;
}

/* ------------------------------------------------------------------------
 * The turn
 */

static napi_value tna_turn_callback(napi_env env, napi_callback_info info) {
    void *data = NULL;
    if (napi_get_cb_info(env, info, NULL, NULL, NULL, &data) != napi_ok || data == NULL) {
        return NULL;
    }
    tna_napi_system *wrapper = data;
    tna_system *system = wrapper->system;
    napi_value first_error = NULL;
    if (!tna_system_closed(system)) {
        tna_system_turn(system);
        for (const tna_turn_record *record = system->sched.records; record != NULL; record = record->next) {
            if (tna_system_closed(system)) {
                break; /* a hook closed the system */
            }
            if (record->kind == TNA_RECORD_DONE) {
                tna_done_record(env, wrapper, record, &first_error);
            } else if (record->kind == TNA_RECORD_FAULT) {
                tna_fault_record(env, wrapper, record, &first_error);
            }
        }
        tna_system_end_turn(system);
        if (tna_settled(system)) {
            tna_resolve_waiters(env, wrapper);
        }
    }
    tna_pump_unhold(wrapper);
    if (first_error != NULL) {
        napi_throw(env, first_error);
    }
    return NULL;
}

/* ------------------------------------------------------------------------
 * Close
 */

static void tna_delete(napi_env env, napi_ref *ref, bool env_usable) {
    if (*ref != NULL && env_usable) {
        napi_delete_reference(env, *ref);
    }
    *ref = NULL;
}

void tna_pump_close(tna_napi_system *wrapper, bool env_usable) {
    napi_env env = wrapper->env;
    if (env == NULL) {
        return;
    }
    wrapper->system->wake = NULL;
    if (env_usable && wrapper->waiter_count > 0) {
        napi_value error = tna_error(env, "closed", "the system was closed");
        for (uint32_t index = 0; index < wrapper->waiter_count; index += 1) {
            napi_reject_deferred(env, wrapper->waiters[index], error);
        }
    }
    /* Waiters that cannot be settled (finalizer, teardown) are abandoned. */
    wrapper->waiter_count = 0;
    free(wrapper->waiters);
    wrapper->waiters = NULL;
    wrapper->waiter_capacity = 0;
    bool refs_usable = !wrapper->env_data->torn_down;
    tna_delete(env, &wrapper->turn_function, refs_usable);
    tna_delete(env, &wrapper->facade, refs_usable);
    tna_delete(env, &wrapper->done_hook, refs_usable);
    tna_delete(env, &wrapper->fault_hook, refs_usable);
}
