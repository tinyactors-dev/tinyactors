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
    ta_value_release(&binding);
    free(instance);
    tat_json_datamodel_context_destroy(&context);
    puts("tinyactors-test JSON data model test passed");
    return 0;
}
