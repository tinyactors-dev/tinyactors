/*
 * ADR 0074: a stable named actor offloads to bytes and loads back from
 * them, in the system that wrote them; and a host re-arms an actor's
 * pending delayed sends when it spawns the actor again.
 *
 * Includes the amalgamation, so it can look at the actor table.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.c"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }

/* An allocator that counts live blocks and can refuse the n-th allocation. */
static long live_blocks;
static long allocations;
static long fail_at = -1;

static void *count_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    if (fail_at >= 0 && allocations++ == fail_at) {
        return NULL;
    }
    void *block = calloc(1, size == 0 ? 1 : size);
    live_blocks += block != NULL ? 1 : 0;
    return block;
}

static void count_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) {
        live_blocks -= 1;
        free(params->ptr);
    }
}

static const ta_allocator_vtable count_vtable = { .alloc = count_alloc, .free = count_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &count_vtable };

/* A one-address directory, enough for one named actor to unload and load. */
typedef struct {
    bool used;
    ta_actor_directory_entry entry;
} one_directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    one_directory *dir = self;
    if (dir->used) {
        return TA_ERR_STALE;
    }
    dir->used = true;
    dir->entry = params->entry;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    one_directory *dir = self;
    *params->out = dir->used ? dir->entry : (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static bool same_entry(ta_actor_directory_entry left, ta_actor_directory_entry right) {
    return left.kind == right.kind && (left.kind != TA_ACTOR_DIRECTORY_RESIDENT || left.actor == right.actor);
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    one_directory *dir = self;
    if (!dir->used || !same_entry(dir->entry, params->expected)) {
        return TA_ERR_STALE;
    }
    dir->entry = params->replacement;
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    one_directory *dir = self;
    if (!dir->used || !same_entry(dir->entry, params->expected)) {
        return TA_ERR_STALE;
    }
    dir->used = false;
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/*
 * The chart: `main` remembers its last child in a shallow history. Entering
 * `a` arms two delayed sends, `tick` with the author's ID "tick-1" and
 * `ping` with none. `go` moves to `b`, `leave` leaves `main`, `back`
 * returns through the history, and `stop` cancels "tick-1".
 */
static const ta_scxml_state_spec timed_chart[] = {
    {
        .kind = TA_SCXML_STATE_COMPOUND,
        .id = STR("main"),
        .states = (const ta_scxml_state_spec[]){
            { .kind = TA_SCXML_STATE_HISTORY_SHALLOW, .id = STR("h"),
              .transitions = (const ta_scxml_transition_spec[]){
                  { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("a"), {0} } }, {0} } },
            {
                .kind = TA_SCXML_STATE_ATOMIC,
                .id = STR("a"),
                .onentry = (const ta_scxml_handler_spec[]){
                    { .actions = (const ta_scxml_action_spec[]){
                          { .kind = TA_SCXML_ACTION_SEND,
                            .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("tick-1") },
                                         .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("tick") },
                                         .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("5s") } } },
                          { .kind = TA_SCXML_ACTION_SEND,
                            .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("ping") },
                                         .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("7s") } } },
                          {0} } },
                    {0} },
                .transitions = (const ta_scxml_transition_spec[]){
                    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"),
                      .targets = (const ta_str[]){ STR("b"), {0} } },
                    {0} },
            },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b") },
            {0},
        },
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("leave"),
              .targets = (const ta_str[]){ STR("away"), {0} } },
            { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("stop"),
              .actions = (const ta_scxml_action_spec[]){
                  { .kind = TA_SCXML_ACTION_CANCEL,
                    .as.cancel = { .sendid = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("tick-1") } } },
                  {0} } },
            {0} },
    },
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("away"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("back"),
              .targets = (const ta_str[]){ STR("h"), {0} } },
            {0} },
    },
    {0},
};

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    one_directory dir;
    ta_arena arena;
} fixture;

static const ta_actor_address the_address = { .family = { STR("timed") }, .name = STR("one") };

static void start(fixture *f) {
    *f = (fixture){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &f->system) == TA_OK);
    CHECK(ta_system_set_actor_directory(f->system, (ta_actor_directory){ .self = &f->dir, .v = &dir_vtable }) ==
          TA_OK);
    ta_definition_identity identity = {
        .family = { STR("timed") },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              f->system, &(ta_definition_compile_params){
                             .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                       .states = timed_chart },
                             .datamodel = ta_null_datamodel(),
                             .allocator = test_allocator,
                             .out = &definition,
                         }) == TA_OK);
    CHECK(ta_system_register_definition(f->system, definition, &f->definition) == TA_OK);
    CHECK(ta_arena_init(&f->arena, test_allocator, 256) == TA_OK);
}

static void finish(fixture *f) {
    ta_arena_destroy(&f->arena);
    ta_system_destroy(f->system);
}

static void run(fixture *f, ta_actor_id actor) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(f->system, &(ta_actor_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
    CHECK(status == TA_RUN_STABLE);
}

static ta_actor_id spawn(fixture *f, const ta_actor_address *address) {
    ta_actor_id id = 0;
    CHECK(ta_actor_spawn(f->system, &(ta_actor_spawn_params){ .definition = f->definition, .address = address,
                                                            .out = &id }) == TA_OK);
    run(f, id);
    return id;
}

static void deliver(fixture *f, ta_actor_id actor, const char *name) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(f->system, (ta_str){ .ptr = name, .len = (uint32_t)strlen(name) }, &atom) == TA_OK);
    ta_event_name_id event = 0;
    CHECK(ta_event_name_intern(f->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &event) == TA_OK);
    CHECK(ta_actor_deliver(f->system, &(ta_actor_deliver_params){ .time = ta_system_time(f->system),
                                          .actor = actor,
                                          .event = &(ta_event){ .name = event, .type = TA_EVENT_EXTERNAL } }) == TA_OK);
    run(f, actor);
}

/* True when `state` of the chart is active in `actor`. */
static bool active(fixture *f, ta_actor_id actor, const char *id) {
    ta_state_id state = 0;
    CHECK(ta_definition_find_state(f->system, &(ta_definition_find_state_params){
                                                  .definition = f->definition,
                                                  .id = { .ptr = id, .len = (uint32_t)strlen(id) },
                                                  .out = &state }) == TA_OK);
    ta_state_id states[16];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(f->system, &(ta_actor_active_states_params){
                                                .actor = actor, .states = states, .capacity = 16, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count && index < 16; index += 1) {
        if (states[index] == state) {
            return true;
        }
    }
    return false;
}

static ta_actor_inspection inspect(fixture *f, ta_actor_id actor) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f->system, actor, &view) == TA_OK);
    return view;
}

static ta_bytes offload(fixture *f, ta_actor_id actor) {
    ta_bytes bytes = {0};
    CHECK(ta_actor_offload(f->system, &(ta_actor_offload_params){ .actor = actor, .arena = &f->arena,
                                                                  .out = &bytes }) == TA_OK);
    return bytes;
}

static ta_actor_id load(fixture *f, ta_bytes bytes) {
    ta_actor_id id = 0;
    CHECK(ta_actor_load(f->system, &(ta_actor_load_params){ .bytes = bytes, .out = &id }) == TA_OK);
    return id;
}

/* An offloaded actor comes back with its session, state and delayed sends. */
static void test_round_trip(void) {
    fixture f;
    start(&f);
    ta_actor_id actor = spawn(&f, &the_address);
    ta_actor_inspection before = inspect(&f, actor);
    CHECK(before.delayed_send_count == 2);
    ta_delayed_send_info sends[2];
    memcpy(sends, before.delayed_sends, sizeof sends);
    uint64_t session = before.session_id;

    ta_bytes bytes = offload(&f, actor);
    CHECK(bytes.len > 0);
    CHECK(ta_actor_lookup(f.system, actor) == NULL);
    CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_UNLOADED);

    ta_actor_id loaded = load(&f, bytes);
    CHECK(loaded != actor);
    CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_RESIDENT && f.dir.entry.actor == loaded);
    ta_actor_inspection after = inspect(&f, loaded);
    CHECK(after.session_id == session);
    CHECK(active(&f, loaded, "a"));
    CHECK(after.delayed_send_count == 2);
    for (uint32_t index = 0; index < 2 && after.delayed_send_count == 2; index += 1) {
        CHECK(after.delayed_sends[index].send_id == sends[index].send_id);
        CHECK(after.delayed_sends[index].due == sends[index].due);
        CHECK(after.delayed_sends[index].event == sends[index].event);
    }
    /* It runs on from where it was. */
    deliver(&f, loaded, "go");
    CHECK(active(&f, loaded, "b"));
    finish(&f);
}

/* The history a state recorded before the offload is there after the load. */
static void test_history_survives(void) {
    fixture f;
    start(&f);
    ta_actor_id actor = spawn(&f, &the_address);
    deliver(&f, actor, "go");
    deliver(&f, actor, "leave");
    CHECK(active(&f, actor, "away"));
    actor = load(&f, offload(&f, actor));
    deliver(&f, actor, "back");
    CHECK(active(&f, actor, "b"));
    CHECK(!active(&f, actor, "a"));
    finish(&f);
}

/* Only a stable named actor offloads, and a refusal changes nothing. */
static void test_refusals(void) {
    fixture f;
    start(&f);
    ta_actor_id anonymous = spawn(&f, NULL);
    ta_bytes bytes = {0};
    CHECK(ta_actor_offload(f.system, &(ta_actor_offload_params){ .actor = anonymous, .arena = &f.arena,
                                                                 .out = &bytes }) == TA_ERR_INVALID);
    ta_actor_id named = 0;
    /* Spawned but not bootstrapped: its first macrostep has not run. */
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.definition, .address = &the_address,
                                                           .out = &named }) == TA_OK);
    CHECK(ta_actor_offload(f.system, &(ta_actor_offload_params){ .actor = named, .arena = &f.arena,
                                                                 .out = &bytes }) == TA_ERR_BUSY);
    CHECK(bytes.len == 0);
    CHECK(ta_actor_lookup(f.system, named) != NULL);
    CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_RESIDENT);
    run(&f, named);
    CHECK(offload(&f, named).len > 0);
    finish(&f);
}

/* Bytes only load in the system that wrote them. */
static void test_other_system_is_stale(void) {
    fixture first;
    fixture second;
    start(&first);
    start(&second);
    ta_bytes bytes = offload(&first, spawn(&first, &the_address));
    ta_actor_id id = 0;
    CHECK(ta_actor_load(second.system, &(ta_actor_load_params){ .bytes = bytes, .out = &id }) == TA_ERR_STALE);
    /* Garbage is refused, too. */
    uint8_t junk[16] = {0};
    CHECK(ta_actor_load(first.system, &(ta_actor_load_params){ .bytes = { .ptr = junk, .len = sizeof junk },
                                                              .out = &id }) == TA_ERR_INVALID);
    CHECK(load(&first, bytes) != 0);
    finish(&second);
    finish(&first);
}

/*
 * ADR 0070: each allocation of an offload, and of a load, fails in turn.
 * A failed offload leaves the actor resident; a failed load leaves it
 * unloaded with its bytes intact; and nothing leaks.
 */
static void test_allocation_failures(void) {
    for (long step = 0;; step += 1) {
        fixture f;
        start(&f);
        ta_actor_id actor = spawn(&f, &the_address);
        allocations = 0;
        fail_at = step;
        ta_bytes bytes = {0};
        ta_result result = ta_actor_offload(f.system, &(ta_actor_offload_params){ .actor = actor, .arena = &f.arena,
                                                                                  .out = &bytes });
        fail_at = -1;
        if (result == TA_OK) {
            finish(&f);
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(ta_actor_lookup(f.system, actor) != NULL);
        CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_RESIDENT);
        CHECK(offload(&f, actor).len > 0);
        finish(&f);
    }
    for (long step = 0;; step += 1) {
        fixture f;
        start(&f);
        ta_bytes bytes = offload(&f, spawn(&f, &the_address));
        allocations = 0;
        fail_at = step;
        ta_actor_id id = 0;
        ta_result result = ta_actor_load(f.system, &(ta_actor_load_params){ .bytes = bytes, .out = &id });
        fail_at = -1;
        if (result == TA_OK) {
            finish(&f);
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_UNLOADED);
        ta_actor_id again = load(&f, bytes);
        CHECK(active(&f, again, "a"));
        finish(&f);
    }
}

/*
 * ADR 0074 section 8: the pending sends of one session, captured through
 * inspection, are re-armed when the actor is spawned again. They keep their
 * IDs and due times; the new session's own generated IDs do not collide
 * with them; and <cancel> finds a re-armed send by its ID.
 */
static void test_rearm_at_spawn(void) {
    fixture f;
    start(&f);
    CHECK(ta_system_set_time(f.system, 1000) == TA_OK);
    ta_actor_id first = spawn(&f, NULL);
    ta_actor_inspection view = inspect(&f, first);
    CHECK(view.delayed_send_count == 2);
    ta_delayed_send_info captured[2];
    memcpy(captured, view.delayed_sends, sizeof captured);
    CHECK(ta_actor_destroy(f.system, first) == TA_OK);
    /* By due time: "tick-1" (6000), then "ping" (8000), which has no ID. A
     * generated ID needs idlocation, which the null data model cannot
     * express, so "ping" is given one here. */
    CHECK(captured[0].due == 6000 && captured[1].due == 8000);
    captured[1].send_id = TA_GENERATED_SEND_ID_BIT | 5u;
    /* Both are sends to self, resolved to the first session's runtime ID.
     * A host stores such a destination as runtime ID 0, which re-arms it
     * to the new session; another actor takes the first one's slot. */
    CHECK(captured[0].destination.kind == TA_ACTOR_REF_RUNTIME && captured[0].destination.as.runtime == first);
    captured[0].destination.as.runtime = 0;
    captured[1].destination.as.runtime = 0;
    ta_actor_id squatter = spawn(&f, NULL);

    CHECK(ta_system_set_time(f.system, 3000) == TA_OK);
    ta_actor_id second = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.definition,
                                                            .pending_sends = captured,
                                                            .pending_send_count = 2,
                                                            .out = &second }) == TA_OK);
    /* The session's own generated IDs start past the re-armed #_send.5. */
    CHECK(ta_actor_lookup(f.system, second)->next_send_id == 5u);
    run(&f, second);
    view = inspect(&f, second);
    /* The two re-armed, then the two its own bootstrap armed at time 3000. */
    CHECK(view.delayed_send_count == 4);
    uint32_t rearmed = 0;
    for (uint32_t index = 0; index < view.delayed_send_count; index += 1) {
        const ta_delayed_send_info *send = &view.delayed_sends[index];
        for (uint32_t old = 0; old < 2; old += 1) {
            rearmed += send->send_id == captured[old].send_id && send->due == captured[old].due ? 1u : 0u;
        }
    }
    CHECK(rearmed == 2);
    for (uint32_t index = 0; index < view.delayed_send_count; index += 1) {
        const ta_actor_ref *to = &view.delayed_sends[index].destination;
        CHECK(to->kind == TA_ACTOR_REF_RUNTIME && to->as.runtime == second && to->as.runtime != squatter);
    }

    /* <cancel sendid="tick-1"> finds the re-armed send, and the new one. */
    deliver(&f, second, "stop");
    view = inspect(&f, second);
    CHECK(view.delayed_send_count == 2);
    for (uint32_t index = 0; index < view.delayed_send_count; index += 1) {
        CHECK(view.delayed_sends[index].send_id != captured[0].send_id);
    }

    /* A record naming an event this system never issued is refused. */
    ta_delayed_send_info bad = captured[0];
    bad.event = 0xfffffff0u;
    ta_actor_id refused = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.definition, .pending_sends = &bad,
                                                            .pending_send_count = 1,
                                                            .out = &refused }) == TA_ERR_INVALID);
    finish(&f);
}

/*
 * ADR 0090 sections 1 and 2: values reach the offload through the data
 * model's streaming codec. A blob value is written in pieces of
 * `write_piece` bytes and read back in pieces of `read_piece`, so the
 * arena sink and the memory source see every split.
 */
typedef struct {
    long refs;
    uint32_t len;
    uint8_t bytes[];
} blob;

static long live_blobs;
static uint32_t write_piece = 7;
static uint32_t read_piece = 7;
static bool write_refuses;

static void blob_retain(void *self) {
    ((blob *)self)->refs += 1;
}

static void blob_release(void *self) {
    blob *b = self;
    if (--b->refs == 0) {
        live_blobs -= 1;
        free(b);
    }
}

static const ta_value_vtable blob_vtable = { .retain = blob_retain, .release = blob_release };

static ta_value blob_make(uint32_t len, uint8_t seed) {
    blob *b = malloc(sizeof(blob) + len);
    b->refs = 1;
    b->len = len;
    for (uint32_t index = 0; index < len; index += 1) {
        b->bytes[index] = (uint8_t)(seed + index * 31u);
    }
    live_blobs += 1;
    return (ta_value){ .self = b, .v = &blob_vtable };
}

static bool blob_equal(const ta_value *a, const ta_value *b) {
    const blob *x = a->self;
    const blob *y = b->self;
    return a->v == &blob_vtable && b->v == &blob_vtable && x->len == y->len && memcmp(x->bytes, y->bytes, x->len) == 0;
}

static ta_op_status blob_write(void *self, const ta_datamodel_value_write_params *params) {
    (void)self;
    CHECK(!params->durable);
    if (write_refuses) {
        *params->fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED, .message = STR("refused"), .data = blob_make(3, 1) };
        return TA_OP_ERROR;
    }
    const blob *b = params->value->self;
    uint32_t len = b->len;
    ta_op_status status = params->sink.v->write(params->sink.self,
                                                (ta_bytes){ .ptr = (const uint8_t *)&len, .len = sizeof len });
    for (uint32_t at = 0; status == TA_OP_OK && at < b->len; at += write_piece) {
        uint32_t count = b->len - at < write_piece ? b->len - at : write_piece;
        status = params->sink.v->write(params->sink.self, (ta_bytes){ .ptr = b->bytes + at, .len = count });
    }
    return status;
}

static ta_op_status blob_read(void *self, const ta_datamodel_value_read_params *params) {
    (void)self;
    uint8_t piece[4096];
    uint8_t header[sizeof(uint32_t)];
    uint32_t have = 0;
    uint32_t got = 0;
    blob *b = NULL;
    uint32_t filled = 0;
    for (;;) {
        uint32_t want = read_piece < sizeof piece ? read_piece : (uint32_t)sizeof piece;
        ta_op_status status = params->source.v->read(params->source.self, &(ta_byte_read_params){
                                                                              .buffer = piece,
                                                                              .capacity = want,
                                                                              .got = &got });
        if (status != TA_OP_OK) {
            return status;
        }
        if (got == 0) {
            break;
        }
        for (uint32_t index = 0; index < got; index += 1) {
            if (have < sizeof header) {
                header[have++] = piece[index];
                if (have == sizeof header) {
                    uint32_t len;
                    memcpy(&len, header, sizeof len);
                    ta_value made = blob_make(len, 0);
                    b = made.self;
                }
            } else if (b != NULL && filled < b->len) {
                b->bytes[filled++] = piece[index];
            }
        }
    }
    if (b == NULL || filled != b->len) {
        if (b != NULL) {
            blob_release(b);
        }
        *params->fault = (ta_fault){ .code = TA_ERR_INVALID, .message = STR("no blob") };
        return TA_OP_ERROR;
    }
    *params->out = (ta_value){ .self = b, .v = &blob_vtable };
    return TA_OP_OK;
}

static ta_datamodel_vtable blob_model_vtable;

/* Unlike the null data model it wraps, it takes a binding: the root. */
static ta_result blob_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

/* The timed chart again, on a data model whose values are blobs. */
static void start_blobs(fixture *f) {
    *f = (fixture){0};
    blob_model_vtable = *ta_null_datamodel().v;
    blob_model_vtable.value_write = blob_write;
    blob_model_vtable.value_read = blob_read;
    blob_model_vtable.init_instance = blob_init_instance;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &f->system) == TA_OK);
    CHECK(ta_system_set_actor_directory(f->system, (ta_actor_directory){ .self = &f->dir, .v = &dir_vtable }) ==
          TA_OK);
    ta_definition_identity identity = {
        .family = { STR("timed") },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              f->system, &(ta_definition_compile_params){
                             .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                       .states = timed_chart },
                             .datamodel = { .self = NULL, .v = &blob_model_vtable },
                             .allocator = test_allocator,
                             .out = &definition,
                         }) == TA_OK);
    CHECK(ta_system_register_definition(f->system, definition, &f->definition) == TA_OK);
    CHECK(ta_arena_init(&f->arena, test_allocator, 256) == TA_OK);
}

/* A root of 10,000 bytes goes out and comes back whole, for every split. */
static void test_streaming_codec(void) {
    static const uint32_t pieces[][2] = { { 1, 1 }, { 7, 7 }, { 7, 1 }, { 1, 4096 }, { 4096, 3 } };
    for (size_t index = 0; index < sizeof pieces / sizeof pieces[0]; index += 1) {
        write_piece = pieces[index][0];
        read_piece = pieces[index][1];
        fixture f;
        start_blobs(&f);
        ta_actor_id actor = spawn(&f, &the_address);
        ta_value root = blob_make(10000, (uint8_t)index);
        ta_bytes bytes = {0};
        CHECK(ta_actor_offload(f.system, &(ta_actor_offload_params){ .actor = actor, .root = &root,
                                                                     .arena = &f.arena, .out = &bytes }) == TA_OK);
        ta_value back = {0};
        ta_actor_id id = 0;
        CHECK(ta_actor_load(f.system, &(ta_actor_load_params){ .bytes = bytes, .out = &id, .root = &back }) == TA_OK);
        CHECK(blob_equal(&root, &back));
        CHECK(active(&f, id, "a"));
        ta_value_release(&back);
        ta_value_release(&root);
        finish(&f);
    }
    write_piece = 7;
    read_piece = 7;
    CHECK(live_blobs == 0);
}

/* A value the data model will not write refuses the offload, and the fault's data is released. */
static void test_codec_refusal(void) {
    fixture f;
    start_blobs(&f);
    ta_actor_id actor = spawn(&f, &the_address);
    ta_value root = blob_make(16, 9);
    write_refuses = true;
    ta_bytes bytes = {0};
    CHECK(ta_actor_offload(f.system, &(ta_actor_offload_params){ .actor = actor, .root = &root,
                                                                 .arena = &f.arena, .out = &bytes }) ==
          TA_ERR_UNSUPPORTED);
    write_refuses = false;
    CHECK(bytes.len == 0);
    CHECK(ta_actor_lookup(f.system, actor) != NULL);
    CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_RESIDENT);
    ta_value_release(&root);
    finish(&f);
    CHECK(live_blobs == 0);
}

/* ADR 0070 with values: the arena sink's growth fails at each allocation in turn. */
static void test_codec_allocation_failures(void) {
    write_piece = 1;
    for (long step = 0;; step += 1) {
        fixture f;
        start_blobs(&f);
        ta_actor_id actor = spawn(&f, &the_address);
        ta_value root = blob_make(3000, 5);
        allocations = 0;
        fail_at = step;
        ta_bytes bytes = {0};
        ta_result result = ta_actor_offload(f.system, &(ta_actor_offload_params){ .actor = actor, .root = &root,
                                                                                  .arena = &f.arena, .out = &bytes });
        fail_at = -1;
        if (result == TA_OK) {
            ta_value back = {0};
            ta_actor_id id = 0;
            CHECK(ta_actor_load(f.system, &(ta_actor_load_params){ .bytes = bytes, .out = &id, .root = &back }) ==
                  TA_OK);
            CHECK(blob_equal(&root, &back));
            ta_value_release(&back);
            ta_value_release(&root);
            finish(&f);
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(ta_actor_lookup(f.system, actor) != NULL);
        CHECK(f.dir.entry.kind == TA_ACTOR_DIRECTORY_RESIDENT);
        ta_value_release(&root);
        finish(&f);
    }
    write_piece = 7;
    CHECK(live_blobs == 0);
}

int main(void) {
    test_round_trip();
    test_history_survives();
    test_refusals();
    test_other_system_is_stale();
    test_allocation_failures();
    test_rearm_at_spawn();
    test_streaming_codec();
    test_codec_refusal();
    test_codec_allocation_failures();
    CHECK(live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "offload_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("offload_test: passed\n");
    return 0;
}
