/*
 * The SCXML codec: decoding every construct, where decoded text points,
 * <content> serialization (SCXML G.6), errors and their positions, the src
 * resolver and the lowering adapter, the round trip through
 * ta_definition_import() and ta_definition_encode(), and every limit on both
 * sides of its boundary.
 *
 * The test uses the C library; the codec does not.
 */
#include "ta_scxml.h"

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

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

static bool str_is(ta_str text, const char *literal) {
    size_t length = strlen(literal);
    return text.len == length && (length == 0 || memcmp(text.ptr, literal, length) == 0);
}

static bool bytes_is(ta_bytes bytes, const char *literal) {
    size_t length = strlen(literal);
    return bytes.len == length && (length == 0 || memcmp(bytes.ptr, literal, length) == 0);
}

static bool contains(ta_bytes bytes, const char *needle) {
    size_t length = strlen(needle);
    for (size_t at = 0; at + length <= bytes.len; at += 1u) {
        if (memcmp(bytes.ptr + at, needle, length) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Fixtures.
 */

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    return aligned_alloc(alignment, size == 0 ? alignment : size);
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &test_allocator_vtable };

/* A data model that accepts every expression and evaluates none. */
static ta_result stub_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}
static void stub_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self, (void)shared, (void)allocator;
}
static uint32_t stub_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t stub_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result stub_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void stub_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}
static const ta_datamodel_vtable stub_datamodel_vtable = {
    .name = { .ptr = "stub", .len = 4 },
    .prepare_definition = stub_prepare,
    .destroy_definition = stub_destroy_definition,
    .instance_size = stub_instance_size,
    .instance_alignment = stub_instance_alignment,
    .init_instance = stub_init_instance,
    .destroy_instance = stub_destroy_instance,
};
static const ta_datamodel stub_datamodel = { .self = NULL, .v = &stub_datamodel_vtable };

/* The embedder's resources: two files, and nothing else. */
static ta_result resolve(void *self, ta_str src, ta_arena *arena, ta_bytes *out) {
    (void)self;
    const char *text = str_is(src, "file:data.json") ? "[1, 2]" : str_is(src, "file:script.js") ? "counter = 0;" : NULL;
    if (text == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    size_t length = strlen(text);
    uint8_t *copy = ta_arena_alloc(arena, length, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    memcpy(copy, text, length);
    *out = (ta_bytes){ .ptr = copy, .len = (uint32_t)length };
    return TA_OK;
}
static const ta_chart_src_resolver_vtable resolver_vtable = { .fetch = resolve };
static const ta_chart_src_resolver resolver = { .self = NULL, .v = &resolver_vtable };

/* Lowers <ex:ping/> in urn:example to a send of `ping` to processor urn:example. */
static ta_bytes lowered_element;
static ta_result lower(void *self, const ta_chart_position *at, ta_bytes element, ta_arena *arena,
                       ta_scxml_send_spec *out) {
    (void)self, (void)at, (void)arena;
    lowered_element = element;
    if (!contains(element, "ping")) {
        return TA_ERR_NOT_FOUND;
    }
    *out = (ta_scxml_send_spec){
        .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("urn:example") },
        .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("ping") },
    };
    return TA_OK;
}
static const ta_chart_lowering_vtable lowering_vtable = { .lower = lower };
static const ta_chart_lowering lowering = { .self = NULL, .v = &lowering_vtable };

static ta_scxml_codec default_codec;

typedef struct {
    ta_result result;
    const ta_scxml_spec *spec;
    ta_chart_error error;
} decoded;

static decoded decode_with(ta_scxml_codec *codec, ta_arena *arena, const char *text) {
    decoded out = { 0 };
    ta_chart_codec adapter = ta_scxml_codec_adapter(codec);
    out.result = adapter.v->decode(adapter.self, &(ta_chart_decode_params){
                                                     .input = { .ptr = (const uint8_t *)text, .len = (uint32_t)strlen(text) },
                                                     .arena = arena,
                                                     .resolver = resolver,
                                                     .lowering = lowering,
                                                     .out = &out.spec,
                                                     .error = &out.error,
                                                 });
    return out;
}

static decoded decode(ta_arena *arena, const char *text) {
    return decode_with(&default_codec, arena, text);
}

static bool inside(const char *pointer, const char *text) {
    return pointer >= text && pointer < text + strlen(text);
}

#define NS "xmlns=\"http://www.w3.org/2005/07/scxml\""

/* ------------------------------------------------------------------------
 * Decoding every construct.
 */

static const char EVERYTHING[] =
    "<?xml version=\"1.0\"?>\n"
    "<scxml " NS " version=\"1.0\" name=\"everything\" initial=\"main\" binding=\"late\">\n"
    "  <datamodel>\n"
    "    <data id=\"a\" expr=\"1 + 1\"/>\n"
    "    <data id=\"b\">{\"k\": [1, 2]}</data>\n"
    "    <data id=\"c\" src=\"file:data.json\"/>\n"
    "    <data id=\"d\"/>\n"
    "  </datamodel>\n"
    "  <script src=\"file:script.js\"/>\n"
    "  <state id=\"main\">\n"
    "    <initial><transition target=\"work\"><log label=\"start\"/></transition></initial>\n"
    "    <onentry><raise event=\"go\"/><send event=\"tick\" target=\"#_internal\" delay=\"1s\" namelist=\"a b\">"
    "<param name=\"p\" expr=\"a &lt; b\"/></send></onentry>\n"
    "    <onentry/>\n"
    "    <onexit><cancel sendid=\"s1\"/><cancel sendidexpr=\"x\"/></onexit>\n"
    "    <transition event=\"done.* error\" cond=\"a &gt; 0\" target=\"work end\" type=\"internal\">\n"
    "      <assign location=\"a\" expr=\"2\"/>\n"
    "      <assign location=\"b\"><x " NS "/></assign>\n"
    "      <if cond=\"a == 1\"><log expr=\"1\"/><elseif cond=\"a == 2\"/><log expr=\"2\"/><else/><log expr=\"3\"/></if>\n"
    "      <foreach array=\"list\" item=\"v\" index=\"i\"><script><![CDATA[v < i]]></script></foreach>\n"
    "      <send eventexpr=\"'e'\" typeexpr=\"t\" targetexpr=\"u\" idlocation=\"loc\" delayexpr=\"'2s'\">"
    "<content expr=\"payload\"/></send>\n"
    "    </transition>\n"
    "    <state id=\"work\">\n"
    "      <invoke type=\"http://www.w3.org/TR/scxml/\" id=\"child\" autoforward=\"true\" namelist=\"a\">\n"
    "        <param name=\"q\" location=\"b\"/>\n"
    "        <content><scxml " NS " version=\"1.0\"><final id=\"f\"/></scxml></content>\n"
    "        <finalize><log label=\"finalized\"/></finalize>\n"
    "      </invoke>\n"
    "      <history id=\"h\" type=\"deep\"><transition target=\"w0\"/></history>\n"
    "      <state id=\"w0\"/>\n"
    "      <state/>\n"
    "    </state>\n"
    "  </state>\n"
    "  <parallel id=\"both\"><state id=\"left\"/><state id=\"right\"/></parallel>\n"
    "  <final id=\"end\"><donedata><content>  text &amp; more  </content></donedata></final>\n"
    "</scxml>\n";

static void test_decode_everything(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
    decoded d = decode(&arena, EVERYTHING);
    CHECK(d.result == TA_OK);
    if (d.result != TA_OK) {
        fprintf(stderr, "  decode: %.*s (%.*s) at %u:%u\n", (int)d.error.message.len, d.error.message.ptr,
                (int)d.error.construct.len, d.error.construct.ptr, d.error.at.line, d.error.at.column);
        ta_arena_destroy(&arena);
        return;
    }
    const ta_scxml_spec *spec = d.spec;
    CHECK(str_is(spec->name, "everything"));
    CHECK(spec->binding == TA_SCXML_BINDING_LATE);
    CHECK(spec->initial != NULL && str_is(spec->initial->targets[0], "main"));
    /* The name is a slice of the input: nothing had to change it. */
    CHECK(inside(spec->name.ptr, EVERYTHING));

    const ta_scxml_data_spec *data = spec->data;
    CHECK(data[0].initializer.kind == TA_SCXML_VALUE_EXPR && str_is(data[0].initializer.as.expr.as.source, "1 + 1"));
    CHECK(data[1].initializer.kind == TA_SCXML_VALUE_CONTENT &&
          bytes_is(data[1].initializer.as.content.bytes, "{\"k\": [1, 2]}") &&
          str_is(data[1].initializer.as.content.media_type, TA_SCXML_MEDIA_TYPE));
    CHECK(inside((const char *)data[1].initializer.as.content.bytes.ptr, EVERYTHING));
    CHECK(data[2].initializer.kind == TA_SCXML_VALUE_CONTENT && bytes_is(data[2].initializer.as.content.bytes, "[1, 2]"));
    CHECK(data[3].initializer.kind == TA_SCXML_VALUE_NONE && data[4].name.ptr == NULL);
    CHECK(spec->script.kind == TA_EXPRESSION_SOURCE && str_is(spec->script.as.source, "counter = 0;"));

    const ta_scxml_state_spec *main_state = &spec->states[0];
    CHECK(main_state->kind == TA_SCXML_STATE_COMPOUND && str_is(main_state->id, "main"));
    CHECK(main_state->initial != NULL && str_is(main_state->initial->targets[0], "work") &&
          main_state->initial->actions[0].kind == TA_SCXML_ACTION_LOG);
    /* Two handlers, the second empty but present. */
    CHECK(main_state->onentry[0].actions[0].kind == TA_SCXML_ACTION_RAISE);
    CHECK(main_state->onentry[1].actions != NULL && main_state->onentry[1].actions[0].kind == TA_SCXML_ACTION_NONE);
    CHECK(main_state->onentry[2].actions == NULL);
    const ta_scxml_send_spec *tick = &main_state->onentry[0].actions[1].as.send;
    CHECK(tick->event.kind == TA_SCXML_EVENT_NAME && str_is(tick->event.as.name, "tick"));
    CHECK(tick->delay.kind == TA_SCXML_STRING_LITERAL && str_is(tick->delay.as.literal, "1s"));
    /* namelist first, then <param>s; an escaped expression is decoded, and so
     * is not a slice of the input. */
    CHECK(str_is(tick->params[0].name, "a") && tick->params[0].value.kind == TA_SCXML_VALUE_LOCATION);
    CHECK(str_is(tick->params[1].name, "b"));
    CHECK(str_is(tick->params[2].name, "p") && str_is(tick->params[2].value.as.expr.as.source, "a < b"));
    CHECK(!inside(tick->params[2].value.as.expr.as.source.ptr, EVERYTHING));
    CHECK(main_state->onexit[0].actions[0].as.cancel.sendid.kind == TA_SCXML_STRING_LITERAL);
    CHECK(main_state->onexit[0].actions[1].as.cancel.sendid.kind == TA_SCXML_STRING_EXPR);

    const ta_scxml_transition_spec *t = &main_state->transitions[0];
    CHECK(t->kind == TA_SCXML_TRANSITION_INTERNAL && str_is(t->event, "done.* error"));
    CHECK(str_is(t->condition.as.source, "a > 0"));
    CHECK(str_is(t->targets[0], "work") && str_is(t->targets[1], "end") && t->targets[2].ptr == NULL);
    const ta_scxml_action_spec *body = t->actions;
    CHECK(body[0].kind == TA_SCXML_ACTION_ASSIGN && body[0].as.assign.value.kind == TA_SCXML_VALUE_EXPR);
    CHECK(body[1].kind == TA_SCXML_ACTION_ASSIGN && body[1].as.assign.value.kind == TA_SCXML_VALUE_CONTENT);
    CHECK(bytes_is(body[1].as.assign.value.as.content.bytes, "<x " NS "/>"));
    /* <if>/<elseif>/<else> is an if whose else branch is an if. */
    CHECK(body[2].kind == TA_SCXML_ACTION_IF && str_is(body[2].as.conditional.condition.as.source, "a == 1"));
    const ta_scxml_action_spec *elseif = body[2].as.conditional.else_actions;
    CHECK(elseif[0].kind == TA_SCXML_ACTION_IF && elseif[1].kind == TA_SCXML_ACTION_NONE);
    CHECK(str_is(elseif[0].as.conditional.condition.as.source, "a == 2"));
    CHECK(elseif[0].as.conditional.else_actions[0].kind == TA_SCXML_ACTION_LOG);
    CHECK(body[3].kind == TA_SCXML_ACTION_FOREACH && str_is(body[3].as.foreach.index, "i"));
    CHECK(str_is(body[3].as.foreach.body[0].as.script.script.as.source, "v < i"));
    CHECK(inside(body[3].as.foreach.body[0].as.script.script.as.source.ptr, EVERYTHING));
    const ta_scxml_send_spec *dynamic = &body[4].as.send;
    CHECK(dynamic->event.kind == TA_SCXML_EVENT_EXPR && dynamic->type.kind == TA_SCXML_STRING_EXPR &&
          dynamic->target.kind == TA_SCXML_STRING_EXPR && dynamic->id.kind == TA_SCXML_ID_LOCATION &&
          dynamic->delay.kind == TA_SCXML_STRING_EXPR && dynamic->data.kind == TA_SCXML_VALUE_EXPR);

    const ta_scxml_state_spec *work = &main_state->states[0];
    const ta_scxml_invoke_spec *invoke = &work->invokes[0];
    CHECK(invoke->kind == TA_SCXML_INVOKE && invoke->autoforward);
    CHECK(str_is(invoke->params[0].name, "a") && str_is(invoke->params[1].name, "q"));
    /* The inline machine is serialized with the namespace it had in the
     * document (SCXML G.6). */
    CHECK(invoke->data.kind == TA_SCXML_VALUE_CONTENT &&
          bytes_is(invoke->data.as.content.bytes, "<scxml " NS " version=\"1.0\"><final id=\"f\"/></scxml>"));
    CHECK(invoke->finalize[0].kind == TA_SCXML_ACTION_LOG);
    CHECK(work->states[0].kind == TA_SCXML_STATE_HISTORY_DEEP && work->states[0].transitions[0].targets != NULL);
    /* A state without an id gets one no authored id has. */
    CHECK(work->states[2].kind == TA_SCXML_STATE_ATOMIC && str_is(work->states[2].id, "_ta_anonymous_1"));

    CHECK(spec->states[1].kind == TA_SCXML_STATE_PARALLEL);
    const ta_scxml_done_data_spec *done = spec->states[2].done_data;
    CHECK(spec->states[2].kind == TA_SCXML_STATE_FINAL && done != NULL);
    /* Text-only content is its character data, references resolved. */
    CHECK(done->content.kind == TA_SCXML_VALUE_CONTENT && bytes_is(done->content.as.content.bytes, "  text & more  "));
    ta_arena_destroy(&arena);
}

/* ------------------------------------------------------------------------
 * <content> (SCXML G.6).
 */

static void test_content_namespaces(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
    decoded d = decode(&arena, "<scxml " NS " xmlns:p=\"urn:p\" name=\"c\"><final id=\"f\"><donedata>"
                               "<content> <p:a k=\"1\"/>tail &amp; <b/> </content></donedata></final></scxml>");
    CHECK(d.result == TA_OK);
    if (d.result == TA_OK) {
        ta_bytes bytes = d.spec->states[0].done_data->content.as.content.bytes;
        /* Every element carries what it needs from outside the content. */
        CHECK(bytes_is(bytes, " <p:a k=\"1\" xmlns:p=\"urn:p\"/>tail &amp; <b " NS "/> "));
        if (!bytes_is(bytes, " <p:a k=\"1\" xmlns:p=\"urn:p\"/>tail &amp; <b " NS "/> ")) {
            fprintf(stderr, "  content: [%.*s]\n", (int)bytes.len, (const char *)bytes.ptr);
        }
    }
    ta_arena_destroy(&arena);
}

/* ------------------------------------------------------------------------
 * Errors.
 */

typedef struct {
    const char *label;
    const char *document;
    ta_chart_error_code code;
    const char *construct;
    const char *attribute;
    uint32_t line;
    uint32_t column;
} error_case;

static const error_case error_cases[] = {
    { "not well-formed", "<scxml " NS ">\n  <state>\n</scxml>", TA_CHART_ERROR_SYNTAX, "", "", 3, 1 },
    { "not a chart", "<other/>", TA_CHART_ERROR_NOT_CHART, "other", "", 1, 1 },
    { "scxml in no namespace", "<scxml/>", TA_CHART_ERROR_NOT_CHART, "scxml", "", 1, 1 },
    { "an unknown element", "<scxml " NS ">\n<state><bogus/></state></scxml>", TA_CHART_ERROR_MISPLACED_CONSTRUCT,
      "bogus", "", 2, 8 },
    { "an unknown action", "<scxml " NS "><state><onentry><bogus/></onentry></state></scxml>",
      TA_CHART_ERROR_UNKNOWN_CONSTRUCT, "bogus", "", 1, 64 },
    { "an unknown attribute", "<scxml " NS "><state id=\"s\" bogus=\"1\"/></scxml>", TA_CHART_ERROR_UNKNOWN_ATTRIBUTE,
      "state", "bogus", 1, 62 },
    { "a missing attribute", "<scxml " NS "><state><onentry><raise/></onentry></state></scxml>",
      TA_CHART_ERROR_MISSING_ATTRIBUTE, "raise", "event", 1, 64 },
    { "an attribute and its expression", "<scxml " NS "><state><onentry><send event=\"a\" eventexpr=\"b\"/></onentry></state></scxml>",
      TA_CHART_ERROR_CONFLICTING_ATTRIBUTE, "send", "eventexpr", 1, 80 },
    { "a bad value", "<scxml " NS "><state><transition type=\"sideways\"/></state></scxml>", TA_CHART_ERROR_BAD_VALUE,
      "transition", "type", 1, 67 },
    { "else before elseif", "<scxml " NS "><state><onentry><if cond=\"c\"><else/><elseif cond=\"d\"/></if></onentry></state></scxml>",
      TA_CHART_ERROR_MISPLACED_CONSTRUCT, "elseif", "", 1, 84 },
    { "param with content", "<scxml " NS "><state><onentry><send event=\"e\"><param name=\"p\" expr=\"1\"/><content>x</content></send></onentry></state></scxml>",
      TA_CHART_ERROR_MISPLACED_CONSTRUCT, "send", "", 1, 64 },
    { "text between states", "<scxml " NS ">stray</scxml>", TA_CHART_ERROR_MISPLACED_CONSTRUCT, "scxml", "", 1, 48 },
    { "platform content nobody lowers", "<scxml " NS " xmlns:ex=\"urn:example\"><state><onentry><ex:other/></onentry></state></scxml>",
      TA_CHART_ERROR_FOREIGN_CONTENT, "other", "", 1, 87 },
    { "a src nobody knows", "<scxml " NS "><datamodel><data id=\"x\" src=\"file:missing\"/></datamodel></scxml>",
      TA_CHART_ERROR_SRC_UNRESOLVED, "data", "src", 1, 72 },
};

static void test_errors(void) {
    for (size_t index = 0; index < sizeof(error_cases) / sizeof(error_cases[0]); index += 1u) {
        const error_case *item = &error_cases[index];
        ta_arena arena;
        CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
        decoded d = decode(&arena, item->document);
        bool ok = d.result == TA_ERR_INVALID && d.error.code == item->code &&
                  (item->construct[0] == '\0' || str_is(d.error.construct, item->construct)) &&
                  str_is(d.error.attribute, item->attribute) && d.error.at.line == item->line &&
                  d.error.at.column == item->column && d.error.message.len != 0;
        if (!ok) {
            fprintf(stderr, "error case '%s': result %d code %d construct '%.*s' attribute '%.*s' at %u:%u (%.*s)\n",
                    item->label, d.result, d.error.code, (int)d.error.construct.len, d.error.construct.ptr,
                    (int)d.error.attribute.len, d.error.attribute.ptr, d.error.at.line, d.error.at.column,
                    (int)d.error.message.len, d.error.message.ptr);
            failures += 1;
        }
        ta_arena_destroy(&arena);
    }
}

/* Platform-specific content the lowering accepts becomes its <send>. */
static void test_lowering(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
    decoded d = decode(&arena, "<scxml " NS " xmlns:ex=\"urn:example\"><state><onentry><ex:ping to=\"#_parent\"/>"
                               "</onentry></state></scxml>");
    CHECK(d.result == TA_OK);
    if (d.result == TA_OK) {
        const ta_scxml_action_spec *action = &d.spec->states[0].onentry[0].actions[0];
        CHECK(action->kind == TA_SCXML_ACTION_SEND && str_is(action->as.send.event.as.name, "ping"));
        /* The lowering saw the element as markup it can read on its own. */
        CHECK(bytes_is(lowered_element, "<ex:ping xmlns:ex=\"urn:example\" to=\"#_parent\"/>") ||
              bytes_is(lowered_element, "<ex:ping to=\"#_parent\" xmlns:ex=\"urn:example\"/>"));
    }
    ta_arena_destroy(&arena);
}

/* ------------------------------------------------------------------------
 * Round trip through core: import, encode, import the encoding.
 */

static ta_result import_bytes(ta_system *system, ta_arena *arena, ta_bytes input, ta_definition_id *out,
                              ta_import_error *error) {
    return ta_definition_import(system, &(ta_definition_import_params){
                                            .codec = ta_scxml_codec_adapter(&default_codec),
                                            .input = input,
                                            .arena = arena,
                                            .resolver = resolver,
                                            .lowering = lowering,
                                            .datamodel = stub_datamodel,
                                            .allocator = test_allocator,
                                            .out = out,
                                            .error = error,
                                        });
}

static ta_result encode_definition(ta_system *system, ta_arena *arena, ta_definition_id id, ta_bytes *out,
                                   ta_chart_error *error) {
    return ta_definition_encode(system, &(ta_definition_encode_params){
                                            .definition = id,
                                            .codec = ta_scxml_codec_adapter(&default_codec),
                                            .arena = arena,
                                            .out = out,
                                            .error = error,
                                        });
}

static bool same_revision(ta_system *system, ta_definition_id left, ta_definition_id right) {
    ta_definition_inspection a, b;
    if (ta_definition_inspect(system, left, &a) != TA_OK || ta_definition_inspect(system, right, &b) != TA_OK) {
        return false;
    }
    return a.identity.revision.len == b.identity.revision.len &&
           memcmp(a.identity.revision.ptr, b.identity.revision.ptr, a.identity.revision.len) == 0;
}

static void check_round_trip(ta_system *system, const char *label, const char *document) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
    ta_definition_id first = 0;
    ta_import_error import_error = { 0 };
    ta_result result = import_bytes(system, &arena, (ta_bytes){ .ptr = (const uint8_t *)document, .len = (uint32_t)strlen(document) },
                                    &first, &import_error);
    if (result != TA_OK) {
        fprintf(stderr, "round trip '%s': import %d stage %d (%.*s at %u:%u)\n", label, result, import_error.stage,
                (int)import_error.as.decode.message.len, import_error.as.decode.message.ptr, import_error.at.line,
                import_error.at.column);
        failures += 1;
        ta_arena_destroy(&arena);
        return;
    }
    ta_bytes encoded = { 0 };
    ta_chart_error encode_error = { 0 };
    result = encode_definition(system, &arena, first, &encoded, &encode_error);
    ta_definition_id second = 0;
    if (result == TA_OK) {
        result = import_bytes(system, &arena, encoded, &second, &import_error);
    }
    ta_bytes again = { 0 };
    if (result == TA_OK) {
        result = encode_definition(system, &arena, second, &again, &encode_error);
    }
    bool ok = result == TA_OK && same_revision(system, first, second) && again.len == encoded.len &&
              memcmp(again.ptr, encoded.ptr, encoded.len) == 0;
    if (!ok) {
        fprintf(stderr, "round trip '%s': result %d (%.*s), encoded:\n%.*s\nagain:\n%.*s\n", label, result,
                (int)encode_error.message.len, encode_error.message.ptr, (int)encoded.len, (const char *)encoded.ptr,
                (int)again.len, (const char *)again.ptr);
        failures += 1;
    }
    ta_arena_destroy(&arena);
}

static void test_round_trip(ta_system *system) {
    check_round_trip(system, "everything", EVERYTHING);
    check_round_trip(system, "content",
                     "<scxml " NS " xmlns:p=\"urn:p\" name=\"c\"><final id=\"f\"><donedata>"
                     "<content> <p:a k=\"1\"/>tail &amp; <b/> <c xmlns=\"\"/></content></donedata></final></scxml>");
    /* Text that looks like markup stays text. */
    check_round_trip(system, "escaped markup",
                     "<scxml " NS " name=\"e\"><datamodel><data id=\"x\">&lt;y a='1'/&gt;</data><data id=\"z\">a\rb</data>"
                     "</datamodel><final id=\"f\"/></scxml>");
    check_round_trip(system, "lowered",
                     "<scxml " NS " xmlns:ex=\"urn:example\" name=\"l\"><state><onentry><ex:ping/></onentry></state></scxml>");
}

/* A literal content in another media type has no form in this syntax. */
static void test_unrepresentable(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
    ta_scxml_state_spec states[2] = {
        { .kind = TA_SCXML_STATE_FINAL, .id = S("f"),
          .done_data = &(ta_scxml_done_data_spec){
              .content = { .kind = TA_SCXML_VALUE_CONTENT,
                           .as.content = { .media_type = S("application/json"), .bytes = { (const uint8_t *)"{}", 2 } } } } },
        { 0 },
    };
    ta_scxml_spec spec = { .name = S("json"), .states = states };
    ta_chart_codec adapter = ta_scxml_codec_adapter(&default_codec);
    ta_bytes out = { 0 };
    ta_chart_error error = { 0 };
    CHECK(adapter.v->encode(adapter.self, &(ta_chart_encode_params){ .spec = &spec, .arena = &arena, .out = &out,
                                                                     .error = &error }) == TA_ERR_INVALID);
    CHECK(error.code == TA_CHART_ERROR_UNREPRESENTABLE && str_is(error.construct, "donedata"));
    ta_arena_destroy(&arena);
}

/* ------------------------------------------------------------------------
 * Limits: one bound at a time. A document exactly at the bound is accepted,
 * one step beyond it is TA_CHART_ERROR_LIMIT naming the bound.
 */

static ta_result decode_limited(const ta_scxml_codec_params *params, const char *document, ta_chart_error *error) {
    ta_scxml_codec codec;
    if (ta_scxml_codec_init(&codec, params) != TA_OK) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_arena arena;
    if (ta_arena_init(&arena, test_allocator, 4096) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    decoded d = decode_with(&codec, &arena, document);
    *error = d.error;
    ta_arena_destroy(&arena);
    return d.result;
}

static void check_boundary(const char *label, const char *limit, ta_scxml_codec_params at, ta_scxml_codec_params beyond,
                           const char *document) {
    ta_chart_error error = { 0 };
    ta_result accepted = decode_limited(&at, document, &error);
    if (accepted != TA_OK) {
        fprintf(stderr, "limit '%s': the document at the limit was refused: %.*s (%.*s)\n", label,
                (int)error.message.len, error.message.ptr, (int)error.construct.len, error.construct.ptr);
        failures += 1;
    }
    error = (ta_chart_error){ 0 };
    ta_result refused = decode_limited(&beyond, document, &error);
    if (refused != TA_ERR_INVALID || error.code != TA_CHART_ERROR_LIMIT || !str_is(error.construct, limit) ||
        error.at.line == 0) {
        fprintf(stderr, "limit '%s': beyond the limit: result %d code %d construct '%.*s' at %u:%u\n", label, refused,
                error.code, (int)error.construct.len, error.construct.ptr, error.at.line, error.at.column);
        failures += 1;
    }
}

/* The smallest value of a bound that accepts `document`, found by bisection;
 * the boundary is then checked on both sides like the others. */
typedef void (*set_limit)(ta_scxml_codec_params *params, uint64_t value);

static uint64_t smallest_accepted(set_limit set, const char *document, uint64_t high) {
    uint64_t low = 1;
    ta_chart_error error;
    ta_scxml_codec_params params = { 0 };
    set(&params, low);
    if (decode_limited(&params, document, &error) == TA_OK) {
        return low;
    }
    while (high - low > 1u) {
        uint64_t middle = low + (high - low) / 2u;
        params = (ta_scxml_codec_params){ 0 };
        set(&params, middle);
        if (decode_limited(&params, document, &error) == TA_OK) {
            high = middle;
        } else {
            low = middle;
        }
    }
    return high;
}

static void set_names(ta_scxml_codec_params *params, uint64_t value) {
    params->max_names = (uint32_t)value;
}

static void set_parse_bytes(ta_scxml_codec_params *params, uint64_t value) {
    params->max_parse_bytes = value;
}

static void test_limits(void) {
    static const char small[] = "<scxml " NS " version=\"1.0\"><final id=\"f\"/></scxml>";
    uint64_t size = sizeof(small) - 1u;
    check_boundary("document bytes", "max_document_bytes", (ta_scxml_codec_params){ .max_document_bytes = size },
                   (ta_scxml_codec_params){ .max_document_bytes = size - 1u }, small);

    /* scxml > state > state > state > final: five elements deep. */
    static const char deep[] = "<scxml " NS "><state><state><state><final id=\"f\"/></state></state></state></scxml>";
    check_boundary("depth", "max_depth", (ta_scxml_codec_params){ .max_depth = 5 }, (ta_scxml_codec_params){ .max_depth = 4 },
                   deep);

    /* <scxml> carries four: the namespace declaration counts. */
    static const char attributes[] = "<scxml " NS " version=\"1.0\" name=\"n\" initial=\"f\"><final id=\"f\"/></scxml>";
    check_boundary("attributes", "max_attributes", (ta_scxml_codec_params){ .max_attributes = 4 },
                   (ta_scxml_codec_params){ .max_attributes = 3 }, attributes);

    /* The longest text value is the ten-byte script. */
    static const char text[] = "<scxml " NS "><script>0123456789</script><final id=\"f\"/></scxml>";
    check_boundary("text bytes", "max_text_bytes", (ta_scxml_codec_params){ .max_text_bytes = 10 },
                   (ta_scxml_codec_params){ .max_text_bytes = 9 }, text);
    /* ... and it bounds attribute values and literal content too. */
    static const char attribute_text[] = "<scxml " NS " name=\"0123456789\"><final id=\"f\"/></scxml>";
    check_boundary("attribute text bytes", "max_text_bytes", (ta_scxml_codec_params){ .max_text_bytes = 10 },
                   (ta_scxml_codec_params){ .max_text_bytes = 9 }, attribute_text);
    static const char content_text[] =
        "<scxml " NS "><final id=\"f\"><donedata><content><a/><b/></content></donedata></final></scxml>";
    /* <a xmlns="..."/> and <b xmlns="..."/>: 2 * (4 + 40) bytes. */
    uint32_t content_bytes = (uint32_t)(2u * strlen("<a " NS "/>"));
    check_boundary("content bytes", "max_text_bytes", (ta_scxml_codec_params){ .max_text_bytes = content_bytes },
                   (ta_scxml_codec_params){ .max_text_bytes = content_bytes - 1u }, content_text);

    /* One reference to a five-byte entity. */
    static const char entity[] = "<!DOCTYPE scxml [<!ENTITY e \"12345\">]>"
                                 "<scxml " NS " name=\"&e;\"><final id=\"f\"/></scxml>";
    check_boundary("entity expansion", "max_entity_expansion", (ta_scxml_codec_params){ .max_entity_expansion = 5 },
                   (ta_scxml_codec_params){ .max_entity_expansion = 4 }, entity);

    /* The longest name is "version", seven bytes. */
    check_boundary("name length", "max_name_length", (ta_scxml_codec_params){ .max_name_length = 7 },
                   (ta_scxml_codec_params){ .max_name_length = 6 }, small);

    /* The distinct names and namespaces txml interns, found by bisection. */
    uint64_t names = smallest_accepted(set_names, small, 64);
    CHECK(names >= 4u && names <= 8u);
    check_boundary("names", "max_names", (ta_scxml_codec_params){ .max_names = (uint32_t)names },
                   (ta_scxml_codec_params){ .max_names = (uint32_t)names - 1u }, small);

    /* The parsed document's storage, found by bisection. */
    uint64_t parse_bytes = smallest_accepted(set_parse_bytes, small, 1u << 24);
    CHECK(parse_bytes > size);
    check_boundary("parse bytes", "max_parse_bytes", (ta_scxml_codec_params){ .max_parse_bytes = parse_bytes },
                   (ta_scxml_codec_params){ .max_parse_bytes = parse_bytes - 1u }, small);
}

/* Zero is the documented default, field by field, and null params are all
 * zero. Bounds txml cannot represent are refused. */
static void test_default_limits(void) {
    ta_scxml_codec codec;
    CHECK(ta_scxml_codec_init(&codec, &(ta_scxml_codec_params){ 0 }) == TA_OK);
    CHECK(codec.limits.max_document_bytes == TA_SCXML_DEFAULT_MAX_DOCUMENT_BYTES);
    CHECK(codec.limits.max_parse_bytes == TA_SCXML_DEFAULT_MAX_PARSE_BYTES);
    CHECK(codec.limits.max_depth == TA_SCXML_DEFAULT_MAX_DEPTH);
    CHECK(codec.limits.max_attributes == TA_SCXML_DEFAULT_MAX_ATTRIBUTES);
    CHECK(codec.limits.max_text_bytes == TA_SCXML_DEFAULT_MAX_TEXT_BYTES);
    CHECK(codec.limits.max_entity_expansion == TA_SCXML_DEFAULT_MAX_ENTITY_EXPANSION);
    CHECK(codec.limits.max_name_length == TA_SCXML_DEFAULT_MAX_NAME_LENGTH);
    CHECK(codec.limits.max_names == TA_SCXML_DEFAULT_MAX_NAMES);
    ta_scxml_codec from_null;
    CHECK(ta_scxml_codec_init(&from_null, NULL) == TA_OK);
    CHECK(memcmp(&from_null.limits, &codec.limits, sizeof(codec.limits)) == 0);
    /* One field given, the others still default. */
    ta_scxml_codec partial;
    CHECK(ta_scxml_codec_init(&partial, &(ta_scxml_codec_params){ .max_depth = 9 }) == TA_OK);
    CHECK(partial.limits.max_depth == 9 && partial.limits.max_names == TA_SCXML_DEFAULT_MAX_NAMES);
    CHECK(ta_scxml_codec_init(&partial, &(ta_scxml_codec_params){ .max_depth = 70000 }) == TA_ERR_INVALID);
    CHECK(ta_scxml_codec_init(&partial, &(ta_scxml_codec_params){ .max_parse_bytes = (uint64_t)UINT32_MAX + 1u }) ==
          TA_ERR_INVALID);
    /* The defaults in force: a document 65 elements deep is refused. */
    char deep[4096];
    size_t at = (size_t)snprintf(deep, sizeof(deep), "<scxml " NS ">");
    for (int level = 0; level < 63; level += 1) {
        at += (size_t)snprintf(deep + at, sizeof(deep) - at, "<state>");
    }
    at += (size_t)snprintf(deep + at, sizeof(deep) - at, "<final id=\"f\"/>");
    for (int level = 0; level < 63; level += 1) {
        at += (size_t)snprintf(deep + at, sizeof(deep) - at, "</state>");
    }
    snprintf(deep + at, sizeof(deep) - at, "</scxml>");
    ta_chart_error error = { 0 };
    CHECK(decode_limited(NULL, deep, &error) == TA_ERR_INVALID && str_is(error.construct, "max_depth"));
}

int main(void) {
    if (ta_scxml_codec_init(&default_codec, NULL) != TA_OK) {
        fprintf(stderr, "cannot initialize the codec\n");
        return 1;
    }
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    test_decode_everything();
    test_content_namespaces();
    test_errors();
    test_lowering();
    test_round_trip(system);
    test_unrepresentable();
    test_limits();
    test_default_limits();
    ta_system_destroy(system);
    if (failures != 0) {
        fprintf(stderr, "ta_scxml test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("ta_scxml test passed");
    return 0;
}
