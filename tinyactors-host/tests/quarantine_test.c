/*
 * A quarantined invoked child tells its parent (napi ADR 0025), driven
 * without Node-API: the report reaches the parent as
 * error.communication.invoke.<id>.quarantined, and running out of memory
 * for it delays it to a later turn without losing it. (Core's own report
 * running out of memory takes the same retry; tinyactors-test's sweep
 * covers core.) The child is quarantined by a message core refuses as
 * malformed: a runaway no longer quarantines, since core abandons its
 * macrostep at the microstep limit (core ADR 0099 section 7).
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host/hooks.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "scheduler_private.h"
#include "system_private.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

static bool no_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
    return true;
}

static ta_definition_id define(tah_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

/* The in-system invoker's question, answered with the one child chart. */
static ta_definition_id child_chart;

static ta_op_status child_definition(void *context, const tah_child_params *params) {
    (void)context;
    *params->out = child_chart;
    return TA_OP_OK;
}

/* The report's data, which fails for want of memory `fail_data` times. */
typedef struct {
    uint32_t calls;
    uint32_t fail_data;
} data_script;

static bool quarantine_data(void *context, const tah_quarantine_report *report, ta_value *out) {
    data_script *script = context;
    (void)out;
    CHECK(report->code == TA_ERR_INVALID);
    CHECK(strcmp(report->operation, "deliver") == 0);
    script->calls += 1u;
    if (script->fail_data > 0) {
        script->fail_data -= 1u;
        return false;
    }
    return true;
}

static bool in_state(tah_system *system, ta_actor_id actor, const char *state) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    ta_state_id states[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system->core, &(ta_actor_active_states_params){
                                                   .actor = actor, .states = states, .capacity = 8, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count; index += 1) {
        ta_state_info info;
        CHECK(ta_definition_state(system->core, &(ta_definition_state_params){ .definition = view.definition_id, .state = states[index], .out = &info }) == TA_OK);
        if (info.id.len == strlen(state) && memcmp(info.id.ptr, state, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

/* The invoked child: the actor of the first slots that has a parent. */
static ta_actor_id invoked_child(tah_system *system) {
    for (uint32_t slot = 0; slot < 8u; slot += 1u) {
        for (uint32_t generation = 1; generation < 4u; generation += 1u) {
            ta_actor_id actor = ta_actor_id_make(slot, generation);
            ta_actor_inspection view;
            if (ta_actor_inspect(system->core, actor, &view) == TA_OK && view.has_parent) {
                return actor;
            }
        }
    }
    CHECK(false);
    return 0;
}

/* Posts the child a message core refuses as malformed (an event name it never issued): it faults and is quarantined. */
static void fault_the_child(tah_system *system) {
    ta_actor_id child = invoked_child(system);
    ta_event bad = { .name = 0x7ffffff0u, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE }; /* a name this system never issued */
    CHECK(tah_system_post(system, &(tah_message){ .target = child, .event = &bad }) == TA_OK);
}

/* One turn; true when it recorded a fault. */
static bool turn(tah_system *system) {
    tah_system_turn(system);
    bool faulted = false;
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        faulted = faulted || record->kind == TAH_RECORD_FAULT;
    }
    tah_system_end_turn(system);
    return faulted;
}

static void reports(uint32_t fail_data) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    tah_system_set_driver(system, &(tah_pump_driver){ .wake = no_wake });
    data_script script = { .fail_data = fail_data };
    tah_system_set_callbacks(system, &(tah_system_callbacks){
                                         .context = &script,
                                         .child_definition = child_definition,
                                         .quarantine_data = quarantine_data,
                                     });

    /* The child waits; the parent waits for it in `waiting`. */
    ta_scxml_state_spec child_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("spin") }, { 0 } };
    ta_scxml_spec child = { .name = S("spinner"), .states = child_states };
    child_chart = define(system, &child);

    ta_str told_target[] = { S("told"), { 0 } };
    ta_scxml_transition_spec on_report[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("error.communication.invoke.w.quarantined"),
          .targets = told_target },
        { 0 },
    };
    ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE,
          .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = S("w") },
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("http://www.w3.org/TR/scxml/") } },
        { 0 },
    };
    ta_scxml_state_spec parent_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("waiting"), .transitions = on_report, .invokes = invokes },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("told") },
        { 0 },
    };
    ta_scxml_spec parent_spec = { .name = S("waiter"), .states = parent_states };
    ta_actor_id parent = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = define(system, &parent_spec),
                                                                 .out = &parent }) == TA_OK);
    CHECK(tah_hook_actor_created(system, parent) == TA_OK);

    uint32_t turns = 0;
    while (!tah_system_quiet(system)) { /* the invocation starts and the child bootstraps */
        (void)turn(system);
        turns += 1u;
        CHECK(turns < 16u);
    }
    fault_the_child(system);
    while (!turn(system)) {
        turns += 1u;
        CHECK(turns < 16u);
    }
    CHECK(system->sched.quarantined == 1u);
    if (fail_data > 0) {
        /* The report waits, and keeps the system busy until it is taken. */
        CHECK(system->quarantine_reports.count == 1u);
        CHECK(!tah_system_quiet(system));
        CHECK(in_state(system, parent, "waiting"));
    }
    while (!tah_system_quiet(system)) {
        (void)turn(system);
        turns += 1u;
        CHECK(turns < 32u);
    }
    CHECK(system->quarantine_reports.count == 0u);
    CHECK(in_state(system, parent, "told"));
    CHECK(script.calls == 1u + fail_data);
    /* Leaving `waiting` cancelled the invocation, which destroyed the child. */
    CHECK(system->sched.quarantined == 0u);

    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* A parent gone before its child's report is taken: the report is dropped. */
static void parent_gone(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    tah_system_set_driver(system, &(tah_pump_driver){ .wake = no_wake });
    data_script script = { .fail_data = 1 };
    tah_system_set_callbacks(system, &(tah_system_callbacks){
                                         .context = &script,
                                         .child_definition = child_definition,
                                         .quarantine_data = quarantine_data,
                                     });

    ta_scxml_state_spec child_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("spin") }, { 0 } };
    ta_scxml_spec child = { .name = S("spinner"), .states = child_states };
    child_chart = define(system, &child);
    ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE,
          .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = S("w") },
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("http://www.w3.org/TR/scxml/") } },
        { 0 },
    };
    ta_scxml_state_spec parent_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("waiting"), .invokes = invokes },
                                            { 0 } };
    ta_scxml_spec parent_spec = { .name = S("waiter"), .states = parent_states };
    ta_actor_id parent = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = define(system, &parent_spec),
                                                                 .out = &parent }) == TA_OK);
    CHECK(tah_hook_actor_created(system, parent) == TA_OK);
    uint32_t turns = 0;
    while (!tah_system_quiet(system)) {
        (void)turn(system);
        turns += 1u;
        CHECK(turns < 16u);
    }
    fault_the_child(system);
    while (!turn(system)) {
        turns += 1u;
        CHECK(turns < 16u);
    }
    CHECK(system->quarantine_reports.count == 1u);
    /* Destroying the parent cancels the invocation, so core answers NOT_FOUND. */
    CHECK(ta_actor_destroy(system->core, parent) == TA_OK);
    CHECK(tah_hook_actor_removed(system, parent) == TA_OK);
    while (!tah_system_quiet(system)) {
        (void)turn(system);
        turns += 1u;
        CHECK(turns < 32u);
    }
    CHECK(system->quarantine_reports.count == 0u);

    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

int main(void) {
    reports(0);
    reports(2);
    parent_gone();
    puts("quarantine_test passed");
    return 0;
}
