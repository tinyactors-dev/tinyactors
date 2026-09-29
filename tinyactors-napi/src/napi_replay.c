#include "napi_replay.h"
#include "napi_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyactors_host.h"
#include "napi_system.h"
#include "pump.h"
#include "runtime.h"

/* A message the log shows queued and not delivered, owned. */
typedef struct {
    uint64_t host_seq;
    ta_event event;
} tna_owed;

typedef struct {
    tna_napi_system *wrapper; /* held while the feed lives */
    uint8_t *bytes;
    size_t length;
    size_t capacity;
    size_t start;             /* bytes core has read */
    tna_owed *owed;
    uint32_t owed_count;
    uint32_t owed_capacity;
    bool closed;
} tna_replay_feed;

static void tna_feed_release(tna_replay_feed *feed) {
    if (feed->closed) {
        return;
    }
    feed->closed = true;
    bool alive = !tah_system_closed(feed->wrapper->system);
    for (uint32_t index = 0; index < feed->owed_count; index += 1u) {
        if (alive) {
            ta_event_release(&feed->owed[index].event);
        }
    }
    free(feed->owed);
    free(feed->bytes);
    feed->owed = NULL;
    feed->bytes = NULL;
    feed->owed_count = 0;
    tna_napi_system_unhold(feed->wrapper);
}

static void tna_feed_finalize(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    tna_replay_feed *feed = data;
    tna_feed_release(feed);
    free(feed);
}

/* The feed argv[0] names; throws and answers NULL when it is not one of this system's. */
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

/* A source over the feed's next `span` bytes. */
typedef struct {
    const uint8_t *bytes;
    size_t length;
    size_t at;
} tna_span_source;

static ta_op_status tna_span_read(void *self, const ta_byte_read_params *params) {
    tna_span_source *source = self;
    size_t left = source->length - source->at;
    uint32_t count = left < params->capacity ? (uint32_t)left : params->capacity;
    memcpy(params->buffer, source->bytes + source->at, count);
    source->at += count;
    *params->got = count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable tna_span_vtable = { .read = tna_span_read };

static uint64_t tah_le(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0;
    for (uint32_t index = count; index > 0; index -= 1u) {
        value = value << 8 | bytes[index - 1u];
    }
    return value;
}

/* A record the host wrote for its mailbox (core ADR 0090 section 6). */
static bool tna_host_kind(ta_record_kind kind) {
    return kind == TA_RECORD_QUEUED || kind == TA_RECORD_DROPPED || kind == TA_RECORD_EVICTED;
}

/* Keeps a queued message the log owes; drops one an event or a drop answered. */
static void tna_feed_host(tna_replay_feed *feed, ta_host_record *host) {
    if (host->kind == TA_RECORD_QUEUED && host->has_event) {
        if (feed->owed_count == feed->owed_capacity) {
            uint32_t capacity = feed->owed_capacity == 0 ? 8u : feed->owed_capacity * 2u;
            tna_owed *grown = realloc(feed->owed, (size_t)capacity * sizeof *grown);
            if (grown == NULL) {
                ta_event_release(&host->event); /* no memory: that message is lost */
                return;
            }
            feed->owed = grown;
            feed->owed_capacity = capacity;
        }
        feed->owed[feed->owed_count++] = (tna_owed){ .host_seq = host->host_seq, .event = host->event };
        return;
    }
    if (host->has_event) {
        ta_event_release(&host->event);
    }
    if (host->host_seq == 0) {
        return;
    }
    for (uint32_t index = 0; index < feed->owed_count; index += 1u) {
        if (feed->owed[index].host_seq == host->host_seq) {
            ta_event_release(&feed->owed[index].event);
            memmove(&feed->owed[index], &feed->owed[index + 1u],
                    (size_t)(feed->owed_count - index - 1u) * sizeof *feed->owed);
            feed->owed_count -= 1u;
            return;
        }
    }
}

/* Resumes a replaying actor until it is between macrosteps; `done` when it finished. */
static ta_result tna_replay_resume(tah_system *system, ta_actor_id actor, bool unlimited, bool *ignored, bool *done) {
    uint32_t budget = system->budgets.microsteps != 0 ? system->budgets.microsteps : 64u;
    for (;;) {
        ta_run_status status = TA_RUN_STABLE;
        bool took_nothing = false;
        system->inside_core = true;
        ta_result result = ta_actor_resume(system->core, &(ta_actor_resume_params){
                                                             .actor = actor,
                                                             .budget = budget,
                                                             .status = &status,
                                                             .action_budget = system->budgets.actions,
                                                             .no_microstep_limit = unlimited,
                                                             .ignored = &took_nothing,
                                                         });
        system->inside_core = false;
        if (result != TA_OK) {
            return result;
        }
        if (status == TA_RUN_PAUSED) {
            continue;
        }
        *ignored = took_nothing;
        *done = status == TA_RUN_DONE;
        return TA_OK;
    }
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
    if (feed == NULL) {
        return tna_throw(env, "out-of-memory", "replay: cannot make a feed");
    }
    feed->wrapper = wrapper;
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
    /* What core has read goes first: the feed holds one record and a chunk. */
    if (feed->start != 0) {
        memmove(feed->bytes, feed->bytes + feed->start, feed->length - feed->start);
        feed->length -= feed->start;
        feed->start = 0;
    }
    if (feed->length + length > feed->capacity) {
        size_t capacity = feed->capacity == 0 ? 4096u : feed->capacity;
        while (capacity < feed->length + length) {
            capacity *= 2u;
        }
        uint8_t *grown = realloc(feed->bytes, capacity);
        if (grown == NULL) {
            return tna_throw(env, "out-of-memory", "replay: cannot keep the chunk");
        }
        feed->bytes = grown;
        feed->capacity = capacity;
    }
    if (length != 0) {
        memcpy(feed->bytes + feed->length, data, length);
    }
    feed->length += length;
    napi_value out = NULL;
    napi_create_double(env, (double)(feed->length - feed->start), &out);
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
    if (system->inside_core) {
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
    /* Host records before a life's first record (mail that waited for a
     * load of an address no actor had yet) have no actor to decode them
     * with: they are skipped, and that mail reaches the actor from memory. */
    size_t span = 0;
    for (;;) {
        span = tah_record_span(feed->bytes + feed->start, feed->length - feed->start, true);
        if (span == 0 || !tna_host_kind((ta_record_kind)tah_le(feed->bytes + feed->start + 2, 2))) {
            break;
        }
        feed->start += span;
    }
    if (span == 0) {
        napi_value null = NULL;
        napi_get_null(env, &null);
        return null;
    }
    ta_actor_address address = { 0 };
    bool named = false;
    napi_valuetype type = napi_undefined;
    if (argc >= 6 && napi_typeof(env, argv[4], &type) == napi_ok && type == napi_string) {
        named = tna_text_argument(env, system, argv[4], &address.family.name) &&
                tna_text_argument(env, system, argv[5], &address.name);
        if (!named) {
            tah_scratch_reset_call(&system->scratch);
            return tna_throw(env, "invalid", "replayPrepare: the address is (family, name)");
        }
    }
    tna_span_source source = { .bytes = feed->bytes + feed->start, .length = span };
    ta_byte_source log = { .self = &source, .v = &tna_span_vtable };
    ta_actor_id actor = 0;
    system->inside_core = true;
    ta_result result = ta_actor_prepare(system->core, &(ta_actor_prepare_params){
                                                          .incumbent = ta_actor_id_make(slot, generation),
                                                          .address = named ? &address : NULL,
                                                          .definition = definition,
                                                          .log = &log,
                                                          .out = &actor,
                                                      });
    system->inside_core = false;
    tah_scratch_reset_call(&system->scratch);
    if (result != TA_OK) {
        return tna_replay_error(env, result, "the log's first record cannot start a replaying actor");
    }
    feed->start += source.at;
    result = tah_hook_actor_created(system, actor);
    if (result != TA_OK) {
        return tna_replay_error(env, result, "the replaying actor cannot be scheduled");
    }
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    if (entry != NULL) {
        entry->flags |= TAH_SCHED_REPLAYING;
    }
    /* Its bootstrap, from `spawned`; a checkpoint's actor is idle already. */
    bool ignored = false;
    bool done = false;
    result = tna_replay_resume(system, actor, false, &ignored, &done);
    if (result != TA_OK && result != TA_ERR_RUNAWAY) {
        return tna_replay_error(env, result, "the replaying actor's bootstrap failed");
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

/* The name of an EVENT record at `record`, whose span is known, as a JavaScript string. */
static napi_value tna_record_name(napi_env env, const uint8_t *record) {
    uint64_t fixed = tah_le(record + 4, 4);
    uint64_t reference = tah_le(record + 32 + 16, 8); /* the third field */
    uint32_t offset = (uint32_t)reference;
    uint32_t length = (uint32_t)(reference >> 32);
    napi_value out = NULL;
    if (offset != UINT32_MAX) {
        napi_create_string_utf8(env, (const char *)record + fixed + offset, length, &out);
    }
    return out;
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
    if (system->inside_core) {
        return tna_throw(env, "reentrant", "replay: cannot be called from inside a core call (ADR 0013)");
    }
    tna_replay_feed *feed = tna_feed_argument(env, wrapper, argc >= 1 ? argv[0] : NULL);
    if (feed == NULL) {
        return NULL;
    }
    /* Between two lives (after `destroyed`): only host records, up to the next life's first record. */
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
    ta_actor_id actor = ta_actor_id_make(slot, generation);
    /* A replaying actor prepared with `at` joins here: the scheduler leaves
     * it to the replay, and a macrostep it has in progress (its bootstrap)
     * runs first. */
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    if (entry != NULL && (entry->flags & TAH_SCHED_REPLAYING) == 0) {
        entry->flags |= TAH_SCHED_REPLAYING;
        ta_actor_inspection view;
        if (ta_actor_inspect(system->core, actor, &view) == TA_OK && view.macrostep_in_progress) {
            bool took_nothing = false;
            bool done = false;
            ta_result result = tna_replay_resume(system, actor, false, &took_nothing, &done);
            if (result != TA_OK && result != TA_ERR_RUNAWAY) {
                return tna_replay_error(env, result, "the replaying actor's bootstrap failed");
            }
        }
    }
    napi_value ignored_list = NULL;
    napi_create_array(env, &ignored_list);
    uint32_t ignored_count = 0;
    uint32_t records = 0;
    double last_time = -1;
    const char *state = "more";
    while (records < most) {
        const uint8_t *record = feed->bytes + feed->start;
        size_t span = tah_record_span(record, feed->length - feed->start, false);
        if (span == 0) {
            state = "need";
            break;
        }
        ta_record_kind kind = (ta_record_kind)tah_le(record + 2, 2);
        double time = (double)tah_le(record + 16, 8);
        if (between && !tna_host_kind(kind)) {
            state = "life";
            break;
        }
        tna_span_source source = { .bytes = record, .length = span };
        ta_replay_step step = TA_REPLAY_NEXT;
        ta_host_record host = { 0 };
        system->inside_core = true;
        ta_result result = ta_actor_replay(system->core, &(ta_actor_replay_params){
                                                             .actor = actor,
                                                             .source = { .self = &source, .v = &tna_span_vtable },
                                                             .step = &step,
                                                             .host = &host,
                                                         });
        system->inside_core = false;
        if (result != TA_OK) {
            return tna_replay_error(env, result, "a record of the log cannot be replayed");
        }
        napi_value name = kind == TA_RECORD_EVENT ? tna_record_name(env, record) : NULL;
        feed->start += span;
        records += 1u;
        last_time = time;
        tna_feed_host(feed, &host);
        bool took_nothing = false;
        bool done = false;
        if (step == TA_REPLAY_RUN || step == TA_REPLAY_RELEASE) {
            result = tna_replay_resume(system, actor, step == TA_REPLAY_RELEASE, &took_nothing, &done);
            if (result == TA_ERR_RUNAWAY) {
                continue; /* stopped as it was live: the next record abandons or releases it */
            }
            if (result != TA_OK) {
                return tna_replay_error(env, result, "a replayed macrostep failed");
            }
            if (took_nothing && name != NULL) {
                napi_value item = NULL;
                napi_value value = NULL;
                napi_create_object(env, &item);
                napi_create_string_utf8(env, "event", NAPI_AUTO_LENGTH, &value);
                napi_set_named_property(env, item, "kind", value);
                napi_create_double(env, time, &value);
                napi_set_named_property(env, item, "time", value);
                napi_set_named_property(env, item, "name", name);
                napi_set_element(env, ignored_list, ignored_count++, item);
            }
            if (done) {
                state = "finished";
                break;
            }
            continue;
        }
        if (step == TA_REPLAY_FINISHED) {
            state = "finished";
            break;
        }
        if (step == TA_REPLAY_ENDED) {
            state = "ended";
            break;
        }
    }
    napi_value out = NULL;
    napi_value value = NULL;
    napi_create_object(env, &out);
    napi_create_string_utf8(env, state, NAPI_AUTO_LENGTH, &value);
    napi_set_named_property(env, out, "state", value);
    napi_create_uint32(env, records, &value);
    napi_set_named_property(env, out, "records", value);
    napi_set_named_property(env, out, "ignored", ignored_list);
    if (last_time >= 0) {
        napi_create_double(env, last_time, &value);
        napi_set_named_property(env, out, "lastTime", value);
    }
    napi_create_double(env, (double)(feed->length - feed->start), &value);
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
    uint32_t slot = 0;
    uint32_t generation = 0;
    if (argc < 3 || napi_get_value_uint32(env, argv[1], &slot) != napi_ok ||
        napi_get_value_uint32(env, argv[2], &generation) != napi_ok) {
        return tna_throw(env, "invalid", "replayFinish: expected (feed, slot, generation)");
    }
    tah_sched_entry *entry = tah_sched_lookup(&wrapper->system->sched, ta_actor_id_make(slot, generation));
    if (entry != NULL) {
        entry->flags &= (uint8_t)~TAH_SCHED_REPLAYING;
    }
    return NULL;
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
    if (system->inside_core) {
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
    ta_actor_id actor = ta_actor_id_make(slot, generation);
    uint32_t put = 0;
    ta_result failure = TA_OK;
    for (uint32_t index = 0; index < feed->owed_count; index += 1u) {
        tna_owed *owed = &feed->owed[index];
        /* A message this system queued is still in memory (waiting for this
         * very load, say) and reaches the actor from there. */
        if (failure != TA_OK || tah_inputs_seq_is_ours(&system->inputs, owed->host_seq)) {
            ta_event_release(&owed->event);
            continue;
        }
        ta_trace_parent trace = { 0 };
        tah_inputs_redeliver(&system->inputs, owed->host_seq, ta_system_time(system->core));
        ta_result result = tah_system_post(system, &(tah_message){ .target = actor, .event = &owed->event, .trace = &trace });
        tah_inputs_redelivered(&system->inputs);
        if (result != TA_OK) {
            ta_event_release(&owed->event);
            failure = result;
            continue;
        }
        put += 1u;
    }
    feed->owed_count = 0;
    if (failure != TA_OK) {
        return tna_replay_error(env, failure, "an owed message could not be put back");
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
    if (system->inside_core) {
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
    ta_actor_id actor = ta_actor_id_make(slot, generation);
    /* The messages its mailbox holds that the log numbered (ADR 0091 section 5). */
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    uint32_t count = 0;
    for (uint32_t index = entry != NULL ? entry->first : 0; index != 0; index = tah_sched_envelope(&system->sched, index)->next) {
        count += 1u;
    }
    ta_owed_message *owed = count == 0 ? NULL : calloc(count, sizeof *owed);
    if (count != 0 && owed == NULL) {
        return tna_throw(env, "out-of-memory", "checkpoint: cannot list the owed messages");
    }
    uint32_t owed_count = 0;
    for (uint32_t index = entry != NULL ? entry->first : 0; index != 0; index = tah_sched_envelope(&system->sched, index)->next) {
        const tah_envelope *envelope = tah_sched_envelope(&system->sched, index);
        uint64_t seq = tah_inputs_host_seq(system, index);
        if (seq != 0 && envelope->report == TAH_REPORT_NONE) {
            owed[owed_count++] = (ta_owed_message){ .host_seq = seq, .event = &envelope->event };
        }
    }
    uint8_t *buffer = malloc(TNA_CHECKPOINT_CHUNK);
    if (buffer == NULL) {
        free(owed);
        return tna_throw(env, "out-of-memory", "checkpoint: cannot make its buffer");
    }
    tna_checkpoint_sink sink = { .env = env, .write = argv[2], .buffer = buffer };
    system->inside_core = true;
    ta_result result = ta_actor_checkpoint(system->core, &(ta_actor_checkpoint_params){
                                                             .actor = actor,
                                                             .sink = { .self = &sink, .v = &tna_checkpoint_vtable },
                                                             .owed = owed,
                                                             .owed_count = owed_count,
                                                         });
    system->inside_core = false;
    free(buffer);
    free(owed);
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
