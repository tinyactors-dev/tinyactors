/* snapshot_reader.c — requests about a snapshot, answered with no host
 * (include/tinyactors/host.h, "a snapshot reader"; docs/snapshot-requests.md;
 * docs/adr/0021 in the repository root).
 *
 * A reader is one block of its caller's memory: the reader's record, its
 * segments, a copy of the pack and each live segment's region, read with
 * ta_system_read. Nothing in it changes after open. A request is read in the
 * control plane's envelope (host/request_internal.h, shared with
 * ta_host_post), answered from the systems' and images' readers, written
 * into an output over a growable arena of the caller's allocator, and copied
 * into the caller's buffer; the arena is given back before the call returns.
 * Freestanding, no recursion. */
#include <tinyactors/arena.h>
#include <tinyactors/host.h>
#include <tinyactors/image.h>
#include <tinyactors/otlp.h>
#include <tinyactors/system.h>
#include "base/cbor_output.h"
#include "base/memory.h"
#include "host/request_internal.h"

typedef enum reader_constants {
    READER_ALIGN = 8,
    READER_HANDLES = 64,         /* handles read from a system at a time */
    READER_ROWS_DEFAULT = 25,    /* snapshot.actors' rows when no limit is given */
    READER_ROWS_LIMIT = 512,     /* the most rows a snapshot.actors reply holds */
    READER_SPANS_DEFAULT = 40,   /* snapshot.spans' spans when no limit is given */
    READER_SPANS_LIMIT = 512,    /* the most spans a snapshot.spans reply holds */
    READER_CHUNK = 65536,        /* the working arena's chunk */
    READER_MILLISECONDS = 1000
} reader_constants;

typedef struct reader_segment {
    ta_image image;
    ta_system *system; /* NULL: retired */
} reader_segment;

struct ta_host_snapshot_reader {
    ta_host_snapshot_contents contents; /* views into the pack's copy */
    const unsigned char *pack;
    size_t pack_bytes;
    reader_segment *segments;
    uint32_t segment_count, pad;
};

/* ---- the block ------------------------------------------------------------------------ */

static size_t reader_aligned(size_t bytes) { return (bytes + (READER_ALIGN - 1)) & ~(size_t)(READER_ALIGN - 1); }

/* The block's parts, in order: the record, the segments, the pack, each live region. */
static size_t reader_block_size(ta_bytes pack, const ta_host_snapshot_contents *contents) {
    size_t bytes = reader_aligned(sizeof(struct ta_host_snapshot_reader));
    bytes += reader_aligned(sizeof(reader_segment) * (contents->image_count ? contents->image_count : 1u));
    bytes += reader_aligned(pack.length);
    for (uint32_t i = 0; i < contents->image_count; i++)
        bytes += reader_aligned(ta_host_snapshot_region(pack, i).length);
    return bytes;
}

static int reader_pack_aligned(ta_bytes pack) {
    return pack.pointer && ((uintptr_t)pack.pointer & (READER_ALIGN - 1)) == 0;
}

size_t ta_host_snapshot_reader_size(ta_bytes pack) {
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    if (!reader_pack_aligned(pack) || ta_host_snapshot_unpack(pack, &contents) != TA_OK) return 0;
    return reader_block_size(pack, &contents);
}

ta_status ta_host_snapshot_reader_open(const ta_host_snapshot_reader_input *input,
                                       const ta_host_snapshot_reader **out) {
    if (!input || input->size < sizeof *input || !out) return TA_ERROR_INVALID;
    *out = (const ta_host_snapshot_reader *)0;
    ta_bytes given = input->pack;
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    if (!reader_pack_aligned(given) || ta_host_snapshot_unpack(given, &contents) != TA_OK) return TA_ERROR_INVALID;
    unsigned char *block = (unsigned char *)input->memory.pointer;
    if (!block || ((uintptr_t)block & (READER_ALIGN - 1)) != 0) return TA_ERROR_INVALID;
    if (input->memory.capacity < reader_block_size(given, &contents)) return TA_ERROR_NO_MEMORY;

    size_t at = 0;
    struct ta_host_snapshot_reader *reader = (struct ta_host_snapshot_reader *)(void *)block;
    at += reader_aligned(sizeof *reader);
    uint32_t count = contents.image_count;
    reader_segment *segments = (reader_segment *)(void *)(block + at);
    at += reader_aligned(sizeof(reader_segment) * (count ? count : 1u));
    unsigned char *pack = block + at;
    at += reader_aligned(given.length);
    ta_memcpy(pack, given.pointer, given.length);
    ta_bytes copy = {pack, given.length};
    ta_memset(reader, 0, sizeof *reader);
    reader->contents.size = sizeof reader->contents;
    if (ta_host_snapshot_unpack(copy, &reader->contents) != TA_OK) return TA_ERROR_INVALID;
    reader->pack = pack;
    reader->pack_bytes = given.length;
    reader->segments = segments;
    reader->segment_count = count;
    for (uint32_t i = 0; i < count; i++) {
        reader_segment *segment = &segments[i];
        ta_memset(segment, 0, sizeof *segment);
        ta_bytes image = ta_host_snapshot_image(copy, i), region = ta_host_snapshot_region(copy, i);
        if (!image.length || !region.length) continue; /* retired */
        if (ta_image_open(image, &segment->image) != TA_OK) return TA_ERROR_INVALID;
        ta_system_setup setup;
        ta_memset(&setup, 0, sizeof setup);
        setup.memory = (ta_memory){block + at, region.length};
        setup.image = &segment->image;
        at += reader_aligned(region.length);
        ta_status status = ta_system_read(&setup, region, &segment->system);
        if (status != TA_OK) {
            segment->system = (ta_system *)0;
            return status;
        }
    }
    *out = reader;
    return TA_OK;
}

/* ---- what a request works with ---------------------------------------------------------- */

static const char *const reader_actor_states[] = {"free", "idle", "runnable", "waitingWorkspace",
                                                  "waitingAck", "done", "hibernated"};
static const char *const reader_state_kinds[] = {"atomic", "compound", "parallel", "final",
                                                 "shallowHistory", "deepHistory"};
static const char *const reader_transition_types[] = {"external", "internal", "initial"};

/* A table of names, by index. */
typedef struct reader_names {
    const char *const *names;
    uint32_t count;
} reader_names;

static const reader_names reader_actor_state_names = {reader_actor_states, 7};
static const reader_names reader_state_kind_names = {reader_state_kinds, 6};
static const reader_names reader_transition_type_names = {reader_transition_types, 3};

static const char *reader_name_of(const reader_names *table, uint32_t index) {
    return index < table->count ? table->names[index] : "unknown";
}

/* The request's working arena, given back before the request returns. */
typedef struct reader_work {
    ta_growable_arena arena;
    ta_allocator view; /* the arena's allocator, for outputs that grow in it */
} reader_work;

/* The reader's request: its work is the struct after it (snapshot_reader's
 * own frame; request.c sees only `request`). */
typedef struct reader_request {
    request base;
    reader_work *work;
} reader_request;

static reader_work *reader_work_of(request *r) { return ((reader_request *)(void *)r)->work; }

static void *reader_room(request *r, size_t bytes) {
    void *room = ta_growable_arena_alloc(&reader_work_of(r)->arena, bytes ? bytes : READER_ALIGN, READER_ALIGN);
    if (!room) {
        request_path_set(r, (const char *)0, 0, "");
        request_refuse(r, "memory", "the allocator refused the request's working memory");
    }
    return room;
}

static void reader_put_text(request *r, ta_string_view text) { cbor_output_text(&r->out, text); }

static void reader_put_text_or_null(request *r, ta_string_view text) {
    if (text.length) cbor_output_text(&r->out, text);
    else request_put_null(r);
}

static void reader_put_map(request *r, uint32_t pairs) { cbor_output_head(&r->out, CBOR_MAJOR_MAP, pairs); }
static void reader_put_array(request *r, uint32_t items) { cbor_output_head(&r->out, CBOR_MAJOR_ARRAY, items); }

static void reader_put_bytes(request *r, ta_bytes bytes) {
    cbor_output_head(&r->out, CBOR_MAJOR_BYTES, bytes.length);
    ta_output_write(&r->out, bytes.pointer, bytes.length);
}

/* ---- params ------------------------------------------------------------------------------ */

/* An optional uint param, at most `limit`. */
static int reader_read_count(request *r, uint32_t map, const char *key, uint64_t fallback, uint64_t limit,
                             uint64_t *value) {
    request_path_set(r, (const char *)0, 0, key);
    if (!request_read_uint(r, request_member(r, map, key), fallback, value)) return 0;
    if (*value > limit) {
        request_refuse(r, "range", "more than a reply holds");
        return 0;
    }
    return 1;
}

static int reader_read_optional_text(request *r, uint32_t map, const char *key, ta_string_view *value) {
    request_path_set(r, (const char *)0, 0, key);
    return request_read_text(r, request_member(r, map, key), value);
}

/* The live segment `segment` names. */
static const reader_segment *reader_read_segment(request *r, uint32_t map, uint32_t *index) {
    uint32_t at = request_member(r, map, "segment");
    uint64_t value;
    request_path_set(r, (const char *)0, 0, "segment");
    if (at == TA_NIL) {
        request_refuse(r, "missing", "segment is a segment's index in the snapshot");
        return (const reader_segment *)0;
    }
    if (!request_read_uint(r, at, 0, &value)) return (const reader_segment *)0;
    if (value >= r->reader->segment_count || !r->reader->segments[value].system) {
        request_refuse(r, "range", "no live segment of that index in the snapshot");
        return (const reader_segment *)0;
    }
    *index = (uint32_t)value;
    return &r->reader->segments[value];
}

/* The actor `handle` names in the segment, its record in *actor. */
static int reader_read_handle(request *r, uint32_t map, const reader_segment *segment, uint32_t *handle,
                              ta_actor_info *actor) {
    uint32_t at = request_member(r, map, "handle");
    uint64_t value;
    request_path_set(r, (const char *)0, 0, "handle");
    if (at == TA_NIL) {
        request_refuse(r, "missing", "handle is an actor's handle in its segment (snapshot.handles)");
        return 0;
    }
    if (!request_read_uint(r, at, 0, &value)) return 0;
    actor->size = sizeof *actor;
    if (value > UINT32_MAX || ta_actor_info_get(segment->system, (uint32_t)value, actor) != TA_OK ||
        actor->state == TA_ACTOR_FREE) {
        request_refuse(r, "stale", "no actor of that handle in the segment");
        return 0;
    }
    *handle = (uint32_t)value;
    return 1;
}

/* ---- an actor ------------------------------------------------------------------------------ */

/* An actor's active states, read into room that is used again for the next
 * actor and grows in the working arena when it must. */
typedef struct reader_states {
    uint32_t *items;
    uint32_t count, capacity;
} reader_states;

/* 0 after refusing. */
static int reader_states_read(request *r, const reader_segment *segment, uint32_t handle, reader_states *states) {
    uint32_t count = ta_actor_config(segment->system, handle, (ta_uint32_array){(uint32_t *)0, 0});
    if (count > states->capacity) {
        uint32_t capacity = count < 16 ? 16 : count * 2;
        uint32_t *items = (uint32_t *)reader_room(r, sizeof *items * capacity);
        if (!items) return 0;
        states->items = items;
        states->capacity = capacity;
    }
    states->count = ta_actor_config(segment->system, handle, (ta_uint32_array){states->items, count});
    return 1;
}

static uint32_t reader_parent(const reader_segment *segment, uint32_t chart, uint32_t state) {
    ta_state_info info;
    info.size = sizeof info;
    return ta_image_state_info(&segment->image, (ta_chart_item){chart, state}, &info) == TA_OK ? info.parent : TA_NIL;
}

static ta_string_view reader_state_id(const reader_segment *segment, uint32_t chart, uint32_t state) {
    ta_state_info info;
    info.size = sizeof info;
    if (ta_image_state_info(&segment->image, (ta_chart_item){chart, state}, &info) != TA_OK)
        return (ta_string_view){"", 0};
    return info.id;
}

/* 1 when no other active state is a child of states[i]: a leaf of the configuration. */
static int reader_is_leaf(const reader_segment *segment, uint32_t chart, const uint32_t *states, uint32_t count,
                          uint32_t i) {
    for (uint32_t j = 0; j < count; j++)
        if (j != i && reader_parent(segment, chart, states[j]) == states[i]) return 0;
    return 1;
}

/* The ids of an actor's leaf states, as an array. */
static void reader_put_leaves(request *r, const reader_segment *segment, uint32_t chart, const uint32_t *states,
                              uint32_t count) {
    uint32_t leaves = 0;
    for (uint32_t i = 0; i < count; i++)
        leaves += (uint32_t)reader_is_leaf(segment, chart, states, count, i);
    reader_put_array(r, leaves);
    for (uint32_t i = 0; i < count; i++)
        if (reader_is_leaf(segment, chart, states, count, i)) reader_put_text(r, reader_state_id(segment, chart, states[i]));
}

/* 1 when one of the actor's active states has the id `id`. */
static int reader_in_state(const reader_segment *segment, uint32_t chart, const uint32_t *states, uint32_t count,
                           ta_string_view id) {
    for (uint32_t i = 0; i < count; i++) {
        ta_string_view state = reader_state_id(segment, chart, states[i]);
        if (state.length == id.length && ta_memcmp(state.pointer, id.pointer, id.length) == 0) return 1;
    }
    return 0;
}

static void reader_put_actor_record(request *r, const reader_segment *segment, uint32_t handle,
                                    const ta_actor_info *actor) {
    ta_actor_info parent;
    parent.size = sizeof parent;
    reader_put_map(r, 9);
    request_put_key(r, "session");
    cbor_output_uint(&r->out, actor->serial);
    request_put_key(r, "name");
    reader_put_text_or_null(r, ta_system_actor_name(segment->system, handle));
    request_put_key(r, "chart");
    cbor_output_uint(&r->out, actor->chart);
    request_put_key(r, "chartName");
    reader_put_text(r, ta_image_chart_name(&segment->image, actor->chart));
    request_put_key(r, "status");
    request_put_key(r, reader_name_of(&reader_actor_state_names, actor->state));
    request_put_key(r, "parent");
    if (actor->parent && ta_actor_info_get(segment->system, actor->parent, &parent) == TA_OK)
        cbor_output_uint(&r->out, parent.serial);
    else
        request_put_null(r);
    request_put_key(r, "mailbox");
    cbor_output_uint(&r->out, actor->mailbox_length);
    request_put_key(r, "invocations");
    cbor_output_uint(&r->out, actor->invocations);
    request_put_key(r, "services");
    cbor_output_uint(&r->out, actor->services);
}

/* An actor's queued events, as [{"event", "sendid"}] (`names_only`: as [event]). */
static int reader_put_mailbox(request *r, const reader_segment *segment, uint32_t handle, int names_only) {
    uint32_t count = ta_system_actor_mailbox(segment->system, handle, (ta_actor_queued_array){(ta_actor_queued *)0, 0});
    ta_actor_queued *queued = (ta_actor_queued *)reader_room(r, sizeof *queued * count);
    if (!queued) return 0;
    ta_system_actor_mailbox(segment->system, handle, (ta_actor_queued_array){queued, count});
    reader_put_array(r, count);
    for (uint32_t i = 0; i < count; i++) {
        if (names_only) {
            reader_put_text(r, queued[i].event);
            continue;
        }
        reader_put_map(r, 2);
        request_put_key(r, "event");
        reader_put_text(r, queued[i].event);
        request_put_key(r, "sendid");
        reader_put_text_or_null(r, queued[i].sendid);
    }
    return 1;
}

/* The milliseconds from `now` to `due` on a clock of `ticks_per_second`; 0
 * when due; UINT64_MAX when it is past what 64 bits of milliseconds hold. */
static uint64_t reader_milliseconds(uint64_t now, uint64_t due, uint64_t ticks_per_second) {
    if (due <= now || !ticks_per_second) return 0;
    uint64_t ticks = due - now, whole = ticks / ticks_per_second;
    if (whole > UINT64_MAX / READER_MILLISECONDS - 1) return UINT64_MAX;
    return whole * READER_MILLISECONDS + (ticks % ticks_per_second) * READER_MILLISECONDS / ticks_per_second;
}

/* An actor's delayed sends, as [{"due", "event", "sendid"}] (`row`: as
 * [{"event", "sendid", "dueInMs"}], from the segment's now). */
static int reader_put_timers(request *r, const reader_segment *segment, uint32_t handle, int row) {
    uint32_t count = ta_system_actor_timers(segment->system, handle, (ta_actor_timer_array){(ta_actor_timer *)0, 0});
    ta_actor_timer *timers = (ta_actor_timer *)reader_room(r, sizeof *timers * count);
    if (!timers) return 0;
    ta_system_actor_timers(segment->system, handle, (ta_actor_timer_array){timers, count});
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(segment->system, &stats);
    reader_put_array(r, count);
    for (uint32_t i = 0; i < count; i++) {
        reader_put_map(r, 3);
        if (!row) {
            request_put_key(r, "due");
            cbor_output_uint(&r->out, timers[i].due);
        }
        request_put_key(r, "event");
        reader_put_text(r, timers[i].event);
        request_put_key(r, "sendid");
        reader_put_text_or_null(r, timers[i].sendid);
        if (row) {
            request_put_key(r, "dueInMs");
            cbor_output_uint(&r->out, reader_milliseconds(stats.now, timers[i].due, stats.ticks_per_second));
        }
    }
    return 1;
}

/* ---- a chart --------------------------------------------------------------------------------- */

static int reader_put_transition(request *r, const reader_segment *segment, ta_chart_item item) {
    const ta_image *image = &segment->image;
    ta_transition_info transition;
    transition.size = sizeof transition;
    if (ta_image_transition(image, item, &transition) != TA_OK) return 0;
    uint32_t *targets = (uint32_t *)reader_room(r, sizeof *targets * transition.target_count);
    if (!targets) return 0;
    ta_image_transition_targets(image, item, (ta_uint32_array){targets, transition.target_count});
    reader_put_map(r, 5);
    request_put_key(r, "source");
    cbor_output_uint(&r->out, transition.source);
    request_put_key(r, "type");
    request_put_key(r, reader_name_of(&reader_transition_type_names, transition.type));
    request_put_key(r, "events");
    reader_put_array(r, transition.event_count);
    for (uint32_t i = 0; i < transition.event_count; i++) reader_put_text(r, ta_image_transition_event(image, item, i));
    request_put_key(r, "cond");
    ta_expression_info condition;
    if (transition.condition != TA_NIL &&
        ta_image_expression(image, (ta_chart_item){item.chart, transition.condition}, &condition) == TA_OK)
        reader_put_text(r, condition.source);
    else
        request_put_null(r);
    request_put_key(r, "targets");
    reader_put_array(r, transition.target_count);
    for (uint32_t i = 0; i < transition.target_count; i++) cbor_output_uint(&r->out, targets[i]);
    return 1;
}

static int reader_put_chart(request *r, const reader_segment *segment, uint32_t chart) {
    const ta_image *image = &segment->image;
    reader_put_map(r, 5);
    request_put_key(r, "name");
    reader_put_text(r, ta_image_chart_name(image, chart));
    request_put_key(r, "datamodel");
    reader_put_text(r, ta_image_chart_datamodel(image, chart));
    request_put_key(r, "states");
    uint32_t states = ta_image_state_count(image, chart);
    reader_put_array(r, states);
    for (uint32_t i = 0; i < states; i++) {
        ta_state_info state;
        state.size = sizeof state;
        if (ta_image_state_info(image, (ta_chart_item){chart, i}, &state) != TA_OK) return 0;
        reader_put_map(r, 3);
        request_put_key(r, "id");
        reader_put_text(r, state.id);
        request_put_key(r, "kind");
        request_put_key(r, reader_name_of(&reader_state_kind_names, state.kind));
        request_put_key(r, "parent");
        if (state.parent == TA_NIL) request_put_null(r);
        else cbor_output_uint(&r->out, state.parent);
    }
    request_put_key(r, "transitions");
    uint32_t transitions = ta_image_transition_count(image, chart);
    reader_put_array(r, transitions);
    for (uint32_t i = 0; i < transitions; i++)
        if (!reader_put_transition(r, segment, (ta_chart_item){chart, i})) return 0;
    request_put_key(r, "data");
    uint32_t data = ta_image_data_count(image, chart);
    reader_put_array(r, data);
    for (uint32_t i = 0; i < data; i++) {
        ta_data_info datum;
        if (ta_image_data(image, (ta_chart_item){chart, i}, &datum) != TA_OK) return 0;
        reader_put_text(r, datum.id);
    }
    return 1;
}

static uint32_t reader_chart_count(const reader_segment *segment) {
    ta_image_info info;
    info.size = sizeof info;
    ta_image_get_info(&segment->image, &info);
    return info.chart_count;
}

/* ---- trace history --------------------------------------------------------------------------- */

/* The spans a segment's journal kept, as an ExportTraceServiceRequest in the
 * working arena; {NULL, 0} when the segment does not trace, or after
 * refusing (r->refused). */
static ta_bytes reader_history(request *r, uint32_t index) {
    const reader_segment *segment = &r->reader->segments[index];
    ta_allocator *allocator = &reader_work_of(r)->view;
    ta_host_trace_history_input input = {sizeof input, index, segment->system, &segment->image, allocator};
    ta_buffer probe = {(void *)0, 0, 0};
    ta_status status = ta_host_trace_history(&input, &probe);
    if (status == TA_ERROR_STATE) return (ta_bytes){(const void *)0, 0};
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY) {
        request_path_set(r, (const char *)0, 0, "");
        request_refuse(r, "memory", "the trace history could not be written");
        return (ta_bytes){(const void *)0, 0};
    }
    void *room = reader_room(r, probe.length);
    if (!room) return (ta_bytes){(const void *)0, 0};
    ta_buffer history = {room, probe.length, 0};
    if (probe.length && ta_host_trace_history(&input, &history) != TA_OK) {
        request_path_set(r, (const char *)0, 0, "");
        request_refuse(r, "memory", "the trace history could not be written");
        return (ta_bytes){(const void *)0, 0};
    }
    return (ta_bytes){room, history.length};
}

/* ---- the per-segment methods: core's readers, mirrored ------------------------------------- */

static const char *const reader_segment_fields[] = {"segment", (const char *)0};
static const char *const reader_handle_fields[] = {"segment", "handle", (const char *)0};

/* The segment of a method that takes only `segment`; NULL after refusing. */
static const reader_segment *reader_segment_method(request *r, uint32_t map, uint32_t *index) {
    if (!request_keys_known(r, map, (const char *)0, 0, reader_segment_fields)) return (const reader_segment *)0;
    return reader_read_segment(r, map, index);
}

/* The segment and actor of a method that takes `segment` and `handle`. */
static const reader_segment *reader_actor_method(request *r, uint32_t map, uint32_t *handle, ta_actor_info *actor) {
    uint32_t index;
    if (!request_keys_known(r, map, (const char *)0, 0, reader_handle_fields)) return (const reader_segment *)0;
    const reader_segment *segment = reader_read_segment(r, map, &index);
    if (!segment || !reader_read_handle(r, map, segment, handle, actor)) return (const reader_segment *)0;
    return segment;
}

static void reader_run_header(request *r, uint32_t map) {
    static const char *const fields[] = {(const char *)0};
    if (!request_keys_known(r, map, (const char *)0, 0, fields)) return;
    const ta_host_snapshot_contents *contents = &r->reader->contents;
    reader_put_map(r, 5);
    request_put_key(r, "systemID");
    reader_put_text(r, contents->system_id);
    request_put_key(r, "datamodel");
    reader_put_text(r, contents->datamodel);
    request_put_key(r, "time");
    cbor_output_uint(&r->out, contents->time);
    request_put_key(r, "segments");
    cbor_output_uint(&r->out, r->reader->segment_count);
    request_put_key(r, "state");
    reader_put_bytes(r, (ta_bytes){contents->state, contents->state_length});
}

static void reader_run_stats(request *r, uint32_t map) {
    uint32_t index;
    const reader_segment *segment = reader_segment_method(r, map, &index);
    if (!segment) return;
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(segment->system, &stats);
    const struct {
        const char *key;
        uint64_t value;
    } fields[] = {
        {"now", stats.now},       {"ticksPerSecond", stats.ticks_per_second}, {"nextDeadline", stats.next_deadline},
        {"actors", stats.actors}, {"hibernated", stats.hibernated},           {"runnable", stats.runnable},
        {"events", stats.events}, {"timers", stats.timers},                   {"requests", stats.requests},
        {"regionBytes", stats.region_bytes}, {"wallNanoseconds", stats.wall_ns},
    };
    reader_put_map(r, (uint32_t)(sizeof fields / sizeof fields[0]));
    for (uint32_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        request_put_key(r, fields[i].key);
        cbor_output_uint(&r->out, fields[i].value);
    }
}

/* Each actor's handle of a segment, in the system's order; `count` only counts. */
static uint32_t reader_handles(const reader_segment *segment, request *r, int count) {
    uint32_t cursor = 0, handles[READER_HANDLES], total = 0;
    for (uint32_t got; (got = ta_system_actors(segment->system, &cursor, (ta_uint32_array){handles, READER_HANDLES})) != 0;) {
        total += got;
        if (!count)
            for (uint32_t i = 0; i < got; i++) cbor_output_uint(&r->out, handles[i]);
    }
    return total;
}

static void reader_run_handles(request *r, uint32_t map) {
    uint32_t index;
    const reader_segment *segment = reader_segment_method(r, map, &index);
    if (!segment) return;
    reader_put_array(r, reader_handles(segment, r, 1));
    reader_handles(segment, r, 0);
}

static void reader_run_actor(request *r, uint32_t map) {
    uint32_t handle;
    ta_actor_info actor;
    const reader_segment *segment = reader_actor_method(r, map, &handle, &actor);
    if (segment) reader_put_actor_record(r, segment, handle, &actor);
}

static void reader_run_configuration(request *r, uint32_t map) {
    uint32_t handle;
    ta_actor_info actor;
    reader_states states = {(uint32_t *)0, 0, 0};
    const reader_segment *segment = reader_actor_method(r, map, &handle, &actor);
    if (!segment || !reader_states_read(r, segment, handle, &states)) return;
    reader_put_array(r, states.count);
    for (uint32_t i = 0; i < states.count; i++) cbor_output_uint(&r->out, states.items[i]);
}

static void reader_run_mailbox(request *r, uint32_t map) {
    uint32_t handle;
    ta_actor_info actor;
    const reader_segment *segment = reader_actor_method(r, map, &handle, &actor);
    if (segment) reader_put_mailbox(r, segment, handle, 0);
}

static void reader_run_timers(request *r, uint32_t map) {
    uint32_t handle;
    ta_actor_info actor;
    const reader_segment *segment = reader_actor_method(r, map, &handle, &actor);
    if (segment) reader_put_timers(r, segment, handle, 0);
}

static void reader_run_charts(request *r, uint32_t map) {
    uint32_t index;
    const reader_segment *segment = reader_segment_method(r, map, &index);
    if (!segment) return;
    uint32_t charts = reader_chart_count(segment);
    reader_put_array(r, charts);
    for (uint32_t c = 0; c < charts; c++)
        if (!reader_put_chart(r, segment, c)) {
            request_path_set(r, (const char *)0, 0, "");
            request_refuse(r, "malformed", "an image of the snapshot could not be read");
            return;
        }
}

static void reader_put_kept(request *r, const ta_trace_kept *kept) {
    reader_put_map(r, 5);
    request_put_key(r, "records");
    cbor_output_uint(&r->out, kept->records);
    request_put_key(r, "acknowledgedRecords");
    cbor_output_uint(&r->out, kept->acknowledged_records);
    request_put_key(r, "bytes");
    cbor_output_uint(&r->out, (uint64_t)kept->first.length + kept->second.length);
    request_put_key(r, "acknowledgedBytes");
    cbor_output_uint(&r->out, kept->acknowledged);
    request_put_key(r, "lost");
    cbor_output_uint(&r->out, kept->lost);
}

static void reader_run_trace_kept(request *r, uint32_t map) {
    uint32_t index;
    const reader_segment *segment = reader_segment_method(r, map, &index);
    if (!segment) return;
    ta_trace_kept kept;
    kept.size = sizeof kept;
    if (ta_system_trace_kept(segment->system, &kept) != TA_OK) request_put_null(r);
    else reader_put_kept(r, &kept);
}

static void reader_run_traces(request *r, uint32_t map) {
    uint32_t index;
    const reader_segment *segment = reader_segment_method(r, map, &index);
    if (!segment) return;
    ta_bytes history = reader_history(r, index);
    if (r->refused) return;
    ta_trace_kept kept;
    kept.size = sizeof kept;
    if (ta_system_trace_kept(segment->system, &kept) != TA_OK) request_put_null(r);
    else reader_put_bytes(r, history);
}

/* ---- the whole snapshot --------------------------------------------------------------------- */

/* A count of actors resting in one configuration of one chart. */
typedef struct reader_tally {
    ta_string_view chart, key;
    uint64_t count;
} reader_tally;

typedef struct reader_tallies {
    reader_tally *items;
    uint32_t count, capacity;
} reader_tallies;

static int reader_tally_add(request *r, reader_tallies *tallies, ta_string_view chart, ta_bytes key) {
    for (uint32_t i = 0; i < tallies->count; i++) {
        reader_tally *t = &tallies->items[i];
        if (t->chart.pointer == chart.pointer && t->key.length == key.length &&
            ta_memcmp(t->key.pointer, key.pointer, key.length) == 0) {
            t->count++;
            return 1;
        }
    }
    if (tallies->count == tallies->capacity) {
        uint32_t capacity = tallies->capacity ? tallies->capacity * 2 : 16;
        reader_tally *items = (reader_tally *)reader_room(r, sizeof *items * capacity);
        if (!items) return 0;
        if (tallies->count) ta_memcpy(items, tallies->items, sizeof *items * tallies->count);
        tallies->items = items;
        tallies->capacity = capacity;
    }
    char *text = (char *)reader_room(r, key.length);
    if (!text) return 0;
    ta_memcpy(text, key.pointer, key.length);
    tallies->items[tallies->count++] = (reader_tally){chart, {text, (uint32_t)key.length}, 1};
    return 1;
}

static int reader_same_text(ta_string_view a, ta_string_view b) {
    return a.length == b.length && ta_memcmp(a.pointer, b.pointer, a.length) == 0;
}

/* The chart's name as the tallies hold it: the first chart of that name, so
 * a chart in two segments is one key. */
static ta_string_view reader_tally_chart(const reader_tallies *tallies, ta_string_view chart) {
    for (uint32_t i = 0; i < tallies->count; i++)
        if (reader_same_text(tallies->items[i].chart, chart)) return tallies->items[i].chart;
    return chart;
}

/* "(hibernated)", or an actor's leaf states joined by "+", into `key`. */
static void reader_rest_key(const reader_segment *segment, const ta_actor_info *actor, const uint32_t *states,
                            uint32_t count, ta_output *key) {
    key->length = 0;
    if (actor->state == TA_ACTOR_HIBERNATED) {
        ta_string_view text = request_text_of("(hibernated)");
        ta_output_write(key, text.pointer, text.length);
        return;
    }
    int first = 1;
    for (uint32_t i = 0; i < count; i++) {
        if (!reader_is_leaf(segment, actor->chart, states, count, i)) continue;
        if (!first) ta_output_write(key, "+", 1);
        ta_string_view id = reader_state_id(segment, actor->chart, states[i]);
        ta_output_write(key, id.pointer, id.length);
        first = 0;
    }
}

/* Counts of actors per chart and resting configuration, over every live segment. */
static int reader_count_rest(request *r, reader_tallies *tallies) {
    ta_output key = ta_output_over((ta_buffer){(void *)0, 0, 0}, &reader_work_of(r)->view);
    reader_states states = {(uint32_t *)0, 0, 0};
    for (uint32_t s = 0; s < r->reader->segment_count; s++) {
        const reader_segment *segment = &r->reader->segments[s];
        if (!segment->system) continue;
        uint32_t cursor = 0, handles[READER_HANDLES];
        for (uint32_t got; (got = ta_system_actors(segment->system, &cursor, (ta_uint32_array){handles, READER_HANDLES})) != 0;) {
            for (uint32_t i = 0; i < got; i++) {
                ta_actor_info actor;
                actor.size = sizeof actor;
                if (ta_actor_info_get(segment->system, handles[i], &actor) != TA_OK) continue;
                if (!reader_states_read(r, segment, handles[i], &states)) return 0;
                reader_rest_key(segment, &actor, states.items, states.count, &key);
                if (!ta_output_fits(&key)) {
                    request_path_set(r, (const char *)0, 0, "");
                    request_refuse(r, "memory", "the allocator refused the request's working memory");
                    return 0;
                }
                ta_string_view chart = reader_tally_chart(tallies, ta_image_chart_name(&segment->image, actor.chart));
                if (!reader_tally_add(r, tallies, chart, (ta_bytes){key.bytes, key.length})) return 0;
            }
        }
    }
    return 1;
}

static void reader_run_summary(request *r, uint32_t map) {
    static const char *const fields[] = {(const char *)0};
    if (!request_keys_known(r, map, (const char *)0, 0, fields)) return;
    reader_tallies tallies = {(reader_tally *)0, 0, 0};
    if (!reader_count_rest(r, &tallies)) return;
    const ta_host_snapshot_reader *reader = r->reader;
    reader_put_map(r, 5);
    request_put_key(r, "systemID");
    reader_put_text(r, reader->contents.system_id);
    request_put_key(r, "time");
    cbor_output_uint(&r->out, reader->contents.time);
    request_put_key(r, "segments");
    reader_put_array(r, reader->segment_count);
    ta_trace_kept sum;
    ta_memset(&sum, 0, sizeof sum);
    int traced = 0;
    for (uint32_t s = 0; s < reader->segment_count; s++) {
        const reader_segment *segment = &reader->segments[s];
        reader_put_map(r, segment->system ? 4u : 2u);
        request_put_key(r, "segment");
        cbor_output_uint(&r->out, s);
        request_put_key(r, "live");
        request_put_bool(r, segment->system != (ta_system *)0);
        if (!segment->system) continue;
        request_put_key(r, "charts");
        uint32_t charts = reader_chart_count(segment);
        reader_put_array(r, charts);
        for (uint32_t c = 0; c < charts; c++) reader_put_text(r, ta_image_chart_name(&segment->image, c));
        request_put_key(r, "actors");
        cbor_output_uint(&r->out, reader_handles(segment, r, 1));
        ta_trace_kept kept;
        kept.size = sizeof kept;
        if (ta_system_trace_kept(segment->system, &kept) == TA_OK) {
            traced = 1;
            sum.records += kept.records;
            sum.acknowledged_records += kept.acknowledged_records;
            sum.first.length += kept.first.length + kept.second.length;
            sum.acknowledged += kept.acknowledged;
            sum.lost += kept.lost;
        }
    }
    /* {chart: {configuration: actors}}, charts and configurations in the order first met */
    request_put_key(r, "states");
    uint32_t charts = 0;
    for (uint32_t i = 0; i < tallies.count; i++) {
        int seen = 0;
        for (uint32_t j = 0; j < i && !seen; j++) seen = tallies.items[j].chart.pointer == tallies.items[i].chart.pointer;
        charts += (uint32_t)!seen;
    }
    reader_put_map(r, charts);
    for (uint32_t i = 0; i < tallies.count; i++) {
        int seen = 0;
        for (uint32_t j = 0; j < i && !seen; j++) seen = tallies.items[j].chart.pointer == tallies.items[i].chart.pointer;
        if (seen) continue;
        uint32_t keys = 0;
        for (uint32_t j = i; j < tallies.count; j++) keys += (uint32_t)(tallies.items[j].chart.pointer == tallies.items[i].chart.pointer);
        reader_put_text(r, tallies.items[i].chart);
        reader_put_map(r, keys);
        for (uint32_t j = i; j < tallies.count; j++) {
            if (tallies.items[j].chart.pointer != tallies.items[i].chart.pointer) continue;
            reader_put_text(r, tallies.items[j].key);
            cbor_output_uint(&r->out, tallies.items[j].count);
        }
    }
    request_put_key(r, "traces");
    if (!traced) {
        request_put_null(r);
        return;
    }
    reader_put_map(r, 4);
    request_put_key(r, "records");
    cbor_output_uint(&r->out, sum.records);
    request_put_key(r, "acknowledgedRecords");
    cbor_output_uint(&r->out, sum.acknowledged_records);
    request_put_key(r, "bytes");
    cbor_output_uint(&r->out, sum.first.length);
    request_put_key(r, "lost");
    cbor_output_uint(&r->out, sum.lost);
}

/* An actor that passed snapshot.actors' filters. */
typedef struct reader_row {
    uint64_t session;
    uint32_t segment, handle;
} reader_row;

typedef struct reader_rows {
    reader_row *items;
    uint32_t count, capacity;
} reader_rows;

static int reader_rows_add(request *r, reader_rows *rows, reader_row row) {
    if (rows->count == rows->capacity) {
        uint32_t capacity = rows->capacity ? rows->capacity * 2 : 64;
        reader_row *items = (reader_row *)reader_room(r, sizeof *items * capacity);
        if (!items) return 0;
        if (rows->count) ta_memcpy(items, rows->items, sizeof *items * rows->count);
        rows->items = items;
        rows->capacity = capacity;
    }
    rows->items[rows->count++] = row;
    return 1;
}

/* Heapsort by session: in place, no recursion. */
static void reader_rows_sift(reader_row *items, uint32_t start, uint32_t end) {
    uint32_t root = start;
    for (;;) {
        uint32_t child = root * 2 + 1;
        if (child >= end) return;
        if (child + 1 < end && items[child + 1].session > items[child].session) child++;
        if (items[root].session >= items[child].session) return;
        reader_row swap = items[root];
        items[root] = items[child];
        items[child] = swap;
        root = child;
    }
}

static void reader_rows_sort(reader_rows *rows) {
    if (rows->count < 2) return;
    for (uint32_t start = rows->count / 2; start-- > 0;) reader_rows_sift(rows->items, start, rows->count);
    for (uint32_t end = rows->count - 1; end > 0; end--) {
        reader_row swap = rows->items[0];
        rows->items[0] = rows->items[end];
        rows->items[end] = swap;
        reader_rows_sift(rows->items, 0, end);
    }
}

/* snapshot.actors' filters: an absent one matches every actor. */
typedef struct reader_filter {
    ta_string_view chart, state, status, name;
} reader_filter;

static int reader_has_prefix(ta_string_view text, ta_string_view prefix) {
    return text.length >= prefix.length && ta_memcmp(text.pointer, prefix.pointer, prefix.length) == 0;
}

/* What reader_matches reads: the filters, and room for an actor's states. */
typedef struct reader_match {
    const reader_filter *filter;
    reader_states *states;
} reader_match;

static int reader_matches(request *r, const reader_segment *segment, uint32_t handle, const reader_match *match,
                          const ta_actor_info *actor) {
    const reader_filter *filter = match->filter;
    if (filter->chart.length && !reader_same_text(ta_image_chart_name(&segment->image, actor->chart), filter->chart))
        return 0;
    if (filter->status.length &&
        !reader_same_text(request_text_of(reader_name_of(&reader_actor_state_names, actor->state)), filter->status))
        return 0;
    if (filter->name.length && !reader_has_prefix(ta_system_actor_name(segment->system, handle), filter->name))
        return 0;
    if (!filter->state.length) return 1;
    return reader_states_read(r, segment, handle, match->states) &&
           reader_in_state(segment, actor->chart, match->states->items, match->states->count, filter->state);
}

static int reader_put_row(request *r, const reader_row *row, reader_states *states) {
    const reader_segment *segment = &r->reader->segments[row->segment];
    ta_actor_info actor;
    actor.size = sizeof actor;
    if (ta_actor_info_get(segment->system, row->handle, &actor) != TA_OK) return 0;
    if (!reader_states_read(r, segment, row->handle, states)) return 0;
    reader_put_map(r, 9);
    request_put_key(r, "session");
    cbor_output_uint(&r->out, actor.serial);
    request_put_key(r, "name");
    reader_put_text_or_null(r, ta_system_actor_name(segment->system, row->handle));
    request_put_key(r, "chart");
    reader_put_text(r, ta_image_chart_name(&segment->image, actor.chart));
    request_put_key(r, "segment");
    cbor_output_uint(&r->out, row->segment);
    request_put_key(r, "status");
    request_put_key(r, reader_name_of(&reader_actor_state_names, actor.state));
    request_put_key(r, "states");
    reader_put_leaves(r, segment, actor.chart, states->items, states->count);
    request_put_key(r, "mailbox");
    cbor_output_uint(&r->out, actor.mailbox_length);
    request_put_key(r, "queued");
    if (!reader_put_mailbox(r, segment, row->handle, 1)) return 0;
    request_put_key(r, "delayed");
    return reader_put_timers(r, segment, row->handle, 1);
}

static void reader_run_actors(request *r, uint32_t map) {
    static const char *const fields[] = {"chart", "state", "status", "name", "offset", "limit", (const char *)0};
    if (!request_keys_known(r, map, (const char *)0, 0, fields)) return;
    reader_filter filter;
    uint64_t offset, limit;
    if (!reader_read_optional_text(r, map, "chart", &filter.chart) ||
        !reader_read_optional_text(r, map, "state", &filter.state) ||
        !reader_read_optional_text(r, map, "status", &filter.status) ||
        !reader_read_optional_text(r, map, "name", &filter.name) ||
        !reader_read_count(r, map, "offset", 0, UINT32_MAX, &offset) ||
        !reader_read_count(r, map, "limit", READER_ROWS_DEFAULT, READER_ROWS_LIMIT, &limit))
        return;
    reader_rows rows = {(reader_row *)0, 0, 0};
    reader_states states = {(uint32_t *)0, 0, 0};
    reader_match match = {&filter, &states};
    for (uint32_t s = 0; s < r->reader->segment_count; s++) {
        const reader_segment *segment = &r->reader->segments[s];
        if (!segment->system) continue;
        uint32_t cursor = 0, handles[READER_HANDLES];
        for (uint32_t got; (got = ta_system_actors(segment->system, &cursor, (ta_uint32_array){handles, READER_HANDLES})) != 0;)
            for (uint32_t i = 0; i < got; i++) {
                ta_actor_info actor;
                actor.size = sizeof actor;
                if (ta_actor_info_get(segment->system, handles[i], &actor) != TA_OK) continue;
                if (!reader_matches(r, segment, handles[i], &match, &actor)) {
                    if (r->refused) return;
                    continue;
                }
                if (!reader_rows_add(r, &rows, (reader_row){actor.serial, s, handles[i]})) return;
            }
    }
    reader_rows_sort(&rows);
    uint32_t first = offset < rows.count ? (uint32_t)offset : rows.count;
    uint32_t shown = rows.count - first < limit ? rows.count - first : (uint32_t)limit;
    reader_put_map(r, 3);
    request_put_key(r, "total");
    cbor_output_uint(&r->out, rows.count);
    request_put_key(r, "offset");
    cbor_output_uint(&r->out, offset);
    request_put_key(r, "actors");
    reader_put_array(r, shown);
    for (uint32_t i = 0; i < shown; i++)
        if (!reader_put_row(r, &rows.items[first + i], &states)) {
            if (!r->refused) {
                request_path_set(r, (const char *)0, 0, "");
                request_refuse(r, "malformed", "an actor of the snapshot could not be read");
            }
            return;
        }
}

static void reader_run_chart(request *r, uint32_t map) {
    static const char *const fields[] = {"chart", (const char *)0};
    if (!request_keys_known(r, map, (const char *)0, 0, fields)) return;
    uint32_t at = request_member(r, map, "chart");
    ta_string_view name;
    request_path_set(r, (const char *)0, 0, "chart");
    if (at == TA_NIL) {
        request_refuse(r, "missing", "chart is a chart's name");
        return;
    }
    if (!request_read_text(r, at, &name)) return;
    /* the newest live segment that holds the chart */
    uint32_t index = TA_NIL, chart = TA_NIL;
    for (uint32_t s = r->reader->segment_count; s-- > 0 && index == TA_NIL;) {
        const reader_segment *segment = &r->reader->segments[s];
        if (!segment->system) continue;
        chart = ta_image_chart_by_name(&segment->image, name);
        if (chart != TA_NIL) index = s;
    }
    if (index == TA_NIL) {
        request_refuse(r, "stale", "no live segment holds a chart of that name");
        return;
    }
    const reader_segment *segment = &r->reader->segments[index];
    const ta_image *image = &segment->image;
    uint32_t states = ta_image_state_count(image, chart);
    uint64_t *actors = (uint64_t *)reader_room(r, sizeof *actors * (states ? states : 1u));
    if (!actors) return;
    ta_memset(actors, 0, sizeof *actors * (states ? states : 1u));
    uint32_t cursor = 0, handles[READER_HANDLES];
    reader_states active = {(uint32_t *)0, 0, 0};
    for (uint32_t got; (got = ta_system_actors(segment->system, &cursor, (ta_uint32_array){handles, READER_HANDLES})) != 0;)
        for (uint32_t i = 0; i < got; i++) {
            ta_actor_info actor;
            actor.size = sizeof actor;
            if (ta_actor_info_get(segment->system, handles[i], &actor) != TA_OK || actor.chart != chart) continue;
            if (!reader_states_read(r, segment, handles[i], &active)) return;
            for (uint32_t k = 0; k < active.count; k++)
                if (active.items[k] < states) actors[active.items[k]]++;
        }
    reader_put_map(r, 5);
    request_put_key(r, "segment");
    cbor_output_uint(&r->out, index);
    request_put_key(r, "datamodel");
    reader_put_text(r, ta_image_chart_datamodel(image, chart));
    request_put_key(r, "data");
    uint32_t data = ta_image_data_count(image, chart);
    reader_put_array(r, data);
    for (uint32_t i = 0; i < data; i++) {
        ta_data_info datum;
        if (ta_image_data(image, (ta_chart_item){chart, i}, &datum) != TA_OK) datum.id = (ta_string_view){"", 0};
        reader_put_text(r, datum.id);
    }
    /* the states but the root, with the actors in each */
    request_put_key(r, "states");
    reader_put_array(r, states ? states - 1 : 0);
    for (uint32_t i = 0; i < states; i++) {
        ta_state_info state;
        state.size = sizeof state;
        if (ta_image_state_info(image, (ta_chart_item){chart, i}, &state) != TA_OK || state.parent == TA_NIL) continue;
        reader_put_map(r, 4);
        request_put_key(r, "id");
        reader_put_text(r, state.id);
        request_put_key(r, "kind");
        request_put_key(r, reader_name_of(&reader_state_kind_names, state.kind));
        request_put_key(r, "parent");
        if (reader_parent(segment, chart, state.parent) == TA_NIL) request_put_null(r);
        else reader_put_text(r, reader_state_id(segment, chart, state.parent));
        request_put_key(r, "actors");
        cbor_output_uint(&r->out, actors[i]);
    }
    /* the transitions but the initial ones, by state ids */
    request_put_key(r, "transitions");
    uint32_t transitions = ta_image_transition_count(image, chart), shown = 0;
    for (uint32_t i = 0; i < transitions; i++) {
        ta_transition_info transition;
        transition.size = sizeof transition;
        if (ta_image_transition(image, (ta_chart_item){chart, i}, &transition) == TA_OK &&
            transition.type != TA_TRANSITION_INITIAL)
            shown++;
    }
    reader_put_array(r, shown);
    for (uint32_t i = 0; i < transitions; i++) {
        ta_chart_item item = {chart, i};
        ta_transition_info transition;
        transition.size = sizeof transition;
        if (ta_image_transition(image, item, &transition) != TA_OK || transition.type == TA_TRANSITION_INITIAL) continue;
        uint32_t *targets = (uint32_t *)reader_room(r, sizeof *targets * transition.target_count);
        if (!targets) return;
        ta_image_transition_targets(image, item, (ta_uint32_array){targets, transition.target_count});
        reader_put_map(r, 4);
        request_put_key(r, "from");
        reader_put_text(r, reader_state_id(segment, chart, transition.source));
        request_put_key(r, "events");
        reader_put_array(r, transition.event_count);
        for (uint32_t e = 0; e < transition.event_count; e++) reader_put_text(r, ta_image_transition_event(image, item, e));
        request_put_key(r, "cond");
        ta_expression_info condition;
        if (transition.condition != TA_NIL &&
            ta_image_expression(image, (ta_chart_item){chart, transition.condition}, &condition) == TA_OK)
            reader_put_text(r, condition.source);
        else
            request_put_null(r);
        request_put_key(r, "to");
        reader_put_array(r, transition.target_count);
        for (uint32_t t = 0; t < transition.target_count; t++) reader_put_text(r, reader_state_id(segment, chart, targets[t]));
    }
}

/* The spans snapshot.spans keeps: a session's (0: any) whose name starts so. */
typedef struct reader_span_filter {
    uint64_t session;
    ta_string_view name;
} reader_span_filter;

static int reader_span_session_is(const ta_otlp_span *span, uint64_t session) {
    ta_otlp_key_value pair;
    uint32_t at = 0;
    ta_string_view key = request_text_of("scxml.session_id");
    while (ta_otlp_next_attribute(span->attributes, &at, &pair) == 1)
        if (reader_same_text(pair.key, key))
            return pair.value.kind == TA_OTLP_VALUE_INT && pair.value.integer >= 0 &&
                   (uint64_t)pair.value.integer == session;
    return 0;
}

static int reader_span_matches(const ta_otlp_span *span, const reader_span_filter *filter) {
    if (filter->name.length && !reader_has_prefix(span->name, filter->name)) return 0;
    return !filter->session || reader_span_session_is(span, filter->session);
}

/* The spans of `histories` that match, counted; past the first `skip`,
 * written (`write`). */
typedef struct reader_span_pass {
    const ta_bytes *histories;
    uint32_t count;
    int write;
    uint64_t skip;
} reader_span_pass;

static uint64_t reader_spans_pass(request *r, const reader_span_filter *filter, const reader_span_pass *pass) {
    uint64_t matched = 0;
    for (uint32_t h = 0; h < pass->count; h++) {
        ta_otlp_cursor cursor;
        ta_otlp_span span;
        ta_memset(&cursor, 0, sizeof cursor);
        while (ta_otlp_next_span(pass->histories[h], &cursor, &span) == 1) {
            if (!reader_span_matches(&span, filter)) continue;
            if (pass->write && matched >= pass->skip) request_put_span(r, &span);
            matched++;
        }
    }
    return matched;
}

static void reader_run_spans(request *r, uint32_t map) {
    static const char *const fields[] = {"session", "name", "limit", (const char *)0};
    if (!request_keys_known(r, map, (const char *)0, 0, fields)) return;
    reader_span_filter filter;
    uint64_t limit;
    if (!reader_read_count(r, map, "session", 0, UINT64_MAX, &filter.session) ||
        !reader_read_optional_text(r, map, "name", &filter.name) ||
        !reader_read_count(r, map, "limit", READER_SPANS_DEFAULT, READER_SPANS_LIMIT, &limit))
        return;
    const ta_host_snapshot_reader *reader = r->reader;
    ta_bytes *histories = (ta_bytes *)reader_room(r, sizeof *histories * (reader->segment_count ? reader->segment_count : 1u));
    if (!histories) return;
    uint32_t traced = 0;
    ta_trace_kept sum;
    ta_memset(&sum, 0, sizeof sum);
    for (uint32_t s = 0; s < reader->segment_count; s++) {
        const reader_segment *segment = &reader->segments[s];
        ta_trace_kept kept;
        kept.size = sizeof kept;
        if (!segment->system || ta_system_trace_kept(segment->system, &kept) != TA_OK) continue;
        ta_bytes history = reader_history(r, s);
        if (r->refused) return;
        histories[traced++] = history;
        sum.records += kept.records;
        sum.acknowledged_records += kept.acknowledged_records;
        sum.first.length += kept.first.length + kept.second.length;
        sum.lost += kept.lost;
    }
    if (!traced) {
        request_path_set(r, (const char *)0, 0, "");
        request_refuse(r, "state", "the snapshot holds no trace history: its systems do not trace");
        return;
    }
    reader_span_pass pass = {histories, traced, 0, 0};
    uint64_t total = reader_spans_pass(r, &filter, &pass);
    pass.write = 1;
    pass.skip = total > limit ? total - limit : 0;
    reader_put_map(r, 3);
    request_put_key(r, "total");
    cbor_output_uint(&r->out, total);
    request_put_key(r, "retained");
    reader_put_map(r, 4);
    request_put_key(r, "records");
    cbor_output_uint(&r->out, sum.records);
    request_put_key(r, "acknowledgedRecords");
    cbor_output_uint(&r->out, sum.acknowledged_records);
    request_put_key(r, "bytes");
    cbor_output_uint(&r->out, sum.first.length);
    request_put_key(r, "lost");
    cbor_output_uint(&r->out, sum.lost);
    request_put_key(r, "spans");
    reader_put_array(r, (uint32_t)(total - pass.skip));
    reader_spans_pass(r, &filter, &pass);
}

/* ---- the entry point -------------------------------------------------------------------------- */

static const request_method READER_METHODS[] = {
    {"snapshot.header", reader_run_header},
    {"snapshot.stats", reader_run_stats},
    {"snapshot.handles", reader_run_handles},
    {"snapshot.actor", reader_run_actor},
    {"snapshot.configuration", reader_run_configuration},
    {"snapshot.mailbox", reader_run_mailbox},
    {"snapshot.timers", reader_run_timers},
    {"snapshot.charts", reader_run_charts},
    {"snapshot.traceKept", reader_run_trace_kept},
    {"snapshot.traces", reader_run_traces},
    {"snapshot.summary", reader_run_summary},
    {"snapshot.actors", reader_run_actors},
    {"snapshot.chart", reader_run_chart},
    {"snapshot.spans", reader_run_spans},
};

ta_status ta_host_snapshot_reader_request(const ta_host_snapshot_reader *reader,
                                          const ta_host_snapshot_request *message, ta_buffer *reply) {
    if (!reader || !message || message->size < sizeof *message || !message->allocator || !reply)
        return TA_ERROR_INVALID;
    reader_work work;
    ta_growable_arena_config config = {sizeof config, 0, READER_CHUNK, message->allocator};
    ta_growable_arena_init(&work.arena, &config);
    work.view = ta_growable_arena_allocator(&work.arena);
    ta_cbor_token tokens[TA_HOST_REQUEST_TOKENS];
    ta_cbor_document document = {message->message, tokens, TA_HOST_REQUEST_TOKENS, 0};
    reader_request frame;
    ta_memset(&frame, 0, sizeof frame);
    frame.work = &work;
    request *r = &frame.base;
    r->reader = reader;
    r->document = &document;
    r->id = TA_NIL;
    r->replies = 1;
    r->prefix = "params.";
    r->out = ta_output_over((ta_buffer){(void *)0, 0, 0}, &work.view);
    request_methods methods = {READER_METHODS, sizeof READER_METHODS / sizeof READER_METHODS[0]};
    ta_status status = TA_OK;
    if (request_answer(r, methods)) {
        reply->length = 0; /* a notification: nothing to write */
    } else if (!ta_output_fits(&r->out)) {
        reply->length = 0;
        status = TA_ERROR_NO_MEMORY;
    } else if (r->out.length > reply->capacity) {
        reply->length = r->out.length;
        status = TA_ERROR_NO_MEMORY;
    } else {
        if (r->out.length) ta_memcpy(reply->pointer, r->out.bytes, r->out.length);
        reply->length = r->out.length;
    }
    ta_growable_arena_free(&work.arena);
    return status;
}
