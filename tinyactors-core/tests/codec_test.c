/*
 * Chart codecs (ADR 0053): the arena allocator a decode builds on, and the
 * media type of literal content, which is part of a chart's revision and
 * survives export.
 *
 * The test includes the amalgamation, as export_test does, so it can look at
 * a compiled definition before registration; it uses the C library, the core
 * does not.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------------
 * Host fixtures: a counting libc allocator.
 */

static size_t live_blocks = 0;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    void *block = aligned_alloc(alignment, size == 0 ? alignment : size);
    live_blocks += block != NULL ? 1u : 0u;
    return block;
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) {
        live_blocks -= 1u;
    }
    free(params->ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &test_allocator_vtable };

/* A data model that evaluates nothing: compiling needs one. */
static ta_result null_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}
static void null_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self, (void)shared, (void)allocator;
}
static uint32_t null_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t null_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result null_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}

static const ta_datamodel_vtable null_datamodel_vtable = {
    .name = { .ptr = "null", .len = 4 },
    .prepare_definition = null_prepare,
    .destroy_definition = null_destroy_definition,
    .instance_size = null_instance_size,
    .instance_alignment = null_instance_alignment,
    .init_instance = null_init_instance,
    .destroy_instance = null_destroy_instance,
};
static const ta_datamodel null_datamodel = { .self = NULL, .v = &null_datamodel_vtable };

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

static bool same_bytes(const void *left, uint32_t left_len, const void *right, uint32_t right_len) {
    return left_len == right_len && (left_len == 0 || memcmp(left, right, left_len) == 0);
}

/* A chart whose only state sends `payload`, labelled `media_type`, to a host
 * I/O processor on entry. */
typedef struct {
    ta_scxml_action_spec send[2];
    ta_scxml_handler_spec onentry[2];
    ta_scxml_state_spec states[2];
    ta_scxml_spec spec;
} content_chart;

static const ta_scxml_spec *content_chart_init(content_chart *chart, ta_str media_type, ta_str payload) {
    *chart = (content_chart){0};
    chart->send[0] = (ta_scxml_action_spec){
        .kind = TA_SCXML_ACTION_SEND,
        .as.send = {
            .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("urn:io") },
            .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("out") },
            .data = { .kind = TA_SCXML_VALUE_CONTENT,
                      .as.content = { .media_type = media_type,
                                      .bytes = { .ptr = (const uint8_t *)payload.ptr, .len = payload.len } } },
        },
    };
    chart->onentry[0] = (ta_scxml_handler_spec){ .actions = chart->send };
    chart->states[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = S("s"),
                                              .onentry = chart->onentry };
    chart->spec = (ta_scxml_spec){ .name = S("content"), .states = chart->states };
    return &chart->spec;
}

static ta_definition *compile(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                              .spec = spec,
                                                              .datamodel = null_datamodel,
                                                              .allocator = test_allocator,
                                                              .out = &definition,
                                                          });
    CHECK(result == TA_OK);
    return result == TA_OK ? definition : NULL;
}

/*
 * Two charts that differ only in a literal content's media type have
 * different revisions (ADR 0053 section 2), and export gives the media type
 * back, so a decoded chart recompiles to the revision it had.
 */
static void test_media_type(ta_system *system) {
    content_chart plain, labelled, other;
    ta_definition *a = compile(system, content_chart_init(&plain, (ta_str){0}, S("x")));
    ta_definition *b = compile(system, content_chart_init(&labelled, S("text/plain"), S("x")));
    ta_definition *c = compile(system, content_chart_init(&other, S("application/json"), S("x")));
    if (a == NULL || b == NULL || c == NULL) {
        return;
    }
    CHECK(!same_bytes(a->identity.revision.ptr, a->identity.revision.len, b->identity.revision.ptr,
                      b->identity.revision.len));
    CHECK(!same_bytes(b->identity.revision.ptr, b->identity.revision.len, c->identity.revision.ptr,
                      c->identity.revision.len));
    ta_definition_destroy(a);
    ta_definition_destroy(c);

    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, b, &id) == TA_OK);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 256) == TA_OK);
    const ta_scxml_spec *exported = NULL;
    CHECK(ta_definition_export(system, &(ta_definition_export_params){ .definition = id, .arena = &arena,
                                                                        .out = &exported }) == TA_OK);
    if (exported != NULL) {
        const ta_scxml_value_spec *data = &exported->states[0].onentry[0].actions[0].as.send.data;
        CHECK(data->kind == TA_SCXML_VALUE_CONTENT);
        CHECK(same_bytes(data->as.content.media_type.ptr, data->as.content.media_type.len, "text/plain", 10));
        CHECK(same_bytes(data->as.content.bytes.ptr, data->as.content.bytes.len, "x", 1));
    }
    ta_arena_destroy(&arena);
}

/* ------------------------------------------------------------------------
 * ta_arena_allocator: an arena behind the ta_allocator interface.
 */

static void test_arena_allocator(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 64) == TA_OK);
    ta_allocator allocator = ta_arena_allocator(&arena);
    CHECK(allocator.v != NULL && allocator.v->alloc != NULL && allocator.v->free != NULL);

    /* Allocations are aligned, distinct, and outlive free(). */
    uint64_t *first = allocator.v->alloc(allocator.self, sizeof(uint64_t), _Alignof(uint64_t));
    char *second = allocator.v->alloc(allocator.self, 200, 1);
    CHECK(first != NULL && second != NULL);
    CHECK(((uintptr_t)first % _Alignof(uint64_t)) == 0);
    *first = 42;
    memset(second, 'x', 200);
    allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = first, .size = sizeof(uint64_t),
                                                                     .alignment = _Alignof(uint64_t) });
    CHECK(*first == 42);
    CHECK(second[199] == 'x');

    /* Everything goes back to the backing allocator with the arena. */
    CHECK(live_blocks > 0);
    ta_arena_destroy(&arena);
    CHECK(live_blocks == 0);
}

int main(void) {
    test_arena_allocator();
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    test_media_type(system);
    ta_system_destroy(system);
    if (failures != 0) {
        fprintf(stderr, "codec test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core codec test passed");
    return 0;
}
