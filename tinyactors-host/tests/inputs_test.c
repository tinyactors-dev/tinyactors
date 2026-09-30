/*
 * The kernel records the input log at its own call sites (kernel ADR 0002):
 * a named actor of a logged family writes SPAWNED, each message ACCEPTED
 * once with its content and DELIVERED by identity, and FINISHED, numbered
 * from 1 in its life; a message whose data has no durable form is refused
 * before core sees it and leaves no record; and an allocation that fails
 * anywhere on the way leaves every record whole, no delivery without its
 * acceptance, no record numbers skipped, and nothing leaked.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/inputs.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "tinyactors_host/wal.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

/* ------------------------------------------------------------------------
 * The platform's allocator, which fails its `fail_at`th allocation once
 * (counting from 1; 0 fails none).
 */

static uint64_t allocations;
static uint64_t fail_at;
static bool failed;

static void *failing_alloc(void *self, size_t size, size_t alignment) {
    allocations += 1u;
    if (fail_at != 0 && allocations == fail_at) {
        failed = true;
        return NULL;
    }
    return check_platform_alloc(self, size, alignment);
}

static const ta_allocator_vtable failing_vtable = { .alloc = failing_alloc, .free = check_platform_free };

static tah_platform failing_platform(void) {
    tah_platform platform = check_platform();
    platform.allocator = (ta_allocator){ .self = NULL, .v = &failing_vtable };
    return platform;
}

/* ------------------------------------------------------------------------
 * Values as text, and a codec that writes them and refuses one starting '!'.
 */

typedef struct {
    uint32_t refs;
    size_t length;
    char bytes[];
} text_box;

static void text_retain(void *self) {
    ((text_box *)self)->refs += 1u;
}
static void text_release(void *self) {
    text_box *box = self;
    if (--box->refs == 0) {
        free(box);
    }
}
static const ta_value_vtable text_vtable = { .retain = text_retain, .release = text_release };

static ta_value text_value(const char *bytes) {
    size_t length = strlen(bytes);
    text_box *box = malloc(sizeof *box + length);
    CHECK(box != NULL);
    box->refs = 1;
    box->length = length;
    memcpy(box->bytes, bytes, length);
    return (ta_value){ .self = box, .v = &text_vtable };
}

static ta_op_status text_write(void *self, const ta_datamodel_value_write_params *params) {
    (void)self;
    const text_box *box = params->value->self;
    if (box->length != 0 && box->bytes[0] == '!') {
        return TA_OP_ERROR;
    }
    return params->sink.v->write(params->sink.self, (ta_bytes){ .ptr = (const uint8_t *)box->bytes, .len = (uint32_t)box->length });
}

static ta_datamodel_vtable codec_vtable = { .value_write = text_write };

static void no_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
}

/* ------------------------------------------------------------------------
 * A system logging the family "ping": idle --ping--> idle, idle --stop--> end.
 */

static tah_system *make_system(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = failing_platform() }, &system) == TA_OK);
    system->wake = no_wake;
    tah_inputs_set_datamodel(&system->inputs, (ta_datamodel){ .self = NULL, .v = &codec_vtable });
    static const ta_str families[] = { { "ping", 4 } };
    CHECK(tah_inputs_configure(system, families, 1) == TA_OK);
    return system;
}

static void finish_system(tah_system *system) {
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

static ta_definition_id define_ping(tah_system *system) {
    static ta_str to_idle[] = { { "idle", 4 }, { 0 } };
    static ta_str to_end[] = { { "end", 3 }, { 0 } };
    static ta_scxml_transition_spec on_idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "ping", 4 }, .targets = to_idle },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "stop", 4 }, .targets = to_end },
        { 0 },
    };
    static ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "idle", 4 }, .transitions = on_idle },
        { .kind = TA_SCXML_STATE_FINAL, .id = { "end", 3 } },
        { 0 },
    };
    ta_scxml_spec spec = { .name = S("ping"), .states = states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = &spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

/* Spawns ping/p1; answers the spawn's result. */
static ta_result spawn(tah_system *system, ta_definition_id definition, ta_actor_id *actor) {
    ta_actor_address address = { .family = { .name = S("ping") }, .name = S("p1") };
    ta_result result = ta_actor_spawn(system->core, &(ta_actor_spawn_params){
                                                        .definition = definition, .address = &address, .out = actor });
    return result == TA_OK ? tah_hook_actor_spawned(system, *actor) : result;
}

static ta_result post(tah_system *system, ta_actor_id target, const char *event, const char *data) {
    ta_event message = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_result result = tah_event_name_intern(system->core, (ta_str){ .ptr = event, .len = (uint32_t)strlen(event) },
                                             &message.name);
    if (result != TA_OK) {
        return result;
    }
    ta_value value = data != NULL ? text_value(data) : (ta_value){ 0 };
    message.data = value;
    result = tah_system_post(system, &(tah_message){ .target = target, .event = &message });
    if (result != TA_OK) {
        ta_value_release(&value); /* on TA_OK the system owns the event's references */
    }
    return result;
}

/* The log's bytes so far, taken as a binding takes a batch. */
static uint8_t log_bytes[4096];
static uint32_t log_length;

static void take(tah_system *system) {
    uint32_t wakes = 0;
    (void)tah_inputs_wakes(system, &wakes);
    tah_inputs *inputs = &system->inputs;
    for (uint32_t index = 0; index < tah_inputs_entry_count(inputs); index += 1u) {
        tah_input_entry entry = tah_inputs_entry_at(inputs, index);
        CHECK(log_length + entry.length <= sizeof log_bytes);
        memcpy(log_bytes + log_length, entry.bytes, entry.length);
        log_length += (uint32_t)entry.length;
    }
    tah_inputs_release(inputs);
}

static void run(tah_system *system) {
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        tah_system_turn(system);
        take(system);
        tah_system_end_turn(system);
    }
    take(system);
}

/* The log's records, read back. */
enum { MAX_RECORDS = 32 };
static tah_wal_view records[MAX_RECORDS];
static uint32_t record_count;

static void read_log(void) {
    record_count = 0;
    for (uint32_t at = 0; at < log_length;) {
        CHECK(record_count < MAX_RECORDS);
        tah_wal_view *view = &records[record_count];
        CHECK(tah_wal_read(log_bytes + at, log_length - at, view) == TA_OK);
        CHECK(view->span != 0);
        at += (uint32_t)view->span;
        record_count += 1u;
    }
}

static bool text_is(const tah_wal_view *view, uint32_t field, const char *expected) {
    ta_str text = { 0 };
    return tah_wal_text(view, field, &text) && text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

/* ------------------------------------------------------------------------ */

/* The records of a life, golden: kinds, numbers, identities, content once. */
static void records_a_life(void) {
    log_length = 0;
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = 0;
    CHECK(spawn(system, chart, &actor) == TA_OK);
    run(system);
    CHECK(post(system, actor, "ping", "{\"by\":1}") == TA_OK);
    run(system);
    uint32_t before_stop = log_length;
    CHECK(post(system, actor, "stop", NULL) == TA_OK);
    run(system);
    read_log();

    static const tah_wal_kind expected[] = {
        TAH_WAL_SPAWNED, TAH_WAL_ACCEPTED, TAH_WAL_DELIVERED, TAH_WAL_ACCEPTED, TAH_WAL_DELIVERED, TAH_WAL_FINISHED,
    };
    CHECK(record_count == sizeof expected / sizeof expected[0]);
    for (uint32_t index = 0; index < record_count; index += 1u) {
        CHECK(records[index].version == TAH_WAL_VERSION);
        CHECK(records[index].kind == expected[index]);
        CHECK(records[index].seq == index + 1u); /* numbered from 1 in its life */
    }
    CHECK(text_is(&records[0], 0, "ping") && text_is(&records[0], 1, "p1"));
    /* ACCEPTED: identity (the incarnation's hash above, a counter below), content once. */
    uint64_t ping = tah_wal_number(&records[1], 0);
    CHECK(tah_inputs_seq_is_ours(&system->inputs, ping));
    CHECK(tah_wal_number(&records[1], 1) == TA_EVENT_EXTERNAL);
    CHECK(text_is(&records[1], 2, "ping"));
    CHECK(tah_wal_number(&records[1], 7) == TAH_REPORT_NONE);
    CHECK(records[1].value_count == 1);
    CHECK(tah_wal_number(&records[2], 0) == ping);
    CHECK(records[2].value_count == 0 && records[2].field_count == 1);
    uint64_t stop = tah_wal_number(&records[3], 0);
    CHECK(stop == ping + 1u);
    CHECK(records[3].value_count == 0); /* no data: no stream */
    CHECK(tah_wal_number(&records[4], 0) == stop);
    CHECK(text_is(&records[5], 0, "end"));

    /* Bytes per message (kernel ADR 0002 section 2): ACCEPTED with the name
     * "ping" and 8 bytes of data is 32 + 10 * 8 fields + 8 text + 16 + 8
     * value = 144, and DELIVERED 32 + 8 = 40. Version 1 wrote 240. */
    CHECK(records[1].span == 144u);
    CHECK(records[2].span == 40u);
    CHECK(records[1].span + records[2].span == 184u);
    CHECK(log_length - before_stop == records[3].span + records[4].span + records[5].span);
    finish_system(system);
}

/* Data with no durable form is refused before core sees it: no record, no mail. */
static void refuses_what_it_cannot_record(void) {
    log_length = 0;
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = 0;
    CHECK(spawn(system, chart, &actor) == TA_OK);
    run(system);
    uint32_t logged = log_length;
    CHECK(post(system, actor, "ping", "!no durable form") == TA_ERR_UNSUPPORTED);
    run(system);
    CHECK(log_length == logged);
    /* The next message takes the next record number: the refusal left none behind. */
    CHECK(post(system, actor, "ping", "{}") == TA_OK);
    run(system);
    read_log();
    CHECK(record_count == 3);
    CHECK(records[1].kind == TAH_WAL_ACCEPTED && records[1].seq == 2u);
    CHECK(records[2].kind == TAH_WAL_DELIVERED && records[2].seq == 3u);
    finish_system(system);
}

/*
 * Every allocation from the spawn to the end of the life, failed once in
 * turn: whatever the failure refused, the log it left is whole records,
 * numbered without gaps, with each delivery after its acceptance.
 */
static void survives_every_failed_allocation(void) {
    uint32_t sweeps = 0;
    for (uint64_t at = 1;; at += 1u) {
        log_length = 0;
        fail_at = 0;
        tah_system *system = make_system();
        ta_definition_id chart = define_ping(system);
        allocations = 0;
        failed = false;
        fail_at = at;
        ta_actor_id actor = 0;
        if (spawn(system, chart, &actor) == TA_OK) {
            run(system);
            for (int attempt = 0; attempt < 2 && post(system, actor, "ping", "{\"by\":1}") != TA_OK; attempt += 1) {
            }
            run(system);
            for (int attempt = 0; attempt < 2 && post(system, actor, "stop", NULL) != TA_OK; attempt += 1) {
            }
            run(system);
        }
        bool done = !failed;
        fail_at = 0;
        read_log();
        uint64_t seq = 0;
        for (uint32_t index = 0; index < record_count; index += 1u) {
            CHECK(records[index].seq == ++seq);
            if (records[index].kind == TAH_WAL_DELIVERED) {
                uint64_t id = tah_wal_number(&records[index], 0);
                bool accepted = false;
                for (uint32_t before = 0; before < index; before += 1u) {
                    accepted |= records[before].kind == TAH_WAL_ACCEPTED && tah_wal_number(&records[before], 0) == id;
                }
                CHECK(accepted);
            }
        }
        CHECK(record_count == 0 || records[0].kind == TAH_WAL_SPAWNED);
        finish_system(system);
        sweeps += 1u;
        if (done) {
            CHECK(record_count == 6); /* nothing failed: the whole life */
            break;
        }
    }
    CHECK(sweeps > 10);
}

int main(void) {
    records_a_life();
    refuses_what_it_cannot_record();
    survives_every_failed_allocation();
    puts("inputs test passed");
    return 0;
}
