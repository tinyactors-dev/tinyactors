/*
 * The tel ring (ADR 0098): core writes its trace records to a lossy ring
 * it allocates at creation, and a host reads them between calls. Claims:
 *
 *  1. The parameters: telBytes (default 16,384), maxRecordBytes (4,096,
 *     at least 512) and telemetryValueBytes (1,024, at most
 *     maxRecordBytes - 512) are reported by ta_param_info_of; telBytes
 *     must be 0 or at least maxRecordBytes, and creation names the one at
 *     fault.
 *  2. telBytes 0 creates no ring: two allocations fewer, a status of
 *     nothing, masks other than zero refused, nothing to read, and the
 *     system runs as before.
 *  3. Each kind's record is self-contained: the session's ID as text, the
 *     definition's facts on the kinds that open a run, the names of the
 *     position's state, transition and action and of the ones the kind
 *     names, the event's name and data, the send ID, the data's name, the
 *     fault, and every list entry's name beside its ID.
 *  4. Values are text the data model renders for a trace: cut at the
 *     masks' value_bytes, which is kept at most telemetryValueBytes, on the
 *     data model's word (`trace_bytes`), marked cut, and not rendered at
 *     all, with no call to the data model, when either is 0; the reason a
 *     failure gives (the fault's data, the error event's) is rendered
 *     whatever the masks' value_bytes says.
 *  5. A record never passes maxRecordBytes: the lists are cut, the last
 *     first, and the counts say how many entries are there.
 *  6. A full ring drops and counts: the seqs read and the drop reports
 *     account for every seq, a report comes before the next record, and
 *     the status counts every drop.
 *  7. The clock is optional and called once per record written: never for
 *     a kind that is off or a record that is dropped; without one, wall_ns
 *     is 0.
 *  8. Reading: a peek answers the same record until it is released;
 *     releasing an empty ring and null arguments are refused.
 *  9. Each value's text is copied as soon as the data model renders it: a
 *     data model that renders every value into one buffer still gives a
 *     record whose event data, value and fault data are each their own.
 * 10. A record's texts are read in place: ta_trace_record_text answers
 *     each from the record's bytes in the ring, and empty text for a NULL
 *     record, a text that is not a ta_trace_text, and a record that has no
 *     bytes, one no peek answered.
 * 11. The labels consecutive records repeat are kept between records, yet
 *     every record names its own definition's transitions and actions: two
 *     definitions whose transition and action of the same IDs have other
 *     labels, taking turns, each get their own; and a definition's kept
 *     labels and facts are forgotten when it is freed, so the one
 *     registered in its slot after it gets its own.
 *
 * Includes the amalgamation, so it can write a record that carries all
 * three values, which no kind does today. Uses the C library; the core
 * does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.c"
#include "test_allocator.h"
#include "tel_reader.h"

static int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define EXPR(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) })
#define END {0}

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

static bool text_is(ta_str text, const char *expected) {
    return text.len == strlen(expected) && (text.len == 0 || memcmp(text.ptr, expected, text.len) == 0);
}

/* ------------------------------------------------------------------------
 * A data model: conditions "t" and "f", script "e" fails, and values are
 * texts: "v" is "short", "long" is 2,000 bytes, and anything else is itself.
 */

static void no_op(void *self) { (void)self; }
static const ta_value_vtable text_value_vtable = { .retain = no_op, .release = no_op };
static char long_text[2001];

static ta_value text_value(const char *text) {
    return (ta_value){ .self = (void *)text, .v = &text_value_vtable };
}

static ta_str source(const void *shared, ta_expr_id expr) {
    return ((const ta_expression *)shared)[expr].as.source;
}

static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = (void *)params->expressions;
    return TA_OK;
}
static void dm_destroy_definition(void *self, void *shared, ta_allocator unused) {
    (void)self, (void)shared, (void)unused;
}
static uint32_t dm_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t dm_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}
static ta_op_status dm_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->expr);
    *params->out = text.len == 1 && text.ptr[0] == 't';
    return TA_OP_OK;
}
static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->script);
    if (text.len == 1 && text.ptr[0] == 'e') {
        *params->fault = (ta_fault){ .code = 8, .message = STR("boom"), .data = text_value("thrown") };
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}
static ta_op_status dm_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static ta_op_status dm_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static ta_op_status dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->expr);
    *params->out = text_value(text.len == 4 && memcmp(text.ptr, "long", 4) == 0 ? long_text : "short");
    return TA_OP_OK;
}

/* What value_text was last asked, and how often. With `one_buffer`, every
 * text is rendered into the same buffer, valid only until the next call. */
static uint32_t asked_trace_bytes = 0;
static long value_texts = 0;
static bool one_buffer = false;
static char rendered[64];
static ta_op_status dm_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    value_texts += 1;
    asked_trace_bytes = params->trace_bytes;
    const char *text = params->value->self;
    if (one_buffer) {
        snprintf(rendered, sizeof rendered, "%s", text);
        text = rendered;
    }
    *params->out = (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) };
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("tel"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .eval_value = dm_eval_value,
    .assign = dm_assign,
    .set_named = dm_set_named,
    .execute_script = dm_execute_script,
    .value_text = dm_value_text,
};

/* ------------------------------------------------------------------------
 * The chart
 *
 *   data x = v
 *   idle --go [t]--> busy     assign x = v; send later after 1s as "s1"; raise inner
 *   busy: onentry script e (fails); --inner--> done (final)
 */

static const ta_scxml_action_spec go_actions[] = {
    { .kind = TA_SCXML_ACTION_ASSIGN,
      .as.assign = { .location = EXPR("x"), .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("v") } } },
    { .kind = TA_SCXML_ACTION_SEND,
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("s1") },
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("later") },
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("1s") } } },
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("inner") } },
    END,
};
static const ta_scxml_action_spec failing[] = { { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR("e") },
                                                END };
static const ta_scxml_handler_spec busy_entry[] = { { failing }, END };
static const ta_str to_busy[] = { STR("busy"), {0} };
static const ta_str to_done[] = { STR("done"), {0} };
static const ta_scxml_transition_spec idle_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .condition = EXPR("t"), .targets = to_busy,
      .actions = go_actions },
    END,
};
static const ta_scxml_transition_spec busy_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("inner"), .targets = to_done },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("busy"), .onentry = busy_entry, .transitions = busy_transitions },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("done") },
    END,
};
static const ta_scxml_data_spec data[] = {
    { .name = STR("x"), .initializer = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("v") } },
    END,
};
static const ta_scxml_spec chart = { .name = STR("telchart"), .states = roots, .data = data };

/* A parallel state whose 24 regions have long names: one microstep enters them all. */
enum { REGIONS = 24 };
static ta_scxml_state_spec wide_regions[REGIONS + 1];
static char wide_names[REGIONS][32];
static const ta_scxml_state_spec wide_roots[] = {
    { .kind = TA_SCXML_STATE_PARALLEL, .id = STR("p"), .states = wide_regions },
    END,
};
static const ta_scxml_spec wide_chart = { .name = STR("wide"), .states = wide_roots };

static void make_wide_chart(void) {
    for (uint32_t index = 0; index < REGIONS; index += 1u) {
        int len = snprintf(wide_names[index], sizeof wide_names[index], "region-with-a-long-name-%02u", index);
        wide_regions[index] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC,
                                                     .id = { .ptr = wide_names[index], .len = (uint32_t)len } };
    }
}

/* ------------------------------------------------------------------------
 * Driving it
 */

/* A counting clock: 1,000 ns more on every call. */
static uint64_t clock_calls = 0;
static uint64_t tick(void *self) {
    (void)self;
    clock_calls += 1u;
    return clock_calls * 1000u;
}
static const ta_wall_clock_vtable clock_vtable = { .wall_ns = tick };

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    ta_actor_id actor;
} run;

static ta_result create(const ta_param_value *params, uint32_t count, ta_system **out, ta_param *rejected) {
    return ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count,
                                                 .rejected = rejected },
                            out);
}

static ta_definition_id define(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = spec,
                                                                              .datamodel = { .v = &dm_vtable },
                                                                              .allocator = allocator,
                                                                              .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static void settle(run *r) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(r->system, &(ta_actor_resume_params){ .actor = r->actor, .budget = 1000,
                                                                .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE || status == TA_RUN_DONE);
}

static void deliver(run *r, const char *name, ta_value data) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(r->system, (ta_str){ .ptr = name, .len = (uint32_t)strlen(name) }, &atom) == TA_OK);
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE, .data = data };
    CHECK(ta_event_name_intern(r->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &event.name) == TA_OK);
    CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .time = ta_system_time(r->system),
                                                                  .actor = r->actor, .event = &event }) == TA_OK);
}

/* Copies of the records read, with the texts the checks look at. */
enum { KEPT = 64, TEXT = 96, NAMES = 8 };
typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    uint64_t seq;
    uint64_t wall_ns;
    ta_trace_position position;
    char session_id[TEXT];
    char family[TEXT];
    char document_name[TEXT];
    uint32_t revision_len;
    char position_state[TEXT];
    char position_transition[TEXT];
    char position_action[TEXT];
    ta_state_id state;
    char state_name[TEXT];
    char action_name[TEXT];
    char name[TEXT];
    bool has_event;
    char event_name[TEXT];
    char event_data[TEXT];
    char send_id[TEXT];
    bool has_value;
    char value[TEXT];
    uint32_t value_len;
    bool value_cut;
    bool truncated;
    bool lists_cut;
    bool has_fault;
    int32_t fault_code;
    char fault_message[TEXT];
    char fault_data[TEXT];
    uint32_t transition_count;
    uint32_t exited_count;
    uint32_t entered_count;
    ta_state_id entered[NAMES];
    /* Every list entry's name, in list order. */
    char names[NAMES][TEXT];
    uint32_t name_count;
} kept;

static kept records[KEPT];
static uint32_t record_count = 0;

static void copy(char *out, ta_str text) {
    size_t len = text.len < TEXT - 1u ? text.len : TEXT - 1u;
    if (len != 0) {
        memcpy(out, text.ptr, len);
    }
    out[len] = '\0';
}

static void keep(void *self, const ta_trace_record *record) {
    (void)self;
    if (record_count == KEPT) {
        return;
    }
    kept *k = &records[record_count++];
    memset(k, 0, sizeof *k);
    k->kind = record->kind;
    k->key = record->key;
    k->wall_ns = record->wall_ns;
    k->position = record->position;
    copy(k->session_id, ta_trace_record_text(record, TA_TRACE_TEXT_SESSION_ID));
    copy(k->family, ta_trace_record_text(record, TA_TRACE_TEXT_FAMILY));
    copy(k->document_name, ta_trace_record_text(record, TA_TRACE_TEXT_DOCUMENT_NAME));
    k->revision_len = record->revision.len;
    copy(k->position_state, ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_STATE));
    copy(k->position_transition, ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_TRANSITION));
    copy(k->position_action, ta_trace_record_text(record, TA_TRACE_TEXT_POSITION_ACTION));
    k->state = record->state;
    copy(k->state_name, ta_trace_record_text(record, TA_TRACE_TEXT_STATE));
    copy(k->action_name, ta_trace_record_text(record, TA_TRACE_TEXT_ACTION));
    copy(k->name, ta_trace_record_text(record, TA_TRACE_TEXT_NAME));
    k->has_event = record->has_event;
    copy(k->event_name, ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME));
    copy(k->event_data, ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_DATA));
    copy(k->send_id, ta_trace_record_text(record, TA_TRACE_TEXT_SEND_ID));
    k->has_value = record->has_value;
    copy(k->value, ta_trace_record_text(record, TA_TRACE_TEXT_VALUE));
    k->value_len = ta_trace_record_text(record, TA_TRACE_TEXT_VALUE).len;
    k->value_cut = record->value_cut;
    k->truncated = record->truncated;
    k->lists_cut = record->lists_cut;
    k->has_fault = record->has_fault;
    k->fault_code = record->fault_code;
    copy(k->fault_message, ta_trace_record_text(record, TA_TRACE_TEXT_FAULT_MESSAGE));
    copy(k->fault_data, ta_trace_record_text(record, TA_TRACE_TEXT_FAULT_DATA));
    k->transition_count = record->transition_count;
    k->exited_count = record->exited_count;
    k->entered_count = record->entered_count;
    for (uint32_t index = 0; index < record->entered_count && index < NAMES; index += 1u) {
        k->entered[index] = record->entered[index];
    }
    /* The names: a u32 length, then the bytes, per entry. */
    uint32_t total = record->transition_count + record->exited_count + record->entered_count +
                     record->history_count + record->state_count;
    const uint8_t *at = record->list_names.ptr;
    const uint8_t *end = at + record->list_names.len;
    for (uint32_t index = 0; index < total; index += 1u) {
        uint32_t len = 0;
        CHECK(end - at >= 4);
        if (end - at < 4) {
            break;
        }
        memcpy(&len, at, sizeof len);
        at += 4;
        CHECK((uint32_t)(end - at) >= len);
        if (index < NAMES) {
            copy(k->names[index], (ta_str){ .ptr = (const char *)at, .len = len });
            k->name_count = index + 1u;
        }
        at += len;
    }
    CHECK(at == end || total == 0);
}

static const kept *find(ta_trace_kind kind, uint32_t from) {
    for (uint32_t index = from; index < record_count; index += 1u) {
        if (records[index].kind == kind) {
            return &records[index];
        }
    }
    return NULL;
}

static void read_all(ta_system *system) {
    tel_drain(system, keep, NULL);
}

static void start(run *r, const ta_param_value *params, uint32_t count, const ta_scxml_spec *spec,
                  const ta_trace_masks *masks) {
    *r = (run){0};
    record_count = 0;
    CHECK(create(params, count, &r->system, NULL) == TA_OK);
    if (masks != NULL) {
        CHECK(ta_system_set_trace_masks(r->system, masks) == TA_OK);
    }
    r->definition = define(r->system, spec);
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor }) ==
          TA_OK);
}

static void finish(run *r) {
    ta_system_destroy(r->system);
}

static const ta_trace_masks every = { .kinds = UINT32_MAX, .watched_kinds = UINT32_MAX,
                                      .unsampled_kinds = UINT32_MAX, .value_bytes = UINT32_MAX };

/* ------------------------------------------------------------------------
 * Claim 1: the parameters
 */

static void expect_param(ta_param param, const char *name, uint64_t default_value, uint64_t min) {
    ta_param_info info = { 0 };
    CHECK(ta_param_info_of(param, &info) == TA_OK);
    CHECK(text_is(info.name, name) && text_is(info.unit, "bytes"));
    CHECK(info.default_value == default_value && info.min == min);
}

static void test_parameters(void) {
    expect_param(TA_PARAM_TEL_BYTES, "telBytes", 16384, 0);
    expect_param(TA_PARAM_MAX_RECORD_BYTES, "maxRecordBytes", 4096, 512);
    expect_param(TA_PARAM_TELEMETRY_VALUE_BYTES, "telemetryValueBytes", 1024, 0);
    ta_system *system = NULL;
    ta_param rejected = TA_PARAM_COUNT;
    ta_param_value small_ring[] = { { TA_PARAM_TEL_BYTES, 4095 } };
    CHECK(create(small_ring, 1, &system, &rejected) == TA_ERR_INVALID && system == NULL &&
          rejected == TA_PARAM_TEL_BYTES);
    ta_param_value big_value[] = { { TA_PARAM_TELEMETRY_VALUE_BYTES, 4096 - 511 } };
    CHECK(create(big_value, 1, &system, &rejected) == TA_ERR_INVALID && rejected == TA_PARAM_TELEMETRY_VALUE_BYTES);
    ta_param_value tiny_record[] = { { TA_PARAM_MAX_RECORD_BYTES, 511 } };
    CHECK(create(tiny_record, 1, &system, &rejected) == TA_ERR_INVALID && rejected == TA_PARAM_MAX_RECORD_BYTES);
    ta_param_value edges[] = { { TA_PARAM_MAX_RECORD_BYTES, 1024 }, { TA_PARAM_TEL_BYTES, 1024 },
                               { TA_PARAM_TELEMETRY_VALUE_BYTES, 512 } };
    CHECK(create(edges, 3, &system, NULL) == TA_OK);
    uint64_t value = 0;
    CHECK(ta_system_param(system, TA_PARAM_TEL_BYTES, &value) == TA_OK && value == 1024);
    ta_tel_status status;
    CHECK(ta_system_tel_status(system, &status) == TA_OK && status.capacity == 1024 && status.next_seq == 1);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Claim 2: telBytes 0
 */

static void test_no_ring(void) {
    long before = heap.allocations;
    ta_system *system = NULL;
    CHECK(create(NULL, 0, &system, NULL) == TA_OK);
    long with_ring = heap.allocations - before;
    ta_system_destroy(system);

    ta_param_value none[] = { { TA_PARAM_TEL_BYTES, 0 } };
    run r;
    start(&r, none, 1, &chart, NULL);
    ta_tel_status status = { .capacity = 1 };
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK && status.capacity == 0 && status.used == 0 &&
          status.dropped == 0);
    CHECK(ta_system_set_trace_masks(r.system, &every) == TA_ERR_INVALID);
    CHECK(ta_system_set_trace_masks(r.system, &(ta_trace_masks){ .watched_kinds = 1u }) == TA_ERR_INVALID);
    CHECK(ta_system_set_trace_masks(r.system, &(ta_trace_masks){ 0 }) == TA_OK);
    settle(&r);
    deliver(&r, "go", (ta_value){ 0 });
    settle(&r);
    ta_tel_record record;
    CHECK(ta_system_tel_peek(r.system, &record) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_tel_release(r.system) == TA_ERR_NOT_FOUND);
    finish(&r);

    /* The ring and its scratch area are the two allocations it saves. */
    before = heap.allocations;
    CHECK(create(none, 1, &system, NULL) == TA_OK);
    CHECK(heap.allocations - before == with_ring - 2);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Claim 3: self-contained records, kind by kind
 */

static void test_self_contained(void) {
    run r = { 0 };
    record_count = 0;
    CHECK(create(NULL, 0, &r.system, NULL) == TA_OK);
    CHECK(ta_system_set_trace_masks(r.system, &every) == TA_OK);
    r.definition = define(r.system, &chart);
    CHECK(ta_actor_spawn(r.system, &(ta_actor_spawn_params){ .definition = r.definition, .out = &r.actor }) ==
          TA_OK);
    settle(&r);
    deliver(&r, "go", text_value("payload"));
    settle(&r);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(r.system, r.actor, &view) == TA_OK);
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    read_all(r.system);
    char session[64];
    snprintf(session, sizeof session, "0.%llu.telchart", (unsigned long long)view.session_id);

    const kept *registered = find(TA_TRACE_DEFINITION_REGISTERED, 0);
    CHECK(registered != NULL);
    if (registered != NULL) {
        CHECK(strcmp(registered->family, "telchart") == 0 && strcmp(registered->document_name, "telchart") == 0);
        CHECK(registered->revision_len != 0 && registered->key.session == TA_TRACE_DEFINITIONS_SESSION);
    }
    const kept *spawned = find(TA_TRACE_SPAWNED, 0);
    CHECK(spawned != NULL);
    if (spawned != NULL) {
        CHECK(strcmp(spawned->session_id, session) == 0);
        CHECK(strcmp(spawned->family, "telchart") == 0 && spawned->revision_len != 0);
        CHECK(spawned->wall_ns == 0); /* no clock */
    }
    const kept *data_init = find(TA_TRACE_DATA_INIT, 0);
    CHECK(data_init != NULL);
    if (data_init != NULL) {
        CHECK(strcmp(data_init->name, "x") == 0 && data_init->has_value && strcmp(data_init->value, "short") == 0);
    }
    const kept *begin = find(TA_TRACE_MACROSTEP_BEGIN, 0);
    CHECK(begin != NULL && !begin->has_event && strcmp(begin->family, "telchart") == 0);
    const kept *condition = find(TA_TRACE_CONDITION, 0);
    CHECK(condition != NULL);
    if (condition != NULL) {
        CHECK(strcmp(condition->position_state, "idle") == 0 && strcmp(condition->state_name, "idle") == 0);
        CHECK(strcmp(condition->position_transition, "idle \xe2\x86\x92 busy on go [guarded]") == 0);
        CHECK(strcmp(condition->session_id, session) == 0 && condition->family[0] == '\0');
    }
    const kept *micro = NULL;
    for (uint32_t from = 0; (micro = find(TA_TRACE_MICROSTEP, from)) != NULL; from = (uint32_t)(micro - records) + 1u) {
        if (strcmp(micro->event_name, "go") == 0) {
            break;
        }
    }
    CHECK(micro != NULL);
    if (micro != NULL) {
        CHECK(micro->has_event && strcmp(micro->event_data, "payload") == 0);
        CHECK(micro->transition_count == 1 && micro->exited_count == 1 && micro->entered_count == 1);
        CHECK(micro->name_count == 3);
        CHECK(strcmp(micro->names[0], "idle \xe2\x86\x92 busy on go [guarded]") == 0);
        CHECK(strcmp(micro->names[1], "idle") == 0 && strcmp(micro->names[2], "busy") == 0);
    }
    const kept *assign = find(TA_TRACE_ASSIGN, 0);
    CHECK(assign != NULL);
    if (assign != NULL) {
        CHECK(strcmp(assign->position_action, "<assign> in idle \xe2\x86\x92 busy on go [guarded]") == 0);
        CHECK(assign->has_value && strcmp(assign->value, "short") == 0 && !assign->value_cut);
    }
    const kept *sent = find(TA_TRACE_SENT, 0);
    CHECK(sent != NULL && strcmp(sent->send_id, "s1") == 0 && strcmp(sent->event_name, "later") == 0);
    const kept *raised = find(TA_TRACE_RAISED, 0);
    CHECK(raised != NULL && strcmp(raised->event_name, "inner") == 0);
    const kept *failed = find(TA_TRACE_ACTION_FAILED, 0);
    CHECK(failed != NULL);
    if (failed != NULL) {
        CHECK(strcmp(failed->action_name, "<script> in onentry of busy") == 0);
        CHECK(failed->has_fault && failed->fault_code == 8 && strcmp(failed->fault_message, "boom") == 0);
        CHECK(strcmp(failed->fault_data, "thrown") == 0);
    }
    const kept *error = NULL;
    for (uint32_t from = 0; (error = find(TA_TRACE_RAISED, from)) != NULL; from = (uint32_t)(error - records) + 1u) {
        if (strcmp(error->event_name, "error.execution") == 0) {
            break;
        }
    }
    CHECK(error != NULL);
    const kept *finished = find(TA_TRACE_FINISHED, 0);
    CHECK(finished != NULL && strcmp(finished->state_name, "done") == 0);
    const kept *destroyed = find(TA_TRACE_DESTROYED, 0);
    CHECK(destroyed != NULL && strcmp(destroyed->session_id, session) == 0);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 4: values as text
 */

static void test_values(void) {
    memset(long_text, 'y', sizeof long_text - 1u);
    /* The long value through an assign: a chart whose assign evaluates "long". */
    static const ta_scxml_action_spec long_actions[] = {
        { .kind = TA_SCXML_ACTION_ASSIGN,
          .as.assign = { .location = EXPR("x"), .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("long") } } },
        END,
    };
    static const ta_scxml_transition_spec long_transitions[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("go"), .actions = long_actions },
        END,
    };
    static const ta_scxml_state_spec long_roots[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("only"), .transitions = long_transitions },
        END,
    };
    static const ta_scxml_spec long_chart = { .name = STR("long"), .states = long_roots };

    /* The masks ask for every byte; telemetryValueBytes keeps 100. */
    ta_param_value cut[] = { { TA_PARAM_TELEMETRY_VALUE_BYTES, 100 } };
    ta_trace_masks assigns = { .kinds = TA_TRACE_BIT(TA_TRACE_ASSIGN), .value_bytes = UINT32_MAX };
    run r;
    start(&r, cut, 1, &long_chart, &assigns);
    settle(&r);
    asked_trace_bytes = 0;
    deliver(&r, "go", (ta_value){ 0 });
    settle(&r);
    read_all(r.system);
    CHECK(asked_trace_bytes == 100);
    const kept *assign = find(TA_TRACE_ASSIGN, 0);
    CHECK(assign != NULL && assign->has_value && assign->value_len == 100 && assign->value_cut && assign->truncated);
    finish(&r);

    /* telemetryValueBytes 0: the record says it had a value, and none is rendered. */
    ta_param_value off[] = { { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    start(&r, off, 1, &long_chart, &assigns);
    settle(&r);
    value_texts = 0;
    deliver(&r, "go", text_value("payload"));
    settle(&r);
    read_all(r.system);
    CHECK(value_texts == 0);
    assign = find(TA_TRACE_ASSIGN, 0);
    CHECK(assign != NULL && assign->has_value && assign->value_len == 0 && !assign->value_cut);
    finish(&r);

    /* The masks ask for fewer bytes than telemetryValueBytes: they win. */
    ta_trace_masks few = { .kinds = TA_TRACE_BIT(TA_TRACE_ASSIGN), .value_bytes = 10 };
    start(&r, NULL, 0, &long_chart, &few);
    settle(&r);
    asked_trace_bytes = 0;
    deliver(&r, "go", (ta_value){ 0 });
    settle(&r);
    read_all(r.system);
    CHECK(asked_trace_bytes == 10);
    assign = find(TA_TRACE_ASSIGN, 0);
    CHECK(assign != NULL && assign->value_len == 10 && assign->value_cut && assign->truncated);
    finish(&r);

    /* The masks ask for no value: no data model is called, and the record
     * still says it had one. */
    ta_trace_masks none = { .kinds = TA_TRACE_BIT(TA_TRACE_ASSIGN) | TA_TRACE_BIT(TA_TRACE_MICROSTEP) };
    start(&r, NULL, 0, &long_chart, &none);
    settle(&r);
    value_texts = 0;
    deliver(&r, "go", text_value("payload"));
    settle(&r);
    read_all(r.system);
    CHECK(value_texts == 0);
    assign = find(TA_TRACE_ASSIGN, 0);
    CHECK(assign != NULL && assign->has_value && assign->value_len == 0 && !assign->value_cut);
    const kept *micro = NULL;
    for (uint32_t from = 0; (micro = find(TA_TRACE_MICROSTEP, from)) != NULL; from = (uint32_t)(micro - records) + 1u) {
        if (micro->has_event) {
            break;
        }
    }
    CHECK(micro != NULL && strcmp(micro->event_name, "go") == 0 && micro->event_data[0] == '\0');
    finish(&r);

    /* A failure's reason is rendered whatever value_bytes says. */
    ta_trace_masks reasons = { .kinds = TA_TRACE_BIT(TA_TRACE_ACTION_FAILED) | TA_TRACE_BIT(TA_TRACE_RAISED) |
                               TA_TRACE_BIT(TA_TRACE_ASSIGN) };
    start(&r, NULL, 0, &chart, &reasons);
    settle(&r);
    deliver(&r, "go", text_value("payload"));
    settle(&r);
    read_all(r.system);
    assign = find(TA_TRACE_ASSIGN, 0);
    CHECK(assign != NULL && assign->has_value && assign->value_len == 0);
    const kept *failed = find(TA_TRACE_ACTION_FAILED, 0);
    CHECK(failed != NULL && strcmp(failed->fault_data, "thrown") == 0);
    const kept *error = NULL;
    for (uint32_t from = 0; (error = find(TA_TRACE_RAISED, from)) != NULL; from = (uint32_t)(error - records) + 1u) {
        if (strcmp(error->event_name, "error.execution") == 0) {
            break;
        }
    }
    CHECK(error != NULL && strcmp(error->event_data, "thrown") == 0);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 9: each value's text is copied when it is rendered
 */

static void test_one_buffer(void) {
    run r;
    start(&r, NULL, 0, &chart, &every);
    settle(&r);
    read_all(r.system);
    record_count = 0;
    /* A record with an event, a value and a fault, each with data. */
    ta_actor *actor = ta_actor_lookup(r.system, r.actor);
    CHECK(actor != NULL);
    if (actor == NULL) {
        finish(&r);
        return;
    }
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE, .data = text_value("first") };
    ta_trace_entry entry = ta_trace_entry_blank(TA_TRACE_ACTION_FAILED);
    entry.key = (ta_step_key){ .session = actor->session_id, .family = actor->definition->family };
    entry.actor = actor;
    entry.definition = actor->definition;
    entry.event = &event;
    entry.has_value = true;
    entry.value = text_value("second");
    entry.has_fault = true;
    entry.fault = (ta_fault){ .code = 8, .message = STR("boom"), .data = text_value("third") };
    one_buffer = true;
    value_texts = 0;
    ta_trace_write(r.system, &entry);
    one_buffer = false;
    CHECK(value_texts == 3);
    read_all(r.system);
    CHECK(record_count == 1);
    if (record_count == 1) {
        CHECK(strcmp(records[0].event_data, "first") == 0);
        CHECK(strcmp(records[0].value, "second") == 0);
        CHECK(strcmp(records[0].fault_data, "third") == 0);
        CHECK(!records[0].truncated);
    }
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 5: a record fits maxRecordBytes
 */

static void test_lists_cut(void) {
    make_wide_chart();
    ta_param_value small[] = { { TA_PARAM_MAX_RECORD_BYTES, 512 }, { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    ta_trace_masks microsteps = { .kinds = TA_TRACE_BIT(TA_TRACE_MICROSTEP) };
    run r;
    start(&r, small, 2, &wide_chart, &microsteps);
    settle(&r);
    ta_tel_status status;
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK && status.used <= 512 && status.used != 0 &&
          status.dropped == 0);
    read_all(r.system);
    CHECK(record_count == 1);
    if (record_count == 1) {
        const kept *micro = &records[0];
        /* The bootstrap enters p and its 24 regions; few of them fit. */
        CHECK(micro->lists_cut && micro->truncated);
        CHECK(micro->entered_count > 0 && micro->entered_count < REGIONS + 1u);
        CHECK(micro->name_count == micro->entered_count);
        CHECK(strcmp(micro->names[0], "p") == 0);
        if (micro->entered_count > 1) {
            CHECK(strcmp(micro->names[1], "region-with-a-long-name-00") == 0);
        }
    }
    finish(&r);

    /* The same with room: nothing is cut. */
    start(&r, NULL, 0, &wide_chart, &microsteps);
    settle(&r);
    read_all(r.system);
    CHECK(record_count == 1 && !records[0].lists_cut && !records[0].truncated &&
          records[0].entered_count == REGIONS + 1u);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 6: drops
 */

static void test_drops(void) {
    ta_param_value tight[] = { { TA_PARAM_MAX_RECORD_BYTES, 1024 }, { TA_PARAM_TEL_BYTES, 1024 },
                               { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    run r;
    start(&r, tight, 3, &chart, &every);
    settle(&r);
    deliver(&r, "go", (ta_value){ 0 });
    settle(&r); /* nothing read: most records are dropped */
    ta_tel_status status;
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK && status.dropped > 0);
    uint64_t dropped_before = status.dropped;

    uint64_t expected = 1;
    uint64_t reported = 0;
    uint32_t reports = 0;
    bool report_first = false;
    for (int pass = 0; pass < 2; pass += 1) {
        ta_tel_record record;
        while (ta_system_tel_peek(r.system, &record) == TA_OK) {
            if (record.kind == TA_TEL_DROPPED) {
                /* The gap begins where the report says, and ends at the report. */
                CHECK(record.first_dropped == expected);
                CHECK(record.seq == record.first_dropped + record.dropped);
                reported += record.dropped;
                reports += 1u;
            } else {
                CHECK(record.seq == expected);
            }
            expected = record.seq + 1u;
            CHECK(ta_system_tel_release(r.system) == TA_OK);
        }
        if (pass == 0) {
            /* The next record is preceded by the report of what was lost. */
            CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
            CHECK(ta_system_tel_peek(r.system, &record) == TA_OK && record.kind == TA_TEL_DROPPED);
            report_first = record.kind == TA_TEL_DROPPED;
        }
    }
    CHECK(report_first && reports >= 1u);
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK);
    CHECK(reported == status.dropped && status.dropped >= dropped_before && status.next_seq == expected);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 7: the clock
 */

static void test_clock(void) {
    run r;
    ta_trace_masks begins = { .kinds = TA_TRACE_BIT(TA_TRACE_MACROSTEP_BEGIN) };
    start(&r, NULL, 0, &chart, NULL);
    CHECK(ta_system_set_wall_clock(r.system, (ta_wall_clock){ .v = &(ta_wall_clock_vtable){ 0 } }) == TA_ERR_INVALID);
    CHECK(ta_system_set_wall_clock(NULL, (ta_wall_clock){ .v = &clock_vtable }) == TA_ERR_INVALID);
    CHECK(ta_system_set_wall_clock(r.system, (ta_wall_clock){ .v = &clock_vtable }) == TA_OK);
    clock_calls = 0;
    settle(&r); /* nothing traced: no call */
    CHECK(clock_calls == 0);
    CHECK(ta_system_set_trace_masks(r.system, &begins) == TA_OK);
    deliver(&r, "go", (ta_value){ 0 });
    settle(&r);
    read_all(r.system);
    CHECK(clock_calls == (uint64_t)record_count && record_count == 1);
    CHECK(record_count == 1 && records[0].wall_ns == 1000u);
    /* Uninstalled: wall_ns 0, and no call. */
    CHECK(ta_system_set_wall_clock(r.system, (ta_wall_clock){ 0 }) == TA_OK);
    CHECK(ta_system_set_trace_masks(r.system, &every) == TA_OK);
    record_count = 0;
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    read_all(r.system);
    CHECK(clock_calls == 1 && record_count >= 1 && records[0].wall_ns == 0);
    finish(&r);

    /* A dropped record costs no call. */
    ta_param_value tight[] = { { TA_PARAM_MAX_RECORD_BYTES, 1024 }, { TA_PARAM_TEL_BYTES, 1024 },
                               { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    start(&r, tight, 3, &chart, &every);
    CHECK(ta_system_set_wall_clock(r.system, (ta_wall_clock){ .v = &clock_vtable }) == TA_OK);
    read_all(r.system); /* the records written before the clock was installed */
    clock_calls = 0;
    settle(&r);
    deliver(&r, "go", (ta_value){ 0 });
    settle(&r);
    ta_tel_status status;
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK && status.dropped > 0);
    long traces = tel_drain(r.system, keep, NULL);
    CHECK(clock_calls == (uint64_t)traces);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 8: reading
 */

static void test_reading(void) {
    run r;
    start(&r, NULL, 0, &chart, &every);
    ta_tel_record first;
    ta_tel_record again;
    CHECK(ta_system_tel_peek(r.system, &first) == TA_OK);
    CHECK(ta_system_tel_peek(r.system, &again) == TA_OK && again.seq == first.seq && again.kind == first.kind);
    CHECK(ta_system_tel_peek(NULL, &first) == TA_ERR_INVALID && ta_system_tel_peek(r.system, NULL) == TA_ERR_INVALID);
    CHECK(ta_system_tel_release(NULL) == TA_ERR_INVALID);
    CHECK(ta_system_tel_status(r.system, NULL) == TA_ERR_INVALID);
    CHECK(ta_system_tel_release(r.system) == TA_OK);
    CHECK(ta_system_tel_peek(r.system, &again) == TA_OK && again.seq == first.seq + 1u);
    read_all(r.system);
    CHECK(ta_system_tel_peek(r.system, &again) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_tel_release(r.system) == TA_ERR_NOT_FOUND);
    ta_tel_status status;
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK && status.used == 0);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 10: texts read in place
 */

static void test_texts_in_place(void) {
    run r;
    start(&r, NULL, 0, &chart, &every);
    settle(&r);
    ta_tel_record record;
    bool seen = false;
    while (ta_system_tel_peek(r.system, &record) == TA_OK) {
        if (record.kind == TA_TEL_TRACE && record.trace.kind == TA_TRACE_SPAWNED) {
            const ta_trace_record *trace = &record.trace;
            ta_str family = ta_trace_record_text(trace, TA_TRACE_TEXT_FAMILY);
            CHECK(text_is(family, "telchart"));
            CHECK((const uint8_t *)family.ptr > trace->bytes.ptr &&
                  (const uint8_t *)family.ptr + family.len <= trace->bytes.ptr + trace->bytes.len);
            CHECK(ta_trace_record_text(trace, TA_TRACE_TEXT_SESSION_ID).len != 0);
            CHECK(ta_trace_record_text(trace, (ta_trace_text)(TA_TRACE_TEXT_FAULT_DATA + 1)).len == 0);
            CHECK(ta_trace_record_text(trace, (ta_trace_text)UINT32_MAX).len == 0);
            CHECK(ta_trace_record_text(NULL, TA_TRACE_TEXT_FAMILY).len == 0);
            ta_trace_record bare = *trace;
            bare.bytes = (ta_bytes){ 0 };
            CHECK(ta_trace_record_text(&bare, TA_TRACE_TEXT_FAMILY).len == 0);
            seen = true;
        }
        CHECK(ta_system_tel_release(r.system) == TA_OK);
    }
    CHECK(seen);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Claim 11: kept labels name their own definition's
 *
 *   a --go--> b     assign x = v
 *
 * Its transition and action have the IDs of the chart's idle --go--> busy
 * and its <assign>, with other labels.
 */

static const ta_scxml_action_spec other_actions[] = {
    { .kind = TA_SCXML_ACTION_ASSIGN,
      .as.assign = { .location = EXPR("x"), .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("v") } } },
    END,
};
static const ta_str to_b[] = { STR("b"), {0} };
static const ta_scxml_transition_spec a_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_b, .actions = other_actions },
    END,
};
static const ta_scxml_state_spec other_roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = a_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b") },
    END,
};
static const ta_scxml_spec other_chart = { .name = STR("other"), .states = other_roots, .data = data };

/* The ASSIGN record read since the start, from `from` on, and its index. */
static const kept *assign_from(uint32_t from) {
    return find(TA_TRACE_ASSIGN, from);
}

static void test_kept_labels(void) {
    run first;
    start(&first, NULL, 0, &chart, &every);
    settle(&first);
    run second = { .system = first.system, .definition = define(first.system, &other_chart) };
    CHECK(ta_actor_spawn(second.system, &(ta_actor_spawn_params){ .definition = second.definition,
                                                                  .out = &second.actor }) == TA_OK);
    settle(&second);
    read_all(first.system);

    for (uint32_t turn = 0; turn < 2u; turn += 1u) {
        record_count = 0;
        run *r = turn == 0 ? &first : &second;
        deliver(r, "go", text_value("payload"));
        settle(r);
        read_all(first.system);
        const kept *assign = assign_from(0);
        CHECK(assign != NULL);
        if (assign != NULL) {
            CHECK(strcmp(assign->position_transition,
                         turn == 0 ? "idle \xe2\x86\x92 busy on go [guarded]" : "a \xe2\x86\x92 b on go") == 0);
            CHECK(strcmp(assign->position_action, turn == 0 ? "<assign> in idle \xe2\x86\x92 busy on go [guarded]"
                                                            : "<assign> in a \xe2\x86\x92 b on go") == 0);
        }
    }
    CHECK(first.system->trace.transition_label.key.definition == second.definition);

    /* Freed, its kept labels and facts go; the next one in its slot names its own. */
    CHECK(ta_actor_destroy(second.system, second.actor) == TA_OK);
    CHECK(ta_system_release_definition(second.system, second.definition) == TA_OK);
    CHECK(ta_system_definition(second.system, second.definition) == NULL);
    const ta_tracer *tracer = &first.system->trace;
    CHECK(tracer->transition_label.key.definition == 0 && tracer->action_label.key.definition == 0);
    CHECK(tracer->facts.definition != second.definition);
    read_all(first.system);
    record_count = 0;
    run third = { .system = first.system, .definition = define(first.system, &other_chart) };
    CHECK(ta_handle_slot(third.definition) == ta_handle_slot(second.definition));
    CHECK(ta_actor_spawn(third.system, &(ta_actor_spawn_params){ .definition = third.definition,
                                                                 .out = &third.actor }) == TA_OK);
    settle(&third);
    deliver(&third, "go", text_value("payload"));
    settle(&third);
    read_all(first.system);
    const kept *assign = assign_from(0);
    CHECK(assign != NULL && strcmp(assign->position_action, "<assign> in a \xe2\x86\x92 b on go") == 0);
    const kept *begin = find(TA_TRACE_MACROSTEP_BEGIN, 0);
    CHECK(begin != NULL && strcmp(begin->family, "other") == 0 && strcmp(begin->document_name, "other") == 0);
    finish(&first);
}

int main(void) {
    test_parameters();
    test_no_ring();
    test_self_contained();
    test_values();
    test_lists_cut();
    test_drops();
    test_clock();
    test_reading();
    test_one_buffer();
    test_texts_in_place();
    test_kept_labels();
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "tel_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("tel_test: ok");
    return 0;
}
