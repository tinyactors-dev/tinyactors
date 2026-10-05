/*
 * Values leaving a session are copies (ADR 0097, SCXML C.1): the data of a
 * <send> to anything but #_internal, an <invoke>'s params and content expr,
 * and autoforwarded events go through the data model's copy_out; a data
 * model without copy_out shares; a refused copy is error.execution, and one
 * that runs out of memory runs again.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "test_drive.h"

static int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define SOURCE(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) })
#define END {0}

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * Mutable values: a reference-counted box holding a number
 */

typedef struct {
    int refs;
    int number;
} box;

static int live_boxes = 0;

static void box_retain(void *self) {
    ((box *)self)->refs += 1;
}

static void box_release(void *self) {
    box *value = self;
    value->refs -= 1;
    if (value->refs == 0) {
        free(value);
        live_boxes -= 1;
    }
}

static const ta_value_vtable box_vtable = { .retain = box_retain, .release = box_release };

static ta_value box_new(int number) {
    box *value = malloc(sizeof *value);
    *value = (box){ .refs = 1, .number = number };
    live_boxes += 1;
    return (ta_value){ .self = value, .v = &box_vtable };
}

static box *box_of(const ta_value *value) {
    return value->v == &box_vtable ? value->self : NULL;
}

/* ------------------------------------------------------------------------
 * A data model whose one variable, `held`, is a box every expression
 * evaluates to. copy_out makes a new box, refuses, or runs out of memory.
 */

typedef enum { COPY_OK, COPY_REFUSE, COPY_NOMEM_ONCE } copy_mode;

static copy_mode mode = COPY_OK;
static int copies = 0;  /* copy_out calls */
static box *held = NULL; /* the last actor's variable */

static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}
static void dm_destroy_definition(void *self, void *shared, ta_allocator unused) {
    (void)self, (void)shared, (void)unused;
}
static uint32_t dm_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return sizeof(ta_value);
}
static uint32_t dm_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return _Alignof(ta_value);
}
static ta_result dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    ta_value *variable = params->instance;
    *variable = box_new(1);
    held = variable->self;
    return TA_OK;
}
static void dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared;
    ta_value_release(instance);
}
static ta_op_status dm_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    *params->out = true;
    return TA_OP_OK;
}
static ta_op_status dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    *params->out = *(ta_value *)params->instance;
    ta_value_retain(params->out);
    return TA_OP_OK;
}
/* Params are the first param's value, which is enough to see it copied. */
static ta_op_status dm_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    *params->out = params->values[0].value;
    ta_value_retain(params->out);
    return TA_OP_OK;
}
static ta_op_status dm_copy_out(void *self, const ta_datamodel_copy_out_params *params) {
    (void)self;
    copies += 1;
    if (mode == COPY_REFUSE) {
        *params->fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED, .message = STR("cannot copy") };
        return TA_OP_ERROR;
    }
    if (mode == COPY_NOMEM_ONCE) {
        mode = COPY_OK;
        return TA_OP_NOMEM;
    }
    const box *original = box_of(params->value);
    CHECK(original != NULL);
    *params->out = box_new(original != NULL ? original->number : 0);
    return TA_OP_OK;
}

#define DM_MEMBERS                                   \
    .prepare_definition = dm_prepare,                \
    .destroy_definition = dm_destroy_definition,     \
    .instance_size = dm_instance_size,               \
    .instance_alignment = dm_instance_alignment,     \
    .init_instance = dm_init_instance,               \
    .destroy_instance = dm_destroy_instance,         \
    .eval_condition = dm_eval_condition,             \
    .eval_value = dm_eval_value,                     \
    .read_location = dm_eval_value_location,         \
    .make_params = dm_make_params

static ta_op_status dm_eval_value_location(void *self, const ta_datamodel_read_location_params *params) {
    (void)self;
    *params->out = *(ta_value *)params->instance;
    ta_value_retain(params->out);
    return TA_OP_OK;
}

static const ta_datamodel_vtable copying = { .name = STR("copying"), DM_MEMBERS, .copy_out = dm_copy_out };
static const ta_datamodel_vtable sharing = { .name = STR("sharing"), DM_MEMBERS };

/* ------------------------------------------------------------------------
 * A router and an invoker that keep what reaches them
 */

enum { KEPT = 8 };

typedef struct {
    ta_value data[KEPT];
    uint32_t count;
} kept_values;

static kept_values routed;    /* event data the router was offered */
static kept_values started;   /* an invocation's params, then its data */
static kept_values forwarded; /* autoforwarded event data */

static void keep(kept_values *kept, const ta_value *value) {
    CHECK(kept->count < KEPT);
    if (kept->count < KEPT) {
        kept->data[kept->count] = *value;
        ta_value_retain(&kept->data[kept->count]);
        kept->count += 1u;
    }
}

static void forget(kept_values *kept) {
    for (uint32_t index = 0; index < kept->count; index += 1u) {
        ta_value_release(&kept->data[index]);
    }
    *kept = (kept_values){0};
}

static ta_external_event_submit_status route(void *self, const ta_external_event_submit_params *params) {
    (void)self;
    keep(&routed, &params->delivery->event.data);
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable router_vtable = { .submit = route };

static ta_op_status invoker_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    keep(&started, &params->request->params);
    keep(&started, &params->request->data);
    *params->service_handle = 1;
    return TA_OP_OK;
}

static ta_op_status invoker_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    (void)self, (void)service_handle, (void)fault;
    return TA_OP_OK;
}

static ta_op_status invoker_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self;
    keep(&forwarded, &params->event->data);
    return TA_OP_OK;
}

static const ta_invoker_vtable invoker_vtable = { .start = invoker_start, .cancel = invoker_cancel,
                                                  .forward = invoker_forward };

/* ------------------------------------------------------------------------
 * Helpers
 */

static ta_system *create(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    CHECK(ta_system_set_external_event_router(system, (ta_external_event_router){ .v = &router_vtable }) ==
          TA_OK);
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(system, STR("svc"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(system, type, (ta_invoker){ .v = &invoker_vtable }) == TA_OK);
    return system;
}

static ta_actor_id start(ta_system *system, const ta_scxml_spec *spec, const ta_datamodel_vtable *datamodel) {
    ta_definition *definition = NULL;
    ta_definition_id id = 0;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = spec,
                                                                             .datamodel = { .v = datamodel },
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    CHECK(definition != NULL && ta_system_register_definition(system, definition, &id) == TA_OK);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
    return actor;
}

static ta_result resume(ta_system *system, ta_actor_id actor) {
    return test_drive_run(system, actor, &(test_drive_run_params){ .budget = 1000 });
}

static bool in_state(ta_system *system, ta_actor_id actor, const char *id) {
    ta_state_id active[4];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                          .capacity = 4, .count = &count }) ==
          TA_OK);
    ta_actor_inspection inspection = {0};
    CHECK(ta_actor_inspect(system, actor, &inspection) == TA_OK);
    for (uint32_t index = 0; index < count; index += 1) {
        ta_state_info info = {0};
        CHECK(ta_definition_state(system, &(ta_definition_state_params){ .definition = inspection.definition_id,
                                                                         .state = active[index],
                                                                         .out = &info }) == TA_OK);
        if (info.id.len == strlen(id) && memcmp(info.id.ptr, id, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

static void reset(copy_mode next) {
    mode = next;
    copies = 0;
    held = NULL;
}

static void finish(ta_system *system) {
    ta_system_destroy(system);
    forget(&routed);
    forget(&started);
    forget(&forwarded);
    CHECK(live_boxes == 0);
}

/* ------------------------------------------------------------------------
 * Charts
 */

static const ta_str to_failed[] = { STR("failed"), END };

/* On entry: a send to self and one to #_internal, both with `held`. */
static const ta_scxml_action_spec two_sends[] = {
    { .kind = TA_SCXML_ACTION_SEND,
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("out") },
                   .data = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SOURCE("held") } } },
    { .kind = TA_SCXML_ACTION_SEND,
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("in") },
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("#_internal") },
                   .data = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SOURCE("held") } } },
    END,
};
static const ta_scxml_handler_spec sends_on_entry[] = { { .actions = two_sends }, END };
static const ta_scxml_transition_spec on_error[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"), .targets = to_failed },
    END,
};
static const ta_scxml_state_spec sender_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .onentry = sends_on_entry, .transitions = on_error },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
    END,
};
static const ta_scxml_spec sender = { .name = STR("sender"), .states = sender_states };

/* ------------------------------------------------------------------------
 * <send>
 */

static void test_send_copies(void) {
    reset(COPY_OK);
    ta_system *system = create();
    ta_actor_id actor = start(system, &sender, &copying);
    CHECK(resume(system, actor) == TA_OK);
    /* the send to self was copied; the one to #_internal was not */
    CHECK(copies == 1);
    CHECK(routed.count == 1);
    box *sent = routed.count == 1 ? box_of(&routed.data[0]) : NULL;
    CHECK(sent != NULL && sent != held);
    /* a mutation in the sender does not show in what it sent */
    held->number = 99;
    CHECK(sent != NULL && sent->number == 1);
    CHECK(in_state(system, actor, "start"));
    finish(system);
}

static void test_without_copy_out_values_are_shared(void) {
    reset(COPY_OK);
    ta_system *system = create();
    ta_actor_id actor = start(system, &sender, &sharing);
    CHECK(resume(system, actor) == TA_OK);
    CHECK(copies == 0);
    CHECK(routed.count == 1 && box_of(&routed.data[0]) == held);
    finish(system);
}

static void test_refused_copy_is_an_execution_error(void) {
    reset(COPY_REFUSE);
    ta_system *system = create();
    ta_actor_id actor = start(system, &sender, &copying);
    CHECK(resume(system, actor) == TA_OK);
    CHECK(copies == 1);
    CHECK(routed.count == 0);
    CHECK(in_state(system, actor, "failed"));
    finish(system);
}

static void test_copy_out_of_memory_runs_again(void) {
    reset(COPY_NOMEM_ONCE);
    ta_system *system = create();
    ta_actor_id actor = start(system, &sender, &copying);
    CHECK(resume(system, actor) == TA_ERR_NOMEM);
    CHECK(routed.count == 0);
    CHECK(resume(system, actor) == TA_OK);
    CHECK(copies == 2);
    CHECK(routed.count == 1 && box_of(&routed.data[0]) != held);
    CHECK(in_state(system, actor, "start"));
    finish(system);
}

/* ------------------------------------------------------------------------
 * <invoke> params and content, and autoforwarding
 */

static const ta_scxml_param_spec invoke_params[] = {
    { .name = STR("p"), .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SOURCE("held") } },
    END,
};
static const ta_scxml_invoke_spec invokes[] = {
    { .kind = TA_SCXML_INVOKE,
      .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") },
      .data = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SOURCE("held") },
      .params = invoke_params,
      .autoforward = true },
    END,
};
static const ta_scxml_state_spec invoker_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .invokes = invokes, .transitions = on_error },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
    END,
};
static const ta_scxml_spec invoking = { .name = STR("invoking"), .states = invoker_states };

static void test_invoke_copies(void) {
    reset(COPY_OK);
    ta_system *system = create();
    ta_actor_id actor = start(system, &invoking, &copying);
    CHECK(resume(system, actor) == TA_OK);
    CHECK(copies == 2);
    CHECK(started.count == 2);
    for (uint32_t index = 0; index < started.count; index += 1u) {
        box *given = box_of(&started.data[index]);
        CHECK(given != NULL && given != held);
    }
    held->number = 7;
    CHECK(started.count == 2 && box_of(&started.data[0])->number == 1 && box_of(&started.data[1])->number == 1);

    /* An event delivered to the parent is forwarded as a copy. */
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, STR("ext"), &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    ta_value data = box_new(5);
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE, .data = data };
    CHECK(test_drive_deliver(system,
                             &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                         .event = &event },
                             &(test_drive_run_params){ .budget = 1000 }) == TA_OK);
    CHECK(copies == 3);
    CHECK(forwarded.count == 1);
    box *given = forwarded.count == 1 ? box_of(&forwarded.data[0]) : NULL;
    CHECK(given != NULL && given != data.self && given->number == 5);
    ta_value_release(&data);
    finish(system);
}

static void test_refused_invoke_copy_fails_the_invocation(void) {
    reset(COPY_REFUSE);
    ta_system *system = create();
    ta_actor_id actor = start(system, &invoking, &copying);
    CHECK(resume(system, actor) == TA_OK);
    CHECK(started.count == 0);
    CHECK(in_state(system, actor, "failed"));
    finish(system);
}

int main(void) {
    test_send_copies();
    test_without_copy_out_values_are_shared();
    test_refused_copy_is_an_execution_error();
    test_copy_out_of_memory_runs_again();
    test_invoke_copies();
    test_refused_invoke_copy_fails_the_invocation();
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "copy test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core copy test passed");
    return 0;
}
