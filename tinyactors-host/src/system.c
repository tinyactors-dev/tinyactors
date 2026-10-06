#include "tinyactors_host/system.h"

#include "memory.h"

#include "tinyactors_host/adapter_invoker.h"
#include "tinyactors_host/adapter_io.h"
#include "tinyactors_host/adapter_log.h"
#include "tinyactors_host/hooks.h"
#include "system_private.h"

/* Gives the record back to the platform's allocator it came from. */
static void tah_system_release_record(tah_system *system) {
    ta_allocator allocator = system->platform.allocator;
    allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = system, .size = sizeof *system,
                                                                    .alignment = _Alignof(tah_system) });
}

/*
 * Core offers the trace parent of mail it runs for the sessions the host
 * watches (watch.h): the same mark a delivery gets before it is admitted.
 */
static void tah_system_mark_input(void *self, ta_actor_id actor, ta_trace_parent *trace) {
    tah_system *system = self;
    tah_trace_watch_mark(&system->trace_watch, system->core, actor, trace);
}

static const ta_input_watch_vtable tah_system_input_watch = { .mark = tah_system_mark_input };

ta_result tah_system_create(const tah_system_create_params *params, tah_system **out) {
    if (params == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    *out = NULL;
    const tah_platform *platform = &params->platform;
    if (platform->v == NULL || platform->allocator.v == NULL) {
        return TA_ERR_INVALID;
    }
    /* The record holds the counting allocator, so it comes from the
     * platform's allocator directly and is not counted (allocator.h). */
    tah_system *system = platform->allocator.v->alloc(platform->allocator.self, sizeof *system, _Alignof(tah_system));
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    tah_params napi;
    if (params->hosting != NULL) {
        napi = *params->hosting;
    } else {
        tah_params_defaults(&napi);
    }
    *system = (tah_system){
        .budgets = {
            .inputs = (uint32_t)napi.values[TAH_PARAM_MESSAGE_BUDGET],
            .microsteps = (uint32_t)napi.values[TAH_PARAM_RESUME_BUDGET],
            .actions = (uint32_t)napi.values[TAH_PARAM_ACTION_BUDGET],
        },
        .due_budget = (uint32_t)napi.values[TAH_PARAM_DUE_BUDGET],
        .clock = { .kind = TAH_CLOCK_REALTIME, .now = platform->v->monotonic_ns, .now_context = platform->self },
        .platform = *platform,
        .params = napi,
    };
    system->clock.origin = system->clock.now(system->clock.now_context);
    tah_allocator_init(&system->allocator, platform->allocator);
    tah_sched_init(&system->sched, tah_allocator_for(&system->allocator, TAH_MEMORY_SCHEDULING),
                   tah_allocator_for(&system->allocator, TAH_MEMORY_ENVELOPES));
    tah_sched_set_limits(&system->sched, (uint32_t)napi.values[TAH_PARAM_MAX_QUEUED_MESSAGES],
                         napi.values[TAH_PARAM_MAX_RETAINED_BYTES]);
    tah_directory_init(&system->directory, tah_allocator_for(&system->allocator, TAH_MEMORY_DIRECTORY));
    tah_loads_init(&system->loads, tah_allocator_for(&system->allocator, TAH_MEMORY_DIRECTORY));
    tah_offloads_init(&system->offloads, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    tah_trace_watch_init(&system->trace_watch, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    tah_definitions_init(&system->definitions, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    tah_vec_init(&system->io_processors, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER),
                 TAH_LAYOUT(tah_io_processor *));
    tah_host_metrics_init(&system->metrics);
    tah_recorder_init(&system->recorder, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    ta_result result = tah_scratch_init(&system->scratch, &system->allocator);
    if (result != TA_OK) {
        tah_system_release_record(system);
        return result;
    }
    tah_effects_init(&system->effects, &system->scratch.turn);
    ta_system_config config = {
        .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
        .initial_time = 0,
        .incarnation = params->incarnation,
        .params = params->params,
        .param_count = params->param_count,
        .rejected = params->rejected,
    };
    result = ta_system_create(&config, &system->core);
    if (result != TA_OK) {
        tah_scratch_destroy(&system->scratch);
        tah_system_release_record(system);
        return result;
    }
    (void)ta_system_set_actor_directory(system->core, tah_directory_adapter(&system->directory));
    (void)ta_system_set_input_watch(system->core, (ta_input_watch){ .self = system, .v = &tah_system_input_watch });
    result = tah_scxml_invoker_register(system);
    if (result != TA_OK) {
        ta_system_destroy(system->core);
        tah_scratch_destroy(&system->scratch);
        tah_system_release_record(system);
        return result;
    }
    *out = system;
    return TA_OK;
}

void tah_system_close(tah_system *system) {
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
    (void)ta_system_set_input_watch(system->core, (ta_input_watch){0});
    (void)ta_system_set_actor_directory(system->core, (ta_actor_directory){0});
    /* A hook may close the system while the turn's records still own data. */
    tah_sched_release_records(&system->sched);
    tah_sched_discard(&system->sched);
    /* Offloaded bytes may hold value handles; they go while the directory still keys them. */
    tah_offloads_destroy(&system->offloads);
    tah_directory_clear(&system->directory);
    tah_loads_close(&system->loads);
    /* Waiting effects hold values and addresses; what core's teardown asks
     * of a service from here on is dropped (the adapters in adapters/). */
    tah_effects_close(&system->effects);
    /* Records hold values; the log goes while core is alive. */
    system->sched.observer = (tah_sched_observer){ 0 };
    tah_recorder_destroy(&system->recorder);
    if (system->tracing) {
        tah_otlp_destroy(&system->otlp);
        system->tracing = false;
    }
    ta_system_destroy(system->core);
    system->core = NULL;
    /* Core held the I/O processor records as adapters until now. */
    tah_io_close(system);
    tah_sched_destroy(&system->sched);
    tah_trace_watch_destroy(&system->trace_watch);
    /* Core freed every definition in its teardown, so the origins are empty. */
    tah_definitions_destroy(&system->definitions);
    if (system->callbacks.after_core != NULL) {
        system->callbacks.after_core(system->callbacks.context);
    }
    tah_scratch_destroy(&system->scratch);
    system->leaked_bytes = tah_allocator_total(&system->allocator).bytes_in_use;
}

bool tah_system_closed(const tah_system *system) {
    return system == NULL || system->closed;
}

ta_system *tah_system_core(tah_system *system) {
    return system->core;
}

tah_allocator *tah_system_allocator(tah_system *system) {
    return &system->allocator;
}

tah_scratch *tah_system_scratch(tah_system *system) {
    return &system->scratch;
}

tah_sched *tah_system_sched(tah_system *system) {
    return &system->sched;
}

tah_directory *tah_system_directory(tah_system *system) {
    return &system->directory;
}

tah_loads *tah_system_loads(tah_system *system) {
    return &system->loads;
}

tah_offloads *tah_system_offloads(tah_system *system) {
    return &system->offloads;
}

tah_effects *tah_system_effects(tah_system *system) {
    return &system->effects;
}

tah_otlp *tah_system_otlp(tah_system *system) {
    return &system->otlp;
}

tah_trace_watch *tah_system_trace_watch(tah_system *system) {
    return &system->trace_watch;
}

tah_definitions *tah_system_definitions(tah_system *system) {
    return &system->definitions;
}

tah_host_metrics *tah_system_metrics(tah_system *system) {
    return &system->metrics;
}


const tah_params *tah_system_params(const tah_system *system) {
    return &system->params;
}

bool tah_system_inside_core(const tah_system *system) {
    return system->inside_core;
}

void tah_system_enter_core(tah_system *system) {
    system->inside_core = true;
}

void tah_system_leave_core(tah_system *system) {
    system->inside_core = false;
    tah_trace_drain(system);
}

bool tah_system_tracing(const tah_system *system) {
    return system->tracing;
}

void tah_system_set_tracing(tah_system *system, bool tracing) {
    system->tracing = tracing;
}

void tah_system_set_log_to_js(tah_system *system, bool log_to_js) {
    system->log_to_js = log_to_js;
}

void tah_system_set_callbacks(tah_system *system, const tah_system_callbacks *callbacks) {
    system->callbacks = callbacks == NULL ? (tah_system_callbacks){ 0 } : *callbacks;
}

void tah_system_set_driver(tah_system *system, const tah_pump_driver *driver) {
    system->driver = driver == NULL ? (tah_pump_driver){ 0 } : *driver;
    if (driver == NULL) {
        system->clock.armed = false;
    }
}

bool tah_system_turn_pending(const tah_system *system) {
    return system->turn_pending;
}

tah_clock_kind tah_system_clock_kind(const tah_system *system) {
    return system->clock.kind;
}

size_t tah_system_leaked_bytes(const tah_system *system) {
    return system == NULL ? 0 : system->leaked_bytes;
}

void tah_system_free(tah_system *system) {
    if (system == NULL) {
        return;
    }
    tah_system_close(system);
    tah_system_release_record(system);
}

/* The earliest of core's next deadline and the next wake-up of an unloaded
 * actor (napi ADR 0020); false when there is neither. */
static bool tah_system_next_deadline(const tah_system *system, ta_time *out) {
    ta_time deadline = 0;
    ta_time wake = 0;
    bool core_due = system->core != NULL && ta_system_next_deadline(system->core, &deadline) == TA_OK;
    bool wake_due = tah_loads_next_wake(&system->loads, &wake);
    if (!core_due && !wake_due) {
        return false;
    }
    *out = !core_due ? wake : !wake_due ? deadline : (wake < deadline ? wake : deadline);
    return true;
}

/* Something is due at core's current time. */
static bool tah_system_due_now(const tah_system *system) {
    ta_time deadline = 0;
    return system->core != NULL && tah_system_next_deadline(system, &deadline) &&
           deadline <= ta_system_time(system->core);
}

bool tah_system_quiet(const tah_system *system) {
    return tah_sched_quiet(&system->sched) && (system->core == NULL || !ta_run_pending(system->core)) &&
           system->effects.count == 0 &&
           system->recorder.records.count == 0 && system->recorder.touched.count == 0 &&
           tah_loads_pending_requests(&system->loads) == 0 &&
           (!system->tracing || system->otlp.span_count == 0) &&
           !tah_system_due_now(system);
}

uint64_t tah_system_wall_ns(const tah_system *system) {
    return system->platform.v->wall_ns(system->platform.self);
}

uint64_t tah_system_monotonic_ns(const tah_system *system) {
    return system->platform.v->monotonic_ns(system->platform.self);
}

double tah_system_overdue_seconds(const tah_system *system) {
    ta_time deadline = 0;
    if (system == NULL || system->closed || ta_system_next_deadline(system->core, &deadline) != TA_OK) {
        return 0.0;
    }
    /* The host's time in core's units, without moving core's clock. */
    ta_time now = ta_system_time(system->core);
    if (system->clock.kind == TAH_CLOCK_REALTIME) {
        uint64_t wall = system->clock.now(system->clock.now_context);
        ta_time host = system->clock.initial +
                       (ta_time)(wall > system->clock.origin ? (wall - system->clock.origin) / 1000000u : 0u);
        now = host > now ? host : now;
    }
    return now > deadline ? (double)(now - deadline) / 1000.0 : 0.0;
}

void tah_system_record_turn(tah_system *system, uint64_t started_ns) {
    uint64_t now = tah_system_monotonic_ns(system);
    double seconds = now > started_ns ? (double)(now - started_ns) / 1e9 : 0.0;
    tah_exemplar exemplar = { 0 };
    if (system->tracing && system->otlp.last_macrostep.set) {
        exemplar = system->otlp.last_macrostep;
        system->otlp.last_macrostep.set = false;
    }
    tah_histogram_record(&system->metrics.turn_seconds, seconds, &exemplar);
}

void tah_system_record_lateness(tah_system *system, ta_time due, const ta_trace_parent *trace) {
    ta_time now = ta_system_time(system->core);
    double seconds = now > due ? (double)(now - due) / 1000.0 : 0.0;
    tah_exemplar exemplar = trace == NULL ? (tah_exemplar){ 0 }
                                          : tah_exemplar_of(&trace->context, ta_step_key_span_id(trace->cause),
                                                            tah_system_wall_ns(system));
    tah_histogram_record(&system->metrics.lateness_seconds, seconds, &exemplar);
}

ta_result tah_system_refresh_log_sink(tah_system *system) {
    bool wanted = system->log_to_js || (system->tracing && system->otlp.config.log_limit != 0);
    return ta_system_set_log_sink(system->core, wanted ? tah_log_sink_adapter(system) : (ta_log_sink){ 0 });
}

uint64_t tah_system_random_seed(tah_system *system) {
    return system->platform.v->random_seed(system->platform.self);
}

void tah_system_start_trace(tah_system *system, ta_trace_context *context) {
    if (!system->tracing || context->trace_id_high != 0 || context->trace_id_low != 0) {
        return;
    }
    uint8_t detail = context->detail;
    *context = ta_system_new_trace(system->core);
    context->detail = detail;
}

bool tah_system_effect(tah_system *system, const tah_effect *effect) {
    bool queued = tah_effects_push(&system->effects, effect);
    if (queued) {
        tah_system_want_turn(system);
    }
    return queued;
}


ta_result tah_system_set_clock(tah_system *system, tah_clock_kind kind, ta_time initial) {
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

void tah_system_sync_time(tah_system *system) {
    if (system == NULL || system->closed || system->clock.kind != TAH_CLOCK_REALTIME || system->inside_core) {
        return;
    }
    uint64_t now = system->clock.now(system->clock.now_context);
    uint64_t elapsed = now > system->clock.origin ? (now - system->clock.origin) / 1000000u : 0;
    ta_time time = system->clock.initial + (ta_time)elapsed;
    if (time > ta_system_time(system->core)) {
        (void)ta_system_set_time(system->core, time);
    }
}

ta_result tah_system_advance_to(tah_system *system, ta_time time) {
    if (system == NULL || system->closed || system->clock.kind != TAH_CLOCK_MANUAL) {
        return TA_ERR_INVALID;
    }
    if (system->inside_core) {
        return TA_ERR_BUSY;
    }
    ta_result result = ta_system_set_time(system->core, time);
    if (result == TA_OK) {
        tah_system_want_turn(system);
    }
    return result;
}

/*
 * Realtime: arms the host timer for the next deadline, when there is one in
 * the future that is earlier than the armed one (ADR 0007 point 3). A
 * deadline that is due already needs a turn, not a timer.
 */
static void tah_system_arm(tah_system *system);

void tah_system_arm_timer(tah_system *system) {
    if (system == NULL || system->closed) {
        return;
    }
    tah_system_want_turn(system);
    tah_system_arm(system);
}

static void tah_system_arm(tah_system *system) {
    tah_clock *clock = &system->clock;
    ta_time deadline = 0;
    if (system->closed || clock->kind != TAH_CLOCK_REALTIME || system->driver.arm == NULL ||
        !tah_system_next_deadline(system, &deadline)) {
        return;
    }
    tah_system_sync_time(system);
    ta_time now = ta_system_time(system->core);
    if (deadline <= now) {
        tah_system_want_turn(system);
        return;
    }
    if (clock->armed && clock->armed_deadline <= deadline) {
        return;
    }
    clock->armed = true;
    clock->armed_deadline = deadline;
    if (!system->driver.arm(system, deadline - now, system->driver.context)) {
        clock->armed = false; /* the next turn arms again */
    }
}

void tah_system_timer_fired(tah_system *system) {
    if (system == NULL || system->closed) {
        return;
    }
    system->clock.armed = false;
    tah_system_sync_time(system);
    tah_system_want_turn(system);
    tah_system_arm(system);
}

void tah_system_want_turn(tah_system *system) {
    if (system == NULL || system->closed || system->turn_pending || tah_system_quiet(system)) {
        return;
    }
    system->turn_pending = true;
    if (system->driver.wake != NULL && !system->driver.wake(system, system->driver.context)) {
        system->turn_pending = false; /* a later request asks again */
    }
}

/* Records one dispatch call keeps for the mail core placed (ta_due_fired); more are left due for the next call. */
#define TAH_DUE_FIRED_BATCH 32u

/*
 * Dispatches the sends due now, up to the due budget. A due send to a
 * resident session is an envelope core made itself (core ADR 0099 section 4);
 * the rest are sends to host I/O processors, which leave core here. What core placed
 * is reported in `fired`, and recorded here: its lateness, and the sender's
 * wake, which moves because its timer was consumed. What was delivered runs
 * in a later turn. A pool at its bound, a full `fired` array, a refusal (a
 * full queue) or TA_ERR_NOMEM leaves the rest due, so the system is not quiet
 * and the next turn tries again. Returns true when sends are still due. The
 * turn calls it after its deliveries, and evict() before it removes an actor
 * (napi ADR 0032 section 8).
 */
bool tah_system_dispatch_due(tah_system *system) {
    ta_time core_deadline = 0;
    if (ta_system_next_deadline(system->core, &core_deadline) != TA_OK ||
        core_deadline > ta_system_time(system->core)) {
        return false;
    }
    uint32_t dispatched = 0;
    bool more_due = false;
    ta_due_fired fired[TAH_DUE_FIRED_BATCH];
    uint32_t fired_count = 0;
    system->inside_core = true;
    system->dispatching_due = true;
    ta_result result = ta_system_dispatch_due(system->core, &(ta_due_dispatch_params){
                                                                .budget = system->due_budget,
                                                                .dispatched = &dispatched,
                                                                .more_due = &more_due,
                                                                .fired = fired,
                                                                .fired_capacity = TAH_DUE_FIRED_BATCH,
                                                                .fired_count = &fired_count,
                                                            });
    system->dispatching_due = false;
    /* Before leaving core: the senders are borrowed addresses, valid only until
     * the trace drain below could run a hook (ta_due_fired.sender). */
    for (uint32_t index = 0; index < fired_count; index += 1u) {
        if (fired[index].delivered) {
            tah_system_record_lateness(system, fired[index].due, &fired[index].trace);
        }
        tah_recorder_fired(system, &fired[index].sender); /* consumed: its wake moves */
    }
    tah_system_leave_core(system);
    return more_due || result != TA_OK;
}

/*
 * Core wants `notice->address` loaded: the first input for an address that no
 * session holds is waiting (core ADR 0099 section 4). A load for it is
 * requested of the host when its family has a loader, and joins the one in
 * flight when there is one. Otherwise nothing would bring the address
 * back, and the answer is at once: core recycles what waits (mail becomes
 * error.communication for its sender, napi ADR 0022). Running out of memory
 * for the load's record is a failed load. Called from the turn, after the
 * call into core that wrote the notice has returned.
 */
static void tah_system_load_needed(void *context, const ta_run_notice *notice) {
    tah_system *system = context;
    ta_result result = tah_loads_wanted(&system->loads, &(tah_load_wanted){ .address = (ta_address *)notice->address,
                                                                            .trace = &notice->trace,
                                                                            .session = notice->session,
                                                                            .now_ns = tah_system_monotonic_ns(system) });
    if (result == TA_OK) {
        return;
    }
    ta_refusal_reason reason = TA_REFUSED_LOAD_FAILED;
    if (result == TA_ERR_NOT_FOUND) {
        ta_actor_directory_entry entry = tah_directory_find(&system->directory, notice->address);
        reason = !notice->session && entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TA_REFUSED_LOAD_UNLOADED
                                                                               : TA_REFUSED_LOAD_NOT_FOUND;
    }
    system->inside_core = true;
    (void)ta_address_load_failed(system->core, &(ta_address_load_failed_params){ .address = notice->address,
                                                                                .reason = reason });
    tah_system_leave_core(system);
}

/*
 * A turn stops once it has staged the encoder's staging bound of spans and
 * logs (napi ADR 0035; the bound is the encoder's setting since core ADR
 * 0098): the sink gets this turn's request, and the next turn goes on where
 * this one stopped, so the staging stays near its bound and nothing is
 * dropped. One macrostep's records are never split.
 */
static bool tah_system_staging_full(void *context) {
    tah_system *system = context;
    return system->tracing && tah_otlp_traces_size(&system->otlp) + tah_otlp_logs_size(&system->otlp) >=
                                  tah_otlp_staging_bytes(&system->otlp);
}

/* The scheduler left core after a resume, a delivery or a report: the tel ring is read before the next call. */
static void tah_system_left_core(void *context) {
    tah_trace_drain(context);
}

tah_turn_params tah_system_turn_params(tah_system *system) {
    return (tah_turn_params){
        .core = system->core,
        .arena = &system->scratch.turn,
        .inside_core = &system->inside_core,
        .budgets = system->budgets,
        .stop = tah_system_staging_full,
        .stop_context = system,
        .left_core = tah_system_left_core,
        .load_needed = tah_system_load_needed,
    };
}

void tah_system_turn(tah_system *system) {
    system->turn_pending = false;
    if (system->closed) {
        return;
    }
    tah_system_sync_time(system);
    tah_turn_params turn = tah_system_turn_params(system);
    tah_sched_turn(&system->sched, &turn);
    /* Unloaded actors whose own timers are due wake through their loader
     * (napi ADR 0020): the request goes to JavaScript after this turn. */
    tah_loads_fire_wakes(&system->loads, &(tah_wake_fire){ .directory = &system->directory,
                                                           .now = ta_system_time(system->core),
                                                           .now_ns = tah_system_monotonic_ns(system) });
    tah_system_dispatch_due(system);
}

void tah_system_end_turn(tah_system *system) {
    if (system->closed) {
        return;
    }
    tah_sched_release_records(&system->sched);
    tah_scratch_reset_turn(&system->scratch);
    tah_system_want_turn(system);
    tah_system_arm(system);
}

bool tah_system_cancel(tah_system *system, uint32_t index) {
    if (system == NULL || system->closed) {
        return false;
    }
    bool cancelled = tah_sched_cancel(&system->sched, system->core, index);
    if (cancelled) {
        tah_system_want_turn(system); /* a withdrawn input is released where the run queue reaches it */
    }
    return cancelled;
}

bool tah_system_reentrant(const tah_system *system) {
    return system->inside_core && !ta_run_executing(system->core) && !system->dispatching_due;
}

ta_result tah_system_post(tah_system *system, const tah_message *message) {
    if (system == NULL || system->closed) {
        return TA_ERR_INVALID;
    }
    if (tah_system_reentrant(system)) {
        return TA_ERR_BUSY;
    }
    /* The family's limit on waiting messages counts only for a name: a post by actor ID never waits. */
    uint32_t max_waiting = message->address != NULL ? tah_loads_limit(&system->loads, message->address) : 0;
    ta_result result = tah_sched_post(&system->sched, &(tah_post){ .core = system->core,
                                                                   .message = message,
                                                                   .time = ta_system_time(system->core),
                                                                   .max_waiting = max_waiting });
    if (result == TA_OK) {
        tah_system_want_turn(system);
    }
    return result;
}

/* The scheduler's observer: the recorder. */
static void tah_system_note(void *self, const tah_sched_note *note) {
    tah_recorder_note(self, note);
}

void tah_system_observe(tah_system *system) {
    system->sched.observer = system->recorder.enabled
                                 ? (tah_sched_observer){ .self = system, .note = tah_system_note }
                                 : (tah_sched_observer){ 0 };
}

ta_result tah_system_withdraw_send(tah_system *system, ta_actor_id actor, ta_send_id send) {
    system->inside_core = true;
    ta_result result = ta_actor_withdraw_send(system->core, actor, send);
    tah_system_leave_core(system);
    if (result == TA_OK) {
        /* Its wake may move (kernel ADR 0006 section 2). */
        ta_actor_inspection view;
        if (ta_actor_inspect(system->core, actor, &view) == TA_OK) {
            tah_recorder_fired(system, &view.self);
        }
    }
    return result;
}

ta_result tah_system_abandon(tah_system *system, ta_actor_id actor) {
    system->inside_core = true;
    ta_result result = ta_actor_abandon(system->core, actor);
    tah_system_leave_core(system);
    return result == TA_OK ? tah_hook_actor_abandoned(system, actor) : result;
}
