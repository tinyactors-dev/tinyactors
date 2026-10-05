#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "json_datamodel.h"
#include "byte_buffers.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "json datamodel test failed: %s\n", message);
        exit(1);
    }
}

int main(void) {
    ta_expression expressions[] = {
        {.kind = TA_EXPRESSION_SOURCE, .as.source = {.ptr = ".ready", .len = 6}},
        {.kind = TA_EXPRESSION_SOURCE, .as.source = {.ptr = ".count", .len = 6}},
        {.kind = TA_EXPRESSION_SOURCE, .as.source = {.ptr = ".count + 1", .len = 10}},
        {.kind = TA_EXPRESSION_SOURCE,
         .as.source = {.ptr = ".count = .count + 1", .len = 19}},
    };
    tat_json_datamodel_context context = {0};
    require(tat_json_datamodel_context_init(&context, tat_libc_allocator()) == TA_OK,
            "context initialization");
    ta_datamodel model = tat_json_datamodel(&context);
    void *shared = NULL;
    require(model.v->prepare_definition(
                model.self,
                &(ta_datamodel_prepare_definition_params){
                    .expressions = expressions,
                    .expression_count = (uint32_t)ARRAY_COUNT(expressions),
                    .allocator = tat_libc_allocator(),
                    .shared = &shared,
                }) == TA_OK,
            "definition preparation");

    void *instance = calloc(1, model.v->instance_size(model.self, shared));
    require(instance != NULL, "instance allocation");
    ta_value binding = {0};
    require(tat_json_value_parse(&context, "{\"ready\":true,\"count\":1}", &binding) == TA_OK,
            "binding parse");
    require(model.v->init_instance(
                model.self,
                &(ta_datamodel_init_instance_params){
                    .shared = shared,
                    .instance = instance,
                    .binding = &binding,
                }) == TA_OK,
            "instance initialization");

    bool condition = false;
    ta_fault fault = {0};
    require(model.v->eval_condition(
                model.self,
                &(ta_datamodel_eval_condition_params){
                    .shared = shared,
                    .instance = instance,
                    .expr = 0,
                    .out = &condition,
                    .fault = &fault,
                }) == TA_OP_OK && condition,
            "condition evaluation");

    ta_value evaluated = {0};
    require(model.v->eval_value(
                model.self,
                &(ta_datamodel_eval_value_params){
                    .shared = shared,
                    .instance = instance,
                    .expr = 2,
                    .out = &evaluated,
                    .fault = &fault,
                }) == TA_OP_OK,
            "value evaluation");
    char text[128];
    size_t needed = 0;
    require(tat_json_value_format(&evaluated, text, sizeof(text), &needed) == TA_OK &&
                strcmp(text, "2") == 0,
            "evaluated value");
    ta_value_release(&evaluated);

    ta_value five = {0};
    require(tat_json_value_parse(&context, "5", &five) == TA_OK, "assignment value parse");
    require(model.v->assign(
                model.self,
                &(ta_datamodel_assign_params){
                    .shared = shared,
                    .instance = instance,
                    .location = 1,
                    .value = &five,
                    .fault = &fault,
                }) == TA_OP_OK,
            "location assignment");
    ta_value_release(&five);

    require(model.v->execute_script(
                model.self,
                &(ta_datamodel_execute_script_params){
                    .shared = shared,
                    .instance = instance,
                    .script = 3,
                    .fault = &fault,
                }) == TA_OP_OK,
            "script execution");

    ta_value inspected = {0};
    require(model.v->inspect(
                model.self,
                &(ta_datamodel_inspect_params){
                    .shared = shared,
                    .instance = instance,
                    .out = &inspected,
                }) == TA_OK,
            "inspection");
    require(tat_json_value_format(&inspected, text, sizeof(text), &needed) == TA_OK &&
                strcmp(text, "{\"ready\":true,\"count\":6}") == 0,
            "mutated root");
    ta_value_release(&inspected);

    ta_value first = {0};
    ta_value second = {0};
    require(tat_json_value_parse(&context, "1", &first) == TA_OK &&
                tat_json_value_parse(&context, "2", &second) == TA_OK,
            "parameter values");
    ta_named_value named[] = {
        {.name = {.ptr = "x", .len = 1}, .value = first},
        {.name = {.ptr = "x", .len = 1}, .value = second},
    };
    ta_value parameters = {0};
    require(model.v->make_params(
                model.self,
                &(ta_datamodel_make_params_params){
                    .shared = shared,
                    .instance = instance,
                    .values = named,
                    .count = ARRAY_COUNT(named),
                    .out = &parameters,
                    .fault = &fault,
                }) == TA_OP_OK,
            "parameter aggregation");
    require(tat_json_value_format(&parameters, text, sizeof(text), &needed) == TA_OK &&
                strcmp(text,
                       "{\"x\":2}") == 0,
            "a later duplicate parameter replaces an earlier one");
    ta_value_release(&parameters);
    ta_value_release(&first);
    ta_value_release(&second);

    /* A value's trace text (core ADR 0098): its JSON, cut at the limit on a
     * character boundary, with `cut` set, and kept until the next call. */
    {
        static const char whole[] = "{\"a\":\"\xc3\xa9\xe2\x82\xac" "x\",\"b\":[1,2]}"; /* e-acute, euro sign */
        ta_value traced = {0};
        require(tat_json_value_parse(&context, whole, &traced) == TA_OK, "trace value");
        ta_str out = {0};
        bool cut = true;
        ta_datamodel_value_trace_text_params call = { .shared = shared, .instance = instance, .value = &traced,
                                                      .limit = 1000, .out = &out, .cut = &cut, .fault = &fault };
        require(model.v->value_trace_text(model.self, &call) == TA_OP_OK && !cut && out.len == sizeof whole - 1u &&
                    memcmp(out.ptr, whole, out.len) == 0,
                "value_trace_text renders the JSON");
        call.limit = 10; /* inside the euro sign, which starts at byte 8 */
        require(model.v->value_trace_text(model.self, &call) == TA_OP_OK && cut && out.len == 8u &&
                    memcmp(out.ptr, whole, 8u) == 0,
                "value_trace_text cuts on a character boundary");
        call.limit = 11; /* exactly after the euro sign */
        require(model.v->value_trace_text(model.self, &call) == TA_OP_OK && cut && out.len == 11u,
                "value_trace_text keeps a whole character");
        ta_value foreign = {0};
        call.value = &foreign;
        require(model.v->value_trace_text(model.self, &call) == TA_OP_ERROR && fault.message.len != 0,
                "value_trace_text refuses a value of another data model");
        ta_value_release(&traced);
    }

    model.v->destroy_instance(model.self, shared, instance);
    model.v->destroy_definition(model.self, shared, tat_libc_allocator());
    /* A value as bytes (core ADR 0090): its JSON text, durable or not,
     * written in pieces and read back from pieces of any size; bytes that
     * are not one JSON value are refused. */
    for (int durable = 0; durable < 2; durable += 1) {
        test_sink sink = {0};
        ta_fault fault = {0};
        require(model.v->value_write(model.self, &(ta_datamodel_value_write_params){
                                                     .shared = shared, .value = &binding,
                                                     .sink = { .self = &sink, .v = &TEST_SINK },
                                                     .durable = durable != 0, .fault = &fault }) == TA_OP_OK,
                "value_write");
        require(sink.len > 2 && sink.bytes[0] == '{' && sink.bytes[sink.len - 1u] == '}',
                "value_write writes the JSON text");
        static const uint32_t pieces[] = { 1, 7, 0 };
        for (size_t index = 0; index < ARRAY_COUNT(pieces); index += 1) {
            test_source source = { .bytes = sink.bytes, .len = sink.len, .piece = pieces[index] };
            ta_value back = {0};
            require(model.v->value_read(model.self, &(ta_datamodel_value_read_params){
                                                        .shared = shared,
                                                        .source = { .self = &source, .v = &TEST_SOURCE },
                                                        .durable = durable != 0, .out = &back,
                                                        .fault = &fault }) == TA_OP_OK,
                    "value_read");
            test_sink again = {0};
            require(model.v->value_write(model.self, &(ta_datamodel_value_write_params){
                                                         .shared = shared, .value = &back,
                                                         .sink = { .self = &again, .v = &TEST_SINK },
                                                         .durable = durable != 0, .fault = &fault }) == TA_OP_OK &&
                        again.len == sink.len && memcmp(again.bytes, sink.bytes, sink.len) == 0,
                    "the read value equals the written one");
            free(again.bytes);
            ta_value_release(&back);
        }
        free(sink.bytes);
    }
    const uint8_t *not_json[] = { (const uint8_t *)"{", (const uint8_t *)"1 2", (const uint8_t *)"" };
    for (size_t index = 0; index < ARRAY_COUNT(not_json); index += 1) {
        test_source source = { .bytes = not_json[index], .len = strlen((const char *)not_json[index]), .piece = 1 };
        ta_value back = {0};
        ta_fault fault = {0};
        require(model.v->value_read(model.self, &(ta_datamodel_value_read_params){
                                                    .shared = shared, .source = { .self = &source, .v = &TEST_SOURCE },
                                                    .durable = true, .out = &back, .fault = &fault }) == TA_OP_ERROR,
                "value_read refuses bytes that are not one JSON value");
    }
    /* The writer streams, and writes jv_dump_string(value, 0)'s bytes: each
     * value is wrapped in an array, whose value_text is jq's own text. */
    static const char *const texts[] = {
        "null", "true", "false", "0", "-1.50", "1e3", "12345678901234567890", "3.141592653589793",
        "\"\"", "\"a\\\"b\\\\c/d\\b\\f\\n\\r\\t\\u0001\\u001f\\u007fé€\\ud83d\\ude00\"",
        "[]", "{}", "[[],{},[[[1]]]]", "{\"b\":1,\"a\":[true,{\"c\\n\":null}],\"\":\"x\"}",
    };
    for (size_t index = 0; index < ARRAY_COUNT(texts); index += 1) {
        char wrapped_text[512];
        snprintf(wrapped_text, sizeof wrapped_text, "[%s]", texts[index]);
        ta_value value = {0};
        require(tat_json_value_parse(&context, wrapped_text, &value) == TA_OK, "parse a writer case");
        ta_str jq_text = {0};
        require(model.v->value_text(model.self, &(ta_datamodel_value_text_params){
                                                    .value = &value, .out = &jq_text, .fault = &fault }) == TA_OP_OK,
                "jq's text of a writer case");
        test_sink sink = {0};
        require(model.v->value_write(model.self, &(ta_datamodel_value_write_params){
                                                     .value = &value, .sink = { .self = &sink, .v = &TEST_SINK },
                                                     .durable = true, .fault = &fault }) == TA_OP_OK,
                "value_write of a writer case");
        require(sink.len == jq_text.len && memcmp(sink.bytes, jq_text.ptr, sink.len) == 0,
                "value_write writes jv_dump_string's bytes");
        free(sink.bytes);
        ta_value_release(&value);
    }
    /* A large value crosses in pieces of at most 4 KiB, and deep nesting is
     * walked on an explicit stack: 9,999 levels, the most jq's parser takes. */
    {
        size_t length = 5u * 1024u * 1024u;
        char *text = malloc(length + 3u);
        require(text != NULL, "large text");
        text[0] = '"';
        for (size_t at = 1; at <= length; at += 1) {
            text[at] = (char)('a' + (at % 26u));
        }
        text[length + 1u] = '"';
        text[length + 2u] = '\0';
        ta_value large = {0};
        require(tat_json_value_parse(&context, text, &large) == TA_OK, "parse a large string");
        free(text);
        test_counting_sink counted = {0};
        require(model.v->value_write(model.self, &(ta_datamodel_value_write_params){
                                                     .value = &large,
                                                     .sink = { .self = &counted, .v = &TEST_COUNTING_SINK },
                                                     .durable = true, .fault = &fault }) == TA_OP_OK,
                "value_write of a large string");
        require(counted.total == length + 2u && counted.largest <= 4096u, "a large value is written in pieces");
        ta_value_release(&large);
        size_t depth = 9999u;
        char *deep = malloc(2u * depth + 1u);
        require(deep != NULL, "deep text");
        memset(deep, '[', depth);
        memset(deep + depth, ']', depth);
        deep[2u * depth] = '\0';
        ta_value nested = {0};
        require(tat_json_value_parse(&context, deep, &nested) == TA_OK, "parse deep nesting");
        test_sink sink = {0};
        require(model.v->value_write(model.self, &(ta_datamodel_value_write_params){
                                                     .value = &nested, .sink = { .self = &sink, .v = &TEST_SINK },
                                                     .durable = true, .fault = &fault }) == TA_OP_OK &&
                    sink.len == 2u * depth && memcmp(sink.bytes, deep, sink.len) == 0,
                "deep nesting is written whole");
        free(sink.bytes);
        free(deep);
        ta_value_release(&nested);
    }
    ta_value_release(&binding);
    free(instance);
    tat_json_datamodel_context_destroy(&context);
    puts("tinyactors-test JSON data model test passed");
    return 0;
}
