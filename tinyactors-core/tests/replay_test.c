/* test: runtime asan */
/*
 * An actor is rebuilt by replaying its inputs before it is published (ADR
 * 0089), through the public header and the amalgamation only:
 * - the four timelines of section 2: a timeout sent to the actor itself, a
 *   send to another actor that fired before the actor stopped, one still
 *   pending when it stopped, and a crash between two commits;
 * - a replay under the same revision reproduces the live run's
 *   configuration, history and pending sends;
 * - a replaying actor calls no router, invoker or log sink;
 * - its invocations are active in core, events of the run being replayed
 *   reach their <finalize>, and publishing retires them as unavailable;
 * - the result codes of ta_actor_prepare(), ta_actor_publish() and
 *   a time on every input and publish's `at` (ADR 0094), and withdrawing a held send;
 * - publishing runs out of memory without changing anything, at every
 *   allocation it makes;
 * - a state reports a send the router refused, and a restore leaves it out
 *   (ADR 0094 section 3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "run_support.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,    \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }
#define END {0}

/* ------------------------------------------------------------------------
 * A counting allocator that can refuse the n-th allocation from now.
 */

static ta_test_allocator heap;
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* ------------------------------------------------------------------------
 * Adapters that count every call: a replaying actor must make none.
 */

static int routed;
static bool refusing; /* the router answers RETRY, and counts nothing */
static int invoker_starts;
static int invoker_cancels;
static int invoker_forwards;
static int logs_written;

static ta_external_event_submit_status route(void *self, const ta_external_event_submit_params *params) {
    (void)self;
    (void)params;
    if (refusing) {
        return TA_EXTERNAL_EVENT_RETRY;
    }
    routed += 1;
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable router_vtable = { .submit = route };

static ta_op_status invoker_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    invoker_starts += 1;
    *params->service_handle = 7;
    return TA_OP_OK;
}

static ta_op_status invoker_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)self;
    (void)service;
    (void)fault;
    invoker_cancels += 1;
    return TA_OP_OK;
}

static ta_op_status invoker_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self;
    (void)params;
    invoker_forwards += 1;
    return TA_OP_OK;
}

static const ta_invoker_vtable invoker_vtable = { .start = invoker_start, .cancel = invoker_cancel,
                                                  .forward = invoker_forward };

static void log_write(void *self, const ta_log_record *record) {
    (void)self;
    (void)record;
    logs_written += 1;
}

static const ta_log_sink_vtable log_vtable = { .write = log_write };

/* A small directory: a few addresses by handle. */
#define SLOTS 8
typedef struct {
    ta_address *address[SLOTS];
    ta_actor_directory_entry entry[SLOTS];
} small_directory;

static small_directory directory;

static int dir_find(const ta_address *address) {
    for (int index = 0; index < SLOTS; index += 1) {
        if (directory.address[index] == address) {
            return index;
        }
    }
    return -1;
}

static bool same_entry(ta_actor_directory_entry left, ta_actor_directory_entry right) {
    return left.kind == right.kind && (left.kind != TA_ACTOR_DIRECTORY_RESIDENT || left.actor == right.actor);
}

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    (void)self;
    if (dir_find(params->address) >= 0) {
        return TA_ERR_STALE;
    }
    int slot = dir_find(NULL);
    if (slot < 0) {
        return TA_ERR_NOMEM;
    }
    ta_address_retain(params->address);
    directory.address[slot] = params->address;
    directory.entry[slot] = params->entry;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    (void)self;
    int slot = dir_find(params->address);
    *params->out = slot < 0 ? (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE } : directory.entry[slot];
    return TA_OK;
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    (void)self;
    int slot = dir_find(params->address);
    if (slot < 0 || !same_entry(directory.entry[slot], params->expected)) {
        return TA_ERR_STALE;
    }
    directory.entry[slot] = params->replacement;
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    (void)self;
    int slot = dir_find(params->address);
    if (slot < 0 || !same_entry(directory.entry[slot], params->expected)) {
        return TA_ERR_STALE;
    }
    ta_address_release(directory.address[slot]);
    directory.address[slot] = NULL;
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

static void directory_clear(void) {
    for (int index = 0; index < SLOTS; index += 1) {
        if (directory.address[index] != NULL) {
            ta_address_release(directory.address[index]);
            directory.address[index] = NULL;
        }
    }
}

/* ------------------------------------------------------------------------
 * Charts, all on the null data model.
 */

#define SEND_TO(id_text, event_text, delay_text, target_text)                                     \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) }, \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define SEND_SELF(id_text, event_text, delay_text)                                                \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define RAISE(event_text) { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR(event_text) } }
#define LOG(label_text) { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = STR(label_text) } }
#define TARGETS(...) ((const ta_str[]){ __VA_ARGS__, END })
#define GO(event_text, ...) { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text), .targets = TARGETS(__VA_ARGS__) }

/*
 * On e1: a timeout to itself in 5s, a reply to b now, and a log; a message
 * to actor b in 5s. `timeout` moves to `timedout`.
 */
static const ta_scxml_state_spec timers[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e1"), .targets = TARGETS(STR("waiting")),
            .actions = (const ta_scxml_action_spec[]){
                SEND_SELF("t1", "timeout", "5s"), SEND_TO("b1", "hello", "5s", "#_actor_b/one"),
                SEND_TO("now", "reply", "0s", "#_actor_b/one"), LOG("armed"), END } },
          END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("waiting"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("timeout", STR("timedout")), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("timedout") },
    END,
};

/*
 * A deep history inside `on`: `a` → `a2` on next, `off` leaves, `back`
 * returns through the history; `arm` sets a 9s timer.
 */
static const ta_scxml_state_spec history_chart[] = {
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("on"),
      .initial = &(ta_scxml_initial_spec){ .targets = TARGETS(STR("a1")) },
      .states = (const ta_scxml_state_spec[]){
          { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = STR("h"),
            .transitions = (const ta_scxml_transition_spec[]){ { .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                                 .targets = TARGETS(STR("a1")) }, END } },
          { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a1"),
            .transitions = (const ta_scxml_transition_spec[]){ GO("next", STR("a2")), END } },
          { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a2"),
            .transitions = (const ta_scxml_transition_spec[]){ GO("next", STR("a3")), END } },
          { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a3") },
          END },
      .transitions = (const ta_scxml_transition_spec[]){
          GO("off", STR("off")),
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("arm"),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("tick", "tick", "9s"), END } },
          END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("off"),
      .transitions = (const ta_scxml_transition_spec[]){
          GO("back", STR("h")),
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("arm"),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("tock", "tock", "4s"), END } },
          END } },
    END,
};

/* An invocation with a <finalize> that raises `finalized`, and a timer. */
static const ta_scxml_state_spec invoking[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"),
      .onentry = (const ta_scxml_handler_spec[]){
          { .actions = (const ta_scxml_action_spec[]){ SEND_SELF("later", "later", "5s"), END } }, END },
      .invokes = (const ta_scxml_invoke_spec[]){
          { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("job") },
            .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") }, .autoforward = true,
            .finalize = (const ta_scxml_action_spec[]){ RAISE("finalized"), END } },
          END },
      .transitions = (const ta_scxml_transition_spec[]){
          GO("finalized", STR("finished_job")), GO("error.communication", STR("lost")),
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("poke") }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("finished_job") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("lost") },
    END,
};

/* Reaches a top-level final state in its bootstrap. */
static const ta_scxml_state_spec at_once_final[] = {
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("done") },
    END,
};

static ta_definition_id define(ta_system *system, const char *family, const ta_scxml_state_spec *states) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        system, &(ta_definition_compile_params){
                    .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity, .states = states },
                    .datamodel = ta_null_datamodel(),
                    .allocator = allocator,
                    .out = &definition,
                });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(result == TA_OK && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static ta_system *create_system(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    CHECK(ta_system_set_external_event_router(system,
                                              (ta_external_event_router){ .v = &router_vtable }) == TA_OK);
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .v = &dir_vtable }) == TA_OK);
    CHECK(ta_system_set_log_sink(system, (ta_log_sink){ .v = &log_vtable }) == TA_OK);
    ta_atom svc = 0;
    CHECK(ta_system_intern_atom(system, (ta_str)STR("svc"), &svc) == TA_OK);
    CHECK(ta_system_register_invoker(system, svc, (ta_invoker){ .v = &invoker_vtable }) == TA_OK);
    routed = invoker_starts = invoker_cancels = invoker_forwards = logs_written = 0;
    return system;
}

static void destroy_system(ta_system *system) {
    directory_clear();
    ta_system_destroy(system);
}

static test_run_status run(ta_system *system, ta_actor_id actor) {
    test_run_status status = TEST_RUN_ERROR;
    CHECK(test_resume(system, &(test_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
    return status;
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atoms[4];
    uint32_t count = 0;
    const char *start = text;
    for (const char *at = text;; at += 1) {
        if (*at == '.' || *at == '\0') {
            CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = start, .len = (uint32_t)(at - start) },
                                        &atoms[count]) == TA_OK);
            count += 1;
            if (*at == '\0') {
                break;
            }
            start = at + 1;
        }
    }
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = atoms, .count = count }, &name) == TA_OK);
    return name;
}

/* An input at `time` (ADR 0094 section 1), run to idle. */
static void deliver_at(ta_system *system, ta_actor_id actor, const char *event, ta_time time) {
    ta_event delivered = { .name = event_name(system, event), .type = TA_EVENT_EXTERNAL,
                           .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = actor, .event = &delivered,
                                                               .time = time }) == TA_OK);
    CHECK(run(system, actor) == TEST_RUN_STABLE);
}

/* An input to a live actor, at the system's time. */
static void deliver(ta_system *system, ta_actor_id actor, const char *event) {
    deliver_at(system, actor, event, ta_system_time(system));
}

static ta_state_id state_of(ta_system *system, ta_actor_id actor, const char *id) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    ta_state_id state = 0;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                               .definition = view.definition_id,
                                               .id = { .ptr = id, .len = (uint32_t)strlen(id) },
                                               .out = &state }) == TA_OK);
    return state;
}

/* Whether `id` is the actor's only active atomic state. */
static bool in_state(ta_system *system, ta_actor_id actor, const char *id) {
    ta_state_id active[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                          .capacity = 8, .count = &count }) ==
          TA_OK);
    ta_state_id wanted = state_of(system, actor, id);
    for (uint32_t index = 0; index < count; index += 1) {
        if (active[index] == wanted) {
            return true;
        }
    }
    return false;
}

static const ta_actor_address address_a = { .family = { .name = STR("timers") }, .name = STR("a") };

static ta_actor_id prepare_replaying(ta_system *system, ta_definition_id definition, const ta_actor_address *address,
                                     ta_time at) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = address, .definition = definition,
                                                               .at = &at, .out = &actor }) == TA_OK);
    CHECK(run(system, actor) == TEST_RUN_STABLE);
    return actor;
}

static ta_result publish(ta_system *system, ta_actor_id actor) {
    return ta_actor_publish(system, &(ta_actor_publish_params){ .actor = actor });
}

/* Publishes a replaying actor after its last clock move, to `at`. */
static ta_result publish_at(ta_system *system, ta_actor_id actor, ta_time at) {
    return ta_actor_publish(system, &(ta_actor_publish_params){ .actor = actor, .at = &at });
}

static uint32_t dispatch(ta_system *system) {
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 100, .dispatched = &dispatched,
                                                                   .more_due = &more }) == TA_OK);
    return dispatched;
}

static uint32_t pending_sends(ta_system *system, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    return view.delayed_send_count;
}

/* ------------------------------------------------------------------------
 * The four timelines of ADR 0089 section 2
 */

/* A live run of `timers` from spawn at 0 to e1 at 1000, as the log records it. */
static ta_actor_id live_until_e1(ta_system *system, ta_definition_id definition) {
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &address_a,
                                                           .out = &live }) == TA_OK);
    CHECK(run(system, live) == TEST_RUN_STABLE);
    CHECK(ta_system_set_time(system, 1000) == TA_OK);
    deliver(system, live, "e1");
    return live;
}

/* A timeout to itself: its firing comes back as an input, so the held send
 * is dropped when the clock passes it, and nothing fires twice. */
static void test_timeline_timeout_to_self(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_actor_id live = live_until_e1(system, definition);
    int routed_live = routed;
    CHECK(routed_live == 1 && logs_written == 1); /* the immediate reply, and the log */
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    CHECK(dispatch(system) == 2); /* t1 to itself, b1 to b */
    CHECK(ta_system_set_time(system, 6500) == TA_OK);
    deliver(system, live, "timeout"); /* the log: timeout @ 6500 */
    CHECK(ta_system_set_time(system, 7000) == TA_OK);
    CHECK(ta_actor_destroy(system, live) == TA_OK); /* evicted @ 7000 */

    int routed_before = routed;
    CHECK(ta_system_set_time(system, 7000 + 7u * 24u * 3600u * 1000u) == TA_OK); /* a week later */
    ta_actor_id replay = prepare_replaying(system, definition, &address_a, 0);
    deliver_at(system, replay, "e1", 1000);
    CHECK(pending_sends(system, replay) == 0); /* held, not pending */
    deliver_at(system, replay, "timeout", 6500); /* t1 and b1 fired: dropped */
    CHECK(in_state(system, replay, "timedout"));
    CHECK(publish_at(system, replay, 7000) == TA_OK);
    ta_time deadline = 0;
    CHECK(ta_system_next_deadline(system, &deadline) == TA_ERR_NOT_FOUND);
    CHECK(routed == routed_before && logs_written == 1 && invoker_starts == 0);
    destroy_system(system);
}

/* A send to b that fired before the actor stopped: dropped at the stop. */
static void test_timeline_fired_before_stop(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_actor_id live = live_until_e1(system, definition);
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    CHECK(dispatch(system) == 2);
    CHECK(ta_system_set_time(system, 7000) == TA_OK);
    CHECK(ta_actor_destroy(system, live) == TA_OK); /* evicted @ 7000, E >= d */

    int routed_before = routed;
    ta_actor_id replay = prepare_replaying(system, definition, &address_a, 0);
    deliver_at(system, replay, "e1", 1000);
    CHECK(publish_at(system, replay, 7000) == TA_OK);
    CHECK(pending_sends(system, replay) == 0);
    CHECK(dispatch(system) == 0 && routed == routed_before);
    destroy_system(system);
}

/* The actor stopped first: its sends are still held at publish, and fire
 * once, late, at the next dispatch. */
static void test_timeline_pending_at_stop(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_actor_id live = live_until_e1(system, definition);
    CHECK(ta_system_set_time(system, 3000) == TA_OK);
    CHECK(ta_actor_destroy(system, live) == TA_OK); /* evicted @ 3000, E < d */

    CHECK(ta_system_set_time(system, 7u * 24u * 3600u * 1000u) == TA_OK); /* a week later */
    int routed_before = routed;
    ta_actor_id replay = prepare_replaying(system, definition, &address_a, 0);
    deliver_at(system, replay, "e1", 1000);
    CHECK(publish_at(system, replay, 3000) == TA_OK); /* d = 6000 > 3000: still held */
    CHECK(pending_sends(system, replay) == 2);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, replay, &view) == TA_OK);
    CHECK(view.delayed_send_count == 2 && view.delayed_sends[0].due == 6000);
    CHECK(dispatch(system) == 2 && routed == routed_before + 2);
    CHECK(dispatch(system) == 0);
    destroy_system(system);
}

/* A crash between two commits: the send dispatched after the last horizon
 * fires again at publish, at least once. */
static void test_timeline_crash_between_commits(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_actor_id live = live_until_e1(system, definition);
    CHECK(ta_system_set_time(system, 3000) == TA_OK); /* turn committed, horizon 3000 */
    CHECK(ta_system_set_time(system, 6000) == TA_OK);
    CHECK(dispatch(system) == 2); /* dispatched, then the crash */
    CHECK(ta_actor_destroy(system, live) == TA_OK);

    int routed_before = routed;
    ta_actor_id replay = prepare_replaying(system, definition, &address_a, 0);
    deliver_at(system, replay, "e1", 1000);
    CHECK(publish_at(system, replay, 3000) == TA_OK);
    CHECK(dispatch(system) == 2 && routed == routed_before + 2); /* again */
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Replay reproduces the live run
 */

static void test_replay_matches_live_run(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "hist", history_chart);
    static const ta_actor_address live_address = { .family = { .name = STR("hist") }, .name = STR("live") };
    static const ta_actor_address copy_address = { .family = { .name = STR("hist") }, .name = STR("copy") };
    static const struct { ta_time at; const char *event; } inputs[] = {
        { 100, "next" }, { 200, "arm" }, { 300, "next" }, { 400, "off" }, { 500, "arm" }, { 900, "back" },
    };
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &live_address,
                                                           .out = &live }) == TA_OK);
    CHECK(run(system, live) == TEST_RUN_STABLE);
    for (size_t index = 0; index < sizeof inputs / sizeof inputs[0]; index += 1) {
        CHECK(ta_system_set_time(system, inputs[index].at) == TA_OK);
        deliver(system, live, inputs[index].event);
    }
    int routed_live = routed;

    ta_actor_id copy = prepare_replaying(system, definition, &copy_address, 0);
    for (size_t index = 0; index < sizeof inputs / sizeof inputs[0]; index += 1) {
        deliver_at(system, copy, inputs[index].event, inputs[index].at);
    }
    CHECK(routed == routed_live);
    CHECK(publish(system, copy) == TA_OK);

    ta_state_id live_states[8];
    ta_state_id copy_states[8];
    uint32_t live_count = 0;
    uint32_t copy_count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){
                                             .actor = live, .states = live_states, .capacity = 8,
                                             .count = &live_count }) == TA_OK);
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){
                                             .actor = copy, .states = copy_states, .capacity = 8,
                                             .count = &copy_count }) == TA_OK);
    CHECK(live_count == copy_count && memcmp(live_states, copy_states, live_count * sizeof(ta_state_id)) == 0);
    CHECK(in_state(system, copy, "a3"));

    ta_state_id history = state_of(system, live, "h");
    ta_state_id live_recorded[4];
    ta_state_id copy_recorded[4];
    uint32_t live_recorded_count = 0;
    uint32_t copy_recorded_count = 0;
    CHECK(ta_actor_history(system, &(ta_actor_history_params){ .actor = live, .history = history,
                                                                .states = live_recorded, .capacity = 4,
                                                                .count = &live_recorded_count }) == TA_OK);
    CHECK(ta_actor_history(system, &(ta_actor_history_params){ .actor = copy, .history = history,
                                                                .states = copy_recorded, .capacity = 4,
                                                                .count = &copy_recorded_count }) == TA_OK);
    CHECK(live_recorded_count == 1 && copy_recorded_count == 1 && live_recorded[0] == copy_recorded[0]);

    ta_actor_inspection live_view;
    ta_actor_inspection copy_view;
    CHECK(ta_actor_inspect(system, live, &live_view) == TA_OK);
    CHECK(ta_actor_inspect(system, copy, &copy_view) == TA_OK);
    CHECK(live_view.delayed_send_count == 2 && copy_view.delayed_send_count == 2);
    for (uint32_t index = 0; index < 2 && live_view.delayed_send_count == 2 && copy_view.delayed_send_count == 2;
         index += 1) {
        CHECK(live_view.delayed_sends[index].due == copy_view.delayed_sends[index].due);
        CHECK(live_view.delayed_sends[index].send_id == copy_view.delayed_sends[index].send_id);
        CHECK(live_view.delayed_sends[index].event == copy_view.delayed_sends[index].event);
    }
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Invocations
 */

static const ta_actor_address address_job = { .family = { .name = STR("jobs") }, .name = STR("j") };

static ta_invoke_id active_invoke(ta_system *system, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    CHECK(view.invoke_count == 1);
    return view.invoke_count == 1 ? view.invokes[0].id : 0;
}

/* The service's reply, replayed, reaches <finalize>; no invoker is called. */
static void test_replayed_invocation_finalizes(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "jobs", invoking);
    ta_actor_id replay = prepare_replaying(system, definition, &address_job, 0);
    ta_invoke_id job = active_invoke(system, replay);
    ta_str text = {0};
    CHECK(ta_system_invoke_id_text(system, job, &text) == TA_OK && text.len == 3 && memcmp(text.ptr, "job", 3) == 0);
    CHECK(invoker_starts == 0);
    deliver(system, replay, "poke"); /* autoforwarded: to nothing */
    CHECK(invoker_forwards == 0);
    ta_event reply = { .name = event_name(system, "svc.reply"), .type = TA_EVENT_EXTERNAL, .invoke_id = job };
    CHECK(ta_system_set_time(system, 10) == TA_OK);
    CHECK(ta_actor_report_invoke(system, &(ta_invoke_report_params){ .actor = replay, .invoke_id = job,
                                                                     .kind = TA_INVOKE_REPORT_EVENT,
                                                                     .time = 10, .as.event = &reply }) == TA_OK);
    CHECK(run(system, replay) == TEST_RUN_STABLE);
    CHECK(in_state(system, replay, "finished_job"));
    CHECK(invoker_starts == 0 && invoker_cancels == 0 && invoker_forwards == 0);
    CHECK(publish(system, replay) == TA_OK);
    CHECK(invoker_starts == 0);
    destroy_system(system);
}

/* An invocation still active at publish is retired as unavailable. */
static void test_publish_retires_invocations(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "jobs", invoking);
    ta_actor_id replay = prepare_replaying(system, definition, &address_job, 0);
    (void)active_invoke(system, replay);
    CHECK(publish(system, replay) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, replay, &view) == TA_OK);
    CHECK(view.invoke_count == 0 && view.macrostep_in_progress);
    CHECK(run(system, replay) == TEST_RUN_STABLE);
    CHECK(in_state(system, replay, "lost"));
    CHECK(invoker_starts == 0 && invoker_cancels == 0);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Result codes and withdrawal
 */

static void test_result_codes(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_definition_id finishing = define(system, "fin", at_once_final);
    ta_actor_id out = 0;
    ta_time later = 50;

    /* prepare */
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .definition = definition, .out = &out }) ==
          TA_ERR_INVALID); /* neither incumbent nor address */
    static const ta_actor_address other_family = { .family = { .name = STR("other") }, .name = STR("x") };
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &other_family, .definition = definition,
                                                               .out = &out }) == TA_ERR_INVALID);
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &address_a, .definition = definition,
                                                               .at = &later, .out = &out }) == TA_ERR_INVALID);
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &address_a, .definition = 12345,
                                                               .out = &out }) == TA_ERR_NOT_FOUND);

    /* an input's time (ADR 0094 section 1) */
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &live }) == TA_OK);
    CHECK(run(system, live) == TEST_RUN_STABLE);
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1, .time = 5 }) ==
          TA_ERR_INVALID); /* a live actor's input is at the system's time */
    CHECK(ta_system_set_time(system, 100) == TA_OK);
    ta_actor_id replay = prepare_replaying(system, definition, &address_a, 20);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = replay, .event = &e1, .time = 10 }) ==
          TA_ERR_INVALID); /* backwards */
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = replay, .event = &e1, .time = 101 }) ==
          TA_ERR_INVALID); /* the future */
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = replay, .event = &e1, .time = 25 }) == TA_OK);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = replay, .event = &e1, .time = 30 }) ==
          TA_OK); /* admitted behind the first (ADR 0099 section 5): nothing is busy */
    CHECK(publish(system, replay) == TA_ERR_BUSY); /* inputs are waiting */
    CHECK(run(system, replay) == TEST_RUN_STABLE);
    ta_time before = 20;
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = replay, .at = &before }) ==
          TA_ERR_INVALID); /* publish's `at` is not earlier than the clock */
    ta_time live_at = 100;
    CHECK(ta_actor_publish(system, &(ta_actor_publish_params){ .actor = live, .at = &live_at }) ==
          TA_ERR_INVALID); /* `at` is for a replaying actor only */

    /* withdraw reaches a held send */
    ta_send_id t1 = 0;
    CHECK(ta_send_id_parse(system, (ta_str)STR("t1"), &t1) == TA_OK);
    CHECK(ta_actor_withdraw_send(system, replay, t1) == TA_OK);
    CHECK(ta_actor_withdraw_send(system, replay, t1) == TA_ERR_NOT_FOUND);

    /* publish: a resident actor at the address is BUSY; an UNLOADED entry is replaced */
    ta_actor_id holder = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &address_a,
                                                           .out = &holder }) == TA_OK);
    CHECK(run(system, holder) == TEST_RUN_STABLE);
    CHECK(publish(system, replay) == TA_ERR_BUSY);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = holder, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = holder, .image = image }) == TA_OK);
    CHECK(publish(system, replay) == TA_OK); /* the UNLOADED entry, and its session, end */
    ta_session_image_destroy(image);
    CHECK(publish(system, replay) == TA_ERR_INVALID); /* published */
    CHECK(pending_sends(system, replay) == 1);        /* b1 only: t1 was withdrawn */

    /* an actor that finished before it was published */
    static const ta_actor_address fin_address = { .family = { .name = STR("fin") }, .name = STR("f") };
    ta_time zero = 0;
    ta_actor_id done = 0;
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &fin_address, .definition = finishing,
                                                               .at = &zero, .out = &done }) == TA_OK);
    test_run_status status = run(system, done);
    CHECK(status == TEST_RUN_DONE);
    CHECK(publish(system, done) == TA_ERR_DONE);
    CHECK(ta_actor_destroy(system, done) == TA_OK);
    destroy_system(system);
}

/* ------------------------------------------------------------------------
 * Running out of memory while publishing changes nothing
 */

static void test_publish_out_of_memory(void) {
    bool published = false;
    int refused = 0;
    for (long countdown = 0; countdown < 64 && !published; countdown += 1) {
        ta_system *system = create_system();
        ta_definition_id definition = define(system, "jobs", invoking);
        ta_actor_id replay = prepare_replaying(system, definition, &address_job, 0);
        ta_test_allocator_fail_nth(&heap, (uint64_t)countdown + 1u);
        ta_result result = publish(system, replay);
        ta_test_allocator_disarm(&heap);
        CHECK(result == TA_OK || result == TA_ERR_NOMEM);
        if (result == TA_ERR_NOMEM) {
            refused += 1;
            ta_actor_inspection view;
            CHECK(ta_actor_inspect(system, replay, &view) == TA_OK);
            CHECK(!view.published && view.invoke_count == 1);
            CHECK(publish(system, replay) == TA_OK);
        }
        published = result == TA_OK;
        CHECK(run(system, replay) == TEST_RUN_STABLE);
        CHECK(in_state(system, replay, "lost"));
        destroy_system(system);
    }
    CHECK(published && refused > 0);
}

/* ------------------------------------------------------------------------
 * An actor's state, exported and restored (ADR 0094 section 3)
 */

typedef struct {
    ta_state_id states[16];
    ta_history_entry history[16];
    ta_invoke_state invokes[4];
    uint32_t state_count;
    uint32_t history_count;
    uint32_t invoke_count;
    ta_actor_state state;
} exported;

static ta_result export_into(ta_system *system, ta_actor_id actor, exported *out) {
    return ta_actor_export_state(system, &(ta_actor_export_params){
                                             .actor = actor,
                                             .states = out->states, .state_capacity = 16,
                                             .state_count = &out->state_count,
                                             .history = out->history, .history_capacity = 16,
                                             .history_count = &out->history_count,
                                             .invokes = out->invokes, .invoke_capacity = 4,
                                             .invoke_count = &out->invoke_count,
                                             .out = &out->state,
                                         });
}

static ta_actor_id prepare_from_state(ta_system *system, ta_definition_id definition,
                                      const ta_actor_address *address, const ta_actor_state *state,
                                      ta_result *result) {
    ta_actor_id actor = 0;
    *result = ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = address, .definition = definition,
                                                                    .state = state, .out = &actor });
    return actor;
}

/* Export, then prepare from the state: the configuration, the history and
 * the pending sends come back, and no bootstrap runs. */
static void test_export_restores(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "hist", history_chart);
    static const ta_actor_address live_address = { .family = { .name = STR("hist") }, .name = STR("live") };
    static const ta_actor_address copy_address = { .family = { .name = STR("hist") }, .name = STR("copy") };
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &live_address,
                                                           .out = &live }) == TA_OK);
    CHECK(run(system, live) == TEST_RUN_STABLE);
    static const struct { ta_time at; const char *event; } inputs[] = {
        { 100, "next" }, { 200, "arm" }, { 300, "next" }, { 400, "off" }, { 500, "arm" },
    };
    for (size_t index = 0; index < sizeof inputs / sizeof inputs[0]; index += 1) {
        CHECK(ta_system_set_time(system, inputs[index].at) == TA_OK);
        deliver(system, live, inputs[index].event);
    }

    /* A size query: zero capacities answer NOMEM with the totals. */
    uint32_t states = 0;
    uint32_t history = 0;
    uint32_t invokes = 0;
    ta_actor_state state;
    CHECK(ta_actor_export_state(system, &(ta_actor_export_params){ .actor = live, .state_count = &states,
                                                                   .history_count = &history,
                                                                   .invoke_count = &invokes,
                                                                   .out = &state }) == TA_ERR_NOMEM);
    CHECK(states == 1 && history == 1 && invokes == 0); /* off; h recorded a3 */

    exported out;
    uint64_t before = heap.stats.allocations;
    CHECK(export_into(system, live, &out) == TA_OK);
    CHECK(heap.stats.allocations == before); /* the null data model has no root to build */
    CHECK(out.state.active_count == 1 && out.state.active[0] == state_of(system, live, "off"));
    CHECK(out.state.history_count == 1 && out.state.history[0].history == state_of(system, live, "h") &&
          out.state.history[0].state == state_of(system, live, "a3"));
    CHECK(out.state.send_count == 2 && out.state.clock == 500);

    CHECK(ta_system_set_time(system, 600) == TA_OK);
    ta_result result = TA_OK;
    ta_actor_id copy = prepare_from_state(system, definition, &copy_address, &out.state, &result);
    CHECK(result == TA_OK);
    CHECK(pending_sends(system, copy) == 0); /* held, not pending */
    CHECK(in_state(system, copy, "off"));
    deliver_at(system, copy, "back", 600); /* through the restored history */
    CHECK(in_state(system, copy, "a3"));
    deliver_at(system, live, "back", 600);
    CHECK(publish(system, copy) == TA_OK);
    ta_actor_inspection live_view;
    ta_actor_inspection copy_view;
    CHECK(ta_actor_inspect(system, live, &live_view) == TA_OK);
    CHECK(ta_actor_inspect(system, copy, &copy_view) == TA_OK);
    CHECK(live_view.delayed_send_count == 2 && copy_view.delayed_send_count == 2);
    for (uint32_t index = 0; index < 2 && live_view.delayed_send_count == 2 && copy_view.delayed_send_count == 2;
         index += 1) {
        CHECK(live_view.delayed_sends[index].due == copy_view.delayed_sends[index].due);
        CHECK(live_view.delayed_sends[index].send_id == copy_view.delayed_sends[index].send_id);
    }
    destroy_system(system);
}

/* An invocation active in the state is active in the restored actor, with
 * no invoker called, and retired as unavailable when it is published. */
static void test_export_restores_invocations(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "jobs", invoking);
    static const ta_actor_address copy_address = { .family = { .name = STR("jobs") }, .name = STR("copy") };
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &address_job,
                                                           .out = &live }) == TA_OK);
    CHECK(run(system, live) == TEST_RUN_STABLE);
    CHECK(invoker_starts == 1);
    exported out;
    CHECK(export_into(system, live, &out) == TA_OK);
    CHECK(out.state.invoke_count == 1 && out.state.send_count == 1);
    ta_result result = TA_OK;
    ta_actor_id copy = prepare_from_state(system, definition, &copy_address, &out.state, &result);
    CHECK(result == TA_OK);
    CHECK(active_invoke(system, copy) == out.state.invokes[0].id);
    CHECK(invoker_starts == 1);
    CHECK(publish(system, copy) == TA_OK);
    CHECK(run(system, copy) == TEST_RUN_STABLE);
    CHECK(in_state(system, copy, "lost"));
    CHECK(invoker_starts == 1);
    destroy_system(system);
}

/*
 * A send the router refused waits, submitted, and the state exported then
 * reports it; a restore does not hold it (ADR 0094 section 3; kernel ADR
 * 0003 removed ADR 0095's restore of it): only the unsubmitted sends come
 * back.
 */
static void test_export_leaves_submitted(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    refusing = true;
    ta_actor_id live = live_until_e1(system, definition); /* `now` to b is refused */
    refusing = false;
    exported out;
    CHECK(export_into(system, live, &out) == TA_OK);
    CHECK(out.state.send_count == 3);
    uint32_t submitted = 0;
    for (uint32_t index = 0; index < out.state.send_count; index += 1u) {
        submitted += out.state.sends[index].submitted ? 1u : 0u;
    }
    CHECK(submitted == 1);
    static const ta_actor_address copy_address = { .family = { .name = STR("timers") }, .name = STR("c") };
    ta_result result = TA_OK;
    ta_actor_id copy = prepare_from_state(system, definition, &copy_address, &out.state, &result);
    CHECK(result == TA_OK);
    CHECK(publish_at(system, copy, 1000) == TA_OK);
    CHECK(pending_sends(system, copy) == 2); /* t1 and b1, due at 6000 */
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, copy, &view) == TA_OK);
    for (uint32_t index = 0; index < view.delayed_send_count; index += 1u) {
        CHECK(!view.delayed_sends[index].submitted);
    }
    destroy_system(system);
}

static void test_export_result_codes(void) {
    ta_system *system = create_system();
    ta_definition_id definition = define(system, "timers", timers);
    ta_definition_id finishing = define(system, "fin", at_once_final);
    exported out;
    CHECK(export_into(system, 12345, &out) == TA_ERR_NOT_FOUND);
    ta_actor_id live = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = &address_a,
                                                           .out = &live }) == TA_OK);
    CHECK(export_into(system, live, &out) == TA_ERR_BUSY); /* its bootstrap has not run */
    CHECK(run(system, live) == TEST_RUN_STABLE);
    ta_event e1 = { .name = event_name(system, "e1"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = live, .event = &e1,
                                                               .time = ta_system_time(system) }) == TA_OK);
    CHECK(export_into(system, live, &out) == TA_ERR_BUSY); /* holding an event */
    CHECK(run(system, live) == TEST_RUN_STABLE);
    CHECK(export_into(system, live, &out) == TA_OK);

    /* Another revision; a state the definition does not have. */
    ta_actor_state other = out.state;
    other.definition.revision = (ta_bytes){ .ptr = (const uint8_t *)"v2", .len = 2 };
    ta_result result = TA_OK;
    static const ta_actor_address copy_address = { .family = { .name = STR("timers") }, .name = STR("c") };
    (void)prepare_from_state(system, definition, &copy_address, &other, &result);
    CHECK(result == TA_ERR_DEFINITION_MISMATCH);
    ta_state_id missing = 999;
    ta_actor_state unknown = out.state;
    unknown.active = &missing;
    unknown.active_count = 1;
    (void)prepare_from_state(system, definition, &copy_address, &unknown, &result);
    CHECK(result == TA_ERR_NOT_FOUND);
    ta_time at = 0;
    CHECK(ta_actor_prepare(system, &(ta_actor_prepare_params){ .address = &copy_address, .definition = definition,
                                                               .state = &out.state, .at = &at,
                                                               .out = &(ta_actor_id){ 0 } }) == TA_ERR_INVALID);

    /* Unpublished, and finished. */
    ta_actor_id replay = prepare_replaying(system, definition, &copy_address, 0);
    CHECK(export_into(system, replay, &out) == TA_ERR_INVALID);
    ta_actor_id done = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = finishing, .out = &done }) == TA_OK);
    CHECK(run(system, done) == TEST_RUN_DONE);
    CHECK(export_into(system, done, &out) == TA_ERR_DONE);
    destroy_system(system);
}

int main(void) {
    test_timeline_timeout_to_self();
    test_timeline_fired_before_stop();
    test_timeline_pending_at_stop();
    test_timeline_crash_between_commits();
    test_replay_matches_live_run();
    test_replayed_invocation_finalizes();
    test_publish_retires_invocations();
    test_result_codes();
    test_publish_out_of_memory();
    test_export_restores();
    test_export_restores_invocations();
    test_export_leaves_submitted();
    test_export_result_codes();
    CHECK(heap.stats.live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "replay_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("replay_test: ok\n");
    return 0;
}
