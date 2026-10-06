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
 * A session restored from an image that held an external event holds it in
 * the wake its macrostep starts with, until the wake is taken. Inspection
 * reads the event from that wake, and a session that goes before the wake
 * is taken (destroyed, unloaded or its macrostep abandoned) releases the
 * event at once, not when the system is destroyed: the wake stays queued,
 * bare, and is dropped when it is reached.
 *
 * The value here counts its references and never reuses its storage, so a
 * retain or release after the last release is recorded, with no sanitizer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "test_drive.h"

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

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

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
 * A one-address directory, enough for one named actor to unload and load.
 */

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

static one_directory directory;

/* A system with one bouncer, at `address` when it is given. */
static fixture start_at(const ta_actor_address *address) {
    fixture f = {0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &f.system) == TA_OK);
    if (address != NULL) {
        directory = (one_directory){0};
        CHECK(ta_system_set_actor_directory(f.system, (ta_actor_directory){ .self = &directory, .v = &dir_vtable }) ==
              TA_OK);
    }
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
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = id, .address = address, .out = &f.actor }) ==
          TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, f.actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("poke"), &atom) == TA_OK);
    CHECK(ta_event_name_intern(f.system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &f.poke) == TA_OK);
    return f;
}

static fixture start(void) {
    return start_at(NULL);
}

static void run_to_stable(fixture *f) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f->system, f->actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
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
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = f.actor, .name = test_event_text(f.system, &event), .event = &event }) == TA_OK);
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
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = f.actor, .name = test_event_text(f.system, &event), .event = &event }) == TA_OK);
    counted_release(&value);
    /* One step takes it, and the macrostep pauses holding it. */
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, f.actor, &(test_drive_run_params){ .budget = 1, .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
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
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = f.actor, .name = test_event_text(f.system, &event), .event = &event }) == TA_OK);
    ta_address_release(origin); /* core's references are now the last */
    run_to_stable(&f);          /* ...and releasing the event frees the address */
    capture_and_destroy(&f);
    ta_system_destroy(f.system);
}

/* Delivers `poke` with `value` and pauses the macrostep that took it, holding
 * it; then captures the actor. The host's own reference to `value` is let go,
 * so core's and the image's are all that are left. */
static ta_session_image *hold_and_capture(fixture *f, counted_value *value) {
    ta_event event = { .name = f->poke, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .data = { .self = value, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(f->system, &(ta_actor_deliver_params){ .time = ta_system_time(f->system),
                                                                 .actor = f->actor, .name = test_event_text(f->system, &event), .event = &event }) == TA_OK);
    counted_release(value);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f->system, f->actor, &(test_drive_run_params){ .budget = 1, .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f->system, &(ta_actor_capture_params){ .actor = f->actor, .out = &image }) == TA_OK);
    return image;
}

/* An actor reconstructed from `image`, which is then destroyed: the wake the
 * actor's macrostep starts with holds the last reference to the event. */
static ta_actor_id reconstruct_holding(fixture *f, ta_session_image *image, const counted_value *value) {
    CHECK(ta_actor_destroy(f->system, f->actor) == TA_OK);
    ta_actor_id restored = 0;
    CHECK(ta_actor_reconstruct(f->system, &(ta_actor_reconstruct_params){ .image = image, .out = &restored }) ==
          TA_OK);
    ta_session_image_destroy(image);
    CHECK(value->references == 1);
    return restored;
}

/* Inspection reads a restored event from the wake that holds it, before the
 * wake is taken: the event's name and its very data value. */
static void test_inspect_an_event_restored_into_a_wake(void) {
    fixture f = start();
    counted_value value = { .references = 1 };
    ta_actor_id restored = reconstruct_holding(&f, hold_and_capture(&f, &value), &value);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, restored, &view) == TA_OK);
    CHECK(view.macrostep_in_progress && view.current_event != NULL);
    if (view.current_event != NULL) {
        CHECK(view.current_event->name == f.poke);
        CHECK(view.current_event->type == TA_EVENT_EXTERNAL);
        CHECK(view.current_event->data.self == &value && view.current_event->data.v == &counted_vtable);
    }
    CHECK(value.references == 1); /* inspection borrows; it takes no reference */
    /* Taken and run to its end, the macrostep releases the event. */
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, restored, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
    CHECK(value.references == 0 && value.used_after_free == 0);
    ta_system_destroy(f.system);
    CHECK(value.references == 0 && value.used_after_free == 0);
}

/* A restored session destroyed before its wake is taken releases the event
 * at once. */
static void test_destroy_releases_a_restored_event(void) {
    fixture f = start();
    counted_value value = { .references = 1 };
    ta_actor_id restored = reconstruct_holding(&f, hold_and_capture(&f, &value), &value);
    CHECK(ta_actor_destroy(f.system, restored) == TA_OK);
    CHECK(value.references == 0 && value.used_after_free == 0);
    ta_system_destroy(f.system);
    CHECK(value.references == 0 && value.used_after_free == 0);
}

/* A restored session whose macrostep is abandoned before its wake is taken
 * releases the event at once; the bare wake is dropped when it is reached. */
static void test_abandon_releases_a_restored_event(void) {
    fixture f = start();
    counted_value value = { .references = 1 };
    ta_actor_id restored = reconstruct_holding(&f, hold_and_capture(&f, &value), &value);
    CHECK(ta_actor_abandon(f.system, restored) == TA_OK);
    CHECK(value.references == 0 && value.used_after_free == 0);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, restored, &view) == TA_OK);
    CHECK(!view.macrostep_in_progress && view.current_event == NULL);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, restored, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
    ta_system_destroy(f.system);
    CHECK(value.references == 0 && value.used_after_free == 0);
}

/* A loaded session unloaded again before its wake is taken releases the
 * event at once: only the image that unloaded it still holds it. */
static void test_unload_releases_a_restored_event(void) {
    static const ta_actor_address address = { .family = { STR("bouncer") }, .name = STR("one") };
    fixture f = start_at(&address);
    counted_value value = { .references = 1 };
    ta_session_image *image = hold_and_capture(&f, &value);
    CHECK(ta_actor_unload(f.system, &(ta_actor_unload_params){ .actor = f.actor, .image = image }) == TA_OK);
    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(f.system, &(ta_actor_load_params){ .image = image, .out = &loaded }) == TA_OK);
    ta_session_image_destroy(image);
    CHECK(value.references == 1); /* the loaded session's wake */
    image = NULL;
    CHECK(ta_actor_capture(f.system, &(ta_actor_capture_params){ .actor = loaded, .out = &image }) == TA_OK);
    CHECK(value.references == 2);
    CHECK(ta_actor_unload(f.system, &(ta_actor_unload_params){ .actor = loaded, .image = image }) == TA_OK);
    CHECK(value.references == 1); /* the image's alone */
    ta_session_image_destroy(image);
    CHECK(value.references == 0 && value.used_after_free == 0);
    ta_system_destroy(f.system);
    CHECK(value.references == 0 && value.used_after_free == 0);
}

int main(void) {
    test_capture_skips_a_released_external_event();
    test_capture_keeps_a_held_external_event();
    test_capture_skips_a_released_origin();
    test_inspect_an_event_restored_into_a_wake();
    test_destroy_releases_a_restored_event();
    test_abandon_releases_a_restored_event();
    test_unload_releases_a_restored_event();
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "session_image_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("session_image_test: passed\n");
    return 0;
}
