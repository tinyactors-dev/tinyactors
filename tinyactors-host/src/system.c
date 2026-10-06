#include "tinyactors_host/system.h"

#include "memory.h"

#include "tinyactors_host/adapter_invoker.h"
#include "tinyactors_host/adapter_io.h"
#include "tinyactors_host/adapter_log.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/router.h"
#include "system_private.h"

/* Core's wall clock (core ADR 0098): the platform's, so every tel record
 * carries the time it was written. */
static uint64_t tah_system_core_wall_ns(void *self) {
    const tah_system *system = self;
    return system->platform.v->wall_ns(system->platform.self);
}

static const ta_wall_clock_vtable tah_system_core_clock = { .wall_ns = tah_system_core_wall_ns };

/*
 * The records one microstep writes at the most detail, for sizing tel: the
 * microstep, a guard, an action, and the event it raises.
 */
#define TAH_TEL_RECORDS_PER_MICROSTEP 4u
/* The records a ta_run() call writes besides its microsteps: a macrostep's begin and end. */
#define TAH_TEL_RECORDS_PER_RESUME 2u
/* Records of maxRecordBytes any ring holds when valueBytes makes records
 * larger than core's default: a macrostep writes the event's data twice
 * (MACROSTEP_BEGIN, MICROSTEP). */
#define TAH_TEL_LARGEST_RECORDS 4u
/* telBytes' bound in core (ta_param_info_of). */
#define TAH_TEL_BYTES_MAX 2147483648u

/* Core's parameters as the kernel passes them: at most one of each. */
typedef struct {
    ta_param_value values[TA_PARAM_COUNT];
    uint32_t count;
} tah_core_params;

/* Where `param` is in `out`, added with core's default when it is not there;
 * UINT32_MAX when there is no room (a list with repeats, which core refuses). */
static uint32_t tah_core_param_at(tah_core_params *out, ta_param param, uint64_t default_value) {
    for (uint32_t at = 0; at < out->count; at += 1u) {
        if (out->values[at].param == param) {
            return at;
        }
    }
    if (out->count == (uint32_t)TA_PARAM_COUNT) {
        return UINT32_MAX;
    }
    out->values[out->count] = (ta_param_value){ .param = param, .value = default_value };
    out->count += 1u;
    return out->count - 1u;
}

/*
 * Core's parameters as the binding gave them, with the trace sizes the
 * kernel's own parameters ask for (core ADR 0098), and the pool's size.
 * Each follows the kernel while it is unset or at core's default; one set to
 * anything else stays as given. A binding may pass every parameter,
 * defaults included (napi does), so a value equal to the default counts as
 * unset: an explicit telemetryValueBytes of 1,024 follows valueBytes too.
 *
 * - telemetryValueBytes is valueBytes.
 * - maxRecordBytes leaves that value room beside the rest of a record: at
 *   least telemetryValueBytes + 512.
 * - When the binding may turn the encoder on (`traced`), telBytes holds
 *   one ta_run() call's records, since the kernel reads tel only between calls
 *   (tah_system_staging_full): resumeBudget microsteps of
 *   TAH_TEL_RECORDS_PER_MICROSTEP records, and a macrostep's begin and end,
 *   each of maxRecordBytes, at most core's bound. The encoder is turned on
 *   after creation, if at all, and core sizes its ring at creation, so
 *   such a system gets that ring whether or not it traces; its pages are
 *   touched only as records are written. A system that is not `traced`
 *   keeps core's 16 KiB, for whoever reads tel some other way, or four of
 *   the largest records when valueBytes makes them larger than core's
 *   default, so that telBytes stays at least maxRecordBytes.
 * - poolEnvelopes and nameAreaBytes are core's largest (core ADR 0099): a
 *   binding over the kernel admits what its host posts up to the pool's
 *   watermark, and the pool fills on demand, so a system pays for its
 *   peak, not for the bound; a binding that wants a smaller pool sets
 *   them.
 *
 * Answers the list to give core: `out`'s, or the binding's own list when it
 * is longer than any list core accepts, which core then refuses.
 */
static const ta_param_value *tah_system_core_params(const tah_system_create_params *params, const tah_params *hosting,
                                                    tah_core_params *out) {
    out->count = params->param_count;
    if (params->param_count > (uint32_t)TA_PARAM_COUNT || (params->param_count != 0 && params->params == NULL)) {
        return params->params;
    }
    for (uint32_t at = 0; at < params->param_count; at += 1u) {
        out->values[at] = params->params[at];
    }
    uint32_t value_at = tah_core_param_at(out, TA_PARAM_TELEMETRY_VALUE_BYTES, TA_PARAM_DEFAULT_TELEMETRY_VALUE_BYTES);
    uint32_t record_at = tah_core_param_at(out, TA_PARAM_MAX_RECORD_BYTES, TA_PARAM_DEFAULT_MAX_RECORD_BYTES);
    uint32_t ring_at = tah_core_param_at(out, TA_PARAM_TEL_BYTES, TA_PARAM_DEFAULT_TEL_BYTES);
    if (value_at == UINT32_MAX || record_at == UINT32_MAX || ring_at == UINT32_MAX) {
        return out->values; /* a list with repeats, which core refuses as it is */
    }
    ta_param_info pool_info = { 0 };
    ta_param_info names_info = { 0 };
    (void)ta_param_info_of(TA_PARAM_POOL_ENVELOPES, &pool_info);
    (void)ta_param_info_of(TA_PARAM_NAME_AREA_BYTES, &names_info);
    uint32_t pool_at = tah_core_param_at(out, TA_PARAM_POOL_ENVELOPES, TA_PARAM_DEFAULT_POOL_ENVELOPES);
    uint32_t names_at = tah_core_param_at(out, TA_PARAM_NAME_AREA_BYTES, TA_PARAM_DEFAULT_NAME_AREA_BYTES);
    if (pool_at == UINT32_MAX || names_at == UINT32_MAX) {
        return out->values;
    }
    if (out->values[pool_at].value == TA_PARAM_DEFAULT_POOL_ENVELOPES) {
        out->values[pool_at].value = pool_info.max;
    }
    if (out->values[names_at].value == TA_PARAM_DEFAULT_NAME_AREA_BYTES) {
        out->values[names_at].value = names_info.max;
    }
    ta_param_value *value = &out->values[value_at];
    ta_param_value *record = &out->values[record_at];
    ta_param_value *ring = &out->values[ring_at];
    if (value->value == TA_PARAM_DEFAULT_TELEMETRY_VALUE_BYTES) {
        value->value = hosting->values[TAH_PARAM_VALUE_BYTES];
    }
    uint64_t wanted = (value->value + 512u + 7u) & ~(uint64_t)7u;
    if (record->value == TA_PARAM_DEFAULT_MAX_RECORD_BYTES && wanted > record->value) {
        record->value = wanted;
    }
    uint64_t record_bytes = record->value & ~(uint64_t)7u;
    uint64_t ring_bytes = 0;
    if (record_bytes > TA_PARAM_DEFAULT_MAX_RECORD_BYTES) {
        ring_bytes = TAH_TEL_LARGEST_RECORDS * record_bytes;
    }
    if (params->traced) {
        uint64_t records = hosting->values[TAH_PARAM_RESUME_BUDGET] * TAH_TEL_RECORDS_PER_MICROSTEP +
                           TAH_TEL_RECORDS_PER_RESUME;
        ring_bytes = records * record_bytes > ring_bytes ? records * record_bytes : ring_bytes;
    }
    ring_bytes = ring_bytes > TAH_TEL_BYTES_MAX ? TAH_TEL_BYTES_MAX : ring_bytes;
    if (ring->value == TA_PARAM_DEFAULT_TEL_BYTES && ring_bytes > ring->value) {
        ring->value = ring_bytes;
    }
    return out->values;
}

/* Gives the record back to the platform's allocator it came from. */
static void tah_system_release_record(tah_system *system) {
    ta_allocator allocator = system->platform.allocator;
    allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = system, .size = sizeof *system,
                                                                    .alignment = _Alignof(tah_system) });
}

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
            .resumes = (uint32_t)napi.values[TAH_PARAM_RESUMES_PER_TURN],
            .messages = (uint32_t)napi.values[TAH_PARAM_MESSAGE_BUDGET],
            .microsteps = (uint32_t)napi.values[TAH_PARAM_RESUME_BUDGET],
            .actions = (uint32_t)napi.values[TAH_PARAM_ACTION_BUDGET],
        },
        .due_budget = (uint32_t)napi.values[TAH_PARAM_DUE_BUDGET],
        .clock = { .kind = TAH_CLOCK_REALTIME, .now = platform->v->monotonic_ns, .now_context = platform->self },
        .platform = *platform,
    };
    system->clock.origin = system->clock.now(system->clock.now_context);
    tah_allocator_init(&system->allocator, platform->allocator);
    tah_sched_init(&system->sched, tah_allocator_for(&system->allocator, TAH_MEMORY_SCHEDULING),
                   tah_allocator_for(&system->allocator, TAH_MEMORY_ENVELOPES));
    system->sched.max_queued = (uint32_t)napi.values[TAH_PARAM_MAX_QUEUED_MESSAGES];
    system->sched.max_retained = napi.values[TAH_PARAM_MAX_RETAINED_BYTES];
    tah_directory_init(&system->directory, tah_allocator_for(&system->allocator, TAH_MEMORY_DIRECTORY));
    tah_loads_init(&system->loads, tah_allocator_for(&system->allocator, TAH_MEMORY_DIRECTORY));
    tah_offloads_init(&system->offloads, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    tah_trace_watch_init(&system->trace_watch, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    tah_definitions_init(&system->definitions, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    tah_vec_init(&system->io_processors, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER),
                 TAH_LAYOUT(tah_io_processor *));
    tah_vec_init(&system->quarantine_reports, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER),
                 TAH_LAYOUT(tah_quarantine_report));
    tah_host_metrics_init(&system->metrics);
    tah_recorder_init(&system->recorder, tah_allocator_for(&system->allocator, TAH_MEMORY_OTHER));
    ta_result result = tah_scratch_init(&system->scratch, &system->allocator);
    if (result != TA_OK) {
        tah_system_release_record(system);
        return result;
    }
    tah_effects_init(&system->effects, &system->scratch.turn);
    tah_core_params core_params;
    const ta_param_value *given = tah_system_core_params(params, &napi, &core_params);
    ta_system_config config = {
        .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
        .initial_time = 0,
        .incarnation = params->incarnation,
        .params = given,
        .param_count = core_params.count,
        .rejected = params->rejected,
    };
    result = ta_system_create(&config, &system->core);
    if (result != TA_OK) {
        tah_scratch_destroy(&system->scratch);
        tah_system_release_record(system);
        return result;
    }
    tah_sched_attach(&system->sched, &(tah_sched_attach_params){ .core = system->core,
                                                                .inside_core = &system->inside_core,
                                                                .watch = &system->trace_watch });
    (void)ta_system_set_wall_clock(system->core, (ta_wall_clock){ .self = system, .v = &tah_system_core_clock });
    (void)ta_system_set_actor_directory(system->core, tah_directory_adapter(&system->directory));
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
    (void)ta_system_set_actor_directory(system->core, (ta_actor_directory){0});
    /* A hook may close the system while the turn's records still own data. */
    tah_sched_release_records(&system->sched);
    tah_sched_discard_mail(&system->sched);
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
    tao_destroy(system->otlp);
    system->otlp = NULL;
    ta_system_destroy(system->core);
    system->core = NULL;
    /* Core held the I/O processor records as adapters until now. */
    tah_io_close(system);
    tah_sched_destroy(&system->sched);
    tah_vec_destroy(&system->quarantine_reports);
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

tao_encoder *tah_system_otlp(tah_system *system) {
    return system->otlp;
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

bool tah_system_inside_core(const tah_system *system) {
    return system->inside_core;
}

void tah_system_enter_core(tah_system *system) {
    system->inside_core = true;
}

void tah_system_leave_core(tah_system *system) {
    system->inside_core = false;
    tah_system_drain_telemetry(system);
}

bool tah_system_tracing(const tah_system *system) {
    return system->otlp != NULL;
}

void tah_system_set_otlp(tah_system *system, tao_encoder *encoder) {
    if (system->otlp != encoder) {
        tao_destroy(system->otlp);
    }
    system->otlp = encoder;
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

/* Whether core's tel ring holds nothing the encoder has not read. */
static bool tah_system_tel_empty(const tah_system *system) {
    ta_tel_status status = { 0 };
    return system->core == NULL || ta_system_tel_status(system->core, &status) != TA_OK || status.used == 0;
}

bool tah_system_quiet(const tah_system *system) {
    return tah_sched_quiet(&system->sched) && system->effects.count == 0 &&
           system->recorder.records.count == 0 && system->recorder.touched.count == 0 &&
           system->quarantine_reports.count == 0 &&
           tah_loads_pending_requests(&system->loads) == 0 &&
           (system->otlp == NULL || (tao_traces_size(system->otlp) == 0 && tah_system_tel_empty(system))) &&
           !tah_system_due_now(system);
}

void tah_system_drain_telemetry(tah_system *system) {
    if (system == NULL || system->core == NULL || system->otlp == NULL) {
        return;
    }
    if (tao_read_tel(system->otlp, system->core) != 0) {
        tah_system_want_turn(system);
    }
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
    tao_span_ref macrostep;
    if (system->otlp != NULL && tao_take_last_macrostep(system->otlp, &macrostep)) {
        exemplar = tah_exemplar_of(&macrostep.context, macrostep.span_id, macrostep.time_ns);
    }
    tah_histogram_record(&system->metrics.turn_seconds, seconds, &exemplar);
}

void tah_system_wake_sender(tah_system *system, const ta_actor_ref *sender) {
    if (!system->dispatching_due) {
        return;
    }
    ta_actor_id actor = sender->as.runtime;
    if (sender->kind == TA_ACTOR_REF_NAMED) {
        ta_actor_directory_entry entry = tah_directory_find(&system->directory, sender->as.named);
        if (entry.kind != TA_ACTOR_DIRECTORY_RESIDENT) {
            return;
        }
        actor = entry.actor;
    }
    if (tah_sched_wake(&system->sched, actor) == TA_OK) {
        tah_system_want_turn(system);
    }
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
    bool wanted = system->log_to_js || (system->otlp != NULL && tao_logs_enabled(system->otlp));
    return ta_system_set_log_sink(system->core, wanted ? tah_log_sink_adapter(system) : (ta_log_sink){ 0 });
}

uint64_t tah_system_random_seed(tah_system *system) {
    return system->platform.v->random_seed(system->platform.self);
}

void tah_system_start_trace(tah_system *system, ta_trace_context *context) {
    if (system->otlp == NULL || context->trace_id_high != 0 || context->trace_id_low != 0) {
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

/*
 * Queues one report in the parent's mailbox (napi ADR 0025), where it waits
 * for the parent to be between macrosteps (napi ADR 0032 section 3). True
 * when it is finished with: queued, or dropped because the parent is gone
 * (the scheduler drops a report for a gone target). False when it must
 * wait for a later turn: memory ran out, or the mailbox is full.
 */
static bool tah_quarantine_report_try(tah_system *system, tah_quarantine_report *report) {
    ta_event event = { .type = TA_EVENT_PLATFORM, .invoke_id = report->invoke_id };
    if (system->callbacks.quarantine_data != NULL &&
        !system->callbacks.quarantine_data(system->callbacks.context, report, &event.data)) {
        return false;
    }
    ta_result result = tah_sched_post(&system->sched, &(tah_message){ .target = report->parent, .event = &event,
                                                                      .trace = &report->trace,
                                                                      .report = TAH_REPORT_QUARANTINED });
    if (result != TA_OK) {
        ta_value_release(&event.data);
        return false;
    }
    tah_system_want_turn(system);
    return true;
}

/* Tries the reports still waiting, keeping those that must wait again. */
static void tah_quarantine_reports_retry(tah_system *system) {
    tah_vec *reports = &system->quarantine_reports;
    uint32_t kept = 0;
    for (uint32_t index = 0; index < reports->count; index += 1) {
        tah_quarantine_report *report = tah_vec_at(reports, index);
        if (!tah_quarantine_report_try(system, report)) {
            *(tah_quarantine_report *)tah_vec_at(reports, kept) = *report;
            kept += 1u;
        }
    }
    reports->count = kept;
}

/*
 * The turn's quarantines of invoked children tell their parents (napi ADR
 * 0025). The child is inspected now, while it still stands where it
 * faulted; a report that must wait is kept for the next turn.
 */
static void tah_quarantine_reports_make(tah_system *system) {
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        ta_actor_inspection view;
        if (record->kind != TAH_RECORD_FAULT || ta_actor_inspect(system->core, record->actor, &view) != TA_OK ||
            !view.has_parent) {
            continue;
        }
        tah_quarantine_report report = {
            .parent = view.parent.actor,
            .invoke_id = view.parent.invoke_id,
            .trace = view.trace,
            .code = record->code,
            .operation = record->operation,
        };
        if (!tah_quarantine_report_try(system, &report)) {
            /* A push that fails loses the report: the parent is not told,
             * though the fault hook, the failure log and the quarantine
             * gauge still are. */
            (void)tah_vec_push(&system->quarantine_reports, &report);
        }
    }
}

/* Answers every load request core owes (router.h): mail a chart sent to a
 * target that is not resident waits in core until then. */
static void tah_system_answer_loads(tah_system *system) {
    ta_load_request request;
    while (ta_system_load_needed(system->core, &request) == TA_OK) {
        tah_router_answer(system, &request);
        /* What core delivered or recycled runs at its next ta_run(). */
        tah_sched_core_work(&system->sched);
    }
}

/* Sends one ta_system_dispatch_due() call reports at most. */
#define TAH_DUE_FIRED 64u

/*
 * Dispatches the sends due now, up to the due budget. A send to another
 * session goes into core's pool and runs at core's next ta_run(), or waits
 * there for its target's load, which this answers (router.h); any other
 * goes to its I/O processor. For each send that
 * fired, its sender's wake is reported again (the recorder), and, when it
 * was delivered, how late it fired (the lateness histogram). A refusal (no
 * room, a full queue) or TA_ERR_NOMEM leaves the rest due, so the system is
 * not quiet and the next turn tries again. Returns true when sends are
 * still due. The turn calls it after its deliveries, and evict() before it
 * removes an actor (napi ADR 0032 section 8).
 */
bool tah_system_dispatch_due(tah_system *system) {
    ta_time core_deadline = 0;
    if (ta_system_next_deadline(system->core, &core_deadline) != TA_OK ||
        core_deadline > ta_system_time(system->core)) {
        return false;
    }
    bool more_due = false;
    uint32_t left = system->due_budget;
    do {
        ta_due_fired fired[TAH_DUE_FIRED];
        uint32_t fired_count = 0;
        uint32_t dispatched = 0;
        system->inside_core = true;
        system->dispatching_due = true;
        (void)ta_system_dispatch_due(system->core, &(ta_due_dispatch_params){
                                                       .budget = left,
                                                       .dispatched = &dispatched,
                                                       .more_due = &more_due,
                                                       .fired = fired,
                                                       .fired_capacity = TAH_DUE_FIRED,
                                                       .fired_count = &fired_count,
                                                   });
        system->dispatching_due = false;
        system->inside_core = false;
        for (uint32_t at = 0; at < fired_count; at += 1u) {
            tah_recorder_fired(system, &(ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = fired[at].sender });
            if (fired[at].delivered) {
                tah_system_record_lateness(system, fired[at].due, &fired[at].trace);
            }
        }
        if (dispatched != 0) {
            /* What went into core's pool, or an error a sender is told of,
             * runs at its next ta_run(). */
            tah_sched_core_work(&system->sched);
            tah_system_want_turn(system);
        }
        left -= dispatched < left ? dispatched : left;
        if (dispatched == 0 || fired_count < TAH_DUE_FIRED) {
            break;
        }
    } while (left != 0 && more_due);
    tah_system_answer_loads(system);
    tah_system_drain_telemetry(system);
    return more_due;
}

/*
 * A turn stops once it has staged traceStagingBytes of spans and logs (napi
 * ADR 0035): the sink gets this turn's request, and the next turn goes on
 * where this one stopped, so the staging stays near its bound and nothing
 * is dropped. One macrostep's records are never split.
 */
static bool tah_system_staging_full(void *context) {
    tah_system *system = context;
    tah_system_drain_telemetry(system); /* what the ta_run() before wrote */
    return system->otlp != NULL && tao_staging_full(system->otlp);
}

void tah_system_turn(tah_system *system) {
    system->turn_pending = false;
    if (system->closed) {
        return;
    }
    tah_system_sync_time(system);
    tah_sched_turn(&system->sched, &(tah_turn_params){
                                       .arena = &system->scratch.turn,
                                       .budgets = system->budgets,
                                       .stop = tah_system_staging_full,
                                       .stop_context = system,
                                   });
    tah_system_answer_loads(system);
    tah_system_drain_telemetry(system);
    tah_quarantine_reports_retry(system);
    tah_quarantine_reports_make(system);
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
    tah_waiter *waiter = tah_sched_waiter(&system->sched, index);
    if (waiter == NULL) {
        return false;
    }
    bool cancelled = waiter->state == TAH_WAITER_QUEUED &&
                     tah_loads_withdraw(&system->loads, &system->sched, waiter->envelope);
    if (cancelled) {
        waiter->state = TAH_WAITER_SETTLING;
    } else {
        cancelled = tah_sched_cancel(&system->sched, index);
    }
    if (cancelled) {
        tah_system_want_turn(system); /* a withdrawn message makes room the overflow may wait for (kernel ADR 0003) */
    }
    return cancelled;
}

ta_result tah_system_post(tah_system *system, const tah_message *message) {
    if (system == NULL || system->closed) {
        return TA_ERR_INVALID;
    }
    ta_result result = tah_sched_post(&system->sched, message);
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
    system->inside_core = false;
    tah_system_drain_telemetry(system);
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
    system->inside_core = false;
    tah_system_drain_telemetry(system);
    return result == TA_OK ? tah_hook_actor_abandoned(system, actor) : result;
}

ta_result tah_system_finish_load(tah_system *system, const tah_system_load_finish *finish) {
    uint64_t request = tah_loads_core_request(&system->loads, finish->index, finish->generation);
    /* The load's address goes when it finishes: kept for the answer below. */
    tah_load_info info = tah_loads_info(&system->loads, finish->index, finish->generation);
    ta_address *address = (ta_address *)info.address;
    if (address != NULL) {
        ta_address_retain(address);
    }
    ta_result result = tah_loads_finish(&system->loads, &(tah_load_finish){ .sched = &system->sched,
                                                                            .directory = &system->directory,
                                                                            .metrics = &system->metrics,
                                                                            .index = finish->index,
                                                                            .generation = finish->generation,
                                                                            .outcome = finish->outcome,
                                                                            .actor = finish->actor,
                                                                            .now_ns = tah_system_monotonic_ns(system),
                                                                            .wall_ns = tah_system_wall_ns(system) });
    if (result != TA_ERR_STALE && result != TA_ERR_NOT_FOUND && request != 0) {
        /* The mail core kept for the load goes where the load's own went. */
        if (result == TA_OK && (finish->outcome == TAH_LOAD_LOADED || finish->outcome == TAH_LOAD_SPAWNED)) {
            (void)ta_system_load_settled(system->core,
                                         &(ta_load_settled_params){ .request = request, .actor = finish->actor });
        } else {
            ta_actor_ref destination = { .kind = TA_ACTOR_REF_NAMED, .as.named = address };
            tah_router_fail(system, request,
                            &(tah_router_failure){ .reason = tah_load_dead_reason(finish->outcome),
                                                   .destination = &destination });
        }
        tah_sched_core_work(&system->sched);
        tah_system_want_turn(system);
    }
    if (address != NULL) {
        ta_address_release(address);
    }
    return result;
}
