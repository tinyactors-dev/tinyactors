/* loop.c — the queue loop on the runtime's event loop (bindings.md,
 * section 3): advance the clocks, run, hand each effect to its handler,
 * ack, and by the run's status run again, give the event loop a turn, or
 * arm one timer for the next deadline. The global timers (setTimeout,
 * setImmediate and their clear functions) are called here only (the lint
 * rule one-timer-in-loop). And SimulatedClock: a caller's own clock, which
 * arms no timer (section 3; ROADMAP.md, M1). */
#include "addon.h"

#include <stdlib.h>
#include <string.h>

const napi_type_tag ta_napi_clock_tag = {0x7461636c6f636b4eu, 0x41504963636c6f63u};

/* Fuel per ta_host_run: how much work one macrotask may do before the event
 * loop gets a turn (bindings.md, section 3, "Bound each turn"): I/O
 * behaviour of this binding's loop, not a policy of core's
 * (docs/decisions.md, "The run budget"). */
#define TA_NAPI_RUN_BUDGET 100000u

/* A simulated clock and the hosts it drives. */
struct ta_napi_clock {
    double milliseconds;
    ta_napi_host **hosts;
    uint32_t count, capacity;
};

/* ---- the runtime's globals ------------------------------------------------------------ */

/* globalThis[name](argv...) for the timers; NULL when it threw. */
typedef struct ta_napi_global_call {
    const char *name;
    size_t argc;
    const napi_value *argv;
} ta_napi_global_call;

static napi_value ta_napi_global(napi_env env, const ta_napi_global_call *call) {
    napi_value global, function, result = NULL;
    if (napi_get_global(env, &global) != napi_ok ||
        napi_get_named_property(env, global, call->name, &function) != napi_ok ||
        napi_call_function(env, global, function, call->argc, call->argv, &result) != napi_ok)
        return NULL;
    return result;
}

/* performance.now() and Date.now(), in milliseconds. */
static ta_napi_time ta_napi_runtime_now(napi_env env) {
    ta_napi_time now = {0, 0};
    napi_value global, object, function, result;
    double value = 0;
    napi_get_global(env, &global);
    if (napi_get_named_property(env, global, "performance", &object) == napi_ok &&
        napi_get_named_property(env, object, "now", &function) == napi_ok &&
        napi_call_function(env, object, function, 0, NULL, &result) == napi_ok &&
        napi_get_value_double(env, result, &value) == napi_ok)
        now.milliseconds = value;
    if (napi_get_named_property(env, global, "Date", &object) == napi_ok &&
        napi_get_named_property(env, object, "now", &function) == napi_ok &&
        napi_call_function(env, object, function, 0, NULL, &result) == napi_ok &&
        napi_get_value_double(env, result, &value) == napi_ok && value > 0)
        now.wall_ns = (uint64_t)value * 1000000u;
    return now;
}

ta_napi_time ta_napi_loop_now(ta_napi_host *host) {
    if (!host->clock) return ta_napi_runtime_now(host->env);
    ta_napi_time now = {host->clock->milliseconds, (uint64_t)(host->clock->milliseconds * 1e6)};
    return now;
}

/* Milliseconds as the host's ticks (host.config's ticksPerSecond). */
static ta_time ta_napi_ticks(const ta_napi_host *host, double milliseconds) {
    double ticks = milliseconds * (double)host->ticks_per_second / 1000.0;
    return ticks > 0 ? (ta_time)ticks : 0;
}

/* Ticks as milliseconds, rounded up: a timer that fires early runs nothing. */
static double ta_napi_milliseconds(const ta_napi_host *host, ta_time ticks) {
    double milliseconds = (double)ticks * 1000.0 / (double)host->ticks_per_second;
    double whole = (double)(uint64_t)milliseconds;
    return whole < milliseconds ? whole + 1 : whole;
}

/* ---- arming ----------------------------------------------------------------------------- */

/* While a timer or an immediate is armed, its callback needs the host: the
 * Host object is held then, so the garbage collector cannot free it under
 * a pending deadline (bun collected an unreachable Host with a pending
 * delayed send, and the timer then ran on freed memory: test/rules.test.mjs,
 * "a pending deadline keeps the process alive"). */
static void ta_napi_hold(ta_napi_host *host) {
    int armed = host->timer || host->immediate;
    uint32_t count = 0;
    if (armed && !host->held && host->self && napi_reference_ref(host->env, host->self, &count) == napi_ok)
        host->held = 1;
    else if (!armed && host->held && host->self && napi_reference_unref(host->env, host->self, &count) == napi_ok)
        host->held = 0;
}

static void ta_napi_clear(napi_env env, napi_ref *reference, const char *clear) {
    if (!*reference) return;
    napi_value handle;
    if (napi_get_reference_value(env, *reference, &handle) == napi_ok && handle) {
        ta_napi_global_call call = {clear, 1, &handle};
        ta_napi_global(env, &call);
    }
    napi_delete_reference(env, *reference);
    *reference = NULL;
}

void ta_napi_loop_disarm(ta_napi_host *host) {
    ta_napi_clear(host->env, &host->timer, "clearTimeout");
    ta_napi_clear(host->env, &host->immediate, "clearImmediate");
    ta_napi_hold(host);
}

static napi_value ta_napi_loop_wake(napi_env env, napi_callback_info info);

/* The function the timer and the immediate call, made once per host. */
static napi_value ta_napi_wake_function(ta_napi_host *host) {
    napi_env env = host->env;
    napi_value function = NULL;
    if (host->wake && napi_get_reference_value(env, host->wake, &function) == napi_ok && function) return function;
    if (napi_create_function(env, "wake", NAPI_AUTO_LENGTH, ta_napi_loop_wake, host, &function) != napi_ok)
        return NULL;
    napi_create_reference(env, function, 1, &host->wake);
    return function;
}

/* Give the event loop a turn, then run again (a yielded run, an input). */
static void ta_napi_arm_immediate(ta_napi_host *host) {
    if (host->immediate || host->clock) return;
    napi_value function = ta_napi_wake_function(host);
    if (!function) return;
    ta_napi_global_call call = {"setImmediate", 1, &function};
    napi_value handle = ta_napi_global(host->env, &call);
    if (handle) napi_create_reference(host->env, handle, 1, &host->immediate);
    ta_napi_hold(host);
}

/* One timer at the next deadline (UINT64_MAX: none), ref'd while it is
 * pending: a pending deadline keeps the process alive (bindings.md,
 * section 11). */
static void ta_napi_arm_timer(ta_napi_host *host, ta_time deadline, ta_time now) {
    if (host->clock) return;
    if (host->timer && host->timer_deadline == deadline) return;
    ta_napi_clear(host->env, &host->timer, "clearTimeout");
    if (deadline == UINT64_MAX) {
        ta_napi_hold(host);
        return;
    }
    napi_value argv[2];
    argv[0] = ta_napi_wake_function(host);
    if (!argv[0]) return;
    napi_create_double(host->env, deadline > now ? ta_napi_milliseconds(host, deadline - now) : 0, &argv[1]);
    ta_napi_global_call call = {"setTimeout", 2, argv};
    napi_value handle = ta_napi_global(host->env, &call);
    if (handle) napi_create_reference(host->env, handle, 1, &host->timer);
    host->timer_deadline = deadline;
    ta_napi_hold(host);
}

/* ---- the turn ------------------------------------------------------------------------------ */

/* The first exception a handler threw: kept, and thrown when the turn ends. */
static void ta_napi_keep_exception(ta_napi_host *host) {
    napi_value thrown;
    if (!ta_napi_exception_pending(host->env) || napi_get_and_clear_last_exception(host->env, &thrown) != napi_ok)
        return;
    if (!host->error) napi_create_reference(host->env, thrown, 1, &host->error);
}

static void ta_napi_advance(ta_napi_host *host, ta_napi_time now) {
    ta_host_time time = {ta_napi_ticks(host, now.milliseconds), now.wall_ns};
    ta_host_advance_time(host->host, time);
}

void ta_napi_loop_turn(ta_napi_host *host) {
    if (host->closed) return;
    if (host->running) {
        host->again = 1;
        return;
    }
    host->running = 1;
    ta_napi_clear(host->env, &host->immediate, "clearImmediate");
    ta_napi_time now = ta_napi_loop_now(host);
    ta_napi_advance(host, now);
    ta_run run;
    for (;;) {
        host->again = 0;
        run = ta_host_run(host->host, TA_NAPI_RUN_BUDGET);
        const ta_host_effect *effects = NULL;
        uint32_t count = ta_host_effects(host->host, &effects);
        for (uint32_t i = 0; i < count; i++) {
            ta_napi_effect_handle(host, i);
            ta_napi_keep_exception(host);
        }
        ta_host_effects_ack(host->host);
        ta_napi_datamodel_drain(host->native); /* the run and the ack returned */
        if (host->closing) break;
        if (run.status == TA_RUN_EFFECTS_FULL || run.status == TA_RUN_EFFECTS) continue;
        if (run.status == TA_RUN_YIELDED) {
            if (host->clock) continue; /* a simulated clock runs until idle */
            ta_napi_arm_immediate(host);
            break;
        }
        if (run.status == TA_RUN_IDLE && host->again) {
            ta_napi_advance(host, ta_napi_loop_now(host));
            continue;
        }
        break;
    }
    host->running = 0;
    if (host->closing) {
        ta_napi_loop_disarm(host);
        ta_napi_host_release(host);
    } else if (run.status == TA_RUN_IDLE) {
        ta_napi_arm_timer(host, run.next_deadline, ta_napi_ticks(host, now.milliseconds));
    } else if (run.status == TA_RUN_EXHAUSTED || run.status == TA_RUN_FAULT) {
        /* the allocator refused, or a segment could not run: report it */
        if (!host->error) ta_napi_throw_status(host->env, "ta_host_run", run.status == TA_RUN_EXHAUSTED ? TA_ERROR_NO_MEMORY : TA_ERROR_STATE);
        return;
    }
    if (host->error) {
        napi_value thrown;
        napi_ref error = host->error;
        host->error = NULL;
        if (napi_get_reference_value(host->env, error, &thrown) == napi_ok) napi_throw(host->env, thrown);
        napi_delete_reference(host->env, error);
    }
}

void ta_napi_loop_input(ta_napi_host *host) {
    if (!host->running) ta_napi_datamodel_drain(host->native); /* the input's call into core returned */
    if (host->running) {
        host->again = 1;
    } else if (host->clock) {
        ta_napi_loop_turn(host);
    } else {
        ta_napi_arm_immediate(host);
    }
}

/* The timer or the immediate fired. */
static napi_value ta_napi_loop_wake(napi_env env, napi_callback_info info) {
    void *data = NULL;
    if (napi_get_cb_info(env, info, NULL, NULL, NULL, &data) != napi_ok) return NULL;
    ta_napi_host *host = (ta_napi_host *)data;
    if (host->timer) {
        napi_delete_reference(env, host->timer);
        host->timer = NULL;
    }
    if (host->immediate) {
        napi_delete_reference(env, host->immediate);
        host->immediate = NULL;
    }
    ta_napi_loop_turn(host);
    if (!host->closed) ta_napi_hold(host); /* nothing armed: the Host may go */
    return NULL;
}

/* An input that depends on the time (spawn, deliver): the clocks first
 * (bindings.md, section 7). */
void ta_napi_loop_advance(ta_napi_host *host) {
    if (!host->running) ta_napi_advance(host, ta_napi_loop_now(host));
}

/* ---- SimulatedClock ------------------------------------------------------------------------- */

ta_status ta_napi_clock_add(ta_napi_clock *clock, ta_napi_host *host) {
    if (clock->count == clock->capacity) {
        uint32_t capacity = clock->capacity ? clock->capacity * 2 : 4;
        ta_napi_host **hosts = (ta_napi_host **)realloc(clock->hosts, capacity * sizeof *hosts);
        if (!hosts) return TA_ERROR_NO_MEMORY;
        clock->hosts = hosts;
        clock->capacity = capacity;
    }
    clock->hosts[clock->count++] = host;
    return TA_OK;
}

void ta_napi_clock_forget(ta_napi_clock *clock, ta_napi_host *host) {
    for (uint32_t i = 0; i < clock->count; i++) {
        if (clock->hosts[i] != host) continue;
        clock->hosts[i] = clock->hosts[--clock->count];
        return;
    }
}

static void ta_napi_clock_finalize(node_api_basic_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    ta_napi_clock *clock = (ta_napi_clock *)data;
    for (uint32_t i = 0; i < clock->count; i++)
        clock->hosts[i]->clock = NULL;
    free(clock->hosts);
    free(clock);
}

static napi_value ta_napi_clock_new(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    if (!ta_napi_call_read(env, info, &call)) return NULL;
    ta_napi_clock *clock = (ta_napi_clock *)calloc(1, sizeof *clock);
    if (!clock) {
        ta_napi_throw_status(env, "SimulatedClock", TA_ERROR_NO_MEMORY);
        return NULL;
    }
    if (napi_wrap(env, call.self, clock, ta_napi_clock_finalize, NULL, NULL) != napi_ok) {
        free(clock);
        return NULL;
    }
    napi_type_tag_object(env, call.self, &ta_napi_clock_tag);
    return call.self;
}

static ta_napi_clock *ta_napi_clock_this(napi_env env, napi_callback_info info, ta_napi_call *call) {
    bool tagged = false;
    void *clock = NULL;
    if (!ta_napi_call_read(env, info, call)) return NULL;
    if (napi_check_object_type_tag(env, call->self, &ta_napi_clock_tag, &tagged) != napi_ok || !tagged ||
        napi_unwrap(env, call->self, &clock) != napi_ok) {
        ta_napi_throw_status(env, "SimulatedClock", TA_ERROR_INVALID);
        return NULL;
    }
    return (ta_napi_clock *)clock;
}

/* increment(ms): the time moves on, and each host it drives runs the turn
 * until it is idle (ta_host_advance_time, ta_host_run). */
static napi_value ta_napi_clock_increment(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_clock *clock = ta_napi_clock_this(env, info, &call);
    double milliseconds = 0;
    if (!clock) return NULL;
    if (napi_get_value_double(env, call.argv[0], &milliseconds) != napi_ok || !(milliseconds >= 0)) {
        ta_napi_throw_status(env, "SimulatedClock.increment", TA_ERROR_INVALID);
        return NULL;
    }
    clock->milliseconds += milliseconds;
    for (uint32_t i = 0; i < clock->count && !ta_napi_exception_pending(env); i++)
        ta_napi_loop_turn(clock->hosts[i]);
    return NULL;
}

/* now(): the simulated time, in milliseconds. */
static napi_value ta_napi_clock_now(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_clock *clock = ta_napi_clock_this(env, info, &call);
    napi_value result = NULL;
    if (clock) napi_create_double(env, clock->milliseconds, &result);
    return result;
}

napi_value ta_napi_clock_class(napi_env env) {
    napi_property_descriptor methods[] = {
        {"increment", NULL, ta_napi_clock_increment, NULL, NULL, NULL, napi_default_method, NULL},
        {"now", NULL, ta_napi_clock_now, NULL, NULL, NULL, napi_default_method, NULL},
    };
    napi_value class = NULL;
    napi_define_class(env, "SimulatedClock", NAPI_AUTO_LENGTH, ta_napi_clock_new, NULL,
                      sizeof methods / sizeof methods[0], methods, &class);
    return class;
}
