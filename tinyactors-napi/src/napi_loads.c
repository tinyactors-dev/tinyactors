#include "napi_loads.h"

#include <stdio.h>
#include <string.h>

#include "hooks.h"
#include "loads.h"
#include "napi_trace.h"
#include "pump.h"
#include "runtime.h"

static napi_value tna_undefined_value(napi_env env) {
    napi_value out = NULL;
    napi_get_undefined(env, &out);
    return out;
}

static napi_status tna_put_text(napi_env env, napi_value object, const char *name, ta_str text) {
    napi_value value = NULL;
    napi_status status = napi_create_string_utf8(env, text.ptr, text.len, &value);
    return status == napi_ok ? napi_set_named_property(env, object, name, value) : status;
}

static napi_status tna_put_number(napi_env env, napi_value object, const char *name, double number) {
    napi_value value = NULL;
    napi_status status = napi_create_double(env, number, &value);
    return status == napi_ok ? napi_set_named_property(env, object, name, value) : status;
}

void tna_loads_dispatch(napi_env env, tna_napi_system *wrapper, napi_value *first_error) {
    tah_system *system = wrapper->system;
    if (tah_loads_pending_requests(&system->loads) == 0) {
        return;
    }
    napi_value requests = NULL;
    if (napi_create_array(env, &requests) != napi_ok) {
        return;
    }
    uint32_t count = 0;
    tah_load_request request;
    while (tah_loads_next_request(&system->loads, &request)) {
        ta_actor_address address = ta_address_view(request.address);
        napi_value item = NULL;
        napi_value why = NULL;
        if (napi_create_object(env, &item) != napi_ok || tna_put_number(env, item, "index", request.index) != napi_ok ||
            tna_put_number(env, item, "generation", request.generation) != napi_ok ||
            tna_put_text(env, item, "family", address.family.name) != napi_ok ||
            tna_put_text(env, item, "name", address.name) != napi_ok ||
            napi_create_string_utf8(env,
                                    request.why == TAH_LOAD_WHY_TIMER     ? "timer"
                                    : request.why == TAH_LOAD_WHY_SESSION ? "session"
                                                                          : "mail",
                                    NAPI_AUTO_LENGTH, &why) != napi_ok ||
            napi_set_named_property(env, item, "why", why) != napi_ok ||
            napi_set_element(env, requests, count, item) != napi_ok) {
            continue;
        }
        count += 1u;
    }
    /* The environment's effect handler, with the System from its weak
     * reference: holding a function that closes over the System would keep
     * it from ever being finalized. */
    tna_env *data = wrapper->env_data;
    napi_value handler = NULL;
    napi_value argv[3] = { NULL, NULL, requests };
    if (count == 0 || data->effects_handler == NULL || wrapper->facade == NULL ||
        napi_get_reference_value(env, data->effects_handler, &handler) != napi_ok || handler == NULL ||
        napi_get_reference_value(env, wrapper->facade, &argv[0]) != napi_ok || argv[0] == NULL ||
        napi_create_string_utf8(env, "loads", NAPI_AUTO_LENGTH, &argv[1]) != napi_ok) {
        return;
    }
    napi_value global = NULL;
    napi_value ignored = NULL;
    napi_get_global(env, &global);
    if (napi_call_function(env, global, handler, 3, argv, &ignored) != napi_ok) {
        napi_value error = NULL;
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
            napi_get_and_clear_last_exception(env, &error) == napi_ok && *first_error == NULL) {
            *first_error = error;
        }
    }
}

napi_value tna_set_loader_family_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_str family = { 0 };
    uint32_t max_waiting = 0;
    if (argc < 2 || !tna_text_argument(env, wrapper->system, argv[0], &family) || family.len == 0 ||
        napi_get_value_uint32(env, argv[1], &max_waiting) != napi_ok) {
        return tna_throw(env, "invalid", "setLoader: expected a family name and maxWaiting");
    }
    ta_result result = tah_loads_set_family(&wrapper->system->loads, family, max_waiting);
    tah_scratch_reset_call(&wrapper->system->scratch);
    if (result != TA_OK) {
        return tna_throw(env, "out-of-memory", "setLoader: no memory for the family");
    }
    return tna_undefined_value(env);
}

/* A load's token: its index and generation, as JavaScript holds them. */
static bool tna_load_token(napi_env env, const napi_value *argv, uint32_t *index, uint32_t *generation) {
    return napi_get_value_uint32(env, argv[0], index) == napi_ok &&
           napi_get_value_uint32(env, argv[1], generation) == napi_ok;
}

static const struct {
    const char *name;
    tah_load_outcome outcome;
} tna_load_outcomes[] = {
    { "loaded", TAH_LOAD_LOADED },       { "spawned", TAH_LOAD_SPAWNED }, { "not-found", TAH_LOAD_NOT_FOUND },
    { "rejected", TAH_LOAD_REJECTED },   { "failed", TAH_LOAD_FAILED },   { "timed-out", TAH_LOAD_TIMED_OUT },
    { "done", TAH_LOAD_DONE },
};

/* The WARN log of a load that failed or timed out (napi ADR 0020 section 7). */
static void tna_load_log(tah_system *system, const tah_load *load, tah_load_outcome outcome, ta_str detail) {
    ta_actor_address address = ta_address_view(load->address);
    char body[512];
    int length = snprintf(body, sizeof body, "%s %.*s/%.*s: %u message(s) dropped%s%.*s",
                          outcome == TAH_LOAD_TIMED_OUT ? "load timed out for" : "load failed for",
                          (int)address.family.name.len, address.family.name.ptr, (int)address.name.len,
                          address.name.ptr, (unsigned)load->mail.count, detail.len != 0 ? ": " : "",
                          (int)detail.len, detail.ptr);
    const char *event_name = outcome == TAH_LOAD_TIMED_OUT ? "tinyactors.load.timed_out" : "tinyactors.load.failed";
    tna_pump_host_log(system, 0,
                      (tah_otlp_host_log_params){
                          .event_name = { event_name, (uint32_t)strlen(event_name) },
                          .body = { body, length > 0 ? (uint32_t)(length < (int)sizeof body ? length : (int)sizeof body - 1)
                                                     : 0u },
                          .key = { "tinyactors.load.outcome", 23 },
                          .value = { tah_load_outcome_name(outcome), (uint32_t)strlen(tah_load_outcome_name(outcome)) },
                      });
}

static napi_value tna_complete_load_inner(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    uint32_t index = 0;
    uint32_t generation = 0;
    ta_str outcome_text = { 0 };
    if (argc < 3 || !tna_load_token(env, argv, &index, &generation) ||
        !tna_text_argument(env, system, argv[2], &outcome_text)) {
        return tna_throw(env, "invalid", "completeLoad: expected (index, generation, outcome)");
    }
    tah_load_outcome outcome = TAH_LOAD_OUTCOMES;
    for (size_t at = 0; at < sizeof tna_load_outcomes / sizeof tna_load_outcomes[0]; at += 1u) {
        if (strlen(tna_load_outcomes[at].name) == outcome_text.len &&
            memcmp(tna_load_outcomes[at].name, outcome_text.ptr, outcome_text.len) == 0) {
            outcome = tna_load_outcomes[at].outcome;
        }
    }
    ta_actor_id actor = 0;
    if (outcome == TAH_LOAD_LOADED || outcome == TAH_LOAD_SPAWNED) {
        uint32_t slot = 0;
        uint32_t actor_generation = 0;
        if (argc < 5 || napi_get_value_uint32(env, argv[3], &slot) != napi_ok ||
            napi_get_value_uint32(env, argv[4], &actor_generation) != napi_ok) {
            tah_scratch_reset_call(&system->scratch);
            return tna_throw(env, "invalid", "completeLoad: a loaded or spawned outcome needs the actor");
        }
        actor = ta_actor_id_make(slot, actor_generation);
    }
    ta_str detail = { 0 };
    if (argc >= 6 && !tna_text_argument(env, system, argv[5], &detail)) {
        detail = (ta_str){ 0 };
    }
    if (outcome == TAH_LOAD_OUTCOMES) {
        tah_scratch_reset_call(&system->scratch);
        return tna_throw(env, "invalid", "completeLoad: unknown outcome");
    }
    const tah_load *load = tah_loads_find(&system->loads, index, generation);
    if (load != NULL && (outcome == TAH_LOAD_FAILED || outcome == TAH_LOAD_TIMED_OUT)) {
        tna_load_log(system, load, outcome, detail);
    }
    ta_result result = tah_loads_finish(&system->loads, &(tah_load_finish){ .sched = &system->sched,
                                                                            .directory = &system->directory,
                                                                            .metrics = &system->metrics,
                                                                            .index = index,
                                                                            .generation = generation,
                                                                            .outcome = outcome,
                                                                            .actor = actor,
                                                                            .now_ns = tah_system_monotonic_ns(system),
                                                                            .wall_ns = tah_system_wall_ns(system) });
    tah_scratch_reset_call(&system->scratch);
    if (result == TA_ERR_NOT_FOUND) {
        return tna_throw(env, "invalid", "completeLoad: the actor is not resident (at the load's address)");
    }
    tah_system_want_turn(system);
    tna_pump_settle_waiters(env, wrapper);
    napi_value out = NULL;
    napi_get_boolean(env, result != TA_ERR_STALE, &out);
    return out;
}

napi_value tna_load_trace_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    uint32_t index = 0;
    uint32_t generation = 0;
    if (argc < 2 || !tna_load_token(env, argv, &index, &generation)) {
        return tna_throw(env, "invalid", "loadTrace: expected (index, generation)");
    }
    const tah_load *load = tah_loads_find(&wrapper->system->loads, index, generation);
    if (load == NULL) {
        return tna_undefined_value(env);
    }
    return tna_trace_parent_object(env, &load->trace);
}

/* The interned address of (family, name) arguments; NULL after throwing. */
static ta_address *tna_address_arguments(napi_env env, tna_napi_system *wrapper, const napi_value *argv,
                                         const char *what) {
    tah_system *system = wrapper->system;
    if (system->inside_core) {
        tna_throw(env, "reentrant", "an address cannot be interned from inside a core call (ADR 0013)");
        return NULL;
    }
    ta_actor_address text = { 0 };
    ta_address *address = NULL;
    bool ok = tna_text_argument(env, system, argv[0], &text.family.name) &&
              tna_text_argument(env, system, argv[1], &text.name);
    ta_result result = ok ? ta_address_intern(system->core, &text, &address) : TA_ERR_INVALID;
    tah_scratch_reset_call(&system->scratch);
    if (result != TA_OK) {
        char message[96];
        snprintf(message, sizeof message, "%s: expected (family, name) with non-empty text", what);
        tna_throw(env, result == TA_ERR_INVALID ? "invalid" : tna_result_code(result), message);
        return NULL;
    }
    return address;
}

napi_value tna_wake_at_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    double time = -1;
    if (argc < 3 || napi_get_value_double(env, argv[2], &time) != napi_ok || !(time >= 0) ||
        time > 9007199254740991.0 || time != (double)(ta_time)time) {
        return tna_throw(env, "invalid", "wakeAt: the time must be a non-negative integer number of milliseconds");
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "wakeAt");
    if (address == NULL) {
        return NULL;
    }
    ta_result result = tah_loads_wake_at(&wrapper->system->loads, address, (ta_time)time);
    ta_address_release(address);
    if (result != TA_OK) {
        return tna_throw(env, "out-of-memory", "wakeAt: no memory for the wake-up");
    }
    tah_system_arm_timer(wrapper->system);
    return tna_undefined_value(env);
}

napi_value tna_cancel_wake_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "cancelWake");
    if (address == NULL) {
        return NULL;
    }
    tah_loads_cancel_wake(&wrapper->system->loads, address);
    ta_address_release(address);
    return tna_undefined_value(env);
}

/* ------------------------------------------------------------------------
 * Offloaded actors (core ADR 0074): unload() writes a stable named actor
 * to bytes in the system's offload store, and a load reads it back. The
 * data model decides what each value's bytes are (napi_datamodel.c).
 */

/* setOffloadValues(serialize, deserialize, byHandle) */
napi_value tna_set_offload_values_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    bool by_handle = false;
    if (argc >= 3) {
        napi_get_value_bool(env, argv[2], &by_handle);
    }
    napi_valuetype first = napi_undefined;
    napi_valuetype second = napi_undefined;
    bool functions = argc >= 2 && napi_typeof(env, argv[0], &first) == napi_ok &&
                     napi_typeof(env, argv[1], &second) == napi_ok && first == napi_function &&
                     second == napi_function;
    if (!tna_js_datamodel_set_offload(&wrapper->dm, &(tna_offload_values){
                                                        .serialize = functions ? argv[0] : NULL,
                                                        .deserialize = functions ? argv[1] : NULL,
                                                        .by_handle = by_handle || !functions,
                                                    })) {
        return tna_throw(env, "out-of-memory", "setOffloadValues: cannot keep the functions");
    }
    return tna_undefined_value(env);
}

/* setDurableCodec(chunks, reader): lib/durable.js's two functions (napi ADR 0034). */
napi_value tna_set_durable_codec_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    if (argc < 2 || !tna_js_datamodel_set_durable(&wrapper->dm, argv[0], argv[1])) {
        return tna_throw(env, "out-of-memory", "setDurableCodec: cannot keep the functions");
    }
    return tna_undefined_value(env);
}

/*
 * A byte sink that gathers into memory, and remembers its largest piece;
 * and a source that hands bytes back in pieces of a chosen size. Internal:
 * the tests read a value's durable bytes through them (core ADR 0090).
 */
typedef struct {
    tah_allocator *allocator;
    uint8_t *bytes;
    size_t len;
    size_t cap;
    size_t largest;
} tna_gather_sink;

static ta_op_status tna_gather_begin(void *self) {
    ((tna_gather_sink *)self)->len = 0;
    return TA_OP_OK;
}

static ta_op_status tna_gather_write(void *self, ta_bytes bytes) {
    tna_gather_sink *sink = self;
    ta_allocator allocator = tah_allocator_for(sink->allocator, TAH_MEMORY_OTHER);
    if (sink->len + bytes.len > sink->cap) {
        size_t cap = sink->cap == 0 ? 4096u : sink->cap;
        while (cap < sink->len + bytes.len) {
            cap *= 2u;
        }
        uint8_t *grown = tah_alloc(allocator, cap, 1);
        if (grown == NULL) {
            return TA_OP_NOMEM;
        }
        if (sink->len != 0) {
            memcpy(grown, sink->bytes, sink->len);
        }
        if (sink->bytes != NULL) {
            tah_free(allocator, &(ta_allocator_free_params){ .ptr = sink->bytes, .size = sink->cap, .alignment = 1 });
        }
        sink->bytes = grown;
        sink->cap = cap;
    }
    if (bytes.len != 0) {
        memcpy(sink->bytes + sink->len, bytes.ptr, bytes.len);
    }
    sink->len += bytes.len;
    sink->largest = bytes.len > sink->largest ? bytes.len : sink->largest;
    return TA_OP_OK;
}

static void tna_gather_end(void *self, bool keep) {
    if (!keep) {
        ((tna_gather_sink *)self)->len = 0;
    }
}

static const ta_byte_sink_vtable tna_gather_vtable = {
    .begin = tna_gather_begin, .write = tna_gather_write, .end = tna_gather_end,
};

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t at;
    uint32_t piece;
} tna_piece_source;

static ta_op_status tna_piece_read(void *self, const ta_byte_read_params *params) {
    tna_piece_source *source = self;
    size_t count = source->len - source->at;
    count = count < params->capacity ? count : params->capacity;
    count = source->piece != 0 && count > source->piece ? source->piece : count;
    if (count != 0) {
        memcpy(params->buffer, source->bytes + source->at, count);
    }
    source->at += count;
    *params->got = (uint32_t)count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable tna_piece_vtable = { .read = tna_piece_read };

static napi_value tna_codec_refusal(napi_env env, const ta_fault *fault, ta_op_status status) {
    if (status == TA_OP_NOMEM) {
        return tna_throw(env, "out-of-memory", "the value's bytes could not be kept");
    }
    char message[256];
    size_t length = fault->message.len < sizeof message - 1u ? fault->message.len : sizeof message - 1u;
    memcpy(message, fault->message.ptr, length);
    message[length] = 0;
    return tna_throw(env, "unsupported", message);
}

/* durableBytes(value, durable) -> { bytes, largest }: the value's bytes and its largest piece. */
napi_value tna_durable_bytes_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    bool durable = true;
    if (argc >= 2) {
        napi_get_value_bool(env, argv[1], &durable);
    }
    ta_value value = { 0 };
    if (!tna_value_make(&wrapper->values, argc >= 1 ? argv[0] : tna_undefined_value(env), &value)) {
        return tna_throw(env, "out-of-memory", "durableBytes: cannot keep the value");
    }
    tna_gather_sink state = { .allocator = &wrapper->system->allocator };
    ta_byte_sink sink = { .self = &state, .v = &tna_gather_vtable };
    ta_fault fault = { 0 };
    sink.v->begin(sink.self);
    tna_js_datamodel_offload_begin(&wrapper->dm);
    ta_op_status status = tna_js_datamodel_value_write(&wrapper->dm, &(ta_datamodel_value_write_params){
                                                                          .value = &value, .sink = sink,
                                                                          .durable = durable, .fault = &fault });
    tna_offload_handles handles = tna_js_datamodel_offload_end(&wrapper->dm);
    tna_js_datamodel_release_handles(&wrapper->dm, handles);
    sink.v->end(sink.self, status == TA_OP_OK);
    ta_value_release(&value);
    napi_value result = NULL;
    if (status != TA_OP_OK) {
        result = tna_codec_refusal(env, &fault, status);
    } else {
        napi_value buffer = NULL;
        napi_value largest = NULL;
        void *data = NULL;
        napi_create_object(env, &result);
        napi_create_buffer_copy(env, state.len, state.len == 0 ? (const void *)"" : state.bytes, &data, &buffer);
        napi_create_double(env, (double)state.largest, &largest);
        napi_set_named_property(env, result, "bytes", buffer);
        napi_set_named_property(env, result, "largest", largest);
    }
    ta_value_release(&fault.data);
    if (state.bytes != NULL) {
        tah_free(tah_allocator_for(state.allocator, TAH_MEMORY_OTHER),
                 &(ta_allocator_free_params){ .ptr = state.bytes, .size = state.cap, .alignment = 1 });
    }
    return result;
}

/* durableValue(bytes, piece): the value read back from `bytes`, handed out `piece` bytes at a time. */
napi_value tna_durable_value_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    void *data = NULL;
    size_t length = 0;
    uint32_t piece = 0;
    if (argc < 1 || napi_get_buffer_info(env, argv[0], &data, &length) != napi_ok) {
        return tna_throw(env, "invalid", "durableValue: expected a Buffer");
    }
    if (argc >= 2) {
        napi_get_value_uint32(env, argv[1], &piece);
    }
    tna_piece_source state = { .bytes = data, .len = length, .piece = piece };
    ta_value value = { 0 };
    ta_fault fault = { 0 };
    ta_op_status status = tna_js_datamodel_value_read(&wrapper->dm, &(ta_datamodel_value_read_params){
                                                                         .source = { .self = &state, .v = &tna_piece_vtable },
                                                                         .durable = true, .out = &value, .fault = &fault });
    napi_value result = NULL;
    if (status != TA_OP_OK) {
        result = tna_codec_refusal(env, &fault, status);
    } else if (!tna_value_get(env, &value, &result)) {
        result = tna_throw(env, "invalid", "durableValue: the value is gone");
    }
    ta_value_release(&value);
    ta_value_release(&fault.data);
    return result;
}

/* Takes the actor back from bytes the store could not keep, so it is not lost. */
static void tna_offload_undo(tna_napi_system *wrapper, ta_bytes bytes) {
    tah_system *system = wrapper->system;
    ta_actor_id back = 0;
    system->inside_core = true;
    ta_result reloaded = ta_actor_load(system->core, &(ta_actor_load_params){ .bytes = bytes, .out = &back });
    system->inside_core = false;
    if (reloaded == TA_OK) {
        (void)tah_hook_actor_created(system, back);
    }
}

/* offload(slot, generation): the actor is unloaded, its bytes in the store. */
static napi_value tna_offload_inner(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "unload() cannot run inside a core call (ADR 0013)");
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 2 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "offload: expected (slot, generation)");
    }
    ta_actor_id actor = ta_actor_id_make(slot, generation);
    ta_actor_inspection view;
    ta_result result = ta_actor_inspect(system->core, actor, &view);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "unload(): no such actor");
    }
    if (view.self.kind != TA_ACTOR_REF_NAMED) {
        return tna_throw(env, "invalid", "unload(): only a named actor can be unloaded");
    }
    /* The directory keeps the unloaded address's reference, which keys the store. */
    const ta_address *address = view.self.as.named;
    ta_value root = { 0 };
    bool has_root = ta_actor_datamodel_root(system->core, actor, &root) == TA_OK;
    tna_js_datamodel_offload_begin(&wrapper->dm);
    ta_bytes bytes = { 0 };
    system->inside_core = true;
    result = ta_actor_offload(system->core, &(ta_actor_offload_params){ .actor = actor,
                                                                        .root = has_root ? &root : NULL,
                                                                        .arena = &system->scratch.call,
                                                                        .out = &bytes });
    system->inside_core = false;
    tna_offload_handles handles = tna_js_datamodel_offload_end(&wrapper->dm);
    ta_value_release(&root);
    if (result != TA_OK) {
        tna_js_datamodel_release_handles(&wrapper->dm, handles);
        tah_scratch_reset_call(&system->scratch);
        return tna_throw(env, tna_result_code(result),
                         result == TA_ERR_BUSY ? "unload(): the actor is not stable; unload it once it is idle"
                                               : "unload(): the actor cannot be offloaded");
    }
    (void)tah_hook_actor_unloaded(system, actor);
    result = tah_offloads_put(&system->offloads, &(tah_offload_put_params){ .address = address,
                                                                            .bytes = bytes,
                                                                            .handles = handles.handles,
                                                                            .handle_count = handles.count });
    if (result != TA_OK) {
        tna_offload_undo(wrapper, bytes);
        tna_js_datamodel_release_handles(&wrapper->dm, handles);
        tah_scratch_reset_call(&system->scratch);
        return tna_throw(env, "out-of-memory", "unload(): no memory for the offloaded bytes");
    }
    tah_scratch_reset_call(&system->scratch);
    return tna_undefined_value(env);
}

/* hasOffloaded(family, name) */
napi_value tna_has_offloaded_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "hasOffloaded");
    if (address == NULL) {
        return NULL;
    }
    bool found = tah_offloads_find(&wrapper->system->offloads, address) != NULL;
    ta_address_release(address);
    napi_value out = NULL;
    napi_get_boolean(env, found, &out);
    return out;
}

/* dropOffloaded(family, name): the address is being removed. */
napi_value tna_drop_offloaded_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    ta_address *address = tna_address_arguments(env, wrapper, argv, "dropOffloaded");
    if (address == NULL) {
        return NULL;
    }
    (void)tah_offloads_drop(&wrapper->system->offloads, address);
    ta_address_release(address);
    return tna_undefined_value(env);
}

/*
 * loadOffloaded(family, name, binding?): the actor comes back from its
 * bytes, which the store then frees; returns { slot, generation }. A
 * binding other than undefined replaces the data-model root the bytes
 * carry (ta_actor_load_params.datamodel_binding).
 */
static napi_value tna_load_offloaded_inner(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    ta_address *address = tna_address_arguments(env, wrapper, argv, "load");
    if (address == NULL) {
        return NULL;
    }
    const tah_offload *entry = tah_offloads_find(&system->offloads, address);
    if (entry == NULL) {
        ta_address_release(address);
        return tna_throw(env, "not-found", "load: the address has no offloaded actor");
    }
    ta_value binding = { NULL, NULL };
    napi_valuetype binding_type = napi_undefined;
    bool bound = argc > 2 && argv[2] != NULL && napi_typeof(env, argv[2], &binding_type) == napi_ok &&
                 binding_type != napi_undefined;
    if (bound && !tna_value_make(&wrapper->values, argv[2], &binding)) {
        ta_address_release(address);
        return tna_throw(env, "out-of-memory", "load: cannot keep the binding");
    }
    ta_actor_id actor = 0;
    system->inside_core = true;
    ta_result result = ta_actor_load(system->core, &(ta_actor_load_params){
                                                       .bytes = { .ptr = entry->bytes, .len = entry->length },
                                                       .datamodel_binding = bound ? &binding : NULL,
                                                       .out = &actor });
    system->inside_core = false;
    /* Core retained the binding if it kept it. */
    ta_value_release(&binding);
    if (result == TA_OK) {
        /* The actor holds its own references now; the entry's go with it. */
        (void)tah_offloads_drop(&system->offloads, address);
        result = tah_hook_actor_created(system, actor);
    }
    ta_address_release(address);
    if (result != TA_OK) {
        return tna_throw(env, tna_result_code(result), "load: the offloaded actor cannot be loaded");
    }
    napi_value out = NULL;
    napi_value slot = NULL;
    napi_value generation = NULL;
    if (napi_create_object(env, &out) != napi_ok ||
        napi_create_uint32(env, ta_actor_id_slot(actor), &slot) != napi_ok ||
        napi_create_uint32(env, ta_actor_id_generation(actor), &generation) != napi_ok ||
        napi_set_named_property(env, out, "slot", slot) != napi_ok ||
        napi_set_named_property(env, out, "generation", generation) != napi_ok) {
        return NULL;
    }
    return out;
}

/* complete_load, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_complete_load_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_complete_load_inner(env, info));
}

/* offload, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_offload_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_offload_inner(env, info));
}

/* load_offloaded, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_load_offloaded_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_load_offloaded_inner(env, info));
}
