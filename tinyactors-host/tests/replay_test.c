/*
 * The kernel's replay (kernel ADR 0002 sections 6 and 7), driven as a
 * binding drives it: open, push the log in pieces, prepare, run, finish,
 * ta_actor_publish() with the published hook, then redeliver.
 *
 * - A life replays whatever the pieces the log is cut into: the actor is
 *   rebuilt in the state the live one reached, windows answer MORE, a
 *   record not all there answers NEED, a final state FINISHED, and the
 *   messages accepted and never delivered go back in its mailbox with
 *   their IDs and data, delivered with no second ACCEPTED and record
 *   numbers that continue the log's.
 * - A checkpoint round-trips: restored from it alone, the actor has the
 *   configuration, held send and owed mail it recorded, and its own
 *   checkpoint is the same bytes but for the clock; records after it that it
 *   covers are skipped, and a checkpoint inside a log is skipped. What
 *   tah_checkpoint_write refuses, and a sink that refuses keeps nothing.
 * - Two lives at one address: ENDED at DESTROYED, LIFE at the next
 *   `spawned`, and the last life is the one published.
 * - Bytes that are no log, a life that starts with anything but its first
 *   record and a delivery never accepted are CORRUPT; a checkpoint of
 *   another revision is DEFINITION_MISMATCH.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/inputs.h"
#include "tinyactors_host/replay.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "tinyactors_host/wal.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })
#define STR(text) { .ptr = (text), .len = sizeof(text) - 1u }
#define END { 0 }

/* ------------------------------------------------------------------------
 * The platform's allocator, which fails its `fail_at`th allocation once
 * (counting from 1; 0 fails none).
 */

static uint64_t allocations;
static uint64_t fail_at;
static bool failed;

static void *failing_alloc(void *self, size_t size, size_t alignment) {
    allocations += 1u;
    if (fail_at != 0 && allocations == fail_at) {
        failed = true;
        return NULL;
    }
    return check_platform_alloc(self, size, alignment);
}

static const ta_allocator_vtable failing_vtable = { .alloc = failing_alloc, .free = check_platform_free };

static tah_platform failing_platform(void) {
    tah_platform platform = check_platform();
    platform.allocator = (ta_allocator){ .self = NULL, .v = &failing_vtable };
    return platform;
}

/* ------------------------------------------------------------------------
 * Values as text, and a codec that writes and reads them.
 */

typedef struct {
    uint32_t refs;
    size_t length;
    char bytes[];
} text_box;

static void text_retain(void *self) {
    ((text_box *)self)->refs += 1u;
}
static void text_release(void *self) {
    text_box *box = self;
    if (--box->refs == 0) {
        free(box);
    }
}
static const ta_value_vtable text_vtable = { .retain = text_retain, .release = text_release };

static ta_value text_value_of(const char *bytes, size_t length) {
    text_box *box = malloc(sizeof *box + length);
    CHECK(box != NULL);
    box->refs = 1;
    box->length = length;
    memcpy(box->bytes, bytes, length);
    return (ta_value){ .self = box, .v = &text_vtable };
}

static ta_value text_value(const char *bytes) {
    return text_value_of(bytes, strlen(bytes));
}

static bool value_is(const ta_value *value, const char *expected) {
    if (value->v != &text_vtable) {
        return false;
    }
    const text_box *box = value->self;
    return box->length == strlen(expected) && memcmp(box->bytes, expected, box->length) == 0;
}

static ta_op_status text_write(void *self, const ta_datamodel_value_write_params *params) {
    (void)self;
    const text_box *box = params->value->self;
    return params->sink.v->write(params->sink.self, (ta_bytes){ .ptr = (const uint8_t *)box->bytes, .len = (uint32_t)box->length });
}

static ta_op_status text_read(void *self, const ta_datamodel_value_read_params *params) {
    (void)self;
    char buffer[1024];
    size_t length = 0;
    for (;;) {
        uint32_t got = 0;
        ta_op_status status = params->source.v->read(params->source.self, &(ta_byte_read_params){
                                                                              .buffer = (uint8_t *)buffer + length,
                                                                              .capacity = (uint32_t)(sizeof buffer - length),
                                                                              .got = &got });
        if (status != TA_OP_OK) {
            return status;
        }
        if (got == 0) {
            break;
        }
        length += got;
    }
    *params->out = text_value_of(buffer, length);
    return TA_OP_OK;
}

static const ta_datamodel_vtable codec_vtable = { .value_write = text_write, .value_read = text_read };

static void no_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
}

/* ------------------------------------------------------------------------
 * Charts, on the null data model.
 *
 * Family "ping" (logged), address ping/p1:
 *   zero --ping--> one --ping--> two --ping--> three
 *   zero --arm--> waiting, arming `tick` to itself in 5s (send ID t1)
 *   zero --relay--> waiting, arming `hello` to sink/b in 5s (send ID b1)
 *   waiting --tick--> ticked
 *   any --stop--> end (final)
 * Family "sink" (not logged), address sink/b:
 *   h0 --hello--> h1 --hello--> h2 --hello--> h3
 */

#define SEND_SELF(id_text, event_text, delay_text)                                                \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define SEND_TO(id_text, event_text, delay_text, target_text)                                     \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) }, \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define TARGETS(...) ((const ta_str[]){ __VA_ARGS__, END })
#define GO(event_text, target_text) \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text), .targets = TARGETS(STR(target_text)) }

static const ta_scxml_state_spec ping_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("zero"),
      .transitions = (const ta_scxml_transition_spec[]){
          GO("ping", "one"),
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("arm"), .targets = TARGETS(STR("waiting")),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("t1", "tick", "5s"), END } },
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("relay"), .targets = TARGETS(STR("waiting")),
            .actions = (const ta_scxml_action_spec[]){ SEND_TO("b1", "hello", "5s", "#_actor_sink/b"), END } },
          GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("one"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("ping", "two"), GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("two"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("ping", "three"), GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("three"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("waiting"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("tick", "ticked"), GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("ticked"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    END,
};

static const ta_actor_address ping_address = { .family = { .name = STR("ping") }, .name = STR("p1") };

static ta_result define(tah_system *system, const char *family, const char *revision, const ta_scxml_state_spec *states,
                        ta_definition_id *out) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)revision, .len = (uint32_t)strlen(revision) },
    };
    ta_scxml_spec spec = { .identity = &identity, .states = states };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                                    .spec = &spec,
                                                                    .datamodel = ta_null_datamodel(),
                                                                    .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                                    .out = &definition,
                                                                });
    if (result == TA_OK) {
        result = ta_system_register_definition(system->core, definition, out);
    }
    return result;
}

static ta_definition_id define_ping(tah_system *system) {
    ta_definition_id id = 0;
    CHECK(define(system, "ping", "v1", ping_states, &id) == TA_OK);
    return id;
}

/* ------------------------------------------------------------------------
 * Systems: a manual clock from 0, the family "ping" logged, and core's
 * incarnation `incarnation`, so a restore in another system is another
 * incarnation, whose message IDs differ from the log's.
 */

static tah_system *make_system(uint64_t incarnation) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = failing_platform(), .incarnation = incarnation },
                            &system) == TA_OK);
    system->wake = no_wake;
    CHECK(tah_system_set_clock(system, TAH_CLOCK_MANUAL, 0) == TA_OK);
    tah_inputs_set_datamodel(&system->inputs, (ta_datamodel){ .self = NULL, .v = &codec_vtable });
    static const ta_str families[] = { STR("ping") };
    CHECK(tah_inputs_configure(system, families, 1) == TA_OK);
    return system;
}

static void finish_system(tah_system *system) {
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

static ta_result spawn_at(tah_system *system, ta_definition_id definition, const ta_actor_address *address,
                          ta_actor_id *actor) {
    ta_result result = ta_actor_spawn(system->core, &(ta_actor_spawn_params){
                                                        .definition = definition, .address = address, .out = actor });
    return result == TA_OK ? tah_hook_actor_spawned(system, *actor) : result;
}

static ta_result post(tah_system *system, ta_actor_id target, const char *event, const char *data) {
    ta_event message = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_result result = tah_event_name_intern(system->core, (ta_str){ .ptr = event, .len = (uint32_t)strlen(event) },
                                             &message.name);
    if (result != TA_OK) {
        return result;
    }
    ta_value value = data != NULL ? text_value(data) : (ta_value){ 0 };
    message.data = value;
    result = tah_system_post(system, &(tah_message){ .target = target, .event = &message });
    if (result != TA_OK) {
        ta_value_release(&value); /* on TA_OK the system owns the event's references */
    }
    return result;
}

static void advance(tah_system *system, ta_time time) {
    CHECK(tah_system_advance_to(system, time) == TA_OK);
}

/* ------------------------------------------------------------------------
 * The log a binding keeps: each turn's batch appended, as a commit would.
 */

typedef struct {
    uint8_t bytes[16384];
    uint32_t length;
    uint32_t unit_start; /* where the unit a sink began starts */
    bool refuse;         /* the sink refuses what it is given */
} log_buffer;

static void append(log_buffer *log, const uint8_t *bytes, uint32_t length) {
    CHECK(log->length + length <= sizeof log->bytes);
    memcpy(log->bytes + log->length, bytes, length);
    log->length += length;
}

static void take(tah_system *system, log_buffer *log) {
    uint32_t wakes = 0;
    (void)tah_inputs_wakes(system, &wakes);
    tah_inputs *inputs = &system->inputs;
    for (uint32_t index = 0; index < inputs->entries.count; index += 1u) {
        const tah_input_entry *entry = tah_vec_at(&inputs->entries, index);
        if (log != NULL) {
            append(log, entry->bytes.items, (uint32_t)entry->bytes.count);
        }
    }
    tah_inputs_release(inputs);
}

/* Runs turns until the system is quiet, committing each batch to `log` (NULL: none). */
static void run(tah_system *system, log_buffer *log) {
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        tah_system_turn(system);
        take(system, log);
        tah_system_end_turn(system);
    }
    take(system, log);
}

/* A log_buffer as a checkpoint's sink: a unit is kept whole, or not at all. */
static ta_op_status buffer_begin(void *self) {
    log_buffer *log = self;
    log->unit_start = log->length;
    return TA_OP_OK;
}

static ta_op_status buffer_write(void *self, ta_bytes bytes) {
    log_buffer *log = self;
    if (log->refuse) {
        return TA_OP_NOMEM;
    }
    append(log, bytes.ptr, bytes.len);
    return TA_OP_OK;
}

static void buffer_end(void *self, bool keep) {
    log_buffer *log = self;
    if (!keep) {
        log->length = log->unit_start;
    }
}

static const ta_byte_sink_vtable buffer_sink_vtable = { .begin = buffer_begin, .write = buffer_write, .end = buffer_end };

static ta_byte_sink buffer_sink(log_buffer *log) {
    return (ta_byte_sink){ .self = log, .v = &buffer_sink_vtable };
}

/* A log's records, read back. */
enum { MAX_RECORDS = 64 };
typedef struct {
    tah_wal_view views[MAX_RECORDS];
    uint32_t count;
} record_list;

static void read_records(const uint8_t *bytes, uint32_t length, record_list *out) {
    out->count = 0;
    for (uint32_t at = 0; at < length;) {
        CHECK(out->count < MAX_RECORDS);
        tah_wal_view *view = &out->views[out->count];
        CHECK(tah_wal_read(bytes + at, length - at, view) == TA_OK);
        CHECK(view->span != 0);
        at += (uint32_t)view->span;
        out->count += 1u;
    }
}

/* ------------------------------------------------------------------------
 * Inspecting actors.
 */

static bool text_equal(ta_str text, const char *expected) {
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

/* Whether the actor's (single) active atomic state is `id`. */
static bool in_state(tah_system *system, ta_actor_id actor, const char *id) {
    ta_state_id states[4];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system->core, &(ta_actor_active_states_params){
                                                   .actor = actor, .states = states, .capacity = 4, .count = &count }) ==
          TA_OK);
    if (count != 1) {
        return false;
    }
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    ta_state_info info;
    CHECK(ta_definition_state(system->core, &(ta_definition_state_params){
                                                .definition = view.definition_id, .state = states[0], .out = &info }) ==
          TA_OK);
    return text_equal(info.id, id);
}

/* The actor's pending delayed sends; `due` is the first one's due time. */
static uint32_t pending_sends(tah_system *system, ta_actor_id actor, ta_time *due) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    if (due != NULL && view.delayed_send_count != 0) {
        *due = view.delayed_sends[0].due;
    }
    return view.delayed_send_count;
}

/* The message at the head of the actor's mailbox, or NULL. */
static const tah_envelope *first_mail(tah_system *system, ta_actor_id actor) {
    tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    CHECK(entry != NULL);
    return entry->first != 0 ? tah_sched_envelope(&system->sched, entry->first) : NULL;
}

static bool event_named(tah_system *system, ta_event_name_id name, const char *expected) {
    char buffer[64];
    uint32_t needed = 0;
    CHECK(ta_event_name_format(system->core, &(ta_event_name_format_params){ .name = name, .buffer = buffer,
                                                                             .capacity = sizeof buffer,
                                                                             .needed = &needed }) == TA_OK);
    return text_equal((ta_str){ .ptr = buffer, .len = needed - 1u }, expected);
}

/* Destroys an actor a binding gives up on: prepared, or published. */
static void drop(tah_system *system, ta_actor_id actor) {
    system->inside_core = true;
    CHECK(ta_actor_destroy(system->core, actor) == TA_OK);
    system->inside_core = false;
    CHECK(tah_hook_actor_removed(system, actor) == TA_OK);
}

/* ------------------------------------------------------------------------
 * Replay, as a binding drives it (tinyactors-test's hosted.restore and
 * napi's #replayAt): the log is pushed `first` bytes, then `chunk` bytes at
 * a time, as the replay asks for more.
 */

typedef struct {
    uint32_t first;  /* bytes pushed before the first prepare */
    uint32_t chunk;  /* bytes pushed each time more are needed; 0 is the rest */
    uint32_t most;   /* records a window applies; 0 is 256 */
    ta_definition_id definition;
    /* Answers. */
    bool found;
    ta_actor_id actor;
    tah_replay_state state; /* the state the last window ended in */
    uint32_t records;       /* records the windows applied */
    uint32_t windows;       /* windows that answered MORE */
    uint32_t needs;         /* windows that answered NEED with a piece still to push */
    uint32_t ended;         /* lives that ended with DESTROYED */
    ta_time last_time;
    bool published;
    uint32_t redelivered;
} restore_run;

typedef struct {
    tah_system *system;
    tah_replay *replay;
    const uint8_t *bytes;
    uint32_t length;
    uint32_t pushed;
    restore_run *out;
} feed;

/* Pushes the next piece; false when the log is all pushed. */
static bool feed_more(feed *in, uint32_t size, ta_result *result) {
    if (in->pushed == in->length) {
        return false;
    }
    uint32_t count = size == 0 || size > in->length - in->pushed ? in->length - in->pushed : size;
    *result = tah_replay_push(in->replay, in->bytes + in->pushed, count);
    if (*result == TA_OK) {
        in->pushed += count;
    }
    return true;
}

/* Prepares an actor from the log's next life, pushing pieces until its first record is there. */
static ta_result feed_prepare(feed *in) {
    restore_run *out = in->out;
    ta_result result = TA_OK;
    for (;;) {
        result = tah_replay_prepare(in->replay, &(tah_replay_prepare_params){
                                                    .definition = out->definition, .address = &ping_address,
                                                    .out = &out->actor, .found = &out->found });
        if (result != TA_OK || out->found || !feed_more(in, out->chunk, &result) || result != TA_OK) {
            return result;
        }
    }
}

/* Runs windows until a life ends, or the log does (NEED with nothing left to push). */
static ta_result feed_run(feed *in, bool between) {
    restore_run *out = in->out;
    out->state = TAH_REPLAY_MORE;
    for (;;) {
        uint32_t records = 0;
        ta_result result = tah_replay_run(in->replay, &(tah_replay_run_params){
                                                          .actor = out->actor,
                                                          .most = out->most != 0 ? out->most : 256u,
                                                          .between = between,
                                                          .state = &out->state,
                                                          .records = &records });
        out->records += records;
        if (result != TA_OK || out->state == TAH_REPLAY_FINISHED || out->state == TAH_REPLAY_ENDED ||
            out->state == TAH_REPLAY_LIFE) {
            return result;
        }
        if (out->state == TAH_REPLAY_MORE) {
            out->windows += 1u;
            continue;
        }
        /* NEED: the next piece, or the end of the log. */
        if (!feed_more(in, out->chunk, &result)) {
            CHECK(tah_replay_buffered(in->replay) == 0); /* the log ends between records */
            return TA_OK;
        }
        if (result != TA_OK) {
            return result;
        }
        out->needs += 1u;
    }
}

/* Publishes the replayed actor at the last record's time, then puts back the mail it owes. */
static ta_result feed_publish(feed *in) {
    restore_run *out = in->out;
    tah_system *system = in->system;
    CHECK(tah_replay_last_time(in->replay, &out->last_time));
    tah_replay_finish(in->replay, out->actor);
    system->inside_core = true;
    ta_actor_publish_params publish = { .actor = out->actor, .at = &out->last_time };
    ta_result result = ta_actor_publish(system->core, &publish);
    system->inside_core = false;
    if (result == TA_OK) {
        result = tah_hook_actor_published(system, &publish);
    }
    out->published = result == TA_OK;
    return result == TA_OK ? tah_replay_redeliver(in->replay, out->actor, &out->redelivered) : result;
}

/*
 * Rebuilds ping/p1 from `bytes` in `system` and publishes it, as napi's
 * #replayAt does: a life that ended (DESTROYED) is dropped once the mail
 * before the next life is read, and the next life replayed; a life that
 * ends in a final state is dropped, not published. On a failure the actor,
 * prepared or published, is destroyed; the result says why.
 */
static ta_result restore(tah_system *system, const uint8_t *bytes, uint32_t length, restore_run *out) {
    out->found = false;
    out->actor = 0;
    out->records = out->windows = out->needs = out->ended = out->redelivered = 0;
    out->published = false;
    feed in = { .system = system, .replay = tah_replay_open(system), .bytes = bytes, .length = length, .out = out };
    if (in.replay == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_result result = TA_OK;
    if (out->first != 0) {
        (void)feed_more(&in, out->first, &result);
    }
    for (;;) {
        if (result == TA_OK) {
            result = feed_prepare(&in);
        }
        if (result != TA_OK || !out->found) {
            break;
        }
        result = feed_run(&in, false);
        if (result != TA_OK || out->state != TAH_REPLAY_ENDED) {
            break;
        }
        out->ended += 1u;
        result = feed_run(&in, true); /* mail between the lives goes to the next one */
        drop(system, out->actor);
        out->found = false;
        if (result != TA_OK || out->state != TAH_REPLAY_LIFE) {
            break;
        }
    }
    if (result == TA_OK && out->found && out->state == TAH_REPLAY_NEED) {
        result = feed_publish(&in);
    }
    if (out->found && (result != TA_OK || out->state == TAH_REPLAY_FINISHED)) {
        drop(system, out->actor);
        out->published = false;
    }
    tah_replay_free(in.replay);
    return result;
}

/* ------------------------------------------------------------------------
 * A live run of ping/p1 to replay: spawned at 0, ping at 100 and 200 with
 * data, and a ping at 300 accepted and never delivered: the process
 * stopped before its turn.
 */

static log_buffer live_log;
static uint64_t pending_id;

static void live_run_with_mail_owed(void) {
    live_log.length = 0;
    tah_system *system = make_system(1);
    ta_definition_id definition = define_ping(system);
    ta_actor_id actor = 0;
    CHECK(spawn_at(system, definition, &ping_address, &actor) == TA_OK);
    run(system, &live_log);
    advance(system, 100);
    CHECK(post(system, actor, "ping", "{\"n\":1}") == TA_OK);
    run(system, &live_log);
    advance(system, 200);
    CHECK(post(system, actor, "ping", "{\"n\":2}") == TA_OK);
    run(system, &live_log);
    advance(system, 300);
    CHECK(post(system, actor, "ping", "{\"n\":3}") == TA_OK);
    take(system, &live_log); /* its ACCEPTED is committed; the turn never runs */
    CHECK(in_state(system, actor, "two"));

    static record_list records;
    read_records(live_log.bytes, live_log.length, &records);
    CHECK(records.count == 6);
    static const tah_wal_kind expected[] = { TAH_WAL_SPAWNED, TAH_WAL_ACCEPTED, TAH_WAL_DELIVERED,
                                             TAH_WAL_ACCEPTED, TAH_WAL_DELIVERED, TAH_WAL_ACCEPTED };
    for (uint32_t index = 0; index < records.count; index += 1u) {
        CHECK(records.views[index].kind == expected[index]);
    }
    CHECK(records.views[5].time == 300 && records.views[5].seq == 6);
    pending_id = tah_wal_number(&records.views[5], 0);
    finish_system(system);
}

/* The replayed actor is where the live one stopped, and takes its owed ping. */
static void check_restored(tah_system *system, const restore_run *out) {
    CHECK(out->found && out->published);
    CHECK(out->state == TAH_REPLAY_NEED);
    CHECK(out->records == 5); /* everything after `spawned` */
    CHECK(out->last_time == 300);
    CHECK(in_state(system, out->actor, "two"));
    CHECK(out->redelivered == 1);
    /* Back in the mailbox with its data. */
    const tah_envelope *mail = first_mail(system, out->actor);
    CHECK(mail != NULL && mail->next == 0);
    CHECK(event_named(system, mail->event.name, "ping"));
    CHECK(value_is(&mail->event.data, "{\"n\":3}"));
}

/* The log pushed whole, and in two pieces cut at every byte. */
static void replays_a_log_cut_anywhere(void) {
    live_run_with_mail_owed();
    for (uint32_t cut = 0; cut <= live_log.length; cut += 1u) {
        tah_system *system = make_system(2);
        ta_definition_id definition = define_ping(system);
        advance(system, 1000);
        restore_run out = { .first = cut, .chunk = 0, .definition = definition };
        CHECK(restore(system, live_log.bytes, live_log.length, &out) == TA_OK);
        check_restored(system, &out);
        run(system, NULL);
        CHECK(in_state(system, out.actor, "three"));
        finish_system(system);
    }
}

/* Pieces of every size from 1 to 40 bytes: each record not all there is NEED. */
static void replays_a_log_in_small_pieces(void) {
    for (uint32_t chunk = 1; chunk <= 40; chunk += 1u) {
        tah_system *system = make_system(2);
        ta_definition_id definition = define_ping(system);
        advance(system, 1000);
        restore_run out = { .first = chunk, .chunk = chunk, .definition = definition };
        CHECK(restore(system, live_log.bytes, live_log.length, &out) == TA_OK);
        check_restored(system, &out);
        CHECK(out.needs >= 5); /* every record after `spawned` came in more than one piece */
        finish_system(system);
    }
}

/* A window applies `most` records at a time, and answers MORE while more are there. */
static void runs_in_windows(void) {
    tah_system *system = make_system(2);
    ta_definition_id definition = define_ping(system);
    advance(system, 1000);
    restore_run out = { .most = 1, .definition = definition };
    CHECK(restore(system, live_log.bytes, live_log.length, &out) == TA_OK);
    check_restored(system, &out);
    CHECK(out.windows == 5 && out.needs == 0);
    finish_system(system);

    system = make_system(2);
    definition = define_ping(system);
    advance(system, 1000);
    out = (restore_run){ .most = 2, .definition = definition };
    CHECK(restore(system, live_log.bytes, live_log.length, &out) == TA_OK);
    check_restored(system, &out);
    CHECK(out.windows == 2); /* 2, 2, then 1 and NEED */
    finish_system(system);
}

/*
 * The owed ping keeps its ID: it is delivered with no second ACCEPTED,
 * and the published actor's record numbers continue the log's.
 */
static void redelivers_with_the_logged_id(void) {
    tah_system *system = make_system(2);
    ta_definition_id definition = define_ping(system);
    advance(system, 1000);
    restore_run out = { .definition = definition };
    CHECK(restore(system, live_log.bytes, live_log.length, &out) == TA_OK);
    check_restored(system, &out);
    CHECK(!tah_inputs_seq_is_ours(&system->inputs, pending_id));
    static log_buffer after;
    after.length = 0;
    run(system, &after);
    CHECK(in_state(system, out.actor, "three"));
    static record_list records;
    read_records(after.bytes, after.length, &records);
    CHECK(records.count == 1);
    CHECK(records.views[0].kind == TAH_WAL_DELIVERED);
    CHECK(tah_wal_number(&records.views[0], 0) == pending_id);
    CHECK(records.views[0].seq == 7);
    CHECK(records.views[0].time == 1000);
    /* A new message takes a new ID of this incarnation. */
    CHECK(post(system, out.actor, "stop", NULL) == TA_OK);
    after.length = 0;
    run(system, &after);
    read_records(after.bytes, after.length, &records);
    CHECK(records.count == 3);
    CHECK(records.views[0].kind == TAH_WAL_ACCEPTED && records.views[0].seq == 8);
    CHECK(tah_inputs_seq_is_ours(&system->inputs, tah_wal_number(&records.views[0], 0)));
    CHECK(records.views[2].kind == TAH_WAL_FINISHED && records.views[2].seq == 10);
    finish_system(system);
}

/* A log that ends in a final state answers FINISHED: nothing is published. */
static void a_finished_life_is_not_published(void) {
    static log_buffer log;
    log.length = 0;
    tah_system *system = make_system(1);
    ta_definition_id definition = define_ping(system);
    ta_actor_id actor = 0;
    CHECK(spawn_at(system, definition, &ping_address, &actor) == TA_OK);
    run(system, &log);
    CHECK(post(system, actor, "ping", NULL) == TA_OK);
    CHECK(post(system, actor, "stop", NULL) == TA_OK);
    run(system, &log);
    finish_system(system);

    system = make_system(2);
    definition = define_ping(system);
    restore_run out = { .first = 7, .chunk = 7, .definition = definition };
    CHECK(restore(system, log.bytes, log.length, &out) == TA_OK);
    CHECK(out.found && out.state == TAH_REPLAY_FINISHED && !out.published);
    CHECK(out.records == 4); /* two ACCEPTED, and the stop's DELIVERED ends it (its FINISHED is not read) */
    ta_actor_id found = 0;
    CHECK(ta_actor_find(system->core, &(ta_actor_find_params){ .address = &ping_address, .out = &found }) ==
          TA_ERR_NOT_FOUND);
    finish_system(system);
}

/* ------------------------------------------------------------------------
 * Checkpoints (kernel ADR 0002 section 7)
 */

/*
 * A live run with a checkpoint: spawned at 0; at 100 `arm` holds a tick
 * to itself due at 5100; at 200 a ping with data is accepted and owed when
 * the checkpoint is written, before that turn's batch is committed; at
 * 6000 the tick fires and both are delivered.
 */
static log_buffer before_checkpoint; /* the batches committed before it */
static log_buffer checkpoint;        /* the checkpoint unit */
static log_buffer after_checkpoint;  /* the batches committed after it */

static void live_run_with_a_checkpoint(void) {
    before_checkpoint.length = checkpoint.length = after_checkpoint.length = 0;
    tah_system *system = make_system(1);
    ta_definition_id definition = define_ping(system);
    ta_actor_id actor = 0;
    CHECK(spawn_at(system, definition, &ping_address, &actor) == TA_OK);
    run(system, &before_checkpoint);
    advance(system, 100);
    CHECK(post(system, actor, "arm", NULL) == TA_OK);
    run(system, &before_checkpoint);
    advance(system, 200);
    CHECK(post(system, actor, "ping", "{\"n\":3}") == TA_OK);
    CHECK(tah_checkpoint_write(system, actor, buffer_sink(&checkpoint)) == TA_OK);
    take(system, &after_checkpoint); /* the ping's ACCEPTED, which the checkpoint covers */
    advance(system, 6000);
    run(system, &after_checkpoint);
    CHECK(in_state(system, actor, "ticked"));

    static record_list records;
    read_records(checkpoint.bytes, checkpoint.length, &records);
    CHECK(records.count == 3);
    CHECK(records.views[0].kind == TAH_WAL_CHECKPOINT && records.views[0].time == 200);
    CHECK(records.views[1].kind == TAH_WAL_CHECKPOINT_SEND && tah_wal_number(&records.views[1], 4) == 5100);
    CHECK((tah_wal_number(&records.views[1], 5) & TAH_WAL_SEND_SELF) != 0);
    CHECK(records.views[2].kind == TAH_WAL_ACCEPTED);
    uint64_t covered = records.views[0].seq;
    for (uint32_t index = 1; index < records.count; index += 1u) {
        CHECK(records.views[index].seq == covered);
    }
    CHECK(tah_wal_span(checkpoint.bytes, checkpoint.length, true) == checkpoint.length);
    read_records(after_checkpoint.bytes, after_checkpoint.length, &records);
    CHECK(records.views[0].kind == TAH_WAL_ACCEPTED && records.views[0].seq == covered);
    CHECK(records.views[records.count - 1u].seq > covered);
    finish_system(system);
}

/*
 * Whether two checkpoints are the same bytes but for each record's time,
 * which is `time` in `restored`: a published actor's clock is the system's.
 */
static bool same_but_time(const log_buffer *restored, const log_buffer *original, ta_time time) {
    if (restored->length != original->length) {
        return false;
    }
    static log_buffer masked;
    masked = *restored;
    for (uint32_t at = 0; at < masked.length;) {
        tah_wal_view view;
        CHECK(tah_wal_read(masked.bytes + at, masked.length - at, &view) == TA_OK);
        CHECK(view.time == time);
        memcpy(masked.bytes + at + 16u, original->bytes + at + 16u, 8u); /* u64 time, after version, kind, size and seq */
        at += (uint32_t)view.span;
    }
    return memcmp(masked.bytes, original->bytes, masked.length) == 0;
}

/*
 * A checkpoint as the whole log restores the state it holds: the
 * configuration, the tick still held, the owed ping back in the mailbox
 * with its data; and a checkpoint of the restored actor is the same
 * bytes, but for the time: published, its clock is the system's.
 */
static void restores_from_a_checkpoint(void) {
    live_run_with_a_checkpoint();
    for (uint32_t chunk = 0; chunk <= 16; chunk += 4u) {
        tah_system *system = make_system(2);
        ta_definition_id definition = define_ping(system);
        advance(system, 1000);
        restore_run out = { .first = chunk, .chunk = chunk, .definition = definition };
        CHECK(restore(system, checkpoint.bytes, checkpoint.length, &out) == TA_OK);
        CHECK(out.found && out.published && out.records == 0);
        CHECK(out.last_time == 200); /* the actor's clock when it was written */
        CHECK(in_state(system, out.actor, "waiting"));
        ta_time due = 0;
        CHECK(pending_sends(system, out.actor, &due) == 1 && due == 5100);
        CHECK(out.redelivered == 1);
        const tah_envelope *mail = first_mail(system, out.actor);
        CHECK(mail != NULL && event_named(system, mail->event.name, "ping"));
        CHECK(value_is(&mail->event.data, "{\"n\":3}"));

        static log_buffer again;
        again.length = 0;
        CHECK(tah_checkpoint_write(system, out.actor, buffer_sink(&again)) == TA_OK);
        CHECK(same_but_time(&again, &checkpoint, 1000));

        /* The tick fires once, at its due time, and the ping is taken as it was live. */
        advance(system, 6000);
        run(system, NULL);
        CHECK(in_state(system, out.actor, "ticked"));
        CHECK(pending_sends(system, out.actor, NULL) == 0);
        finish_system(system);
    }
}

/*
 * The checkpoint, then the batches after it: the records it covers are
 * skipped, the rest applied. And the whole log with the checkpoint in the
 * middle: prepared from `spawned`, the checkpoint unit is skipped.
 */
static void continues_after_a_checkpoint(void) {
    static log_buffer log;
    for (int whole = 0; whole < 2; whole += 1) {
        log.length = 0;
        if (whole) {
            append(&log, before_checkpoint.bytes, before_checkpoint.length);
        }
        append(&log, checkpoint.bytes, checkpoint.length);
        append(&log, after_checkpoint.bytes, after_checkpoint.length);
        for (uint32_t chunk = 0; chunk <= 24; chunk += 8u) {
            tah_system *system = make_system(2);
            ta_definition_id definition = define_ping(system);
            advance(system, 7000);
            restore_run out = { .first = chunk, .chunk = chunk, .definition = definition };
            CHECK(restore(system, log.bytes, log.length, &out) == TA_OK);
            CHECK(out.found && out.published);
            CHECK(out.last_time == 6000);
            CHECK(in_state(system, out.actor, "ticked"));
            CHECK(pending_sends(system, out.actor, NULL) == 0); /* the tick fired, and is not held */
            CHECK(out.redelivered == 0);
            run(system, NULL);
            CHECK(in_state(system, out.actor, "ticked"));
            finish_system(system);
        }
    }
}

/* What tah_checkpoint_write refuses, and a sink that refuses keeps nothing. */
static void checkpoint_refusals(void) {
    tah_system *system = make_system(1);
    ta_definition_id definition = define_ping(system);
    ta_actor_id anonymous = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = definition, .out = &anonymous }) == TA_OK);
    CHECK(tah_hook_actor_created(system, anonymous) == TA_OK);
    ta_actor_id actor = 0;
    CHECK(spawn_at(system, definition, &ping_address, &actor) == TA_OK);
    run(system, NULL);
    static log_buffer sink;
    sink.length = 0;
    CHECK(tah_checkpoint_write(system, anonymous, buffer_sink(&sink)) == TA_ERR_INVALID);
    CHECK(sink.length == 0);
    sink.refuse = true;
    CHECK(tah_checkpoint_write(system, actor, buffer_sink(&sink)) == TA_ERR_NOMEM);
    CHECK(sink.length == 0);
    sink.refuse = false;
    CHECK(tah_checkpoint_write(system, actor, buffer_sink(&sink)) == TA_OK);
    CHECK(sink.length != 0);
    finish_system(system);
}

/*
 * Two lives at one address: the first destroyed (DESTROYED ends it), the
 * second spawned again. The replay answers ENDED, then LIFE at the second
 * `spawned`, and publishes the second life.
 */
static void replays_the_last_of_two_lives(void) {
    static log_buffer log;
    log.length = 0;
    tah_system *system = make_system(1);
    ta_definition_id definition = define_ping(system);
    ta_actor_id actor = 0;
    CHECK(spawn_at(system, definition, &ping_address, &actor) == TA_OK);
    run(system, &log);
    advance(system, 100);
    CHECK(post(system, actor, "ping", NULL) == TA_OK);
    run(system, &log);
    CHECK(tah_inputs_destroying(system, actor) == TA_OK);
    drop(system, actor);
    run(system, &log);
    uint32_t first_life = log.length;
    advance(system, 200);
    CHECK(spawn_at(system, definition, &ping_address, &actor) == TA_OK);
    run(system, &log);
    for (ta_time at = 300; at <= 400; at += 100) {
        advance(system, at);
        CHECK(post(system, actor, "ping", NULL) == TA_OK);
        run(system, &log);
    }
    CHECK(in_state(system, actor, "two"));
    finish_system(system);

    static record_list records;
    read_records(log.bytes, first_life, &records);
    CHECK(records.count == 4 && records.views[3].kind == TAH_WAL_DESTROYED);

    /* By hand: each state in turn. */
    system = make_system(2);
    definition = define_ping(system);
    advance(system, 1000);
    tah_replay *replay = tah_replay_open(system);
    CHECK(replay != NULL);
    CHECK(tah_replay_push(replay, log.bytes, log.length) == TA_OK);
    bool found = false;
    CHECK(tah_replay_prepare(replay, &(tah_replay_prepare_params){ .definition = definition, .address = &ping_address,
                                                                   .out = &actor, .found = &found }) == TA_OK);
    CHECK(found);
    tah_replay_state state = TAH_REPLAY_MORE;
    uint32_t count = 0;
    CHECK(tah_replay_run(replay, &(tah_replay_run_params){ .actor = actor, .most = 100, .state = &state,
                                                           .records = &count }) == TA_OK);
    CHECK(state == TAH_REPLAY_ENDED && count == 3);
    CHECK(in_state(system, actor, "one"));
    CHECK(tah_replay_run(replay, &(tah_replay_run_params){ .actor = actor, .most = 100, .between = true,
                                                           .state = &state, .records = &count }) == TA_OK);
    CHECK(state == TAH_REPLAY_LIFE && count == 0);
    drop(system, actor);
    CHECK(tah_replay_prepare(replay, &(tah_replay_prepare_params){ .definition = definition, .address = &ping_address,
                                                                   .out = &actor, .found = &found }) == TA_OK);
    CHECK(found);
    CHECK(tah_replay_run(replay, &(tah_replay_run_params){ .actor = actor, .most = 100, .state = &state,
                                                           .records = &count }) == TA_OK);
    CHECK(state == TAH_REPLAY_NEED && count == 4);
    CHECK(in_state(system, actor, "two"));
    drop(system, actor);
    tah_replay_free(replay);
    finish_system(system);

    /* As a binding drives it, in pieces. */
    for (uint32_t chunk = 0; chunk <= 30; chunk += 10u) {
        system = make_system(2);
        definition = define_ping(system);
        advance(system, 1000);
        restore_run out = { .first = chunk, .chunk = chunk, .definition = definition };
        CHECK(restore(system, log.bytes, log.length, &out) == TA_OK);
        CHECK(out.ended == 1 && out.published && out.last_time == 400);
        CHECK(in_state(system, out.actor, "two"));
        /* The second life's record numbers continue. */
        static log_buffer after;
        after.length = 0;
        CHECK(post(system, out.actor, "ping", NULL) == TA_OK);
        run(system, &after);
        read_records(after.bytes, after.length, &records);
        CHECK(records.count == 2 && records.views[0].seq == 6);
        finish_system(system);
    }
}

/* Bytes that are no log, and a life that starts with anything but its first record: CORRUPT. */
static void refuses_a_corrupt_log(void) {
    static uint8_t garbage[64];
    memset(garbage, 0xff, sizeof garbage);
    tah_system *system = make_system(2);
    ta_definition_id definition = define_ping(system);
    advance(system, 1000);
    restore_run out = { .definition = definition };
    CHECK(restore(system, garbage, sizeof garbage, &out) == TA_ERR_CORRUPT);
    CHECK(!out.found);

    /* The live log without its `spawned`: its first record is an ACCEPTED, then a DELIVERED. */
    static record_list records;
    read_records(live_log.bytes, live_log.length, &records);
    uint32_t spawned = (uint32_t)records.views[0].span;
    uint32_t accepted = (uint32_t)records.views[1].span;
    CHECK(restore(system, live_log.bytes + spawned + accepted, live_log.length - spawned - accepted, &out) ==
          TA_ERR_CORRUPT);
    CHECK(!out.found);

    /* A delivery of a message the log never accepted. */
    static log_buffer log;
    log.length = 0;
    append(&log, live_log.bytes, spawned);
    append(&log, live_log.bytes + spawned + accepted, live_log.length - spawned - accepted);
    CHECK(restore(system, log.bytes, log.length, &out) == TA_ERR_CORRUPT);
    ta_actor_id found = 0;
    CHECK(ta_actor_find(system->core, &(ta_actor_find_params){ .address = &ping_address, .out = &found }) ==
          TA_ERR_NOT_FOUND);
    finish_system(system);
}

/* A checkpoint of revision v1 prepared with revision v2: DEFINITION_MISMATCH, and nothing made. */
static void refuses_a_checkpoint_of_another_revision(void) {
    tah_system *system = make_system(2);
    ta_definition_id definition = 0;
    CHECK(define(system, "ping", "v2", ping_states, &definition) == TA_OK);
    advance(system, 1000);
    restore_run out = { .definition = definition };
    CHECK(restore(system, checkpoint.bytes, checkpoint.length, &out) == TA_ERR_DEFINITION_MISMATCH);
    CHECK(!out.found);
    /* A `spawned` names no state: the same revision rule does not apply to it. */
    CHECK(restore(system, live_log.bytes, live_log.length, &out) == TA_OK);
    CHECK(out.published && in_state(system, out.actor, "two"));
    finish_system(system);
}

int main(void) {
    replays_a_log_cut_anywhere();
    replays_a_log_in_small_pieces();
    runs_in_windows();
    redelivers_with_the_logged_id();
    a_finished_life_is_not_published();
    restores_from_a_checkpoint();
    continues_after_a_checkpoint();
    checkpoint_refusals();
    replays_the_last_of_two_lives();
    refuses_a_corrupt_log();
    refuses_a_checkpoint_of_another_revision();
    puts("replay test passed");
    return 0;
}
