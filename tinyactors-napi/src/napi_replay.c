#include "napi_replay.h"
#include "napi_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyactors_host.h"
#include "napi_system.h"
#include "pump.h"
#include "runtime.h"

/* A replay feed as JavaScript holds it: the kernel's replay, and the system it holds. */
typedef struct {
    tna_napi_system *wrapper; /* held while the feed lives */
    tah_replay *replay;
    bool closed;
} tna_replay_feed;

static void tna_feed_release(tna_replay_feed *feed) {
    if (feed->closed) {
        return;
    }
    feed->closed = true;
    if (!tah_system_closed(feed->wrapper->system)) {
        tah_replay_free(feed->replay);
    }
    feed->replay = NULL;
    tna_napi_system_unhold(feed->wrapper);
}

static void tna_feed_finalize(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    tna_replay_feed *feed = data;
    tna_feed_release(feed);
    free(feed);
}

/* The feed `value` names; throws and answers NULL when it is not one of this system's. */
static tna_replay_feed *tna_feed_argument(napi_env env, tna_napi_system *wrapper, napi_value value) {
    void *data = NULL;
    napi_valuetype type = napi_undefined;
    if (value == NULL || napi_typeof(env, value, &type) != napi_ok || type != napi_external ||
        napi_get_value_external(env, value, &data) != napi_ok || data == NULL ||
        ((tna_replay_feed *)data)->wrapper != wrapper) {
        tna_throw(env, "invalid", "replay: expected a replay feed of this system");
        return NULL;
    }
    tna_replay_feed *feed = data;
    if (feed->closed) {
        tna_throw(env, "invalid", "replay: the feed is closed");
        return NULL;
    }
    return feed;
}

static napi_value tna_replay_error(napi_env env, ta_result result, const char *what) {
    char message[160];
    snprintf(message, sizeof message, "replay: %s (%s)", what, tna_result_code(result));
    return tna_throw(env, tna_result_code(result), message);
}

napi_value tna_replay_feed_method(napi_env env, napi_callback_info info) {
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_replay_feed *feed = calloc(1, sizeof *feed);
    tah_replay *replay = feed == NULL ? NULL : tah_replay_open(wrapper->system);
    if (replay == NULL) {
        free(feed);
        return tna_throw(env, "out-of-memory", "replay: cannot make a feed");
    }
    feed->wrapper = wrapper;
    feed->replay = replay;
    wrapper->holders += 1u;
    napi_value out = NULL;
    if (napi_create_external(env, feed, tna_feed_finalize, NULL, &out) != napi_ok) {
        tna_feed_release(feed);
        free(feed);
        return NULL;
    }
    return out;
}

napi_value tna_replay_push_method(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    bool typed = false;
    void *data = NULL;
    size_t length = 0;
    napi_typedarray_type type = napi_uint8_array;
    napi_value buffer = NULL;
    size_t offset = 0;
    if (argc < 2 || napi_is_typedarray(env, argv[1], &typed) != napi_ok || !typed ||
        napi_get_typedarray_info(env, argv[1], &type, &length, &data, &buffer, &offset) != napi_ok ||
        type != napi_uint8_array) {
        return tna_throw(env, "invalid", "replay: the log's chunks are Uint8Arrays");
    }
    if (tah_replay_push(feed->replay, data, length) != TA_OK) {
        return tna_throw(env, "out-of-memory", "replay: cannot keep the chunk");
    }
    napi_value out = NULL;
    napi_create_double(env, (double)tah_replay_buffered(feed->replay), &out);
    return out;
}

static napi_value tna_replay_prepare_inner(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value argv[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "replay: cannot be called from inside a core call (ADR 0013)");
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    uint64_t definition = 0;
    bool lossless = false;
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 4 || napi_get_value_bigint_uint64(env, argv[1], &definition, &lossless) != napi_ok || !lossless ||
        napi_get_value_uint32(env, argv[2], &slot) != napi_ok || napi_get_value_uint32(env, argv[3], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "replayPrepare: expected (feed, definition, slot, generation, family?, name?)");
    }
    ta_actor_address address = { 0 };
    bool named = false;
    napi_valuetype type = napi_undefined;
    if (argc >= 6 && napi_typeof(env, argv[4], &type) == napi_ok && type == napi_string) {
        named = tna_text_argument(env, system, argv[4], &address.family.name) &&
                tna_text_argument(env, system, argv[5], &address.name);
        if (!named) {
            tah_scratch_reset_call(tah_system_scratch(system));
            return tna_throw(env, "invalid", "replayPrepare: the address is (family, name)");
        }
    }
    ta_actor_id actor = 0;
    bool found = false;
    ta_result result = tah_replay_prepare(feed->replay, &(tah_replay_prepare_params){
                                                            .definition = definition,
                                                            .incumbent = ta_actor_id_make(slot, generation),
                                                            .address = named ? &address : NULL,
                                                            .out = &actor,
                                                            .found = &found,
                                                        });
    tah_scratch_reset_call(tah_system_scratch(system));
    if (result != TA_OK) {
        return tna_replay_error(env, result, "the log's first record cannot start a replaying actor");
    }
    if (!found) {
        napi_value null = NULL;
        napi_get_null(env, &null);
        return null;
    }
    napi_value out = NULL;
    napi_value value = NULL;
    napi_create_object(env, &out);
    napi_create_uint32(env, ta_actor_id_slot(actor), &value);
    napi_set_named_property(env, out, "slot", value);
    napi_create_uint32(env, ta_actor_id_generation(actor), &value);
    napi_set_named_property(env, out, "generation", value);
    return out;
}

/* The ignored events a run reports, as JavaScript objects. */
typedef struct {
    napi_env env;
    napi_value list;
    uint32_t count;
} tna_ignored;

static void tna_note_ignored(void *context, ta_str name, ta_time time) {
    tna_ignored *ignored = context;
    napi_value item = NULL;
    napi_value value = NULL;
    napi_create_object(ignored->env, &item);
    napi_create_string_utf8(ignored->env, "event", NAPI_AUTO_LENGTH, &value);
    napi_set_named_property(ignored->env, item, "kind", value);
    napi_create_double(ignored->env, (double)time, &value);
    napi_set_named_property(ignored->env, item, "time", value);
    napi_create_string_utf8(ignored->env, name.ptr != NULL ? name.ptr : "", name.len, &value);
    napi_set_named_property(ignored->env, item, "name", value);
    napi_set_element(ignored->env, ignored->list, ignored->count++, item);
}

static napi_value tna_replay_run_inner(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value argv[5] = { NULL, NULL, NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "replay: cannot be called from inside a core call (ADR 0013)");
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    /* Between two lives (after `destroyed`): only mail, up to the next life's first record. */
    bool between = false;
    if (argc >= 5) {
        napi_get_value_bool(env, argv[4], &between);
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    uint32_t most = 0;
    if (argc < 4 || napi_get_value_uint32(env, argv[1], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &generation) != napi_ok || napi_get_value_uint32(env, argv[3], &most) != napi_ok) {
        return tna_throw(env, "invalid", "replayRun: expected (feed, slot, generation, records)");
    }
    tna_ignored ignored = { .env = env };
    napi_create_array(env, &ignored.list);
    tah_replay_state state = TAH_REPLAY_MORE;
    uint32_t records = 0;
    ta_result result = tah_replay_run(feed->replay, &(tah_replay_run_params){
                                                        .actor = ta_actor_id_make(slot, generation),
                                                        .most = most,
                                                        .between = between,
                                                        .ignored = tna_note_ignored,
                                                        .context = &ignored,
                                                        .state = &state,
                                                        .records = &records,
                                                    });
    if (result != TA_OK) {
        return tna_replay_error(env, result, "a record of the log cannot be replayed");
    }
    static const char *const names[] = { "more", "need", "finished", "ended", "life" };
    napi_value out = NULL;
    napi_value value = NULL;
    napi_create_object(env, &out);
    napi_create_string_utf8(env, names[state], NAPI_AUTO_LENGTH, &value);
    napi_set_named_property(env, out, "state", value);
    napi_create_uint32(env, records, &value);
    napi_set_named_property(env, out, "records", value);
    napi_set_named_property(env, out, "ignored", ignored.list);
    ta_time last = 0;
    if (tah_replay_last_time(feed->replay, &last)) {
        napi_create_double(env, (double)last, &value);
        napi_set_named_property(env, out, "lastTime", value);
    }
    napi_create_double(env, (double)tah_replay_buffered(feed->replay), &value);
    napi_set_named_property(env, out, "buffered", value);
    return out;
}

napi_value tna_replay_finish_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 3 || napi_get_value_uint32(env, argv[1], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "replayFinish: expected (feed, slot, generation)");
    }
    tah_replay_finish(feed->replay, ta_actor_id_make(slot, generation));
    return NULL;
}

/* replayLastTime(feed): the time of the log's last record read, or undefined. */
napi_value tna_replay_last_time_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    ta_time last = 0;
    napi_value out = NULL;
    if (tah_replay_last_time(feed->replay, &last)) {
        napi_create_double(env, (double)last, &out);
    } else {
        napi_get_undefined(env, &out);
    }
    return out;
}

napi_value tna_replay_redeliver_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "replay: cannot be called from inside a core call (ADR 0013)");
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 3 || napi_get_value_uint32(env, argv[1], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "replayRedeliver: expected (feed, slot, generation)");
    }
    uint32_t put = 0;
    ta_result result = tah_replay_redeliver(feed->replay, ta_actor_id_make(slot, generation), &put);
    if (result != TA_OK) {
        return tna_replay_error(env, result, "an owed message could not be put back");
    }
    napi_value out = NULL;
    napi_create_uint32(env, put, &out);
    return out;
}

napi_value tna_replay_close_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    void *data = NULL;
    napi_valuetype type = napi_undefined;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < 1 ||
        napi_typeof(env, argv[0], &type) != napi_ok || type != napi_external ||
        napi_get_value_external(env, argv[0], &data) != napi_ok || data == NULL) {
        return NULL;
    }
    tna_feed_release(data);
    return NULL;
}

/* ------------------------------------------------------------------------
 * Checkpoints
 */

#define TNA_CHECKPOINT_CHUNK 65536u

typedef struct {
    napi_env env;
    napi_value write;
    uint8_t *buffer;
    uint32_t used;
    bool failed;
} tna_checkpoint_sink;

static ta_op_status tna_checkpoint_flush(tna_checkpoint_sink *sink) {
    if (sink->used == 0 || sink->failed) {
        return sink->failed ? TA_OP_ERROR : TA_OP_OK;
    }
    napi_value chunk = NULL;
    void *copy = NULL;
    napi_value global = NULL;
    napi_value ignored = NULL;
    if (napi_create_buffer_copy(sink->env, sink->used, sink->buffer, &copy, &chunk) != napi_ok ||
        napi_get_global(sink->env, &global) != napi_ok ||
        napi_call_function(sink->env, global, sink->write, 1, &chunk, &ignored) != napi_ok) {
        sink->failed = true;
        return TA_OP_ERROR;
    }
    sink->used = 0;
    return TA_OP_OK;
}

static ta_op_status tna_checkpoint_begin(void *self) {
    (void)self;
    return TA_OP_OK;
}

static ta_op_status tna_checkpoint_write(void *self, ta_bytes bytes) {
    tna_checkpoint_sink *sink = self;
    uint32_t at = 0;
    while (at < bytes.len) {
        uint32_t room = TNA_CHECKPOINT_CHUNK - sink->used;
        uint32_t count = bytes.len - at < room ? bytes.len - at : room;
        memcpy(sink->buffer + sink->used, bytes.ptr + at, count);
        sink->used += count;
        at += count;
        if (sink->used == TNA_CHECKPOINT_CHUNK && tna_checkpoint_flush(sink) != TA_OP_OK) {
            return TA_OP_ERROR;
        }
    }
    return TA_OP_OK;
}

static void tna_checkpoint_end(void *self, bool keep) {
    tna_checkpoint_sink *sink = self;
    if (keep) {
        (void)tna_checkpoint_flush(sink);
    }
}

static const ta_byte_sink_vtable tna_checkpoint_vtable = {
    .begin = tna_checkpoint_begin,
    .write = tna_checkpoint_write,
    .end = tna_checkpoint_end,
};

napi_value tna_checkpoint_method(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3] = { NULL, NULL, NULL };
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) {
        return NULL;
    }
    tna_napi_system *wrapper = tna_open_this(env, info);
    if (wrapper == NULL) {
        return NULL;
    }
    tah_system *system = wrapper->system;
    if (tah_system_inside_core(system)) {
        return tna_throw(env, "reentrant", "checkpoint: cannot be called from inside a core call (ADR 0013)");
    }
    uint32_t slot = 0;
    uint32_t generation = 0;
    napi_valuetype type = napi_undefined;
    if (argc < 3 || napi_get_value_uint32(env, argv[0], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[1], &generation) != napi_ok || napi_typeof(env, argv[2], &type) != napi_ok ||
        type != napi_function) {
        return tna_throw(env, "invalid", "checkpoint(actor, write): expected an actor and a function");
    }
    uint8_t *buffer = malloc(TNA_CHECKPOINT_CHUNK);
    if (buffer == NULL) {
        return tna_throw(env, "out-of-memory", "checkpoint: cannot make its buffer");
    }
    tna_checkpoint_sink sink = { .env = env, .write = argv[2], .buffer = buffer };
    ta_result result = tah_checkpoint_write(system, ta_actor_id_make(slot, generation),
                                            (ta_byte_sink){ .self = &sink, .v = &tna_checkpoint_vtable });
    free(buffer);
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        return NULL; /* write() threw: that is the error */
    }
    if (result != TA_OK) {
        const char *why = result == TA_ERR_BUSY          ? "the actor is not idle"
                          : result == TA_ERR_INVALID     ? "only a published named actor of a logged family has a checkpoint"
                          : result == TA_ERR_UNSUPPORTED ? "a value has no durable form"
                          : result == TA_ERR_DONE        ? "the actor has finished"
                                                         : "the checkpoint could not be written";
        char message[160];
        snprintf(message, sizeof message, "checkpoint(): %s", why);
        return tna_throw(env, tna_result_code(result), message);
    }
    return NULL;
}

/* replay_prepare, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_replay_prepare_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_replay_prepare_inner(env, info));
}

/* replay_run, then a request for the sink if it filled the staging (napi ADR 0036). */
napi_value tna_replay_run_method(napi_env env, napi_callback_info info) {
    return tna_trace_after_method(env, info, tna_replay_run_inner(env, info));
}
