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

static ta_op_status decode_target(tat_json_datamodel_context *context,
                                  ta_datamodel model,
                                  void *shared,
                                  void *instance,
                                  const char *json,
                                  ta_scxml_target *out) {
    ta_value value = {0};
    require(tat_json_value_parse(context, json, &value) == TA_OK, "target parse");
    ta_fault fault = {0};
    *out = (ta_scxml_target){0};
    ta_op_status status = model.v->decode_scxml_target(
        model.self,
        &(ta_datamodel_decode_scxml_target_params){
            .shared = shared,
            .instance = instance,
            .value = &value,
            .out = out,
            .fault = &fault,
        });
    ta_value_release(&value);
    return status;
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

    ta_scxml_target target = {0};
    require(decode_target(&context, model, shared, instance, "\"#_internal\"", &target) ==
                    TA_OP_OK &&
                target.kind == TA_SCXML_TARGET_INTERNAL,
            "internal target");
    require(decode_target(&context, model, shared, instance, "\"#_self\"", &target) ==
                    TA_OP_OK &&
                target.kind == TA_SCXML_TARGET_SELF,
            "self target");
    require(decode_target(&context, model, shared, instance, "\"#_parent\"", &target) ==
                    TA_OP_OK &&
                target.kind == TA_SCXML_TARGET_PARENT,
            "parent target");
    require(decode_target(&context, model, shared, instance, "\"#_scxml_42\"", &target) ==
                    TA_OP_OK &&
                target.kind == TA_SCXML_TARGET_ACTOR &&
                target.as.actor.kind == TA_ACTOR_REF_RUNTIME &&
                target.as.actor.as.runtime == 42,
            "actor target");
    require(decode_target(&context, model, shared, instance, "\"#_17\"", &target) ==
                    TA_OP_OK &&
                target.kind == TA_SCXML_TARGET_INVOKE && target.as.invoke == 17,
            "invoke target");

    const char *invalid_targets[] = {
        "\"#_scxml_\"", "\"#_scxml_4x\"", "\"#_\"", "\"#_abc\"", "\"elsewhere\"", "7",
    };
    for (size_t index = 0; index < ARRAY_COUNT(invalid_targets); index += 1) {
        require(decode_target(&context, model, shared, instance, invalid_targets[index],
                              &target) == TA_OP_ERROR,
                invalid_targets[index]);
    }

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
    ta_value_release(&binding);
    free(instance);
    tat_json_datamodel_context_destroy(&context);
    puts("tinyactors-test JSON data model test passed");
    return 0;
}
