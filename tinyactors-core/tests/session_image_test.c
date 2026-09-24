/*
 * A session image holds only the events the actor holds (ADR 0038). An
 * actor's event slots are valid only while its flags say so: releasing an
 * event, or taking it off the queue, leaves its bytes behind (ADR 0069).
 *
 * Capture used to copy, retain and detach every slot whatever its flag. It
 * then retained a host value the actor had already released, and the image
 * released it once more when destroyed: a double free in any host that
 * reuses value storage. That is the crash the paging scenario found
 * (tinyactors-test/ruby/scenarios/virtual_actor_paging.rb). It also detached
 * a named origin whose address had already been freed, which reads freed
 * memory under AddressSanitizer (mise run asan).
 *
 * The value here counts its references and never reuses its storage, so a
 * retain or release after the last release is recorded, with no sanitizer.
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

static long live_blocks;

static void *count_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    void *block = malloc(size == 0 ? 1 : size);
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
 * A host value that counts its references and records any use after its
 * last release.
 */

typedef struct {
    int references;
    int used_after_free;
} counted_value;

static void counted_retain(void *self) {
    counted_value *value = self;
    if (value->references == 0) {
        value->used_after_free += 1;
        return;
    }
    value->references += 1;
}

static void counted_release(void *self) {
    counted_value *value = self;
    if (value->references == 0) {
        value->used_after_free += 1;
        return;
    }
    value->references -= 1;
}

static const ta_value_vtable counted_vtable = { .retain = counted_retain, .release = counted_release };

/* ------------------------------------------------------------------------
 * The chart: `poke` raises `bounce`, and `bounce` then becomes the current
 * event, which releases the external `poke` it replaces.
 */

static const ta_scxml_state_spec bouncer_chart[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_INTERNAL,
              .event = STR("poke"),
              .actions = (const ta_scxml_action_spec[]){
                  { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("bounce") } },
                  {0} } },
            { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("bounce") },
            {0},
        },
    },
    {0},
};

typedef struct {
    ta_system *system;
    ta_actor_id actor;
    ta_event_name_id poke;
} fixture;

static fixture start(void) {
    fixture f = {0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &f.system) == TA_OK);
    ta_definition_identity identity = {
        .family = { STR("bouncer") },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              f.system, &(ta_definition_compile_params){
                            .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                      .states = bouncer_chart },
                            .datamodel = ta_null_datamodel(),
                            .allocator = allocator,
                            .out = &definition,
                        }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(f.system, definition, &id) == TA_OK);
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = id, .out = &f.actor }) == TA_OK);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(f.system, &(ta_actor_resume_params){ .actor = f.actor, .budget = 100,
                                                              .status = &status }) == TA_OK);
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("poke"), &atom) == TA_OK);
    CHECK(ta_event_name_intern(f.system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &f.poke) == TA_OK);
    return f;
}

static void run_to_stable(fixture *f) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(f->system, &(ta_actor_resume_params){ .actor = f->actor, .budget = 100,
                                                               .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE);
}

static void capture_and_destroy(fixture *f) {
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f->system, &(ta_actor_capture_params){ .actor = f->actor, .out = &image }) == TA_OK);
    ta_session_image_destroy(image);
}

/* The external event's data, released once the macrostep moved past it, is
 * not retained by a later capture, nor released by the image. */
static void test_capture_skips_a_released_external_event(void) {
    fixture f = start();
    counted_value value = { .references = 1 };
    ta_event event = { .name = f.poke, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .data = { .self = &value, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(f.system, f.actor, &event) == TA_OK);
    counted_release(&value); /* the host lets go; core holds its own */
    run_to_stable(&f);
    CHECK(value.references == 0); /* `bounce` replaced it as the current event */
    capture_and_destroy(&f);
    CHECK(value.used_after_free == 0);
    ta_system_destroy(f.system);
    CHECK(value.references == 0 && value.used_after_free == 0);
}

/* An event the actor does hold is kept alive by the image, past the actor. */
static void test_capture_keeps_a_held_external_event(void) {
    fixture f = start();
    counted_value value = { .references = 1 };
    ta_event event = { .name = f.poke, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .data = { .self = &value, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(f.system, f.actor, &event) == TA_OK);
    counted_release(&value);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f.system, &(ta_actor_capture_params){ .actor = f.actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_destroy(f.system, f.actor) == TA_OK);
    CHECK(value.references > 0); /* the image still holds it */
    ta_session_image_destroy(image);
    CHECK(value.references == 0 && value.used_after_free == 0);
    ta_system_destroy(f.system);
}

/* A named origin whose address was freed with the event is not read by a
 * later capture (AddressSanitizer reports the read; mise run asan). */
static void test_capture_skips_a_released_origin(void) {
    fixture f = start();
    char *family = malloc(5);
    char *name = malloc(8);
    memcpy(family, "host", 5);
    memcpy(name, "client9", 8);
    ta_address *origin = NULL;
    CHECK(ta_address_intern(f.system, &(ta_actor_address){ .family = { .name = { .ptr = family, .len = 4 } },
                                                           .name = { .ptr = name, .len = 7 } },
                            &origin) == TA_OK);
    free(family);
    free(name);
    ta_event event = { .name = f.poke, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .origin = { .kind = TA_ORIGIN_ACTOR,
                                   .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = origin } } };
    CHECK(ta_actor_deliver(f.system, f.actor, &event) == TA_OK);
    ta_address_release(origin); /* core's references are now the last */
    run_to_stable(&f);          /* ...and releasing the event frees the address */
    capture_and_destroy(&f);
    ta_system_destroy(f.system);
}

int main(void) {
    test_capture_skips_a_released_external_event();
    test_capture_keeps_a_held_external_event();
    test_capture_skips_a_released_origin();
    CHECK(live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "session_image_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("session_image_test: passed\n");
    return 0;
}
