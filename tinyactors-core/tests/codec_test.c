/* test: standalone */
/*
 * Chart codecs (ADR 0053): the arena allocator a decode builds on, the media
 * type of literal content, which is part of a chart's revision and survives
 * export, and ta_definition_import() / ta_definition_encode() over a stub
 * codec. The round trip through a real codec is tested where one lives
 * (tinyactors-scxml).
 *
 * The test includes the amalgamation, as export_test does, so it can look at
 * a compiled definition before registration; it uses the C library, the core
 * does not.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

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

static ta_test_allocator heap;
static const ta_allocator test_allocator = TA_TEST_ALLOCATOR_STATIC(&heap);


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
                                                              .datamodel = ta_null_datamodel(),
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
 * A stub codec. Its "syntax" is one state ID: decode reads the whole input as
 * the ID of the chart's only state, pointing into the input as a codec over a
 * syntax tree would; "!" is a syntax error, and "" decodes to a chart with no
 * states, which the compiler refuses. Encode writes the only state's ID.
 */

typedef struct {
    const ta_definition_identity *seen_identity;
    uint32_t decodes;
} stub_codec;

static ta_result stub_decode(void *self, const ta_chart_decode_params *params) {
    stub_codec *codec = self;
    codec->decodes += 1u;
    codec->seen_identity = params->identity;
    if (params->input.len == 1 && params->input.ptr[0] == '!') {
        if (params->error != NULL) {
            *params->error = (ta_chart_error){ .code = TA_CHART_ERROR_SYNTAX,
                                               .at = { .offset = 0, .line = 1, .column = 1 },
                                               .message = S("a bang is not a state") };
        }
        return TA_ERR_INVALID;
    }
    ta_scxml_state_spec *states = ta_arena_alloc(params->arena, 2 * sizeof(ta_scxml_state_spec),
                                                 _Alignof(ta_scxml_state_spec));
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(ta_scxml_spec), _Alignof(ta_scxml_spec));
    if (states == NULL || spec == NULL) {
        return TA_ERR_NOMEM;
    }
    states[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC,
                                       .id = { .ptr = (const char *)params->input.ptr, .len = params->input.len } };
    states[1] = (ta_scxml_state_spec){0};
    /* The stub leaves identity to the caller, which import must cope with. */
    *spec = (ta_scxml_spec){ .name = S("stub"), .states = params->input.len == 0 ? NULL : states };
    *params->out = spec;
    return TA_OK;
}

static ta_result stub_encode(void *self, const ta_chart_encode_params *params) {
    (void)self;
    ta_str id = params->spec->states[0].id;
    uint8_t *bytes = ta_arena_alloc(params->arena, id.len + 1u, 1);
    if (bytes == NULL) {
        return TA_ERR_NOMEM;
    }
    memcpy(bytes, id.ptr, id.len);
    *params->out = (ta_bytes){ .ptr = bytes, .len = id.len };
    return TA_OK;
}

static const ta_chart_codec_vtable stub_codec_vtable = {
    .info = { .name = S("application/x-stub"), .description = S("one state ID"),
              .flags = TA_CHART_CODEC_DECODES | TA_CHART_CODEC_ENCODES },
    .decode = stub_decode,
    .encode = stub_encode,
};
static const ta_chart_codec_vtable stub_reader_vtable = {
    .info = { .name = S("application/x-stub"), .flags = TA_CHART_CODEC_DECODES },
    .decode = stub_decode,
};
static const ta_chart_codec_vtable stub_writer_vtable = {
    .info = { .name = S("application/x-stub"), .flags = TA_CHART_CODEC_ENCODES },
    .encode = stub_encode,
};

static ta_result import(ta_system *system, stub_codec *codec, ta_arena *arena, ta_bytes input,
                        const ta_definition_identity *identity, ta_definition_id *out, ta_import_error *error) {
    return ta_definition_import(system, &(ta_definition_import_params){
                                            .codec = { .self = codec, .v = &stub_codec_vtable },
                                            .input = input,
                                            .arena = arena,
                                            .identity = identity,
                                            .datamodels = (ta_datamodel[]){ ta_null_datamodel() },
                                            .datamodel_count = 1,
                                            .allocator = test_allocator,
                                            .out = out,
                                            .error = error,
                                        });
}

/*
 * A successful import registers a definition that no longer depends on the
 * input: the spec pointed into it, and the compile copied what it kept.
 */
static void test_import(ta_system *system) {
    stub_codec codec = {0};
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 128) == TA_OK);
    char input[] = "running";
    ta_definition_id id = 0;
    ta_import_error error = { .stage = TA_IMPORT_STAGE_COMPILE };
    CHECK(import(system, &codec, &arena, (ta_bytes){ .ptr = (const uint8_t *)input, .len = 7 }, NULL, &id,
                 &error) == TA_OK);
    CHECK(id != 0);
    CHECK(error.stage == TA_IMPORT_STAGE_NONE);
    memset(input, 'x', 7);
    ta_arena_destroy(&arena);
    ta_state_id state = TA_STATE_NONE;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){ .definition = id,
                                                                               .id = S("running"),
                                                                               .out = &state }) == TA_OK);
    CHECK(state == 0);
}

/* A decode error names the decode stage and its position; a compile error
 * names the compile stage and its code (ADR 0058). A chart with no states
 * has no text to point at, so it is not located. */
static void test_import_errors(ta_system *system) {
    stub_codec codec = {0};
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 128) == TA_OK);
    ta_definition_id id = 0;
    ta_import_error error = {0};

    CHECK(import(system, &codec, &arena, (ta_bytes){ .ptr = (const uint8_t *)"!", .len = 1 }, NULL, &id,
                 &error) == TA_ERR_INVALID);
    CHECK(error.stage == TA_IMPORT_STAGE_DECODE);
    CHECK(error.as.decode.code == TA_CHART_ERROR_SYNTAX);
    CHECK(error.located && error.at.line == 1 && error.at.column == 1);

    error = (ta_import_error){0};
    CHECK(import(system, &codec, &arena, (ta_bytes){ .ptr = (const uint8_t *)"", .len = 0 }, NULL, &id, &error) ==
          TA_ERR_INVALID);
    CHECK(error.stage == TA_IMPORT_STAGE_COMPILE);
    CHECK(error.as.compile.code == TA_COMPILE_ERROR_NO_STATES);
    CHECK(!error.located);

    /* A codec that only writes cannot import. */
    CHECK(ta_definition_import(system, &(ta_definition_import_params){
                                           .codec = { .self = &codec, .v = &stub_writer_vtable },
                                           .input = { .ptr = (const uint8_t *)"s", .len = 1 },
                                           .arena = &arena,
                                           .datamodels = (ta_datamodel[]){ ta_null_datamodel() },
                                           .datamodel_count = 1,
                                           .allocator = test_allocator,
                                           .out = &id,
                                       }) == TA_ERR_UNSUPPORTED);
    /* Missing required arguments. */
    CHECK(ta_definition_import(system, &(ta_definition_import_params){
                                           .codec = { .self = &codec, .v = &stub_codec_vtable },
                                           .arena = &arena,
                                           .datamodels = (ta_datamodel[]){ ta_null_datamodel() },
                                           .datamodel_count = 1,
                                           .allocator = test_allocator,
                                       }) == TA_ERR_INVALID);
    ta_arena_destroy(&arena);
}

/*
 * The identity no syntax carries goes in through import and comes back out
 * of encode, and importing the encoding with it gives the same revision.
 */
static void test_encode_round_trip(ta_system *system) {
    stub_codec codec = {0};
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 128) == TA_OK);
    const ta_definition_identity explicit_identity = {
        .family = { .name = S("stub-family") },
        .revision = { .ptr = (const uint8_t *)"r1", .len = 2 },
    };
    ta_definition_id first = 0;
    CHECK(import(system, &codec, &arena, (ta_bytes){ .ptr = (const uint8_t *)"idle", .len = 4 }, &explicit_identity,
                 &first, NULL) == TA_OK);
    CHECK(codec.seen_identity == &explicit_identity);

    ta_bytes encoded = {0};
    const ta_definition_identity *identity = NULL;
    CHECK(ta_definition_encode(system, &(ta_definition_encode_params){
                                           .definition = first,
                                           .codec = { .self = &codec, .v = &stub_codec_vtable },
                                           .arena = &arena,
                                           .out = &encoded,
                                           .identity = &identity,
                                       }) == TA_OK);
    CHECK(same_bytes(encoded.ptr, encoded.len, "idle", 4));
    CHECK(identity != NULL && same_bytes(identity->family.name.ptr, identity->family.name.len, "stub-family", 11));
    CHECK(identity != NULL && same_bytes(identity->revision.ptr, identity->revision.len, "r1", 2));

    /* A fingerprinted definition has no identity to carry. */
    ta_definition_id fingerprinted = 0;
    CHECK(import(system, &codec, &arena, (ta_bytes){ .ptr = (const uint8_t *)"busy", .len = 4 }, NULL,
                 &fingerprinted, NULL) == TA_OK);
    identity = &explicit_identity;
    CHECK(ta_definition_encode(system, &(ta_definition_encode_params){
                                           .definition = fingerprinted,
                                           .codec = { .self = &codec, .v = &stub_codec_vtable },
                                           .arena = &arena,
                                           .out = &encoded,
                                           .identity = &identity,
                                       }) == TA_OK);
    CHECK(identity == NULL);

    /* The encoding, imported again, is the same revision. */
    ta_definition_inspection before, after;
    ta_definition_id again = 0;
    CHECK(import(system, &codec, &arena, encoded, NULL, &again, NULL) == TA_OK);
    CHECK(ta_definition_inspect(system, fingerprinted, &before) == TA_OK);
    CHECK(ta_definition_inspect(system, again, &after) == TA_OK);
    CHECK(same_bytes(before.identity.revision.ptr, before.identity.revision.len, after.identity.revision.ptr,
                     after.identity.revision.len));

    /* A codec that only reads cannot encode; an unknown ID is not found. */
    CHECK(ta_definition_encode(system, &(ta_definition_encode_params){
                                           .definition = first,
                                           .codec = { .self = &codec, .v = &stub_reader_vtable },
                                           .arena = &arena,
                                           .out = &encoded,
                                       }) == TA_ERR_UNSUPPORTED);
    CHECK(ta_definition_encode(system, &(ta_definition_encode_params){
                                           .definition = 0,
                                           .codec = { .self = &codec, .v = &stub_codec_vtable },
                                           .arena = &arena,
                                           .out = &encoded,
                                       }) == TA_ERR_NOT_FOUND);
    ta_arena_destroy(&arena);
}

/*
 * SCXML durations are CSS2 time values, whose whole part is optional: a
 * decoded document hands the compiler ".5s" as written, where the chart DSL
 * used to hand it 500ms.
 */
static void test_durations(void) {
    ta_time out = 0;
    CHECK(ta_duration_parse(S(".5s"), &out) == TA_OK && out == 500);
    CHECK(ta_duration_parse(S("0.25s"), &out) == TA_OK && out == 250);
    CHECK(ta_duration_parse(S("2s"), &out) == TA_OK && out == 2000);
    CHECK(ta_duration_parse(S("15ms"), &out) == TA_OK && out == 15);
    CHECK(ta_duration_parse(S("."), &out) == TA_ERR_INVALID);
    CHECK(ta_duration_parse(S(".s"), &out) == TA_ERR_INVALID);
    CHECK(ta_duration_parse(S("5."), &out) == TA_ERR_INVALID);
    CHECK(ta_duration_parse(S("s"), &out) == TA_ERR_INVALID);
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
    CHECK(heap.stats.live_blocks > 0);
    ta_arena_destroy(&arena);
    CHECK(heap.stats.live_blocks == 0);
}

int main(void) {
    test_arena_allocator();
    test_durations();
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    test_media_type(system);
    test_import(system);
    test_import_errors(system);
    test_encode_round_trip(system);
    ta_system_destroy(system);
    if (failures != 0) {
        fprintf(stderr, "codec test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core codec test passed");
    return 0;
}
