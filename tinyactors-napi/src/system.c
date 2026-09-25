/* clock_gettime and CLOCK_MONOTONIC under -std=c11. */
#if !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "system.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
#include <sys/random.h>
#endif

#include "adapters/invoker.h"
#include "adapters/io.h"
#include "adapters/log.h"
#include "router.h"

ta_result tna_system_create(tna_system **out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    *out = NULL;
    tna_system *system = malloc(sizeof *system);
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    *system = (tna_system){
        .budgets = {
            .resumes = TNA_DEFAULT_RESUMES_PER_TURN,
            .messages = TNA_DEFAULT_MESSAGE_BUDGET,
            .microsteps = TNA_DEFAULT_RESUME_BUDGET,
            .actions = TNA_DEFAULT_ACTION_BUDGET,
        },
        .due_budget = TNA_DEFAULT_DUE_BUDGET,
        .clock = { .kind = TNA_CLOCK_REALTIME, .now = tna_monotonic_ns },
    };
    system->clock.origin = system->clock.now(system->clock.now_context);
    tna_allocator_init(&system->allocator);
    tna_sched_init(&system->sched, tna_allocator_for(&system->allocator, TNA_MEMORY_SCHEDULING),
                   tna_allocator_for(&system->allocator, TNA_MEMORY_ENVELOPES));
    tna_directory_init(&system->directory, tna_allocator_for(&system->allocator, TNA_MEMORY_DIRECTORY));
    tna_trace_watch_init(&system->trace_watch, tna_allocator_for(&system->allocator, TNA_MEMORY_OTHER));
    tna_vec_init(&system->io_processors, tna_allocator_for(&system->allocator, TNA_MEMORY_OTHER),
                 TNA_LAYOUT(tna_io_processor *));
    ta_result result = tna_scratch_init(&system->scratch, &system->allocator);
    if (result != TA_OK) {
        free(system);
        return result;
    }
    tna_effects_init(&system->effects, &system->scratch.turn);
    ta_system_config config = {
        .allocator = tna_allocator_for(&system->allocator, TNA_MEMORY_CORE),
        .initial_time = 0,
    };
    result = ta_system_create(&config, &system->core);
    if (result != TA_OK) {
        tna_scratch_destroy(&system->scratch);
        free(system);
        return result;
    }
    (void)ta_system_set_actor_directory(system->core, tna_directory_adapter(&system->directory));
    (void)ta_system_set_external_event_router(system->core, tna_router_adapter(system));
    result = tna_scxml_invoker_register(system);
    if (result != TA_OK) {
        ta_system_destroy(system->core);
        tna_scratch_destroy(&system->scratch);
        free(system);
        return result;
    }
    *out = system;
    return TA_OK;
}

void tna_system_close(tna_system *system) {
    if (system == NULL || system->closed) {
        return;
    }
    system->closed = true;
    system->turn_pending = false;
    /*
     * Nothing may reach the plumbing while core tears down, and every
     * reference the addon holds to a core object (a named origin in an
     * envelope, an address in the directory) goes back first (ADR 0012).
     */
    (void)ta_system_set_external_event_router(system->core, (ta_external_event_router){0});
    (void)ta_system_set_actor_directory(system->core, (ta_actor_directory){0});
    tna_sched_discard_mail(&system->sched);
    tna_directory_clear(&system->directory);
    /* Waiting effects hold values and addresses; what core's teardown asks
     * of a service from here on is dropped (the adapters in adapters/). */
    tna_effects_close(&system->effects);
    if (system->tracing) {
        tna_otlp_destroy(&system->otlp);
        system->tracing = false;
    }
    ta_system_destroy(system->core);
    system->core = NULL;
    /* Core held the I/O processor records as adapters until now. */
    tna_io_close(system);
    tna_sched_destroy(&system->sched);
    tna_trace_watch_destroy(&system->trace_watch);
    if (system->after_core != NULL) {
        system->after_core(system->after_core_context);
    }
    tna_scratch_destroy(&system->scratch);
    system->leaked_bytes = tna_allocator_total(&system->allocator).bytes_in_use;
}

bool tna_system_closed(const tna_system *system) {
    return system == NULL || system->closed;
}

size_t tna_system_leaked_bytes(const tna_system *system) {
    return system == NULL ? 0 : system->leaked_bytes;
}

void tna_system_free(tna_system *system) {
    if (system == NULL) {
        return;
    }
    tna_system_close(system);
    free(system);
}

/* Something is due at core's current time. */
static bool tna_system_due_now(const tna_system *system) {
    ta_time deadline = 0;
    return system->core != NULL && ta_system_next_deadline(system->core, &deadline) == TA_OK &&
           deadline <= ta_system_time(system->core);
}

bool tna_system_quiet(const tna_system *system) {
    return tna_sched_quiet(&system->sched) && system->effects.count == 0 &&
           (!system->tracing || system->otlp.span_count == 0) &&
           !tna_system_due_now(system);
}

void tna_system_trace(tna_system *system, const ta_trace_record *record) {
    if (!system->tracing) {
        return;
    }
    uint32_t before = system->otlp.span_count;
    tna_otlp_write(&system->otlp, record);
    if (system->otlp.span_count != before) {
        tna_system_want_turn(system);
    }
}

uint64_t tna_system_wall_ns(void) {
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

ta_result tna_system_refresh_log_sink(tna_system *system) {
    bool wanted = system->log_to_js || (system->tracing && system->otlp.config.log_limit != 0);
    return ta_system_set_log_sink(system->core, wanted ? tna_log_sink_adapter(system) : (ta_log_sink){ 0 });
}

uint64_t tna_system_os_seed(tna_system *system) {
    uint64_t seed = 0;
    (void)system;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(&seed, sizeof seed);
#else
    if (getrandom(&seed, sizeof seed, 0) != (ssize_t)sizeof seed) {
        /* No entropy: trace IDs only have to be unlikely to collide. */
        seed = tna_system_wall_ns() ^ (uint64_t)(uintptr_t)system;
    }
#endif
    return seed;
}

void tna_system_start_trace(tna_system *system, ta_trace_context *context) {
    if (!system->tracing || context->trace_id_high != 0 || context->trace_id_low != 0) {
        return;
    }
    uint8_t detail = context->detail;
    *context = ta_system_new_trace(system->core);
    context->detail = detail;
}

bool tna_system_effect(tna_system *system, const tna_effect *effect) {
    bool queued = tna_effects_push(&system->effects, effect);
    if (queued) {
        tna_system_want_turn(system);
    }
    return queued;
}

uint64_t tna_monotonic_ns(void *context) {
    (void)context;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

ta_result tna_system_set_clock(tna_system *system, tna_clock_kind kind, ta_time initial) {
    if (system == NULL || system->closed || ta_system_time(system->core) != 0) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_system_set_time(system->core, initial);
    if (result != TA_OK) {
        return result;
    }
    system->clock.kind = kind;
    system->clock.initial = initial;
    system->clock.origin = system->clock.now(system->clock.now_context);
    return TA_OK;
}

void tna_system_sync_time(tna_system *system) {
    if (system == NULL || system->closed || system->clock.kind != TNA_CLOCK_REALTIME || system->inside_core) {
        return;
    }
    uint64_t now = system->clock.now(system->clock.now_context);
    uint64_t elapsed = now > system->clock.origin ? (now - system->clock.origin) / 1000000u : 0;
    ta_time time = system->clock.initial + (ta_time)elapsed;
    if (time > ta_system_time(system->core)) {
        (void)ta_system_set_time(system->core, time);
    }
}

ta_result tna_system_advance_to(tna_system *system, ta_time time) {
    if (system == NULL || system->closed || system->clock.kind != TNA_CLOCK_MANUAL) {
        return TA_ERR_INVALID;
    }
    if (system->inside_core) {
        return TA_ERR_BUSY;
    }
    ta_result result = ta_system_set_time(system->core, time);
    if (result == TA_OK) {
        tna_system_want_turn(system);
    }
    return result;
}

/*
 * Realtime: arms the host timer for the next deadline, when there is one in
 * the future that is earlier than the armed one (ADR 0007 point 3). A
 * deadline that is due already needs a turn, not a timer.
 */
static void tna_system_arm(tna_system *system) {
    tna_clock *clock = &system->clock;
    ta_time deadline = 0;
    if (system->closed || clock->kind != TNA_CLOCK_REALTIME || clock->arm == NULL ||
        ta_system_next_deadline(system->core, &deadline) != TA_OK) {
        return;
    }
    tna_system_sync_time(system);
    ta_time now = ta_system_time(system->core);
    if (deadline <= now) {
        tna_system_want_turn(system);
        return;
    }
    if (clock->armed && clock->armed_deadline <= deadline) {
        return;
    }
    clock->armed = true;
    clock->armed_deadline = deadline;
    clock->arm(system, deadline - now, system->wake_context);
}

void tna_system_timer_fired(tna_system *system) {
    if (system == NULL || system->closed) {
        return;
    }
    system->clock.armed = false;
    tna_system_sync_time(system);
    tna_system_want_turn(system);
    tna_system_arm(system);
}

void tna_system_want_turn(tna_system *system) {
    if (system == NULL || system->closed || system->turn_pending || tna_system_quiet(system)) {
        return;
    }
    system->turn_pending = true;
    if (system->wake != NULL) {
        system->wake(system, system->wake_context);
    }
}

void tna_system_turn(tna_system *system) {
    system->turn_pending = false;
    if (system->closed) {
        return;
    }
    tna_system_sync_time(system);
    tna_sched_turn(&system->sched, &(tna_turn_params){
                                       .core = system->core,
                                       .arena = &system->scratch.turn,
                                       .inside_core = &system->inside_core,
                                       .budgets = system->budgets,
                                       .watch = &system->trace_watch,
                                   });
    if (tna_system_due_now(system)) {
        /*
         * Due sends go through the router into mailboxes (ADR 0006), so what
         * they deliver runs in a later turn. A refusal (a full queue) or
         * TA_ERR_NOMEM leaves the rest due, so the system is not quiet and
         * the next turn tries again.
         */
        uint32_t dispatched = 0;
        bool more_due = false;
        system->inside_core = true;
        (void)ta_system_dispatch_due(system->core, &(ta_due_dispatch_params){
                                                       .budget = system->due_budget,
                                                       .dispatched = &dispatched,
                                                       .more_due = &more_due,
                                                   });
        system->inside_core = false;
    }
}

void tna_system_end_turn(tna_system *system) {
    if (system->closed) {
        return;
    }
    system->sched.records = NULL;
    system->sched.last_record = NULL;
    system->sched.record_count = 0;
    tna_scratch_reset_turn(&system->scratch);
    tna_system_want_turn(system);
    tna_system_arm(system);
}

ta_result tna_system_post(tna_system *system, const tna_message *message) {
    if (system == NULL || system->closed) {
        return TA_ERR_INVALID;
    }
    ta_result result = tna_sched_post(&system->sched, message);
    if (result == TA_OK) {
        tna_system_want_turn(system);
    }
    return result;
}
