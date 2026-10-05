/*
 * I/O origins and the processor registry (ADR 0068).
 *
 * - The registry gives the SCXML processor index 0 and registered
 *   processors stable indices from 1.
 * - A COMPUTED origin is rendered by its processor and stores nothing.
 * - A SCOPED origin renders its scope's URI until the scope closes, then
 *   nothing.
 * - A MESSAGE origin is stored once, shared by every copy of the event, and
 *   freed with the last copy: after warm-up a URI of up to 48 bytes costs no
 *   allocation.
 * - A session image carries every I/O origin as text and restores it as a
 *   MESSAGE origin; an unknown processor type restores as no origin.
 *
 * Built with AddressSanitizer too (mise run asan): reading an entry after
 * its last release fails the run.
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

static bool text_is(ta_str text, const char *expected) {
    return text.ptr != NULL && text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

/* ------------------------------------------------------------------------
 * A peer processor: computed origins "peer://node/<token>", and a location
 * "peer://session/<id>".
 */

static ta_op_status peer_send(void *self, const ta_send *send, ta_fault *fault) {
    (void)self;
    (void)send;
    (void)fault;
    return TA_OP_OK;
}

static ta_result peer_text(void *self, const ta_io_text_params *params) {
    (void)self;
    char buffer[64];
    int length = params->kind == TA_IO_TEXT_ORIGIN
                     ? snprintf(buffer, sizeof buffer, "peer://node/%llu", (unsigned long long)params->token)
                     : snprintf(buffer, sizeof buffer, "peer://session/%llu", (unsigned long long)params->session_id);
    char *text = params->arena.v->alloc(params->arena.self, (size_t)length, 1);
    if (text == NULL) {
        return TA_ERR_NOMEM;
    }
    memcpy(text, buffer, (size_t)length);
    *params->out = (ta_str){ .ptr = text, .len = (uint32_t)length };
    return TA_OK;
}

static const ta_io_vtable peer_vtable = { .send = peer_send, .text = peer_text };
static const ta_io_vtable plain_vtable = { .send = peer_send };

static ta_atom atom(ta_system *system, const char *text) {
    ta_atom out = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &out) == TA_OK);
    return out;
}

static uint32_t register_io(ta_system *system, const char *type, const ta_io_vtable *vtable) {
    uint32_t index = 0;
    CHECK(ta_system_register_io(system, &(ta_io_registration_params){ .type = atom(system, type),
                                                                       .io = { .v = vtable },
                                                                       .index = &index }) == TA_OK);
    return index;
}

/* The text of an origin, read the way a host that logs events reads it. */
static ta_result origin_text(ta_system *system, const ta_origin *origin, ta_arena *arena, ta_str *type, ta_str *uri) {
    return ta_origin_text(system, &(ta_origin_text_params){ .origin = origin,
                                                            .arena = ta_arena_allocator(arena),
                                                            .type = type,
                                                            .uri = uri });
}

static void test_registry(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    CHECK(register_io(system, "peer", &peer_vtable) == 1);
    CHECK(register_io(system, "conn", &plain_vtable) == 2);
    /* Registering a type again keeps its index. */
    CHECK(register_io(system, "peer", &peer_vtable) == 1);
    uint32_t index = 99;
    CHECK(ta_system_find_io(system, ta_system_scxml_io_type(system), &index) == TA_OK && index == 0);
    CHECK(ta_system_find_io(system, atom(system, "scxml"), &index) == TA_OK && index == 0);
    CHECK(ta_system_find_io(system, atom(system, "conn"), &index) == TA_OK && index == 2);
    CHECK(ta_system_find_io(system, atom(system, "none"), &index) == TA_ERR_NOT_FOUND);
    /* The SCXML processor cannot be replaced. */
    CHECK(ta_system_register_io(system, &(ta_io_registration_params){ .type = ta_system_scxml_io_type(system),
                                                                       .io = { .v = &peer_vtable } }) ==
          TA_ERR_INVALID);
    ta_system_destroy(system);
}

static void test_computed_and_scoped(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    uint32_t peer = register_io(system, "peer", &peer_vtable);
    uint32_t conn = register_io(system, "conn", &plain_vtable);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, allocator, 256) == TA_OK);
    ta_str type = {0};
    ta_str uri = {0};

    ta_origin computed = {0};
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = peer,
                                                                 .origin_class = TA_IO_ORIGIN_COMPUTED,
                                                                 .token = 7,
                                                                 .out = &computed }) == TA_OK);
    CHECK(computed.kind == TA_ORIGIN_IO && computed.as.io.processor == peer);
    CHECK(origin_text(system, &computed, &arena, &type, &uri) == TA_OK);
    CHECK(text_is(type, "peer") && text_is(uri, "peer://node/7"));
    /* A processor without `text` cannot make computed origins. */
    ta_origin refused = {0};
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = conn,
                                                                 .origin_class = TA_IO_ORIGIN_COMPUTED,
                                                                 .out = &refused }) == TA_ERR_UNSUPPORTED);
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = 0,
                                                                 .origin_class = TA_IO_ORIGIN_COMPUTED,
                                                                 .out = &refused }) == TA_ERR_NOT_FOUND);

    ta_io_scope scope = {0};
    CHECK(ta_io_scope_open(system, &(ta_io_scope_open_params){ .processor = conn,
                                                               .uri = STR("ws://conn/1"),
                                                               .out = &scope }) == TA_OK);
    ta_origin scoped = {0};
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = conn,
                                                                 .origin_class = TA_IO_ORIGIN_SCOPED,
                                                                 .scope = scope,
                                                                 .out = &scoped }) == TA_OK);
    CHECK(origin_text(system, &scoped, &arena, &type, &uri) == TA_OK);
    CHECK(text_is(type, "conn") && text_is(uri, "ws://conn/1"));
    /* A scope belongs to its processor. */
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = peer,
                                                                 .origin_class = TA_IO_ORIGIN_SCOPED,
                                                                 .scope = scope,
                                                                 .out = &refused }) == TA_ERR_NOT_FOUND);
    ta_io_scope_close(system, scope);
    ta_io_scope_close(system, scope); /* idempotent */
    CHECK(origin_text(system, &scoped, &arena, &type, &uri) == TA_ERR_NOT_FOUND);
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = conn,
                                                                 .origin_class = TA_IO_ORIGIN_SCOPED,
                                                                 .scope = scope,
                                                                 .out = &refused }) == TA_ERR_NOT_FOUND);
    /* The record is reused, and the stale handle still misses. */
    ta_io_scope again = {0};
    CHECK(ta_io_scope_open(system, &(ta_io_scope_open_params){ .processor = conn,
                                                               .uri = STR("ws://conn/2"),
                                                               .out = &again }) == TA_OK);
    CHECK(again.index == scope.index && again.generation != scope.generation);
    CHECK(origin_text(system, &scoped, &arena, &type, &uri) == TA_ERR_NOT_FOUND);
    ta_io_scope_close(system, again);
    ta_arena_destroy(&arena);
    ta_system_destroy(system);
}

static const char *const long_uri =
    "http://client.example/callbacks/requests/0123456789abcdef0123456789abcdef";

static void test_message_references(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    uint32_t request = register_io(system, "request", &plain_vtable);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, allocator, 256) == TA_OK);

    /* Warm up: the first entry takes a page. */
    ta_origin origin = {0};
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = request,
                                                                 .origin_class = TA_IO_ORIGIN_MESSAGE,
                                                                 .uri = STR("http://c/r/1"),
                                                                 .out = &origin }) == TA_OK);
    ta_origin_release(&origin);
    long before = heap.live;
    long calls = heap.allocations;

    /* A short URI in steady state: no allocation at all. */
    for (int round = 0; round < 1000; round += 1) {
        ta_event event = { .origin = {0} };
        CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = request,
                                                                     .origin_class = TA_IO_ORIGIN_MESSAGE,
                                                                     .uri = STR("http://c/r/2"),
                                                                     .out = &event.origin }) == TA_OK);
        ta_event copy;
        ta_event_retain(&event, &copy);
        ta_event_release(&event);
        ta_str type = {0};
        ta_str uri = {0};
        CHECK(origin_text(system, &copy.origin, &arena, &type, &uri) == TA_OK && text_is(uri, "http://c/r/2"));
        ta_event_release(&copy);
    }
    CHECK(heap.allocations == calls);
    CHECK(heap.live == before);

    /* A long URI spills once, and is freed with its last reference. */
    ta_event event = { .origin = {0} };
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){
                                        .processor = request,
                                        .origin_class = TA_IO_ORIGIN_MESSAGE,
                                        .uri = { .ptr = long_uri, .len = (uint32_t)strlen(long_uri) },
                                        .out = &event.origin }) == TA_OK);
    CHECK(heap.live == before + 1);
    ta_event copy;
    ta_event_retain(&event, &copy);
    ta_event_release(&event);
    ta_str type = {0};
    ta_str uri = {0};
    CHECK(origin_text(system, &copy.origin, &arena, &type, &uri) == TA_OK && text_is(uri, long_uri));
    ta_event_release(&copy);
    CHECK(heap.live == before);
    ta_arena_destroy(&arena);
    ta_system_destroy(system);
    CHECK(heap.live == 0);
}

/* ------------------------------------------------------------------------
 * Delivery: an actor holds its event's origin, and gives it back.
 */

static const ta_scxml_state_spec idle_chart[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
              .event = STR("stop"),
              .targets = (const ta_str[]){ STR("done"), {0} } },
            {0},
        },
    },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("done") },
    {0},
};

static ta_definition_id define_idle(ta_system *system) {
    ta_definition_identity identity = { .family = { STR("idle") }, .revision = { .ptr = (const uint8_t *)"v1", .len = 2 } };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              system, &(ta_definition_compile_params){
                          .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                    .states = idle_chart },
                          .datamodel = ta_null_datamodel(),
                          .allocator = allocator,
                          .out = &definition,
                      }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static ta_actor_id spawn_idle(ta_system *system, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    return actor;
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom segment = atom(system, text);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &segment, .count = 1 }, &name) == TA_OK);
    return name;
}

static ta_event message_event(ta_system *system, uint32_t processor, const char *name, const char *uri) {
    ta_event event = { .name = event_name(system, name), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = processor,
                                                                 .origin_class = TA_IO_ORIGIN_MESSAGE,
                                                                 .uri = { .ptr = uri, .len = (uint32_t)strlen(uri) },
                                                                 .out = &event.origin }) == TA_OK);
    return event;
}

static void test_delivery(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    uint32_t request = register_io(system, "request", &plain_vtable);
    ta_definition_id definition = define_idle(system);
    ta_actor_id actor = spawn_idle(system, definition);

    /* An unknown processor index is refused. */
    ta_event bad = message_event(system, request, "ping", "http://c/r/0");
    bad.origin.as.io.processor = 9;
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &bad }) == TA_ERR_INVALID);
    bad.origin.as.io.processor = request;
    ta_event_release(&bad);
    /* A delivered event lives in an event block (ADR 0069): one plain
     * delivery leaves a block in the system's cache, so the counts below
     * see only the origin. */
    ta_event warm = { .name = event_name(system, "ping"), .type = TA_EVENT_EXTERNAL,
                      .invoke_id = TA_INVOKE_ID_NONE };
    ta_run_status status = TA_RUN_ERROR;
    CHECK(test_drive_deliver(system,
                             &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                         .event = &warm },
                             &(test_drive_run_params){ .status = &status }) == TA_OK);
    long before = heap.live;

    /* The actor holds the origin after the host lets go; destroy gives it back. */
    ta_event event = message_event(system, request, "ping", long_uri);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &event }) == TA_OK);
    ta_event_release(&event);
    CHECK(heap.live == before + 1);
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(system, actor, &inspection) == TA_OK);
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE);
    /* The macrostep is over, and an actor nobody invoked keeps no event
     * between macrosteps (ADR 0069), so the origin is given back. */
    CHECK(heap.live == before);

    /* A finished actor refuses; the host releases, and nothing is left. */
    ta_event stop = message_event(system, request, "stop", long_uri);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &stop }) == TA_OK);
    ta_event_release(&stop);
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_DONE);
    long finished = heap.live;
    ta_event late = message_event(system, request, "ping", long_uri);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &late }) == TA_ERR_DONE);
    ta_event_release(&late);
    CHECK(heap.live == finished);
    CHECK(ta_actor_destroy(system, actor) == TA_OK);
    /* Everything but the actor's own allocation (ADR 0063), counted in `before`. */
    CHECK(heap.live == before - 1);
    ta_system_destroy(system);
    CHECK(heap.live == 0);
}

/* ------------------------------------------------------------------------
 * Images: I/O origins travel as text.
 */

static void test_image(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    uint32_t peer = register_io(system, "peer", &peer_vtable);
    ta_definition_id definition = define_idle(system);
    ta_actor_id actor = spawn_idle(system, definition);

    /* A computed origin, held as the actor's pending external event. */
    ta_event event = { .name = event_name(system, "ping"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_io_origin_make(system, &(ta_io_origin_make_params){ .processor = peer,
                                                                 .origin_class = TA_IO_ORIGIN_COMPUTED,
                                                                 .token = 42,
                                                                 .out = &event.origin }) == TA_OK);
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &event }) == TA_OK);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_destroy(system, actor) == TA_OK);

    /* Restored as a MESSAGE origin holding the rendered text. */
    ta_actor_id restored = 0;
    CHECK(ta_actor_reconstruct(system, &(ta_actor_reconstruct_params){ .image = image, .out = &restored }) == TA_OK);
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(system, restored, &inspection) == TA_OK);
    CHECK(inspection.current_event != NULL);
    if (inspection.current_event != NULL) {
        const ta_origin *origin = &inspection.current_event->origin;
        CHECK(origin->kind == TA_ORIGIN_IO && origin->as.io.origin_class == TA_IO_ORIGIN_MESSAGE &&
              origin->as.io.processor == peer);
        ta_arena arena;
        CHECK(ta_arena_init(&arena, allocator, 256) == TA_OK);
        ta_str type = {0};
        ta_str uri = {0};
        CHECK(origin_text(system, origin, &arena, &type, &uri) == TA_OK);
        CHECK(text_is(type, "peer") && text_is(uri, "peer://node/42"));
        ta_arena_destroy(&arena);
    }

    /* In a system without the processor, the event arrives with no origin. */
    ta_system *other = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &other) == TA_OK);
    (void)define_idle(other);
    ta_actor_id elsewhere = 0;
    CHECK(ta_actor_reconstruct(other, &(ta_actor_reconstruct_params){ .image = image, .out = &elsewhere }) == TA_OK);
    CHECK(ta_actor_inspect(other, elsewhere, &inspection) == TA_OK);
    CHECK(inspection.current_event != NULL && inspection.current_event->origin.kind == TA_ORIGIN_NONE);
    ta_system_destroy(other);

    ta_session_image_destroy(image);
    ta_system_destroy(system);
    CHECK(heap.live == 0);
}

int main(void) {
    test_registry();
    test_computed_and_scoped();
    test_message_references();
    test_delivery();
    test_image();
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "io_origin_test: %d failures\n", failures);
        return 1;
    }
    puts("io_origin_test: ok");
    return 0;
}
