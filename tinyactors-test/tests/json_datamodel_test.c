#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "json_datamodel.h"

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
    /* Offloading (core ADR 0074): a value's bytes are its JSON text, and they
     * decode to an equal value; bytes that are not JSON are refused. */
    ta_arena arena;
    require(ta_arena_init(&arena, tat_libc_allocator(), 64) == TA_OK, "offload arena");
    ta_bytes bytes = {0};
    require(model.v->value_encode(model.self, &(ta_datamodel_value_encode_params){ .shared = shared,
                                                                                    .value = &binding,
                                                                                    .arena = &arena,
                                                                                    .out = &bytes }) == TA_OP_OK,
            "value_encode");
    require(bytes.len > 2 && bytes.ptr[0] == '{' && bytes.ptr[bytes.len - 1u] == '}',
            "value_encode writes the JSON text");
    ta_value back = {0};
    require(model.v->value_decode(model.self, &(ta_datamodel_value_decode_params){ .shared = shared,
                                                                                    .bytes = bytes,
                                                                                    .out = &back }) == TA_OP_OK,
            "value_decode");
    ta_bytes again = {0};
    require(model.v->value_encode(model.self, &(ta_datamodel_value_encode_params){ .shared = shared,
                                                                                    .value = &back,
                                                                                    .arena = &arena,
                                                                                    .out = &again }) == TA_OP_OK &&
                again.len == bytes.len && memcmp(again.ptr, bytes.ptr, bytes.len) == 0,
            "the decoded value equals the encoded one");
    ta_value_release(&back);
    require(model.v->value_decode(model.self, &(ta_datamodel_value_decode_params){
                                                  .shared = shared, .bytes = { .ptr = (const uint8_t *)"{", .len = 1 },
                                                  .out = &back }) == TA_OP_ERROR,
            "value_decode refuses bytes that are not JSON");
    ta_arena_destroy(&arena);
    ta_value_release(&binding);
    free(instance);
    tat_json_datamodel_context_destroy(&context);
    puts("tinyactors-test JSON data model test passed");
    return 0;
}
