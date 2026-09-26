/*
 * An image restores into any system that registers its revision (ADR 0085),
 * exercised through the public header and the amalgamation only.
 *
 * A second system interns names in its own order, so every event name, send
 * ID and I/O type in a copy must be interned there again from the image's
 * text; runtime references to other actors of the capturing system must
 * name nothing; the copy's delayed sends fire on the second system's clock,
 * and the capturing system's actor is untouched. The image outlives the
 * system that captured it, an adopter gives the second system the image's
 * host values, and a restore that runs out of memory at any allocation
 * leaves nothing behind.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"

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

/* ------------------------------------------------------------------------
 * A counting allocator that can refuse the n-th allocation from now.
 */

static long live_blocks;
static long allocations;
static long fail_at = -1;

static void *count_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    if (fail_at == 0) {
        fail_at = -1;
        return NULL;
    }
    if (fail_at > 0) {
        fail_at -= 1;
    }
    void *block = malloc(size == 0 ? 1 : size);
    allocations += 1;
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
static const ta_allocator allocator = { .self = NULL, .v = &count_vtable };

/* ------------------------------------------------------------------------
 * Host values that count their references, and remember which system's
 * host made them.
 */

typedef struct {
    int references;
    int owner;
} counted_value;

static void counted_retain(void *self) {
    ((counted_value *)self)->references += 1;
}

static void counted_release(void *self) {
    counted_value *value = self;
    value->references -= 1;
    if (value->references == 0 && value->owner != 0) {
        free(value); /* a copy an adopter made; the originals live on the stack */
    }
}

static const ta_value_vtable counted_vtable = { .retain = counted_retain, .release = counted_release };

/* Copies each value into a new one owned by `owner`; refuses when asked. */
typedef struct {
    int owner;
    int adopted;
    bool refuse;
} copying_adopter;

static ta_result copy_value(void *self, const ta_value *value, ta_value *out) {
    copying_adopter *adopter = self;
    if (adopter->refuse) {
        return TA_ERR_NOMEM;
    }
    counted_value *copy = malloc(sizeof *copy);
    *copy = (counted_value){ .references = 1, .owner = adopter->owner };
    (void)value;
    adopter->adopted += 1;
    *out = (ta_value){ .self = copy, .v = &counted_vtable };
    return TA_OK;
}

static const ta_value_adopter_vtable copying_vtable = { .adopt = copy_value };

/* ------------------------------------------------------------------------
 * A router that delivers into its own system, as a host's does, and
 * answers ERROR for an actor that is not there.
 */

typedef struct {
    ta_system *system;
    int delivered;
    int undeliverable;
} router_state;

static ta_external_event_submit_status route(void *self, const ta_external_event_submit_params *params) {
    router_state *router = self;
    const ta_external_delivery *delivery = params->delivery;
    ta_result result = TA_ERR_NOT_FOUND;
    if (delivery->destination.kind == TA_ACTOR_REF_RUNTIME) {
        result = ta_actor_deliver(router->system, &(ta_actor_deliver_params){
                                                      .actor = delivery->destination.as.runtime,
                                                      .event = &delivery->event });
    }
    if (result == TA_ERR_BUSY) {
        return TA_EXTERNAL_EVENT_RETRY;
    }
    router->delivered += result == TA_OK ? 1 : 0;
    router->undeliverable += result == TA_OK ? 0 : 1;
    return result == TA_OK ? TA_EXTERNAL_EVENT_ACCEPTED : TA_EXTERNAL_EVENT_ERROR;
}

static const ta_external_event_router_vtable router_vtable = { .submit = route };

/* ------------------------------------------------------------------------
 * Charts. `forked` arms a send to itself and one to another actor of its
 * system, both with literal IDs; the tick moves it on, and the failed send
 * to the other actor (error.communication) cuts it.
 */

#define SEND(id_text, event_text, delay_text, target_str)                                         \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .target = target_str,                                                          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }

#define GO(event_text, target_text)                                                               \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text),                             \
      .targets = (const ta_str[]){ STR(target_text), {0} } }

/* The receiver's session URI: it is the first actor of its system. */
static char receiver_uri[64];

static ta_scxml_action_spec forked_sends[] = {
    SEND("tick", "timer.tick", "10s", {0}),
    SEND("report", "report.status", "20s", {0}),
    {0},
};

static const ta_scxml_state_spec forked_chart[] = {
    { .kind = TA_SCXML_STATE_ATOMIC,
      .id = STR("waiting"),
      .onentry = (const ta_scxml_handler_spec[]){ { .actions = forked_sends }, {0} },
      .transitions = (const ta_scxml_transition_spec[]){ GO("timer.tick", "ticked"), GO("poke", "poked"), {0} } },
    { .kind = TA_SCXML_STATE_ATOMIC,
      .id = STR("ticked"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("report.status", "reported"),
                                                         GO("error.communication", "cut"), {0} } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("poked") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("reported") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("cut") },
    {0},
};

static const ta_scxml_state_spec receiver_chart[] = {
    { .kind = TA_SCXML_STATE_ATOMIC,
      .id = STR("listening"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("report.status", "reported"), {0} } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("reported") },
    {0},
};

/* ------------------------------------------------------------------------
 * Helpers
 */

typedef struct {
    ta_system *system;
    router_state router;
    ta_definition_id forked;
    ta_definition_id receiver;
} world;

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

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atoms[4];
    uint32_t count = 0;
    const char *start = text;
    for (const char *at = text;; at += 1) {
        if (*at == '.' || *at == '\0') {
            CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = start, .len = (uint32_t)(at - start) },
                                        &atoms[count]) == TA_OK);
            count += 1;
            start = at + 1;
        }
        if (*at == '\0') {
            break;
        }
    }
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = atoms, .count = count }, &name) == TA_OK);
    return name;
}

/*
 * A system whose tables already hold other names, so the ones a copy uses
 * get other IDs than in the capturing system; with `skew`, other actors
 * too, so runtime IDs of the capturing system name other actors here.
 */
static void create(world *w, int skew) {
    *w = (world){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &w->system) == TA_OK);
    w->router.system = w->system;
    CHECK(ta_system_set_external_event_router(
              w->system, (ta_external_event_router){ .self = &w->router, .v = &router_vtable }) == TA_OK);
    for (int index = 0; index < skew; index += 1) {
        (void)event_name(w->system, index % 2 == 0 ? "zeta.one" : "report.other");
        ta_atom atom = 0;
        CHECK(ta_system_intern_atom(w->system, (ta_str)STR("filler"), &atom) == TA_OK);
    }
    w->receiver = define(w->system, "receiver", receiver_chart);
    w->forked = define(w->system, "forked", forked_chart);
}

static void run(world *w, ta_actor_id actor) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(w->system, &(ta_actor_resume_params){ .actor = actor, .budget = 100,
                                                               .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE);
}

static ta_actor_id spawn(world *w, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(w->system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    run(w, actor);
    return actor;
}

/* Moves the system's clock and runs what that makes due in `actors`. */
static void advance(world *w, ta_time now, const ta_actor_id *actors, uint32_t count) {
    CHECK(ta_system_set_time(w->system, now) == TA_OK);
    uint32_t dispatched = 0;
    bool more = true;
    while (more) {
        CHECK(ta_system_dispatch_due(w->system, &(ta_due_dispatch_params){ .budget = 16, .dispatched = &dispatched,
                                                                          .more_due = &more }) == TA_OK);
        for (uint32_t index = 0; index < count; index += 1) {
            run(w, actors[index]);
        }
    }
}

static bool in_state(world *w, ta_definition_id definition, ta_actor_id actor, const char *state) {
    ta_state_id wanted = TA_STATE_NONE;
    ta_state_id active[4];
    uint32_t count = 0;
    if (ta_definition_find_state(w->system, &(ta_definition_find_state_params){
                                                .definition = definition,
                                                .id = { .ptr = state, .len = (uint32_t)strlen(state) },
                                                .out = &wanted }) != TA_OK ||
        ta_actor_active_states(w->system, &(ta_actor_active_states_params){
                                              .actor = actor, .states = active, .capacity = 4, .count = &count }) !=
            TA_OK) {
        return false;
    }
    return count == 1 && active[0] == wanted;
}

/* The receiver is spawned first, and the forked actor sends to its session. */
static ta_actor_id start_forked(world *w, ta_actor_id *receiver) {
    *receiver = spawn(w, w->receiver);
    CHECK(*receiver == ta_actor_id_make(0, 1));
    return spawn(w, w->forked);
}

/* ------------------------------------------------------------------------
 * Tests
 */

/*
 * The copy interns its names in the second system; its send to itself
 * fires on the second system's clock, and its send to the receiver, an
 * actor of the capturing system, names nothing there and fails as a send
 * to a destroyed actor does. The original is untouched and runs on its
 * own clock.
 */
static void test_a_copy_runs_on_its_own_system(void) {
    world a;
    world b;
    create(&a, 0);
    create(&b, 3);
    ta_actor_id receiver = 0;
    ta_actor_id original = start_forked(&a, &receiver);
    /* Other actors in B, so A's runtime IDs are B's actors too. */
    ta_actor_id bystanders[2] = { spawn(&b, b.receiver), spawn(&b, b.receiver) };
    CHECK(bystanders[0] == receiver);

    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(a.system, &(ta_actor_capture_params){ .actor = original, .out = &image }) == TA_OK);
    ta_actor_id copy = 0;
    CHECK(ta_actor_reconstruct(b.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy }) == TA_OK);

    ta_actor_inspection view;
    CHECK(ta_actor_inspect(b.system, copy, &view) == TA_OK);
    CHECK(view.session_id == copy); /* a new session: A's session ID is A's runtime ID */
    CHECK(view.delayed_send_count == 2);
    if (view.delayed_send_count == 2) {
        ta_send_id tick = 0;
        ta_send_id report = 0;
        CHECK(ta_send_id_parse(b.system, (ta_str)STR("tick"), &tick) == TA_OK);
        CHECK(ta_send_id_parse(b.system, (ta_str)STR("report"), &report) == TA_OK);
        CHECK(view.delayed_sends[0].event == event_name(b.system, "timer.tick"));
        CHECK(view.delayed_sends[0].send_id == tick);
        CHECK(view.delayed_sends[0].io_type == ta_system_scxml_io_type(b.system));
        CHECK(view.delayed_sends[0].destination.kind == TA_ACTOR_REF_RUNTIME &&
              view.delayed_sends[0].destination.as.runtime == copy);
        CHECK(view.delayed_sends[1].event == event_name(b.system, "report.status"));
        CHECK(view.delayed_sends[1].send_id == report);
        CHECK(view.delayed_sends[1].destination.kind == TA_ACTOR_REF_RUNTIME &&
              view.delayed_sends[1].destination.as.runtime == 0);
        CHECK(view.delayed_sends[0].event != event_name(a.system, "timer.tick"));
    }
    ta_time deadline = 0;
    CHECK(ta_system_next_deadline(b.system, &deadline) == TA_OK && deadline == 10000);

    /* B's clock alone: the copy ticks, then its report fails. */
    ta_actor_id b_actors[3] = { copy, bystanders[0], bystanders[1] };
    advance(&b, 10000, b_actors, 3);
    CHECK(in_state(&b, b.forked, copy, "ticked"));
    CHECK(in_state(&a, a.forked, original, "waiting"));
    advance(&b, 20000, b_actors, 3);
    CHECK(in_state(&b, b.forked, copy, "cut"));
    CHECK(in_state(&b, b.receiver, bystanders[0], "listening")); /* A's receiver's ID, not reached */
    CHECK(b.router.undeliverable == 1);
    CHECK(a.router.delivered == 0 && a.router.undeliverable == 0);

    /* A's clock: the original reaches its receiver. */
    ta_actor_id a_actors[2] = { original, receiver };
    advance(&a, 20000, a_actors, 2);
    CHECK(in_state(&a, a.forked, original, "reported") || in_state(&a, a.forked, original, "ticked"));
    CHECK(in_state(&a, a.receiver, receiver, "reported"));

    ta_session_image_destroy(image);
    ta_system_destroy(a.system);
    ta_system_destroy(b.system);
}

/*
 * The image outlives the system that captured it: a copy restores into
 * another system after it is gone, and a system created after that does
 * not take the image for its own.
 */
static void test_an_image_outlives_its_system(void) {
    world a;
    create(&a, 0);
    ta_actor_id receiver = 0;
    ta_actor_id original = start_forked(&a, &receiver);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(a.system, &(ta_actor_capture_params){ .actor = original, .out = &image }) == TA_OK);
    ta_system_destroy(a.system);

    world c;
    create(&c, 0);
    /* The receiver again, at the runtime ID it had in A, but another system;
     * and the forked actor's ID in A is free here, and will be issued again. */
    ta_actor_id other = spawn(&c, c.receiver);
    CHECK(other == receiver);
    ta_actor_id gone = spawn(&c, c.receiver);
    CHECK(gone == original);
    CHECK(ta_actor_destroy(c.system, gone) == TA_OK);
    ta_actor_id copy = 0;
    CHECK(ta_actor_reconstruct(c.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(c.system, copy, &view) == TA_OK && view.delayed_send_count == 2);
    CHECK(view.session_id == copy && copy != original); /* not A's session ID, which is C's to issue */
    CHECK(view.delayed_send_count != 2 || view.delayed_sends[1].destination.as.runtime == 0);
    ta_session_image_destroy(image);
    ta_system_destroy(c.system);
}

/*
 * The adopter gives the second system the image's host values: each is
 * adopted once, the copy holds the adopted one, and the image keeps its
 * own. A refused adoption fails the restore and leaves nothing behind.
 */
static void test_the_adopter_gives_values_to_the_restoring_system(void) {
    world a;
    world b;
    create(&a, 0);
    create(&b, 1);
    ta_actor_id receiver = 0;
    ta_actor_id original = start_forked(&a, &receiver);
    counted_value data = { .references = 1 };
    ta_event poke = { .name = event_name(a.system, "poke"), .type = TA_EVENT_EXTERNAL,
                      .invoke_id = TA_INVOKE_ID_NONE, .data = { .self = &data, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(a.system, &(ta_actor_deliver_params){ .actor = original, .event = &poke }) == TA_OK);
    CHECK(data.references == 2);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(a.system, &(ta_actor_capture_params){ .actor = original, .out = &image }) == TA_OK);
    CHECK(data.references == 3);

    copying_adopter refusing = { .owner = 2, .refuse = true };
    ta_actor_id copy = 0;
    CHECK(ta_actor_reconstruct(b.system, &(ta_actor_reconstruct_params){
                                             .image = image,
                                             .out = &copy,
                                             .adopter = { .self = &refusing, .v = &copying_vtable } }) ==
          TA_ERR_NOMEM);
    CHECK(data.references == 3);

    copying_adopter copying = { .owner = 2 };
    CHECK(ta_actor_reconstruct(b.system, &(ta_actor_reconstruct_params){
                                             .image = image,
                                             .out = &copy,
                                             .adopter = { .self = &copying, .v = &copying_vtable } }) == TA_OK);
    CHECK(copying.adopted == 1);
    CHECK(data.references == 3);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(b.system, copy, &view) == TA_OK);
    CHECK(view.current_event != NULL && view.current_event->data.v == &counted_vtable &&
          ((counted_value *)view.current_event->data.self)->owner == 2);
    run(&b, copy);
    CHECK(in_state(&b, b.forked, copy, "poked"));

    ta_session_image_destroy(image);
    CHECK(data.references == 2);
    ta_system_destroy(b.system);
    ta_system_destroy(a.system);
    CHECK(data.references == 1);
}

/*
 * An image of another system cannot unload this system's actor, and
 * ta_actor_load() restores it there. The directory accepts every change.
 */
static ta_result claim(void *self, const ta_actor_directory_claim_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static ta_result resolve(void *self, const ta_actor_directory_resolve_params *params) {
    (void)self;
    *params->out = (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static ta_result replace(void *self, const ta_actor_directory_replace_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static ta_result remove_entry(void *self, const ta_actor_directory_remove_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static const ta_actor_directory_vtable directory_vtable = {
    .claim = claim, .resolve = resolve, .replace = replace, .remove = remove_entry,
};

static void test_unload_and_load_across_systems(void) {
    world a;
    world b;
    create(&a, 0);
    create(&b, 0);
    ta_actor_address address = { .family = { STR("forked") }, .name = STR("f1") };
    ta_actor_id in_a = 0;
    ta_actor_id in_b = 0;
    CHECK(ta_system_set_actor_directory(a.system, (ta_actor_directory){ .v = &directory_vtable }) == TA_OK);
    CHECK(ta_system_set_actor_directory(b.system, (ta_actor_directory){ .v = &directory_vtable }) == TA_OK);
    CHECK(ta_actor_spawn(a.system, &(ta_actor_spawn_params){ .definition = a.forked, .address = &address,
                                                             .out = &in_a }) == TA_OK);
    CHECK(ta_actor_spawn(b.system, &(ta_actor_spawn_params){ .definition = b.forked, .address = &address,
                                                             .out = &in_b }) == TA_OK);
    run(&a, in_a);
    run(&b, in_b);
    CHECK(in_a == in_b); /* the same runtime ID and epoch, in two systems */
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(a.system, &(ta_actor_capture_params){ .actor = in_a, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(b.system, &(ta_actor_unload_params){ .actor = in_b, .image = image }) == TA_ERR_STALE);
    CHECK(ta_actor_unload(a.system, &(ta_actor_unload_params){ .actor = in_a, .image = image }) == TA_OK);

    /* Loaded into B, where the directory has it unloaded too: the address is
     * interned in B, and the session is a new one of B's. */
    ta_actor_id loaded = 0;
    CHECK(ta_actor_destroy(b.system, in_b) == TA_OK);
    CHECK(ta_actor_load(b.system, &(ta_actor_load_params){ .image = image, .out = &loaded }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(b.system, loaded, &view) == TA_OK);
    CHECK(view.self.kind == TA_ACTOR_REF_NAMED && view.session_id == loaded);
    CHECK(view.delayed_send_count == 2);
    ta_session_image_destroy(image);
    ta_system_destroy(a.system);
    ta_system_destroy(b.system);
}

/*
 * A restore into another system allocates more than one into the capturing
 * system: the names it interns there. Refusing each of its allocations in
 * turn, against a fresh system each time, fails the restore with
 * TA_ERR_NOMEM and leaves no actor; the system then restores the image.
 */
static void test_a_restore_that_runs_out_of_memory_leaves_nothing(void) {
    world a;
    create(&a, 0);
    ta_actor_id receiver = 0;
    ta_actor_id original = start_forked(&a, &receiver);
    counted_value data = { .references = 1 };
    ta_event poke = { .name = event_name(a.system, "poke"), .type = TA_EVENT_EXTERNAL,
                      .invoke_id = TA_INVOKE_ID_NONE, .data = { .self = &data, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(a.system, &(ta_actor_deliver_params){ .actor = original, .event = &poke }) == TA_OK);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(a.system, &(ta_actor_capture_params){ .actor = original, .out = &image }) == TA_OK);
    int references = data.references;

    world b;
    create(&b, 0);
    long before = allocations;
    ta_actor_id copy = 0;
    CHECK(ta_actor_reconstruct(b.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy }) == TA_OK);
    long needed = allocations - before;
    ta_system_destroy(b.system);
    CHECK(needed > 1);

    for (long failing = 0; failing < needed; failing += 1) {
        create(&b, 0);
        fail_at = failing;
        ta_result result =
            ta_actor_reconstruct(b.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy });
        fail_at = -1;
        CHECK(result == TA_ERR_NOMEM);
        CHECK(data.references == references);
        ta_actor_inspection view;
        CHECK(ta_actor_inspect(b.system, copy, &view) == TA_ERR_NOT_FOUND || result == TA_OK);
        CHECK(ta_actor_reconstruct(b.system, &(ta_actor_reconstruct_params){ .image = image, .out = &copy }) ==
              TA_OK);
        run(&b, copy);
        CHECK(in_state(&b, b.forked, copy, "poked"));
        ta_system_destroy(b.system);
        CHECK(data.references == references);
    }
    ta_session_image_destroy(image);
    ta_system_destroy(a.system);
}

int main(void) {
    snprintf(receiver_uri, sizeof receiver_uri, "#_scxml_%llu", (unsigned long long)ta_actor_id_make(0, 1));
    forked_sends[1].as.send.target = (ta_scxml_string_spec){
        .kind = TA_SCXML_STRING_LITERAL, .as.literal = { .ptr = receiver_uri, .len = (uint32_t)strlen(receiver_uri) } };
    test_a_copy_runs_on_its_own_system();
    test_an_image_outlives_its_system();
    test_the_adopter_gives_values_to_the_restoring_system();
    test_unload_and_load_across_systems();
    test_a_restore_that_runs_out_of_memory_leaves_nothing();
    CHECK(live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "image_across_systems_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("image_across_systems_test: passed\n");
    return 0;
}
