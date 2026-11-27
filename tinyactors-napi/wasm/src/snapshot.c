/* readSnapshot, as the addon's src/snapshot.c: a snapshot read without a
 * host (tinyactors-core/docs/embedding.md, section 12). The pack is copied
 * into a block of the environment's allocator, and each live segment's
 * region read with ta_system_read into a block of its own beside its
 * ta_image; JavaScript holds the snapshot's address and frees it
 * (ta_web_snapshot_free) when its Snapshot is collected or closed. A query
 * answers one of core's readers as CBOR in the environment's arena, which
 * JavaScript reads before its call leaves. */
#include "web.h"

#include <string.h>

typedef struct ta_web_snapshot_segment {
    ta_image image;
    ta_system *system; /* NULL: retired */
    void *memory;
    size_t memory_bytes;
} ta_web_snapshot_segment;

typedef struct ta_web_snapshot {
    void *pack;
    size_t pack_bytes;
    ta_host_snapshot_contents contents;
    ta_web_snapshot_segment *segments;
    uint32_t segment_count, pad;
} ta_web_snapshot;

static void ta_web_snapshot_release(ta_web_env *env, ta_web_snapshot *snapshot) {
    ta_allocator *allocator = &env->allocator;
    for (uint32_t i = 0; snapshot->segments && i < snapshot->segment_count; i++)
        if (snapshot->segments[i].memory)
            allocator->free(allocator->context, snapshot->segments[i].memory, snapshot->segments[i].memory_bytes);
    if (snapshot->segments)
        allocator->free(allocator->context, snapshot->segments, sizeof *snapshot->segments * snapshot->segment_count);
    if (snapshot->pack) allocator->free(allocator->context, snapshot->pack, snapshot->pack_bytes);
    allocator->free(allocator->context, snapshot, sizeof *snapshot);
}

/* Each live segment's image opened and region read. */
static ta_status ta_web_snapshot_segments(ta_web_env *env, ta_web_snapshot *snapshot) {
    ta_allocator *allocator = &env->allocator;
    ta_bytes pack = {snapshot->pack, snapshot->pack_bytes};
    uint32_t count = snapshot->contents.image_count;
    snapshot->segments = (ta_web_snapshot_segment *)allocator->alloc(allocator->context, sizeof *snapshot->segments * (count ? count : 1u), 8);
    if (!snapshot->segments) return TA_ERROR_NO_MEMORY;
    memset(snapshot->segments, 0, sizeof *snapshot->segments * (count ? count : 1u));
    snapshot->segment_count = count;
    for (uint32_t i = 0; i < count; i++) {
        ta_web_snapshot_segment *segment = &snapshot->segments[i];
        ta_bytes image = ta_host_snapshot_image(pack, i), region = ta_host_snapshot_region(pack, i);
        if (!image.length || !region.length) continue; /* retired */
        if (ta_image_open(image, &segment->image) != TA_OK) return TA_ERROR_INVALID;
        segment->memory = allocator->alloc(allocator->context, region.length, 8);
        if (!segment->memory) return TA_ERROR_NO_MEMORY;
        segment->memory_bytes = region.length;
        ta_system_setup setup = {{segment->memory, region.length}, &segment->image, NULL, NULL, 0, 0};
        ta_status status = ta_system_read(&setup, region, &segment->system);
        if (status != TA_OK) {
            segment->system = NULL;
            return status;
        }
    }
    return TA_OK;
}

/* Record: 0-1 the pack's bytes (in the call's arena). Answers the status;
 * the record: 0 the snapshot's address, 1-2 its system id, 3-4 its
 * datamodel's label, 5-6 its time, 7 its segments, 8-9 the datamodels'
 * state, views into the snapshot's copy of the pack. */
TA_WEB_EXPORT int32_t ta_web_snapshot_read(ta_web_env *env) {
    uint32_t *word = env->record.word;
    ta_bytes given = ta_web_bytes_at(word, 0);
    ta_allocator *allocator = &env->allocator;
    ta_web_snapshot *snapshot = (ta_web_snapshot *)allocator->alloc(allocator->context, sizeof *snapshot, 8);
    if (!snapshot) return TA_ERROR_NO_MEMORY;
    memset(snapshot, 0, sizeof *snapshot);
    snapshot->pack_bytes = given.length ? given.length : 8u;
    snapshot->pack = allocator->alloc(allocator->context, snapshot->pack_bytes, 8);
    if (!snapshot->pack) {
        ta_web_snapshot_release(env, snapshot);
        return TA_ERROR_NO_MEMORY;
    }
    if (given.length) memcpy(snapshot->pack, given.pointer, given.length);
    snapshot->contents.size = sizeof snapshot->contents;
    ta_status status = ta_host_snapshot_unpack((ta_bytes){snapshot->pack, given.length}, &snapshot->contents);
    if (status == TA_OK) status = ta_web_snapshot_segments(env, snapshot);
    if (status != TA_OK) {
        ta_web_snapshot_release(env, snapshot);
        return status;
    }
    word[0] = ta_web_address(snapshot);
    ta_web_set_text(word, 1, snapshot->contents.system_id);
    ta_web_set_text(word, 3, snapshot->contents.datamodel);
    ta_web_set_word64(word, 5, snapshot->contents.time);
    word[7] = snapshot->segment_count;
    ta_web_set_bytes(word, 8, (ta_bytes){snapshot->contents.state, snapshot->contents.state_length});
    return TA_OK;
}

/* Record: 0 a snapshot's address. */
TA_WEB_EXPORT void ta_web_snapshot_free(ta_web_env *env) {
    ta_web_snapshot *snapshot = (ta_web_snapshot *)(uintptr_t)env->record.word[0];
    if (snapshot) ta_web_snapshot_release(env, snapshot);
}

/* ---- queries --------------------------------------------------------------------------- */

enum {
    TA_WEB_SNAPSHOT_ACTORS = 0,
    TA_WEB_SNAPSHOT_ACTOR = 1,
    TA_WEB_SNAPSHOT_CONFIGURATION = 2,
    TA_WEB_SNAPSHOT_MAILBOX = 3,
    TA_WEB_SNAPSHOT_TIMERS = 4,
    TA_WEB_SNAPSHOT_STATS = 5,
    TA_WEB_SNAPSHOT_CHARTS = 6,
    TA_WEB_SNAPSHOT_TRACE_KEPT = 7,
    TA_WEB_SNAPSHOT_TRACES = 8
};

/* What a query reads and writes with. */
typedef struct ta_web_snapshot_reading {
    ta_web_env *env;
    ta_web_snapshot_segment *segment;
    uint32_t segment_index, handle;
    ta_cbor_writer *writer;
} ta_web_snapshot_reading;

static void ta_web_key(ta_cbor_writer *writer, const char *key) {
    ta_cbor_write_text(writer, ta_string(key));
}

static void ta_web_text_or_null(ta_cbor_writer *writer, ta_string_view text) {
    if (text.length) ta_cbor_write_text(writer, text);
    else ta_cbor_write_simple(writer, 22); /* null */
}

static const char *const ta_web_actor_states[] = {"free", "idle", "runnable", "waitingWorkspace", "waitingAck", "done", "hibernated"};
static const char *const ta_web_state_kinds[] = {"atomic", "compound", "parallel", "final", "shallowHistory", "deepHistory"};
static const char *const ta_web_transition_types[] = {"external", "internal", "initial"};

/* A table of names, by index. */
typedef struct ta_web_names {
    const char *const *names;
    uint32_t count;
} ta_web_names;

static const ta_web_names ta_web_actor_state_names = {ta_web_actor_states, 7};
static const ta_web_names ta_web_state_kind_names = {ta_web_state_kinds, 6};
static const ta_web_names ta_web_transition_type_names = {ta_web_transition_types, 3};

static void ta_web_name(ta_cbor_writer *writer, const ta_web_names *table, uint32_t index) {
    const char *name = index < table->count ? table->names[index] : "unknown";
    ta_cbor_write_text(writer, ta_string(name));
}

static ta_status ta_web_query_actors(const ta_web_snapshot_reading *query) {
    uint32_t cursor = 0, handles[64];
    ta_cbor_write_array(query->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t count; (count = ta_system_actors(query->segment->system, &cursor, (ta_uint32_array){handles, 64})) != 0;)
        for (uint32_t i = 0; i < count; i++) ta_cbor_write_uint(query->writer, handles[i]);
    ta_cbor_write_end(query->writer);
    return TA_OK;
}

static ta_status ta_web_query_actor(const ta_web_snapshot_reading *query) {
    ta_system *system = query->segment->system;
    ta_cbor_writer *writer = query->writer;
    ta_actor_info actor, parent;
    actor.size = parent.size = sizeof actor;
    ta_actor_info_get(system, query->handle, &actor);
    ta_cbor_write_map(writer, 9);
    ta_web_key(writer, "session");
    ta_cbor_write_uint(writer, actor.serial);
    ta_web_key(writer, "name");
    ta_web_text_or_null(writer, ta_system_actor_name(system, query->handle));
    ta_web_key(writer, "chart");
    ta_cbor_write_uint(writer, actor.chart);
    ta_web_key(writer, "chartName");
    ta_cbor_write_text(writer, ta_image_chart_name(&query->segment->image, actor.chart));
    ta_web_key(writer, "status");
    ta_web_name(writer, &ta_web_actor_state_names, actor.state);
    ta_web_key(writer, "parent");
    if (actor.parent && ta_actor_info_get(system, actor.parent, &parent) == TA_OK) ta_cbor_write_uint(writer, parent.serial);
    else ta_cbor_write_simple(writer, 22);
    ta_web_key(writer, "mailbox");
    ta_cbor_write_uint(writer, actor.mailbox_length);
    ta_web_key(writer, "invocations");
    ta_cbor_write_uint(writer, actor.invocations);
    ta_web_key(writer, "services");
    ta_cbor_write_uint(writer, actor.services);
    return TA_OK;
}

static void *ta_web_query_room(const ta_web_snapshot_reading *query, size_t bytes) {
    return ta_growable_arena_alloc(&query->env->scratch.arena, bytes ? bytes : 8u, 8);
}

static ta_status ta_web_query_configuration(const ta_web_snapshot_reading *query) {
    uint32_t count = ta_actor_config(query->segment->system, query->handle, (ta_uint32_array){NULL, 0});
    uint32_t *states = (uint32_t *)ta_web_query_room(query, sizeof *states * count);
    if (!states) return TA_ERROR_NO_MEMORY;
    ta_actor_config(query->segment->system, query->handle, (ta_uint32_array){states, count});
    ta_cbor_write_array(query->writer, count);
    for (uint32_t i = 0; i < count; i++) ta_cbor_write_uint(query->writer, states[i]);
    return TA_OK;
}

static ta_status ta_web_query_mailbox(const ta_web_snapshot_reading *query) {
    uint32_t count = ta_system_actor_mailbox(query->segment->system, query->handle, (ta_actor_queued_array){NULL, 0});
    ta_actor_queued *queued = (ta_actor_queued *)ta_web_query_room(query, sizeof *queued * count);
    if (!queued) return TA_ERROR_NO_MEMORY;
    ta_system_actor_mailbox(query->segment->system, query->handle, (ta_actor_queued_array){queued, count});
    ta_cbor_write_array(query->writer, count);
    for (uint32_t i = 0; i < count; i++) {
        ta_cbor_write_map(query->writer, 2);
        ta_web_key(query->writer, "event");
        ta_cbor_write_text(query->writer, queued[i].event);
        ta_web_key(query->writer, "sendid");
        ta_web_text_or_null(query->writer, queued[i].sendid);
    }
    return TA_OK;
}

static ta_status ta_web_query_timers(const ta_web_snapshot_reading *query) {
    uint32_t count = ta_system_actor_timers(query->segment->system, query->handle, (ta_actor_timer_array){NULL, 0});
    ta_actor_timer *timers = (ta_actor_timer *)ta_web_query_room(query, sizeof *timers * count);
    if (!timers) return TA_ERROR_NO_MEMORY;
    ta_system_actor_timers(query->segment->system, query->handle, (ta_actor_timer_array){timers, count});
    ta_cbor_write_array(query->writer, count);
    for (uint32_t i = 0; i < count; i++) {
        ta_cbor_write_map(query->writer, 3);
        ta_web_key(query->writer, "due");
        ta_cbor_write_uint(query->writer, timers[i].due);
        ta_web_key(query->writer, "event");
        ta_cbor_write_text(query->writer, timers[i].event);
        ta_web_key(query->writer, "sendid");
        ta_web_text_or_null(query->writer, timers[i].sendid);
    }
    return TA_OK;
}

static ta_status ta_web_query_stats(const ta_web_snapshot_reading *query) {
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(query->segment->system, &stats);
    ta_cbor_writer *writer = query->writer;
    const struct {
        const char *key;
        uint64_t value;
    } fields[] = {
        {"now", stats.now},       {"ticksPerSecond", stats.ticks_per_second}, {"nextDeadline", stats.next_deadline},
        {"actors", stats.actors}, {"hibernated", stats.hibernated},           {"runnable", stats.runnable},
        {"events", stats.events}, {"timers", stats.timers},                   {"requests", stats.requests},
        {"regionBytes", stats.region_bytes}, {"wallNanoseconds", stats.wall_ns},
    };
    ta_cbor_write_map(writer, sizeof fields / sizeof fields[0]);
    for (uint32_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        ta_web_key(writer, fields[i].key);
        ta_cbor_write_uint(writer, fields[i].value);
    }
    return TA_OK;
}

static ta_status ta_web_query_transition(const ta_web_snapshot_reading *query, ta_chart_item item) {
    const ta_image *image = &query->segment->image;
    ta_cbor_writer *writer = query->writer;
    ta_transition_info transition;
    transition.size = sizeof transition;
    if (ta_image_transition(image, item, &transition) != TA_OK) return TA_ERROR_INVALID;
    uint32_t *targets = (uint32_t *)ta_web_query_room(query, sizeof *targets * transition.target_count);
    if (!targets) return TA_ERROR_NO_MEMORY;
    ta_image_transition_targets(image, item, (ta_uint32_array){targets, transition.target_count});
    ta_cbor_write_map(writer, 5);
    ta_web_key(writer, "source");
    ta_cbor_write_uint(writer, transition.source);
    ta_web_key(writer, "type");
    ta_web_name(writer, &ta_web_transition_type_names, transition.type);
    ta_web_key(writer, "events");
    ta_cbor_write_array(writer, transition.event_count);
    for (uint32_t i = 0; i < transition.event_count; i++) ta_cbor_write_text(writer, ta_image_transition_event(image, item, i));
    ta_web_key(writer, "cond");
    ta_expression_info condition;
    if (transition.condition != TA_NIL && ta_image_expression(image, (ta_chart_item){item.chart, transition.condition}, &condition) == TA_OK)
        ta_cbor_write_text(writer, condition.source);
    else
        ta_cbor_write_simple(writer, 22);
    ta_web_key(writer, "targets");
    ta_cbor_write_array(writer, transition.target_count);
    for (uint32_t i = 0; i < transition.target_count; i++) ta_cbor_write_uint(writer, targets[i]);
    return TA_OK;
}

static ta_status ta_web_query_chart(const ta_web_snapshot_reading *query, uint32_t chart) {
    const ta_image *image = &query->segment->image;
    ta_cbor_writer *writer = query->writer;
    ta_cbor_write_map(writer, 5);
    ta_web_key(writer, "name");
    ta_cbor_write_text(writer, ta_image_chart_name(image, chart));
    ta_web_key(writer, "datamodel");
    ta_cbor_write_text(writer, ta_image_chart_datamodel(image, chart));
    ta_web_key(writer, "states");
    uint32_t states = ta_image_state_count(image, chart);
    ta_cbor_write_array(writer, states);
    for (uint32_t i = 0; i < states; i++) {
        ta_state_info state;
        state.size = sizeof state;
        if (ta_image_state_info(image, (ta_chart_item){chart, i}, &state) != TA_OK) return TA_ERROR_INVALID;
        ta_cbor_write_map(writer, 3);
        ta_web_key(writer, "id");
        ta_cbor_write_text(writer, state.id);
        ta_web_key(writer, "kind");
        ta_web_name(writer, &ta_web_state_kind_names, state.kind);
        ta_web_key(writer, "parent");
        if (state.parent == TA_NIL) ta_cbor_write_simple(writer, 22);
        else ta_cbor_write_uint(writer, state.parent);
    }
    ta_web_key(writer, "transitions");
    uint32_t transitions = ta_image_transition_count(image, chart);
    ta_cbor_write_array(writer, transitions);
    for (uint32_t i = 0; i < transitions; i++) {
        ta_status status = ta_web_query_transition(query, (ta_chart_item){chart, i});
        if (status != TA_OK) return status;
    }
    ta_web_key(writer, "data");
    uint32_t data = ta_image_data_count(image, chart);
    ta_cbor_write_array(writer, data);
    for (uint32_t i = 0; i < data; i++) {
        ta_data_info datum;
        if (ta_image_data(image, (ta_chart_item){chart, i}, &datum) != TA_OK) return TA_ERROR_INVALID;
        ta_cbor_write_text(writer, datum.id);
    }
    return TA_OK;
}

static ta_status ta_web_query_charts(const ta_web_snapshot_reading *query) {
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&query->segment->image, &info);
    ta_cbor_write_array(query->writer, info.chart_count);
    for (uint32_t c = 0; c < info.chart_count; c++) {
        ta_status status = ta_web_query_chart(query, c);
        if (status != TA_OK) return status;
    }
    return TA_OK;
}

static ta_status ta_web_query_trace_kept(const ta_web_snapshot_reading *query) {
    ta_trace_kept kept;
    kept.size = sizeof kept;
    if (ta_system_trace_kept(query->segment->system, &kept) != TA_OK) {
        ta_cbor_write_simple(query->writer, 22);
        return TA_OK;
    }
    ta_cbor_writer *writer = query->writer;
    ta_cbor_write_map(writer, 5);
    ta_web_key(writer, "records");
    ta_cbor_write_uint(writer, kept.records);
    ta_web_key(writer, "acknowledgedRecords");
    ta_cbor_write_uint(writer, kept.acknowledged_records);
    ta_web_key(writer, "bytes");
    ta_cbor_write_uint(writer, (uint64_t)kept.first.length + kept.second.length);
    ta_web_key(writer, "acknowledgedBytes");
    ta_cbor_write_uint(writer, kept.acknowledged);
    ta_web_key(writer, "lost");
    ta_cbor_write_uint(writer, kept.lost);
    return TA_OK;
}

static ta_status ta_web_query_traces(const ta_web_snapshot_reading *query) {
    ta_host_trace_history_input input = {sizeof input, query->segment_index, query->segment->system, &query->segment->image,
                                         &query->env->allocator};
    ta_buffer probe = {NULL, 0, 0};
    ta_status status = ta_host_trace_history(&input, &probe);
    if (status == TA_ERROR_STATE) { /* the segment does not trace */
        ta_cbor_write_simple(query->writer, 22);
        return TA_OK;
    }
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY) return status;
    void *room = ta_web_query_room(query, probe.length);
    if (!room) return TA_ERROR_NO_MEMORY;
    ta_buffer request = {room, probe.length, 0};
    status = probe.length ? ta_host_trace_history(&input, &request) : TA_OK;
    if (status != TA_OK) return status;
    ta_cbor_write_bytes(query->writer, (ta_bytes){room, request.length});
    return TA_OK;
}

/* Record: 0 a snapshot's address, 1 a segment, 2 an actor's handle (the
 * kinds that take one). Answers the status (TA_ERROR_INVALID: no live
 * segment of that index; TA_ERROR_STALE: no actor of that handle); the
 * record: 0-1 the answer as CBOR, in the environment's arena. */
TA_WEB_EXPORT int32_t ta_web_snapshot_query(ta_web_env *env, uint32_t kind) {
    uint32_t *word = env->record.word;
    ta_web_snapshot *snapshot = (ta_web_snapshot *)(uintptr_t)word[0];
    uint32_t index = word[1], handle = word[2];
    if (!snapshot || index >= snapshot->segment_count || !snapshot->segments[index].system) return TA_ERROR_INVALID;
    ta_web_snapshot_segment *segment = &snapshot->segments[index];
    bool takes_handle = kind >= TA_WEB_SNAPSHOT_ACTOR && kind <= TA_WEB_SNAPSHOT_TIMERS;
    ta_actor_info actor;
    actor.size = sizeof actor;
    if (takes_handle && ta_actor_info_get(segment->system, handle, &actor) != TA_OK) return TA_ERROR_STALE;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &env->scratch.view);
    ta_web_snapshot_reading query = {env, segment, index, handle, &writer};
    ta_status status = TA_ERROR_INVALID;
    switch (kind) {
    case TA_WEB_SNAPSHOT_ACTORS: status = ta_web_query_actors(&query); break;
    case TA_WEB_SNAPSHOT_ACTOR: status = ta_web_query_actor(&query); break;
    case TA_WEB_SNAPSHOT_CONFIGURATION: status = ta_web_query_configuration(&query); break;
    case TA_WEB_SNAPSHOT_MAILBOX: status = ta_web_query_mailbox(&query); break;
    case TA_WEB_SNAPSHOT_TIMERS: status = ta_web_query_timers(&query); break;
    case TA_WEB_SNAPSHOT_STATS: status = ta_web_query_stats(&query); break;
    case TA_WEB_SNAPSHOT_CHARTS: status = ta_web_query_charts(&query); break;
    case TA_WEB_SNAPSHOT_TRACE_KEPT: status = ta_web_query_trace_kept(&query); break;
    case TA_WEB_SNAPSHOT_TRACES: status = ta_web_query_traces(&query); break;
    default: break;
    }
    ta_bytes cbor = {NULL, 0};
    if (status == TA_OK) status = ta_cbor_writer_bytes(&writer, &cbor);
    ta_web_set_bytes(word, 0, status == TA_OK ? cbor : (ta_bytes){NULL, 0});
    return status; /* the bytes stay in the arena until the call's mark is released */
}
