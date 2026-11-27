/* snapshot.c: readSnapshot(bytes), a snapshot read without a host
 * (tinyactors-core/docs/embedding.md, section 12).
 *
 * The pack is copied into a Buffer, 8-aligned, and each live segment's
 * region (ta_host_snapshot_region) is read with ta_system_read into a
 * second Buffer, beside the segment's ta_image: a system no one runs,
 * which no datamodel is bound to. Both Buffers are kept under the
 * instance's `snapshot` symbol on the Snapshot, which holds a pointer
 * into the second (napi_wrap, no finalizer: the Buffers are the memory,
 * gone with the object). A Buffer's bytes never move, and neither is
 * handed out. The methods mirror the system's and the image's readers,
 * one segment at a time: actors, actor, configuration, mailbox, timers,
 * stats, charts, traceKept, traces. */
#include "addon.h"
#include "tinyactors_runtime.h"
#include <string.h>

/* napi_type_tag_object's tag of a Snapshot. */
static const napi_type_tag ta_napi_snapshot_tag = {0x7461736e61707368ULL, 0x6f74417069303031ULL};

typedef struct ta_napi_snapshot_segment {
    ta_image image;
    ta_system *system; /* NULL: retired */
    uint32_t index, pad;
} ta_napi_snapshot_segment;

typedef struct ta_napi_snapshot {
    uint32_t segment_count, pad;
    ta_napi_snapshot_segment *segments;
} ta_napi_snapshot;

static size_t ta_napi_pad8(size_t length) {
    return (length + 7u) & ~(size_t)7u;
}

/* A Buffer of at least `length` bytes and the 8-aligned start in it. */
typedef struct ta_napi_aligned {
    napi_value buffer;
    unsigned char *data;
} ta_napi_aligned;

static bool ta_napi_aligned_buffer(napi_env env, size_t length, ta_napi_aligned *out) {
    void *data = NULL;
    if (napi_create_buffer(env, length + 8u, &data, &out->buffer) != napi_ok) return false;
    out->data = (unsigned char *)data + ((8u - ((uintptr_t)data & 7u)) & 7u);
    return true;
}

/* ---- fields of an object --------------------------------------------------- */

typedef struct ta_napi_fields {
    napi_env env;
    napi_value object;
    bool ok;
} ta_napi_fields;

static bool ta_napi_fields_open(napi_env env, ta_napi_fields *fields) {
    fields->env = env;
    fields->ok = napi_create_object(env, &fields->object) == napi_ok;
    return fields->ok;
}

static void ta_napi_field(ta_napi_fields *fields, const char *name, napi_value value) {
    if (fields->ok) fields->ok = napi_set_named_property(fields->env, fields->object, name, value) == napi_ok;
}

static napi_value ta_napi_text_value(napi_env env, ta_string_view text) {
    napi_value value = NULL;
    napi_create_string_utf8(env, text.pointer ? text.pointer : "", text.length, &value);
    return value;
}

static napi_value ta_napi_null(napi_env env) {
    napi_value value = NULL;
    napi_get_null(env, &value);
    return value;
}

static void ta_napi_field_text(ta_napi_fields *fields, const char *name, ta_string_view text) {
    ta_napi_field(fields, name, ta_napi_text_value(fields->env, text));
}

/* text, or null when it is empty */
static void ta_napi_field_text_or_null(ta_napi_fields *fields, const char *name, ta_string_view text) {
    ta_napi_field(fields, name, text.length ? ta_napi_text_value(fields->env, text) : ta_napi_null(fields->env));
}

static void ta_napi_field_number(ta_napi_fields *fields, const char *name, double number) {
    napi_value value = NULL;
    if (fields->ok) fields->ok = napi_create_double(fields->env, number, &value) == napi_ok;
    ta_napi_field(fields, name, value);
}

static void ta_napi_field_bigint(ta_napi_fields *fields, const char *name, uint64_t number) {
    napi_value value = NULL;
    if (fields->ok) fields->ok = napi_create_bigint_uint64(fields->env, number, &value) == napi_ok;
    ta_napi_field(fields, name, value);
}

/* ---- readSnapshot ------------------------------------------------------------ */

static napi_value ta_napi_snapshot_refused(napi_env env, const char *message) {
    return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_INVALID, message});
}

/* What readSnapshot built, for the Snapshot's constructor. */
typedef struct ta_napi_snapshot_made {
    ta_napi_snapshot *snapshot;
    ta_host_snapshot_contents contents;
    napi_value pack, block;
} ta_napi_snapshot_made;

static napi_value ta_napi_snapshot_symbol(napi_env env) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value keys, key;
    if (instance == NULL || napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_get_named_property(env, keys, "snapshot", &key) != napi_ok)
        return NULL;
    return key;
}

/* The Snapshot of what was made: type-tagged, its header as properties,
 * the two Buffers under the symbol, the snapshot wrapped last. */
static napi_value ta_napi_snapshot_object(napi_env env, const ta_napi_snapshot_made *made) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value class, self, external, key = ta_napi_snapshot_symbol(env), kept, state;
    void *data = NULL;
    const ta_host_snapshot_contents *contents = &made->contents;
    if (instance == NULL || key == NULL || napi_get_reference_value(env, instance->snapshot, &class) != napi_ok ||
        napi_create_external(env, made->snapshot, NULL, NULL, &external) != napi_ok ||
        napi_new_instance(env, class, 1, &external, &self) != napi_ok ||
        napi_create_array_with_length(env, 2, &kept) != napi_ok || napi_set_element(env, kept, 0, made->pack) != napi_ok ||
        napi_set_element(env, kept, 1, made->block) != napi_ok ||
        napi_create_buffer_copy(env, contents->state_length, contents->state_length ? contents->state : "", &data, &state) !=
            napi_ok)
        return NULL;
    napi_value system_id = ta_napi_text_value(env, contents->system_id), datamodel = ta_napi_text_value(env, contents->datamodel),
               time = NULL, segments = NULL;
    if (napi_create_bigint_uint64(env, contents->time, &time) != napi_ok ||
        napi_create_uint32(env, made->snapshot->segment_count, &segments) != napi_ok)
        return NULL;
    napi_property_descriptor properties[] = {
        {"systemID", NULL, NULL, NULL, NULL, system_id, napi_enumerable, NULL},
        {"datamodel", NULL, NULL, NULL, NULL, datamodel, napi_enumerable, NULL},
        {"time", NULL, NULL, NULL, NULL, time, napi_enumerable, NULL},
        {"segments", NULL, NULL, NULL, NULL, segments, napi_enumerable, NULL},
        {"state", NULL, NULL, NULL, NULL, state, napi_enumerable, NULL},
        {NULL, key, NULL, NULL, NULL, kept, napi_default, NULL},
    };
    if (napi_define_properties(env, self, sizeof properties / sizeof properties[0], properties) != napi_ok ||
        napi_wrap(env, self, made->snapshot, NULL, NULL, NULL) != napi_ok)
        return NULL;
    return self;
}

/* Each live segment's image opened and region read, into the block. */
static napi_value ta_napi_snapshot_read_segments(napi_env env, ta_napi_snapshot_made *made, ta_bytes pack) {
    uint32_t count = made->contents.image_count;
    size_t at = ta_napi_pad8(sizeof(ta_napi_snapshot)) + ta_napi_pad8(sizeof(ta_napi_snapshot_segment) * count);
    size_t total = at;
    for (uint32_t i = 0; i < count; i++) total += ta_napi_pad8(ta_host_snapshot_region(pack, i).length);
    ta_napi_aligned block;
    if (!ta_napi_aligned_buffer(env, total, &block)) return NULL;
    memset(block.data, 0, total);
    made->block = block.buffer;
    made->snapshot = (ta_napi_snapshot *)(void *)block.data;
    made->snapshot->segment_count = count;
    made->snapshot->segments = (ta_napi_snapshot_segment *)(void *)(block.data + ta_napi_pad8(sizeof(ta_napi_snapshot)));
    for (uint32_t i = 0; i < count; i++) {
        ta_napi_snapshot_segment *segment = &made->snapshot->segments[i];
        segment->index = i;
        ta_bytes image = ta_host_snapshot_image(pack, i), region = ta_host_snapshot_region(pack, i);
        if (!image.length || !region.length) continue; /* retired */
        if (ta_image_open(image, &segment->image) != TA_OK)
            return ta_napi_snapshot_refused(env, "a segment's image in the snapshot does not open");
        ta_system_setup setup = {{block.data + at, region.length}, &segment->image, NULL, NULL, 0, 0};
        if (ta_system_read(&setup, region, &segment->system) != TA_OK)
            return ta_napi_snapshot_refused(env, "a segment of the snapshot is not this build's, or is damaged");
        at += ta_napi_pad8(region.length);
    }
    return ta_napi_snapshot_object(env, made);
}

/* readSnapshot(bytes): a Snapshot of a pack (host.snapshot()'s bytes) of
 * this build, read during the call and copied. */
static napi_value ta_napi_read_snapshot(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argument = NULL;
    if (napi_get_cb_info(env, info, &argc, &argument, NULL, NULL) != napi_ok) return NULL;
    ta_napi_bytes bytes = {"readSnapshot takes a Uint8Array or an ArrayBuffer", {NULL, 0}};
    if (!ta_napi_bytes_of(env, argc < 1 ? NULL : argument, &bytes)) return NULL;
    ta_napi_aligned pack;
    if (!ta_napi_aligned_buffer(env, bytes.view.length, &pack)) return NULL;
    if (bytes.view.length) memcpy(pack.data, bytes.view.pointer, bytes.view.length);
    ta_napi_snapshot_made made;
    memset(&made, 0, sizeof made);
    made.pack = pack.buffer;
    made.contents.size = sizeof made.contents;
    ta_bytes copied = {pack.data, bytes.view.length};
    if (ta_host_snapshot_unpack(copied, &made.contents) != TA_OK)
        return ta_napi_snapshot_refused(env, "not a snapshot of this build (another format or layout, or damaged)");
    return ta_napi_snapshot_read_segments(env, &made, copied);
}

/* ---- the methods -------------------------------------------------------------- */

/* A call on a Snapshot: its segment's system, and the handle when the
 * method takes one. */
typedef struct ta_napi_snapshot_call {
    size_t arguments;  /* 1: (segment); 2: (segment, handle) */
    ta_napi_snapshot_segment *segment;
    uint32_t handle;
} ta_napi_snapshot_call;

static bool ta_napi_snapshot_u32(napi_env env, napi_value value, uint32_t *out) {
    napi_valuetype type = napi_undefined;
    double number = 0;
    if (napi_typeof(env, value, &type) != napi_ok || type != napi_number || napi_get_value_double(env, value, &number) != napi_ok ||
        !(number >= 0 && number <= 4294967295.0) || number != (double)(uint32_t)number)
        return false;
    *out = (uint32_t)number;
    return true;
}

static bool ta_napi_snapshot_enter(napi_env env, napi_callback_info info, ta_napi_snapshot_call *call) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL}, self;
    bool tagged = false;
    napi_valuetype type = napi_undefined;
    void *data = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) return false;
    if (napi_typeof(env, self, &type) != napi_ok || type != napi_object ||
        napi_check_object_type_tag(env, self, &ta_napi_snapshot_tag, &tagged) != napi_ok || !tagged ||
        napi_unwrap(env, self, &data) != napi_ok) {
        napi_throw_type_error(env, NULL, "not a Snapshot");
        return false;
    }
    ta_napi_snapshot *snapshot = (ta_napi_snapshot *)data;
    uint32_t index = 0;
    if (argc < 1 || !ta_napi_snapshot_u32(env, argv[0], &index) || index >= snapshot->segment_count ||
        snapshot->segments[index].system == NULL) {
        napi_throw_range_error(env, NULL, "no live segment of that index in the snapshot");
        return false;
    }
    call->segment = &snapshot->segments[index];
    if (call->arguments < 2) return true;
    ta_actor_info actor;
    actor.size = sizeof actor;
    if (argc < 2 || !ta_napi_snapshot_u32(env, argv[1], &call->handle) ||
        ta_actor_info_get(call->segment->system, call->handle, &actor) != TA_OK) {
        napi_throw_range_error(env, NULL, "no actor of that handle in the segment");
        return false;
    }
    return true;
}

/* actors(segment): every live actor's handle, in slot order. */
static napi_value ta_napi_snapshot_actors(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {1, NULL, 0};
    napi_value result, element;
    if (!ta_napi_snapshot_enter(env, info, &call) || napi_create_array(env, &result) != napi_ok) return NULL;
    uint32_t cursor = 0, handles[64], at = 0;
    for (uint32_t count; (count = ta_system_actors(call.segment->system, &cursor, (ta_uint32_array){handles, 64})) != 0;)
        for (uint32_t i = 0; i < count; i++)
            if (napi_create_uint32(env, handles[i], &element) != napi_ok || napi_set_element(env, result, at++, element) != napi_ok)
                return NULL;
    return result;
}

static const char *const ta_napi_actor_states[] = {"free", "idle", "runnable", "waitingWorkspace", "waitingAck", "done", "hibernated"};

/* actor(segment, handle): {session, name, chart, chartName, status,
 * parent, mailbox, invocations, services}. */
static napi_value ta_napi_snapshot_actor(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {2, NULL, 0};
    if (!ta_napi_snapshot_enter(env, info, &call)) return NULL;
    ta_system *system = call.segment->system;
    ta_actor_info actor, parent;
    actor.size = parent.size = sizeof actor;
    ta_actor_info_get(system, call.handle, &actor);
    ta_napi_fields fields;
    if (!ta_napi_fields_open(env, &fields)) return NULL;
    ta_napi_field_bigint(&fields, "session", actor.serial);
    ta_napi_field_text_or_null(&fields, "name", ta_system_actor_name(system, call.handle));
    ta_napi_field_number(&fields, "chart", actor.chart);
    ta_napi_field_text(&fields, "chartName", ta_image_chart_name(&call.segment->image, actor.chart));
    const char *status = actor.state < sizeof ta_napi_actor_states / sizeof ta_napi_actor_states[0]
                             ? ta_napi_actor_states[actor.state]
                             : "unknown";
    ta_napi_field_text(&fields, "status", (ta_string_view){status, (uint32_t)strlen(status)});
    if (actor.parent && ta_actor_info_get(system, actor.parent, &parent) == TA_OK)
        ta_napi_field_bigint(&fields, "parent", parent.serial);
    else
        ta_napi_field(&fields, "parent", ta_napi_null(env));
    ta_napi_field_number(&fields, "mailbox", actor.mailbox_length);
    ta_napi_field_number(&fields, "invocations", actor.invocations);
    ta_napi_field_number(&fields, "services", actor.services);
    return fields.ok ? fields.object : NULL;
}

/* The instance's arena for a call's scratch arrays. */
typedef struct ta_napi_scratch {
    ta_napi_instance *instance;
    ta_growable_arena_mark mark;
} ta_napi_scratch;

static bool ta_napi_scratch_open(napi_env env, ta_napi_scratch *scratch) {
    scratch->instance = ta_napi_instance_of(env);
    if (scratch->instance == NULL) return false;
    scratch->mark = ta_growable_arena_position(&scratch->instance->arena);
    return true;
}

static void *ta_napi_scratch_alloc(ta_napi_scratch *scratch, size_t size) {
    return ta_growable_arena_alloc(&scratch->instance->arena, size ? size : 8u, 8);
}

static void ta_napi_scratch_close(ta_napi_scratch *scratch) {
    ta_napi_arena_release(&scratch->instance->arena, scratch->mark, TA_NAPI_INSTANCE_ARENA_CAP);
}

static napi_value ta_napi_no_memory(napi_env env) {
    return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "no memory to read the snapshot"});
}

/* An array of numbers. */
static napi_value ta_napi_u32_array(napi_env env, ta_uint32_array values) {
    napi_value result, element;
    if (napi_create_array_with_length(env, values.capacity, &result) != napi_ok) return NULL;
    for (uint32_t i = 0; i < values.capacity; i++)
        if (napi_create_uint32(env, values.pointer[i], &element) != napi_ok || napi_set_element(env, result, i, element) != napi_ok)
            return NULL;
    return result;
}

/* configuration(segment, handle): its active states' indices in the
 * chart, in document order; [] for a hibernated actor. */
static napi_value ta_napi_snapshot_configuration(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {2, NULL, 0};
    ta_napi_scratch scratch;
    if (!ta_napi_snapshot_enter(env, info, &call) || !ta_napi_scratch_open(env, &scratch)) return NULL;
    uint32_t count = ta_actor_config(call.segment->system, call.handle, (ta_uint32_array){NULL, 0});
    uint32_t *states = (uint32_t *)ta_napi_scratch_alloc(&scratch, sizeof *states * count);
    napi_value result = NULL;
    if (states == NULL) ta_napi_no_memory(env);
    else {
        ta_actor_config(call.segment->system, call.handle, (ta_uint32_array){states, count});
        result = ta_napi_u32_array(env, (ta_uint32_array){states, count});
    }
    ta_napi_scratch_close(&scratch);
    return result;
}

/* mailbox(segment, handle): its queued events, [{event, sendid}], in the
 * order it takes them. */
static napi_value ta_napi_snapshot_mailbox(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {2, NULL, 0};
    ta_napi_scratch scratch;
    if (!ta_napi_snapshot_enter(env, info, &call) || !ta_napi_scratch_open(env, &scratch)) return NULL;
    uint32_t count = ta_system_actor_mailbox(call.segment->system, call.handle, (ta_actor_queued_array){NULL, 0});
    ta_actor_queued *queued = (ta_actor_queued *)ta_napi_scratch_alloc(&scratch, sizeof *queued * count);
    napi_value result = NULL;
    if (queued == NULL) ta_napi_no_memory(env);
    else if (napi_create_array_with_length(env, count, &result) == napi_ok) {
        ta_system_actor_mailbox(call.segment->system, call.handle, (ta_actor_queued_array){queued, count});
        for (uint32_t i = 0; i < count && result != NULL; i++) {
            ta_napi_fields fields;
            ta_napi_fields_open(env, &fields);
            ta_napi_field_text(&fields, "event", queued[i].event);
            ta_napi_field_text_or_null(&fields, "sendid", queued[i].sendid);
            if (!fields.ok || napi_set_element(env, result, i, fields.object) != napi_ok) result = NULL;
        }
    }
    ta_napi_scratch_close(&scratch);
    return result;
}

/* timers(segment, handle): its delayed sends, [{due, event, sendid}], in
 * firing order; due in the system's ticks (stats().ticksPerSecond). */
static napi_value ta_napi_snapshot_timers(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {2, NULL, 0};
    ta_napi_scratch scratch;
    if (!ta_napi_snapshot_enter(env, info, &call) || !ta_napi_scratch_open(env, &scratch)) return NULL;
    uint32_t count = ta_system_actor_timers(call.segment->system, call.handle, (ta_actor_timer_array){NULL, 0});
    ta_actor_timer *timers = (ta_actor_timer *)ta_napi_scratch_alloc(&scratch, sizeof *timers * count);
    napi_value result = NULL;
    if (timers == NULL) ta_napi_no_memory(env);
    else if (napi_create_array_with_length(env, count, &result) == napi_ok) {
        ta_system_actor_timers(call.segment->system, call.handle, (ta_actor_timer_array){timers, count});
        for (uint32_t i = 0; i < count && result != NULL; i++) {
            ta_napi_fields fields;
            ta_napi_fields_open(env, &fields);
            ta_napi_field_bigint(&fields, "due", timers[i].due);
            ta_napi_field_text(&fields, "event", timers[i].event);
            ta_napi_field_text_or_null(&fields, "sendid", timers[i].sendid);
            if (!fields.ok || napi_set_element(env, result, i, fields.object) != napi_ok) result = NULL;
        }
    }
    ta_napi_scratch_close(&scratch);
    return result;
}

/* stats(segment): {now, ticksPerSecond, nextDeadline, actors, hibernated,
 * runnable, events, timers, requests, regionBytes, wallNanoseconds}. */
static napi_value ta_napi_snapshot_stats(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {1, NULL, 0};
    if (!ta_napi_snapshot_enter(env, info, &call)) return NULL;
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(call.segment->system, &stats);
    ta_napi_fields fields;
    if (!ta_napi_fields_open(env, &fields)) return NULL;
    ta_napi_field_bigint(&fields, "now", stats.now);
    ta_napi_field_number(&fields, "ticksPerSecond", (double)stats.ticks_per_second);
    ta_napi_field_bigint(&fields, "nextDeadline", stats.next_deadline);
    ta_napi_field_number(&fields, "actors", stats.actors);
    ta_napi_field_number(&fields, "hibernated", stats.hibernated);
    ta_napi_field_number(&fields, "runnable", stats.runnable);
    ta_napi_field_number(&fields, "events", stats.events);
    ta_napi_field_number(&fields, "timers", stats.timers);
    ta_napi_field_number(&fields, "requests", stats.requests);
    ta_napi_field_number(&fields, "regionBytes", (double)stats.region_bytes);
    ta_napi_field_bigint(&fields, "wallNanoseconds", stats.wall_ns);
    return fields.ok ? fields.object : NULL;
}

static const char *const ta_napi_state_kinds[] = {"atomic", "compound", "parallel", "final", "shallowHistory", "deepHistory"};
static const char *const ta_napi_transition_types[] = {"external", "internal", "initial"};

/* A table of names, by index. */
typedef struct ta_napi_names {
    const char *const *names;
    uint32_t count;
} ta_napi_names;

static const ta_napi_names ta_napi_state_kind_names = {ta_napi_state_kinds, 6};
static const ta_napi_names ta_napi_transition_type_names = {ta_napi_transition_types, 3};

static ta_string_view ta_napi_name_of(const ta_napi_names *table, uint32_t index) {
    const char *name = index < table->count ? table->names[index] : "unknown";
    return (ta_string_view){name, (uint32_t)strlen(name)};
}

/* What a chart's parts are read with: the image, and the call's scratch. */
typedef struct ta_napi_chart_reading {
    napi_env env;
    const ta_image *image;
    ta_napi_scratch *scratch;
} ta_napi_chart_reading;

/* A chart's states: [{id, kind, parent}], parent an index or null. */
static napi_value ta_napi_chart_states(const ta_napi_chart_reading *reading, uint32_t chart) {
    napi_env env = reading->env;
    uint32_t count = ta_image_state_count(reading->image, chart);
    napi_value result;
    if (napi_create_array_with_length(env, count, &result) != napi_ok) return NULL;
    for (uint32_t i = 0; i < count; i++) {
        ta_state_info state;
        state.size = sizeof state;
        ta_napi_fields fields;
        if (ta_image_state_info(reading->image, (ta_chart_item){chart, i}, &state) != TA_OK || !ta_napi_fields_open(env, &fields))
            return NULL;
        ta_napi_field_text(&fields, "id", state.id);
        ta_napi_field_text(&fields, "kind", ta_napi_name_of(&ta_napi_state_kind_names, state.kind));
        if (state.parent == TA_NIL) ta_napi_field(&fields, "parent", ta_napi_null(env));
        else ta_napi_field_number(&fields, "parent", state.parent);
        if (!fields.ok || napi_set_element(env, result, i, fields.object) != napi_ok) return NULL;
    }
    return result;
}

/* A transition: {source, type, events, cond, targets}. */
static napi_value ta_napi_chart_transition(const ta_napi_chart_reading *reading, ta_chart_item item) {
    napi_env env = reading->env;
    const ta_image *image = reading->image;
    ta_transition_info transition;
    transition.size = sizeof transition;
    ta_napi_fields fields;
    napi_value events;
    if (ta_image_transition(image, item, &transition) != TA_OK || !ta_napi_fields_open(env, &fields) ||
        napi_create_array_with_length(env, transition.event_count, &events) != napi_ok)
        return NULL;
    ta_napi_field_number(&fields, "source", transition.source);
    ta_napi_field_text(&fields, "type", ta_napi_name_of(&ta_napi_transition_type_names, transition.type));
    for (uint32_t i = 0; i < transition.event_count; i++)
        if (napi_set_element(env, events, i, ta_napi_text_value(env, ta_image_transition_event(image, item, i))) != napi_ok)
            return NULL;
    ta_napi_field(&fields, "events", events);
    ta_expression_info condition;
    if (transition.condition != TA_NIL &&
        ta_image_expression(image, (ta_chart_item){item.chart, transition.condition}, &condition) == TA_OK)
        ta_napi_field_text(&fields, "cond", condition.source);
    else
        ta_napi_field(&fields, "cond", ta_napi_null(env));
    uint32_t *targets = (uint32_t *)ta_napi_scratch_alloc(reading->scratch, sizeof *targets * transition.target_count);
    if (targets == NULL) return ta_napi_no_memory(env);
    ta_image_transition_targets(image, item, (ta_uint32_array){targets, transition.target_count});
    ta_napi_field(&fields, "targets", ta_napi_u32_array(env, (ta_uint32_array){targets, transition.target_count}));
    return fields.ok ? fields.object : NULL;
}

/* A chart: {name, datamodel, states, transitions, data}. */
static napi_value ta_napi_chart(const ta_napi_chart_reading *reading, uint32_t chart) {
    napi_env env = reading->env;
    const ta_image *image = reading->image;
    ta_napi_fields fields;
    napi_value transitions, data, transition;
    uint32_t transition_count = ta_image_transition_count(image, chart), data_count = ta_image_data_count(image, chart);
    if (!ta_napi_fields_open(env, &fields) || napi_create_array_with_length(env, transition_count, &transitions) != napi_ok ||
        napi_create_array_with_length(env, data_count, &data) != napi_ok)
        return NULL;
    ta_napi_field_text(&fields, "name", ta_image_chart_name(image, chart));
    ta_napi_field_text(&fields, "datamodel", ta_image_chart_datamodel(image, chart));
    ta_napi_field(&fields, "states", ta_napi_chart_states(reading, chart));
    for (uint32_t i = 0; i < transition_count; i++)
        if ((transition = ta_napi_chart_transition(reading, (ta_chart_item){chart, i})) == NULL ||
            napi_set_element(env, transitions, i, transition) != napi_ok)
            return NULL;
    ta_napi_field(&fields, "transitions", transitions);
    for (uint32_t i = 0; i < data_count; i++) {
        ta_data_info datum;
        if (ta_image_data(image, (ta_chart_item){chart, i}, &datum) != TA_OK ||
            napi_set_element(env, data, i, ta_napi_text_value(env, datum.id)) != napi_ok)
            return NULL;
    }
    ta_napi_field(&fields, "data", data);
    return fields.ok ? fields.object : NULL;
}

/* charts(segment): each chart of the segment's image. */
static napi_value ta_napi_snapshot_charts(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {1, NULL, 0};
    ta_napi_scratch scratch;
    if (!ta_napi_snapshot_enter(env, info, &call) || !ta_napi_scratch_open(env, &scratch)) return NULL;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(&call.segment->image, &image_info);
    ta_napi_chart_reading reading = {env, &call.segment->image, &scratch};
    napi_value result = NULL, chart;
    if (napi_create_array_with_length(env, image_info.chart_count, &result) == napi_ok)
        for (uint32_t c = 0; c < image_info.chart_count && result != NULL; c++)
            if ((chart = ta_napi_chart(&reading, c)) == NULL || napi_set_element(env, result, c, chart) != napi_ok) result = NULL;
    ta_napi_scratch_close(&scratch);
    return result;
}

/* traceKept(segment): {records, acknowledgedRecords, bytes,
 * acknowledgedBytes, lost}, or null when the segment does not trace. */
static napi_value ta_napi_snapshot_trace_kept(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {1, NULL, 0};
    if (!ta_napi_snapshot_enter(env, info, &call)) return NULL;
    ta_trace_kept kept;
    kept.size = sizeof kept;
    if (ta_system_trace_kept(call.segment->system, &kept) != TA_OK) return ta_napi_null(env);
    ta_napi_fields fields;
    if (!ta_napi_fields_open(env, &fields)) return NULL;
    ta_napi_field_number(&fields, "records", kept.records);
    ta_napi_field_number(&fields, "acknowledgedRecords", kept.acknowledged_records);
    ta_napi_field_number(&fields, "bytes", (double)kept.first.length + (double)kept.second.length);
    ta_napi_field_number(&fields, "acknowledgedBytes", kept.acknowledged);
    ta_napi_field_number(&fields, "lost", (double)kept.lost);
    return fields.ok ? fields.object : NULL;
}

/* traces(segment): the spans of the journal's kept records, an
 * ExportTraceServiceRequest (decodeTraces reads it) in a Buffer, or null
 * when the segment does not trace. */
static napi_value ta_napi_snapshot_traces(napi_env env, napi_callback_info info) {
    ta_napi_snapshot_call call = {1, NULL, 0};
    if (!ta_napi_snapshot_enter(env, info, &call)) return NULL;
    ta_allocator allocator = ta_napi_allocator(env);
    ta_host_trace_history_input input = {sizeof input, call.segment->index, call.segment->system, &call.segment->image,
                                         &allocator};
    ta_buffer probe = {NULL, 0, 0};
    ta_status status = ta_host_trace_history(&input, &probe);
    if (status == TA_ERROR_STATE) return ta_napi_null(env); /* the segment does not trace */
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY)
        return ta_napi_throw(env, &(ta_napi_refusal){status, "the journal's records could not be written as spans"});
    void *data = NULL;
    napi_value result;
    if (napi_create_buffer(env, probe.length, &data, &result) != napi_ok) return NULL;
    ta_buffer room = {data, probe.length, 0};
    status = probe.length ? ta_host_trace_history(&input, &room) : TA_OK;
    if (status != TA_OK) return ta_napi_throw(env, &(ta_napi_refusal){status, "the journal's records could not be written as spans"});
    return result;
}

/* The constructor, called by readSnapshot only, with the snapshot as an
 * external: type-tagged; readSnapshot wraps it. */
static napi_value ta_napi_snapshot_new(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argument = NULL, self;
    napi_valuetype type = napi_undefined;
    if (napi_get_cb_info(env, info, &argc, &argument, &self, NULL) != napi_ok) return NULL;
    if (argc < 1 || napi_typeof(env, argument, &type) != napi_ok || type != napi_external) {
        napi_throw_type_error(env, NULL, "a Snapshot is made by readSnapshot");
        return NULL;
    }
    if (napi_type_tag_object(env, self, &ta_napi_snapshot_tag) != napi_ok) return NULL;
    return self;
}

napi_status ta_napi_snapshot_define(napi_env env, napi_value exports) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return napi_pending_exception;
    napi_value keys, description, symbol, class = NULL;
    napi_status status = napi_get_reference_value(env, instance->keys, &keys);
    if (status == napi_ok) status = napi_create_string_utf8(env, "tinyactors.snapshot", NAPI_AUTO_LENGTH, &description);
    if (status == napi_ok) status = napi_create_symbol(env, description, &symbol);
    if (status == napi_ok) status = napi_set_named_property(env, keys, "snapshot", symbol);
    napi_property_descriptor properties[] = {
        {"actors", NULL, ta_napi_snapshot_actors, NULL, NULL, NULL, napi_default_method, NULL},
        {"actor", NULL, ta_napi_snapshot_actor, NULL, NULL, NULL, napi_default_method, NULL},
        {"configuration", NULL, ta_napi_snapshot_configuration, NULL, NULL, NULL, napi_default_method, NULL},
        {"mailbox", NULL, ta_napi_snapshot_mailbox, NULL, NULL, NULL, napi_default_method, NULL},
        {"timers", NULL, ta_napi_snapshot_timers, NULL, NULL, NULL, napi_default_method, NULL},
        {"stats", NULL, ta_napi_snapshot_stats, NULL, NULL, NULL, napi_default_method, NULL},
        {"charts", NULL, ta_napi_snapshot_charts, NULL, NULL, NULL, napi_default_method, NULL},
        {"traceKept", NULL, ta_napi_snapshot_trace_kept, NULL, NULL, NULL, napi_default_method, NULL},
        {"traces", NULL, ta_napi_snapshot_traces, NULL, NULL, NULL, napi_default_method, NULL},
    };
    if (status == napi_ok)
        status = napi_define_class(env, "Snapshot", NAPI_AUTO_LENGTH, ta_napi_snapshot_new, NULL,
                                   sizeof properties / sizeof properties[0], properties, &class);
    if (status == napi_ok) status = napi_create_reference(env, class, 1, &instance->snapshot);
    napi_property_descriptor exported[] = {
        {"readSnapshot", NULL, ta_napi_read_snapshot, NULL, NULL, NULL, napi_enumerable, NULL},
    };
    if (status == napi_ok) status = napi_define_properties(env, exports, 1, exported);
    return status;
}
