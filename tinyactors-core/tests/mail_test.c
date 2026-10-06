/*
 * Mail between resident sessions (src/run.h, ADR 0099): a <send> to a
 * session resident in this system takes an envelope from the pool, waits on
 * the sending macrostep's outgoing list until that macrostep ends, and then
 * joins the run queue; a delayed send that falls due joins the run queue at
 * ta_system_set_time(). Neither asks the host for anything.
 *
 * The test includes the amalgamation so it reaches the private units; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which counts the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. mail waits for its sender's macrostep: while that macrostep is paused
 *    after its first <send>, the envelope is outgoing, parked with the
 *    sender's taken input, and the receiver has nothing to take; once the
 *    macrostep ends its mail joins the run queue in send order, the
 *    receiver takes it, and no load request is made;
 * 2. order holds per sender and receiver across macrosteps: a receiver
 *    takes one sender's mail in the order it was sent, over several of
 *    the sender's macrosteps, and mail from senders whose macrosteps ended
 *    in between in arrival order;
 * 3. a fanout that finds no free envelope fails once (S02): the first
 *    <send> without one raises error.execution.capacity.pool, the sends
 *    before it are delivered when the macrostep ends, the ones after it
 *    in the block do not run, and one error is counted; inside a
 *    macrostep mail takes only an envelope that is free already, so with
 *    none free it answers TA_ERR_FULL for the pool and allocates nothing,
 *    though the bound has room, while outside one it refills first;
 * 4. a fanout that finds no room in the name area fails the same way, as
 *    error.execution.capacity.names;
 * 5. an abandoned macrostep gives its outgoing list back unseen, and so
 *    does destroying a session whose macrostep a fault stopped, which
 *    keeps its mail parked until then, and does not unload, since an image
 *    cannot carry that mail (TA_ERR_BUSY): the pool and the name area are
 *    back where they were, and the receiver takes nothing;
 * 6. a traced envelope carries its sender's trace parent (the trace
 *    context and the sending step) as the last field of its text, and its
 *    first field the step's family, so the step key reads back whole; an
 *    untraced one carries no trace parent at all;
 * 7. a delayed send to a resident session falls due into the pool at
 *    ta_system_set_time(), which refills first: it joins the run
 *    queue at once as a TIMER input, the fired buffer reports it with its
 *    sender, due time and trace parent; a pending send holds its envelope,
 *    so it arrives when the pool is at its bound;
 * 8. a TAKEN outcome carries the event of mail and of a due send,
 *    retained for the host, and none for an input the host admitted;
 * 9. mail to a session that is not resident does not join the run queue:
 *    it waits for its destination, which asks the host for a load once
 *    (waiting_test.c proves the rest).
 */
#include "../include/tinyactors.c"
#include "test_drive.h"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

/* The names of the TAKEN events of one ta_run() call (ta_run_params.names). */
static char taken_names[TA_RUN_NAMES_MIN_BYTES];

static int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define END { 0 }

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * A directory of a few named actors
 */

enum { NAMES = 4 };

typedef struct {
    const ta_address *addresses[NAMES];
    ta_actor_directory_entry entries[NAMES];
    uint32_t count;
} small_directory;

static small_directory directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    small_directory *dir = self;
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            return TA_ERR_STALE;
        }
    }
    if (dir->count == NAMES) {
        return TA_ERR_NOMEM;
    }
    dir->addresses[dir->count] = params->address;
    dir->entries[dir->count] = params->entry;
    dir->count += 1u;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    small_directory *dir = self;
    *params->out = (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            *params->out = dir->entries[index];
        }
    }
    return TA_OK;
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    small_directory *dir = self;
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            dir->entries[index] = params->replacement;
            return TA_OK;
        }
    }
    return TA_ERR_STALE;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    small_directory *dir = self;
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            dir->entries[index] = (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
            return TA_OK;
        }
    }
    return TA_ERR_STALE;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/* The load requests core owes the host, read and left unanswered. */
static uint32_t load_requests(ta_system *system) {
    uint32_t count = 0;
    ta_load_request request;
    while (ta_system_load_needed(system, &request) == TA_OK) {
        count += 1u;
    }
    return count;
}

/* ------------------------------------------------------------------------
 * Charts
 */

#define MAIL(event_text, target_text)                                                              \
    { .kind = TA_SCXML_ACTION_SEND,                                                                \
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },           \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) } } }

#define LATER(event_text, target_text, delay_text)                                                 \
    { .kind = TA_SCXML_ACTION_SEND,                                                                \
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },           \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) },  \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }

/* The receiver: r0 --e1--> r1 --e2--> r2 --e3--> r3, so only e1, e2, e3 in
 * order bring it to r3; any other event is ignored. */
static const ta_str to_r1[] = { STR("r1"), END };
static const ta_str to_r2[] = { STR("r2"), END };
static const ta_str to_r3[] = { STR("r3"), END };
static const ta_scxml_state_spec receiver_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r0"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e1"), .targets = to_r1 }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r1"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e2"), .targets = to_r2 }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r2"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e3"), .targets = to_r3 }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r3") },
    END,
};
static const ta_scxml_spec receiver = { .name = STR("receiver"), .states = receiver_states };

/* The sender: `go` mails e1 and e2 to recv/r in one block, `more` mails
 * e3, `other` mails x to recv/r, `later` sends e1 to recv/r in 1s, and
 * `away` mails gone/g, which no session holds. */
static const ta_scxml_action_spec go_mail[] = { MAIL("e1", "#_actor_recv/r"), MAIL("e2", "#_actor_recv/r"), END };
static const ta_scxml_action_spec more_mail[] = { MAIL("e3", "#_actor_recv/r"), END };
static const ta_scxml_action_spec other_mail[] = { MAIL("x", "#_actor_recv/r"), END };
static const ta_scxml_action_spec later_mail[] = { LATER("e1", "#_actor_recv/r", "1s"), END };
static const ta_scxml_action_spec away_mail[] = { MAIL("e1", "#_actor_gone/g"), END };
static const ta_scxml_state_spec sender_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("go"), .actions = go_mail },
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("more"), .actions = more_mail },
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("other"), .actions = other_mail },
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("later"), .actions = later_mail },
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("away"), .actions = away_mail },
          END } },
    END,
};
static const ta_scxml_spec sender = { .name = STR("sender"), .states = sender_states };

/* The fanout: `go` mails ten x to recv/r in one block, then takes `sent`;
 * a capacity error takes it to `pool` or `names` by its last segment. */
static const ta_scxml_action_spec ten_mails[] = {
    MAIL("x", "#_actor_recv/r"), MAIL("x", "#_actor_recv/r"), MAIL("x", "#_actor_recv/r"),
    MAIL("x", "#_actor_recv/r"), MAIL("x", "#_actor_recv/r"), MAIL("x", "#_actor_recv/r"),
    MAIL("x", "#_actor_recv/r"), MAIL("x", "#_actor_recv/r"), MAIL("x", "#_actor_recv/r"),
    MAIL("x", "#_actor_recv/r"), END,
};
static const ta_str to_sent[] = { STR("sent"), END };
static const ta_str to_pool[] = { STR("pool"), END };
static const ta_str to_names[] = { STR("names"), END };
static const ta_str to_other[] = { STR("other"), END };
static const ta_scxml_state_spec fanout_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_sent, .actions = ten_mails },
          END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("sent"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution.capacity.pool"), .targets = to_pool },
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution.capacity.names"),
            .targets = to_names },
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error"), .targets = to_other },
          END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("pool") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("names") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("other") },
    END,
};
static const ta_scxml_spec fanout = { .name = STR("fanout"), .states = fanout_states };

/* ------------------------------------------------------------------------
 * A system, its actors and its runs
 */

typedef struct {
    ta_system *system;
    ta_actor_id receiver;
    ta_actor_id sender;
    ta_input_outcome outcomes[2];
    uint32_t count;
    ta_run_status status;
} fixture;

static ta_definition_id define(ta_system *system, const ta_scxml_spec *spec, const char *family) {
    ta_definition_identity identity = { .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
                                        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 } };
    ta_scxml_spec named = *spec;
    named.identity = &identity;
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &named,
                                                                             .datamodel = ta_null_datamodel(),
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

/* One ta_run() call of `budget` microsteps that takes one input at most,
 * reporting it TAKEN. */
static ta_result run_limited(fixture *f, ta_actor_id session, uint32_t budget, uint32_t action_budget) {
    for (uint32_t index = 0; index < f->count; index += 1u) {
        ta_event_release(&f->outcomes[index].event);
    }
    f->count = 0;
    return ta_run(f->system, &(ta_run_params){ .budget = budget, .action_budget = action_budget,
                                               .session = session, .report_taken = true, .names = taken_names, .name_capacity = sizeof taken_names,
                                               .outcomes = f->outcomes, .outcome_capacity = 2,
                                               .outcome_count = &f->count, .status = &f->status });
}

static ta_result run_once(fixture *f, ta_actor_id session, uint32_t action_budget) {
    return run_limited(f, session, 1000, action_budget);
}

/* The names of the mail and due sends one session took, in order. */
typedef struct {
    char names[32][16];
    uint32_t count;
} taken_list;

static void note_taken(fixture *f, ta_actor_id actor, taken_list *taken) {
    for (uint32_t index = 0; index < f->count; index += 1u) {
        const ta_input_outcome *outcome = &f->outcomes[index];
        if (outcome->kind != TA_INPUT_TAKEN || !outcome->has_event || outcome->actor != actor ||
            taken->count == 32u) {
            continue;
        }
        char *out = taken->names[taken->count];
        uint32_t needed = 0;
        CHECK(ta_event_name_format(f->system, &(ta_event_name_format_params){
                                                  .name = outcome->event.name, .buffer = out, .capacity = 16,
                                                  .needed = &needed }) == TA_OK);
        taken->count += 1u;
    }
}

/* Runs until core is idle; notes what `actor` took. */
static void run_all(fixture *f, ta_actor_id actor, taken_list *taken) {
    for (uint32_t call = 0; call < 1000u; call += 1u) {
        CHECK(run_once(f, 0, 0) == TA_OK);
        if (taken != NULL) {
            note_taken(f, actor, taken);
        }
        if (f->status == TA_RUN_IDLE) {
            return;
        }
    }
    CHECK(!"core did not become idle");
}

static ta_actor_id spawn_named(fixture *f, ta_definition_id definition, const char *family, const char *name) {
    ta_actor_address address = { .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
                                 .name = { .ptr = name, .len = (uint32_t)strlen(name) } };
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f->system, &(ta_actor_spawn_params){ .definition = definition, .address = &address,
                                                              .out = &actor }) == TA_OK);
    return actor;
}

static fixture open_with(const ta_param_value *params, uint32_t count, const ta_scxml_spec *sending) {
    fixture f = { 0 };
    directory = (small_directory){ 0 };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count },
                           &f.system) == TA_OK);
    CHECK(ta_system_set_actor_directory(f.system, (ta_actor_directory){ .self = &directory, .v = &dir_vtable }) ==
          TA_OK);
    f.receiver = spawn_named(&f, define(f.system, &receiver, "recv"), "recv", "r");
    f.sender = spawn_named(&f, define(f.system, sending, "send"), "send", "s");
    run_all(&f, 0, NULL);
    return f;
}

static void close_fixture(fixture *f) {
    for (uint32_t index = 0; index < f->count; index += 1u) {
        ta_event_release(&f->outcomes[index].event);
    }
    ta_system_destroy(f->system);
    CHECK(heap.live == 0);
}

static ta_event_name_id name_of(fixture *f, const char *text) {
    ta_event_name_id name = 0;
    CHECK(ta_trie_intern_dotted(&f->system->event_names, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) },
                                &name) == TA_OK);
    return name;
}

static ta_result deliver_traced(fixture *f, ta_actor_id actor, const char *text, ta_trace_parent trace) {
    ta_event event = { .name = name_of(f, text), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(f->system, &(ta_actor_deliver_params){ .actor = actor, .name = test_event_text(f->system, &event), .event = &event,
                                                                   .time = ta_system_time(f->system),
                                                                   .trace = trace });
}

static ta_result deliver(fixture *f, ta_actor_id actor, const char *text) {
    return deliver_traced(f, actor, text, (ta_trace_parent){ 0 });
}

static bool in_state(fixture *f, ta_actor_id actor, const char *id) {
    ta_state_id active[4];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(f->system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                             .capacity = 4, .count = &count }) ==
          TA_OK);
    ta_actor_inspection view = { 0 };
    CHECK(ta_actor_inspect(f->system, actor, &view) == TA_OK);
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_state_info info = { 0 };
        CHECK(ta_definition_state(f->system, &(ta_definition_state_params){ .definition = view.definition_id,
                                                                            .state = active[index],
                                                                            .out = &info }) == TA_OK);
        if (info.id.len == strlen(id) && memcmp(info.id.ptr, id, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

/* Envelopes of the pool in `state`. */
static uint32_t envelopes_in(fixture *f, ta_envelope_state state) {
    const ta_envelope_pool *pool = &f->system->run.pool;
    uint32_t count = 0;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        count += envelope != NULL && envelope->state == state ? 1u : 0u;
    }
    return count;
}

static uint64_t execution_errors(fixture *f) {
    ta_counters counters;
    ta_system_counters(f->system, &counters);
    return counters.errors_execution;
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* Claim 1. */
static void test_mail_waits_for_its_senders_macrostep(void) {
    fixture f = open_with(NULL, 0, &sender);
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    /* One unit of executable content: the first <send>, and a pause. */
    CHECK(run_once(&f, f.sender, 1) == TA_OK && f.status == TA_RUN_PAUSED);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) == 1);
    CHECK(f.system->run.pool.run_queue.head == 0u);
    /* It names the receiver, which therefore cannot unload: an image could
     * not take it, and the runtime ID it names would be gone. */
    CHECK(ta_run_has_mail(f.system, ta_actor_lookup(f.system, f.receiver)));
    /* Parked with the sender's input while its macrostep waits. */
    const ta_actor *paused = ta_actor_lookup(f.system, f.sender);
    CHECK(ta_envelope_pool_at(&f.system->run.pool, paused->input)->owner_next != 0u);
    /* The receiver's own call finds nothing. */
    CHECK(run_once(&f, f.receiver, 0) == TA_OK && f.count == 0);
    CHECK(in_state(&f, f.receiver, "r0"));
    /* The macrostep ends: e1 and e2 join the run queue, in send order. */
    CHECK(run_once(&f, f.sender, 0) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) == 0 && envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == 2);
    CHECK(ta_run_has_mail(f.system, ta_actor_lookup(f.system, f.receiver)));
    taken_list taken = { 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 2 && strcmp(taken.names[0], "e1") == 0 && strcmp(taken.names[1], "e2") == 0);
    CHECK(in_state(&f, f.receiver, "r2"));
    CHECK(load_requests(f.system) == 0);
    close_fixture(&f);
}

/* Claim 2. */
static void test_order_per_sender_and_receiver(void) {
    fixture f = open_with(NULL, 0, &sender);
    ta_actor_id second = spawn_named(&f, define(f.system, &sender, "send2"), "send2", "t");
    run_all(&f, 0, NULL);
    /* Admitted: s.go, t.other, s.more. s.go's mail arrives behind t.other,
     * t.other's behind s.more, and s.more's last. */
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    CHECK(deliver(&f, second, "other") == TA_OK);
    CHECK(deliver(&f, f.sender, "more") == TA_OK);
    taken_list taken = { 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 4);
    CHECK(taken.count == 4 && strcmp(taken.names[0], "e1") == 0 && strcmp(taken.names[1], "e2") == 0 &&
          strcmp(taken.names[2], "x") == 0 && strcmp(taken.names[3], "e3") == 0);
    CHECK(in_state(&f, f.receiver, "r3"));
    CHECK(load_requests(f.system) == 0);
    close_fixture(&f);
}

/* Claim 3. */
static void test_a_fanout_fails_once_at_the_first_send_without_an_envelope(void) {
    /* S02: a pool of eight. The fanout's own input holds one envelope. */
    ta_param_value small_pool[] = { { TA_PARAM_POOL_ENVELOPES, 8 } };
    fixture f = open_with(small_pool, 1, &fanout);
    uint64_t errors = execution_errors(&f);
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    CHECK(run_once(&f, f.sender, 0) == TA_OK);
    CHECK(in_state(&f, f.sender, "pool"));
    CHECK(execution_errors(&f) == errors + 1u);
    /* Seven sends fit; the eighth failed; the last two never ran. */
    CHECK(envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == 7);
    taken_list taken = { 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 7);
    CHECK(f.system->run.pool.quota.used == 0);
    close_fixture(&f);

    /* A bound with room: inside a macrostep, mail takes only an envelope
     * that is free already, and allocates nothing; outside one it refills. */
    f = open_with(NULL, 0, &fanout);
    ta_envelope_pool *pool = &f.system->run.pool;
    static uint32_t held[TA_PARAM_DEFAULT_POOL_ENVELOPES];
    uint32_t holding = pool->items.free;
    CHECK(holding <= TA_PARAM_DEFAULT_POOL_ENVELOPES);
    for (uint32_t index = 0; index < holding; index += 1u) {
        CHECK(ta_envelope_pool_take(pool, &held[index]) == TA_OK);
    }
    ta_event event = { .name = name_of(&f, "x"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_run_mail mail = { .target = ta_actor_lookup(f.system, f.receiver), .event = &event,
                         .source = TA_RECORD_INPUT_SOURCE_MAIL };
    ta_record_capacity_resource short_of = (ta_record_capacity_resource)0;
    long allocations = heap.allocations;
    uint32_t pages = pool->items.pages.count;
    f.system->run.running = true;
    CHECK(ta_run_post_mail(f.system, &mail, &short_of) == TA_ERR_FULL);
    f.system->run.running = false;
    CHECK(short_of == TA_RECORD_CAPACITY_RESOURCE_POOL);
    CHECK(heap.allocations == allocations && pool->items.pages.count == pages);
    CHECK(ta_run_post_mail(f.system, &mail, &short_of) == TA_OK);
    CHECK(pool->items.pages.count == pages + 1u && pool->run_queue.head != 0u);
    for (uint32_t index = 0; index < holding; index += 1u) {
        ta_envelope_pool_give(pool, held[index]);
    }
    taken_list taken_after = { 0 };
    run_all(&f, f.receiver, &taken_after);
    CHECK(taken_after.count == 1 && strcmp(taken_after.names[0], "x") == 0);
    close_fixture(&f);
}

/* Claim 4. */
static void test_a_fanout_fails_once_at_the_first_send_without_name_room(void) {
    /* Eight chunks of name area; the fanout's own input holds one. */
    ta_param_value small_names[] = { { TA_PARAM_NAME_AREA_BYTES, 512 },
                                     { TA_PARAM_MAX_RECORD_BYTES, 512 },
                                     { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    fixture f = open_with(small_names, 3, &fanout);
    uint64_t errors = execution_errors(&f);
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    const ta_name_area *names = &f.system->run.names;
    uint32_t per_mail = ta_name_area_field_need(
        names, 0u, (uint32_t)(sizeof(ta_event_name_id) + 1u + sizeof(ta_origin) + sizeof(ta_time) + sizeof(ta_atom)));
    uint32_t fits = (names->quota.capacity - names->quota.used) / per_mail;
    CHECK(fits >= 1u && fits < 10u);
    CHECK(run_once(&f, f.sender, 0) == TA_OK);
    CHECK(in_state(&f, f.sender, "names"));
    CHECK(execution_errors(&f) == errors + 1u);
    CHECK(envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == fits);
    taken_list taken = { 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == fits);
    CHECK(names->quota.used == 0u && f.system->run.pool.quota.used == 0u);
    close_fixture(&f);
}

/* The looper: `go` enters a, which mails x to recv/r on entry and loops
 * through b back to itself until the microstep limit stops it. */
static const ta_str to_a[] = { STR("a"), END };
static const ta_str to_b[] = { STR("b"), END };
static const ta_scxml_action_spec one_mail[] = { MAIL("x", "#_actor_recv/r"), END };
static const ta_scxml_handler_spec mail_on_entry[] = { { .actions = one_mail }, END };
static const ta_scxml_state_spec looper_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_a }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .onentry = mail_on_entry,
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_b }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_a }, END } },
    END,
};
static const ta_scxml_spec looper = { .name = STR("looper"), .states = looper_states };

/* Claim 5. */
static void test_an_abandoned_macrostep_gives_its_mail_back(void) {
    /* Paused between microsteps, after a's entry mailed once. */
    fixture f = open_with(NULL, 0, &looper);
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    CHECK(run_limited(&f, f.sender, 2, 0) == TA_OK && f.status == TA_RUN_PAUSED);
    uint32_t parked = envelopes_in(&f, TA_ENVELOPE_OUTGOING);
    CHECK(parked >= 1u);
    CHECK(f.system->run.pool.quota.used == parked + 1u); /* the input and its mail */
    CHECK(ta_actor_abandon(f.system, f.sender) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) == 0 && f.system->run.pool.quota.used == 0u);
    CHECK(f.system->run.names.quota.used == 0u);
    taken_list taken = { 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 0 && in_state(&f, f.receiver, "r0"));
    close_fixture(&f);

    /* A fault stops the macrostep: its mail waits with it, and goes when
     * the session is destroyed. */
    ta_param_value limit[] = { { TA_PARAM_MICROSTEP_LIMIT, 6 } };
    f = open_with(limit, 1, &looper);
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    CHECK(run_once(&f, f.sender, 0) == TA_OK);
    CHECK(f.count == 2 && f.outcomes[1].kind == TA_INPUT_FAULTED && f.outcomes[1].code == TA_ERR_RUNAWAY);
    parked = envelopes_in(&f, TA_ENVELOPE_OUTGOING);
    CHECK(parked >= 2u);
    CHECK(ta_run_has_mail(f.system, ta_actor_lookup(f.system, f.receiver)));
    CHECK(f.system->run.pool.quota.used == parked + 1u);
    /* An image cannot carry the parked mail: the session does not unload. */
    CHECK(ta_run_has_unsent_mail(f.system, ta_actor_lookup(f.system, f.sender)));
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f.system, &(ta_actor_capture_params){ .actor = f.sender, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(f.system, &(ta_actor_unload_params){ .actor = f.sender, .image = image }) == TA_ERR_BUSY);
    ta_session_image_destroy(image);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) == parked);
    CHECK(ta_actor_destroy(f.system, f.sender) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) == 0 && f.system->run.pool.quota.used == 0u);
    taken = (taken_list){ 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 0);
    close_fixture(&f);
}

/* The receiver's runnable envelopes, in run queue order. */
static uint32_t queued_for(fixture *f, ta_actor_id actor, uint32_t *out, uint32_t capacity) {
    ta_envelope_pool *pool = &f->system->run.pool;
    ta_list_links links = ta_envelope_pool_links(pool);
    uint32_t count = 0;
    for (uint32_t index = ta_list_head(&pool->run_queue); index != 0u; index = ta_list_next(&links, index)) {
        if (ta_envelope_pool_at(pool, index)->target == actor && count < capacity) {
            out[count++] = index;
        }
    }
    return count;
}

/* Claim 6. */
static void test_a_traced_envelope_carries_its_senders_trace_parent(void) {
    fixture f = open_with(NULL, 0, &sender);
    ta_trace_parent request = { .context = { .trace_id_high = 0xaa, .trace_id_low = 0xbb,
                                             .flags = TA_TRACE_FLAG_SAMPLED },
                                .cause = { .session = 0, .macrostep = 42 } };
    CHECK(deliver_traced(&f, f.sender, "go", request) == TA_OK);
    CHECK(run_once(&f, f.sender, 0) == TA_OK);
    const ta_actor *sending = ta_actor_lookup(f.system, f.sender);
    uint32_t mail[4];
    CHECK(queued_for(&f, f.receiver, mail, 4) == 2);
    uint32_t record = (uint32_t)(sizeof(ta_event_name_id) + 1u + sizeof(ta_origin) + sizeof(ta_time) + sizeof(ta_atom));
    for (uint32_t at = 0; at < 2u; at += 1u) {
        const ta_envelope *envelope = ta_envelope_pool_at(&f.system->run.pool, mail[at]);
        CHECK((envelope->flags & TA_ENVELOPE_FLAG_TRACED) != 0u);
        CHECK(envelope->source == TA_RECORD_INPUT_SOURCE_MAIL && envelope->from == sending->session_id);
        CHECK(envelope->text.length == TA_NAME_AREA_PREFIX_BYTES + record + TA_NAME_AREA_TRACE_FIELD_BYTES);
        ta_trace_parent parent = { 0 };
        CHECK(ta_name_area_trace_parent(&f.system->run.names, envelope->text, &parent) == TA_OK);
        CHECK(parent.context.trace_id_high == 0xaa && parent.context.trace_id_low == 0xbb);
        CHECK(parent.context.flags == TA_TRACE_FLAG_SAMPLED);
        /* The sending step: the sender's session, its macrostep, the microstep that sent. */
        CHECK(parent.cause.session == sending->session_id && parent.cause.macrostep == sending->trace_macrostep &&
              parent.cause.microstep == 1u);
        /* The field leaves the cause's family out; the record keeps it. */
        CHECK(ta_run_trace_of(f.system, mail[at]).cause.family == sending->definition->family);
        CHECK(sending->definition->family != 0u);
    }
    run_all(&f, 0, NULL);

    /* Untraced: no trace parent, only the record. */
    CHECK(deliver(&f, f.sender, "more") == TA_OK);
    CHECK(run_once(&f, f.sender, 0) == TA_OK);
    CHECK(queued_for(&f, f.receiver, mail, 4) == 1);
    const ta_envelope *untraced = ta_envelope_pool_at(&f.system->run.pool, mail[0]);
    CHECK((untraced->flags & TA_ENVELOPE_FLAG_TRACED) == 0u);
    CHECK(untraced->text.length == TA_NAME_AREA_PREFIX_BYTES + record);
    close_fixture(&f);
}

/* Claim 7. */
static void test_a_due_send_falls_into_the_pool(void) {
    fixture f = open_with(NULL, 0, &sender);
    ta_trace_parent request = { .context = { .trace_id_high = 0xcc, .trace_id_low = 0xdd,
                                             .flags = TA_TRACE_FLAG_SAMPLED } };
    CHECK(deliver_traced(&f, f.sender, "later", request) == TA_OK);
    run_all(&f, 0, NULL);
    /* Every free envelope taken: the dispatch is a safe point and refills. */
    ta_envelope_pool *pool = &f.system->run.pool;
    static uint32_t held[TA_PARAM_DEFAULT_POOL_ENVELOPES];
    uint32_t holding = pool->items.free;
    CHECK(holding <= TA_PARAM_DEFAULT_POOL_ENVELOPES);
    for (uint32_t index = 0; index < holding; index += 1u) {
        CHECK(ta_envelope_pool_take(pool, &held[index]) == TA_OK);
    }
    uint32_t pages = pool->items.pages.count;
    CHECK(ta_system_set_time(f.system, &(ta_set_time_params){ .now = 1000 }) == TA_OK);
    ta_due_fired fired[4];
    uint32_t dispatched = 0;
    uint32_t fired_count = 0;
    bool more = true;
    CHECK(ta_system_set_time(f.system, &(ta_set_time_params){ .now = ta_system_time(f.system), .budget = 10, .dispatched = &dispatched,
                                                                      .more_due = &more, .fired = fired,
                                                                      .fired_capacity = 4,
                                                                      .fired_count = &fired_count }) == TA_OK);
    CHECK(dispatched == 1 && fired_count == 1 && !more);
    CHECK(pool->items.pages.count == pages + 1u);
    CHECK(fired[0].sender == f.sender && fired[0].due == 1000 && fired[0].delivered);
    CHECK(fired[0].trace.context.trace_id_high == 0xcc && fired[0].trace.context.trace_id_low == 0xdd);
    uint32_t mail[2];
    CHECK(queued_for(&f, f.receiver, mail, 2) == 1);
    CHECK(ta_envelope_pool_at(pool, mail[0])->source == TA_RECORD_INPUT_SOURCE_TIMER);
    for (uint32_t index = 0; index < holding; index += 1u) {
        ta_envelope_pool_give(pool, held[index]);
    }
    taken_list taken = { 0 };
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 1 && strcmp(taken.names[0], "e1") == 0 && in_state(&f, f.receiver, "r1"));
    CHECK(load_requests(f.system) == 0);
    close_fixture(&f);

    /* A pool at its bound: the pending send holds its envelope, which it
     * keeps as it falls due (ADR 0101), so it arrives with none free. */
    ta_param_value small_pool[] = { { TA_PARAM_POOL_ENVELOPES, 8 } };
    f = open_with(small_pool, 1, &sender);
    CHECK(deliver(&f, f.sender, "later") == TA_OK);
    run_all(&f, 0, NULL);
    pool = &f.system->run.pool;
    uint32_t full = 0;
    while (full < 8u && ta_envelope_pool_take(pool, &held[full]) == TA_OK) {
        full += 1u;
    }
    CHECK(full < 8u && ta_quota_available(&pool->quota) == 0);
    CHECK(ta_system_set_time(f.system, &(ta_set_time_params){ .now = 1000 }) == TA_OK);
    CHECK(ta_system_set_time(f.system, &(ta_set_time_params){ .now = ta_system_time(f.system), .budget = 10, .dispatched = &dispatched,
                                                                      .more_due = &more }) == TA_OK);
    CHECK(dispatched == 1 && !more);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f.system, f.sender, &view) == TA_OK && view.delayed_send_count == 0);
    for (uint32_t index = 0; index < full; index += 1u) {
        ta_envelope_pool_give(pool, held[index]);
    }
    run_all(&f, f.receiver, &taken);
    CHECK(taken.count == 2 && in_state(&f, f.receiver, "r1"));
    close_fixture(&f);
}

/* Claim 8. */
static void test_taken_outcomes_carry_mail_events(void) {
    fixture f = open_with(NULL, 0, &sender);
    CHECK(deliver(&f, f.sender, "go") == TA_OK);
    CHECK(run_once(&f, 0, 0) == TA_OK);
    CHECK(f.count == 2 && f.outcomes[0].kind == TA_INPUT_TAKEN && f.outcomes[0].actor == f.sender);
    CHECK(!f.outcomes[0].has_event && f.outcomes[0].event.name == 0u); /* the host's own input */
    CHECK(run_once(&f, 0, 0) == TA_OK);
    CHECK(f.count == 2 && f.outcomes[0].kind == TA_INPUT_TAKEN && f.outcomes[0].actor == f.receiver);
    CHECK(f.outcomes[0].has_event && f.outcomes[0].correlation == 0u);
    const ta_event *event = &f.outcomes[0].event;
    CHECK(event->name == name_of(&f, "e1") && event->type == TA_EVENT_EXTERNAL);
    CHECK(event->origin.kind == TA_ORIGIN_ACTOR && event->origin.as.actor.kind == TA_ACTOR_REF_NAMED);
    ta_str origin = event->origin.kind == TA_ORIGIN_ACTOR && event->origin.as.actor.kind == TA_ACTOR_REF_NAMED
                        ? ta_address_view(event->origin.as.actor.as.named).name
                        : (ta_str){ 0 };
    CHECK(origin.len == 1 && origin.ptr[0] == 's');
    CHECK(f.outcomes[1].kind == TA_INPUT_COMPLETED && !f.outcomes[1].has_event);
    close_fixture(&f); /* releases the event the host was given */
}

/* Claim 9. */
static void test_mail_to_a_session_that_is_not_resident_waits(void) {
    fixture f = open_with(NULL, 0, &sender);
    CHECK(deliver(&f, f.sender, "away") == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == 0 && envelopes_in(&f, TA_ENVELOPE_WAITING) == 1);
    CHECK(load_requests(f.system) == 1);
    close_fixture(&f);
}

int main(void) {
    test_mail_waits_for_its_senders_macrostep();
    test_order_per_sender_and_receiver();
    test_a_fanout_fails_once_at_the_first_send_without_an_envelope();
    test_a_fanout_fails_once_at_the_first_send_without_name_room();
    test_an_abandoned_macrostep_gives_its_mail_back();
    test_a_traced_envelope_carries_its_senders_trace_parent();
    test_a_due_send_falls_into_the_pool();
    test_taken_outcomes_carry_mail_events();
    test_mail_to_a_session_that_is_not_resident_waits();
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "mail_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("mail_test: ok\n");
    return 0;
}
