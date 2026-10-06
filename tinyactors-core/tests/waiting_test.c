/*
 * Mail for a target that is not resident (src/waiting.h, src/run.h, ADR
 * 0099): it waits in core on its destination's waiting entry, core asks
 * the host once per destination (ta_system_load_needed), and the host's
 * answer delivers it (ta_system_load_settled) or turns each message into
 * error.communication for its sender (ta_system_load_failed). Also the
 * index by correlation that ta_system_withdraw_input reads, and mail for a
 * session that is not published.
 *
 * The test includes the amalgamation so it reaches the private units; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which counts the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. mail to a named address with no resident actor waits on one waiting
 *    entry for the address: it arrives there when its sender's macrostep
 *    ends, one load request is owed, naming the address, with the hint
 *    TA_ERR_NOT_FOUND and the count of messages waiting; it is read once,
 *    and more mail for the address joins the same entry and asks nothing
 *    more until the host answers;
 * 2. settling the request moves the waiting mail to the run queue, in the
 *    order it waited, for the actor the host names; the entry is freed, a
 *    second answer is TA_ERR_STALE, and later mail goes to the resident
 *    actor directly;
 * 3. failing the request makes each waiting message error.communication
 *    for its sender, once per message, carrying the fault's data, which
 *    each error retains and releases; the entry is freed, a second answer
 *    is TA_ERR_STALE, and later mail for the address asks again;
 * 4. mail a macrostep sends to an address that is not resident counts in
 *    the entry from the send: when the macrostep is abandoned, its mail is
 *    given back, no load request is owed, and the entry is freed;
 * 5. mail whose resident target is destroyed, or finishes, before it is
 *    taken becomes error.communication for its sender, instead of being
 *    dropped;
 * 6. mail for a session that is not published waits on its deferred list
 *    until it is published, and is taken after that;
 * 7. withdrawal finds an admitted input by its correlation: the first
 *    admitted with a correlation goes, then the next, a taken input
 *    cannot be withdrawn, and an unknown one is TA_ERR_NOT_FOUND;
 * 8. teardown with mail waiting, and with requests owed, releases every
 *    reference and leaks nothing;
 * 9. a send to a session ID that names no family, which no session holds,
 *    waits for nothing: no host could bring it back, so it fails at once,
 *    in the sending macrostep, before an event the macrostep raised after
 *    it (SCXML 6.2.4), and no load request is owed;
 * 10. an event the host posts to an address (ta_actor_deliver with no
 *    actor) goes to the actor resident there; with none, it is admitted
 *    and waits for the address on the same entry as mail does, under the
 *    host's correlation, and core asks once; settling delivers it, and the
 *    run reports it under its correlation; failing refuses it, and the next
 *    run reports it REFUSED with the fault's code; a waiting one can be
 *    withdrawn, leaving the request owed.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

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

enum { NAMES = 8 };

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
            if (dir->entries[index].kind != TA_ACTOR_DIRECTORY_NONE) {
                return TA_ERR_STALE;
            }
            dir->entries[index] = params->entry;
            return TA_OK;
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

/* ------------------------------------------------------------------------
 * A counted value, for the data a failed load's errors carry
 */

typedef struct {
    int references;
} counted_value;

static void counted_retain(void *self) {
    ((counted_value *)self)->references += 1;
}

static void counted_release(void *self) {
    ((counted_value *)self)->references -= 1;
}

static const ta_value_vtable counted_vtable = { .retain = counted_retain, .release = counted_release };

/* ------------------------------------------------------------------------
 * Charts
 */

#define MAIL(event_text, target_text)                                                              \
    { .kind = TA_SCXML_ACTION_SEND,                                                                \
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },           \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) } } }

/* The receiver: r0 --e1--> r1 --e2--> r2 --e3--> r3; `fin` ends it. */
static const ta_str to_r1[] = { STR("r1"), END };
static const ta_str to_r2[] = { STR("r2"), END };
static const ta_str to_r3[] = { STR("r3"), END };
static const ta_str to_end[] = { STR("end"), END };
static const ta_scxml_state_spec receiver_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r0"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e1"), .targets = to_r1 },
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("fin"), .targets = to_end }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r1"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e2"), .targets = to_r2 }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r2"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e3"), .targets = to_r3 }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r3") },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    END,
};
static const ta_scxml_spec receiver = { .name = STR("receiver"), .states = receiver_states };

/* The sender: `one` mails e1 to gone/g, `two` mails e2 and e3 to it, and
 * `near` mails e1 to recv/r. Errors are ignored; the counters tell. */
static const ta_scxml_action_spec one_mail[] = { MAIL("e1", "#_actor_gone/g"), END };
static const ta_scxml_action_spec two_mails[] = { MAIL("e2", "#_actor_gone/g"), MAIL("e3", "#_actor_gone/g"), END };
static const ta_scxml_action_spec near_mail[] = { MAIL("e1", "#_actor_recv/r"), END };
static const ta_scxml_state_spec sender_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"),
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("one"), .actions = one_mail },
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("two"), .actions = two_mails },
          { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("near"), .actions = near_mail },
          END } },
    END,
};
static const ta_scxml_spec sender = { .name = STR("sender"), .states = sender_states };

/* The looper: `go` enters a, which mails e1 to gone/g on entry and loops
 * through b back to itself. */
static const ta_str to_a[] = { STR("a"), END };
static const ta_str to_b[] = { STR("b"), END };
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

/* The prober: on entry it mails event to #_scxml_foo, which names no
 * session, and raises foo; error.communication first takes it to pass. */
static const ta_str to_pass[] = { STR("pass"), END };
static const ta_str to_fail[] = { STR("fail"), END };
static const ta_scxml_action_spec probe_actions[] = {
    MAIL("event", "#_scxml_foo"),
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("foo") } },
    END,
};
static const ta_scxml_handler_spec probe_on_entry[] = { { .actions = probe_actions }, END };
static const ta_scxml_state_spec prober_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s0"), .onentry = probe_on_entry,
      .transitions = (const ta_scxml_transition_spec[]){
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.communication"), .targets = to_pass },
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("*"), .targets = to_fail }, END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("pass") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("fail") },
    END,
};
static const ta_scxml_spec prober = { .name = STR("prober"), .states = prober_states };

/* ------------------------------------------------------------------------
 * A system, its actors and its runs
 */

typedef struct {
    ta_system *system;
    ta_definition_id receiver_definition;
    ta_actor_id sender;
    ta_actor_id receiver;
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

static ta_result run_once(fixture *f, uint32_t action_budget) {
    for (uint32_t index = 0; index < f->count; index += 1u) {
        ta_event_release(&f->outcomes[index].event);
    }
    f->count = 0;
    return ta_run(f->system, &(ta_run_params){ .budget = 1000, .action_budget = action_budget,
                                               .report_taken = true, .outcomes = f->outcomes,
                                               .outcome_capacity = 2, .outcome_count = &f->count,
                                               .status = &f->status });
}

/* The names of the mail one session took, in order. */
typedef struct {
    char names[16][16];
    uint32_t count;
} taken_list;

static void note_taken(fixture *f, ta_actor_id actor, taken_list *taken) {
    for (uint32_t index = 0; index < f->count; index += 1u) {
        const ta_input_outcome *outcome = &f->outcomes[index];
        if (outcome->kind != TA_INPUT_TAKEN || !outcome->has_event || outcome->actor != actor ||
            taken->count == 16u) {
            continue;
        }
        uint32_t needed = 0;
        CHECK(ta_event_name_format(f->system, &(ta_event_name_format_params){
                                                  .name = outcome->event.name, .buffer = taken->names[taken->count],
                                                  .capacity = 16, .needed = &needed }) == TA_OK);
        taken->count += 1u;
    }
}

static void run_all(fixture *f, ta_actor_id actor, taken_list *taken) {
    for (uint32_t call = 0; call < 1000u; call += 1u) {
        CHECK(run_once(f, 0) == TA_OK);
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

static fixture open_fixture(void) {
    fixture f = { 0 };
    directory = (small_directory){ 0 };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &f.system) == TA_OK);
    CHECK(ta_system_set_actor_directory(f.system, (ta_actor_directory){ .self = &directory, .v = &dir_vtable }) ==
          TA_OK);
    f.receiver_definition = define(f.system, &receiver, "gone");
    f.sender = spawn_named(&f, define(f.system, &sender, "send"), "send", "s");
    f.receiver = spawn_named(&f, define(f.system, &receiver, "recv"), "recv", "r");
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

static ta_result deliver(fixture *f, ta_actor_id actor, const char *text, uint64_t correlation) {
    ta_event_name_id name = 0;
    CHECK(ta_trie_intern_dotted(&f->system->event_names, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) },
                                &name) == TA_OK);
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(f->system, &(ta_actor_deliver_params){ .actor = actor, .event = &event,
                                                                   .time = ta_system_time(f->system),
                                                                   .correlation = correlation });
}

static uint32_t envelopes_in(fixture *f, ta_envelope_state state) {
    const ta_envelope_pool *pool = &f->system->run.pool;
    uint32_t count = 0;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        count += envelope != NULL && envelope->state == state ? 1u : 0u;
    }
    return count;
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

static uint64_t communication_errors(fixture *f) {
    ta_counters counters = { 0 };
    ta_system_counters(f->system, &counters);
    return counters.errors_communication;
}

static bool names_address(const ta_load_request *request, const char *family, const char *name) {
    if (request->destination.kind != TA_ACTOR_REF_NAMED) {
        return false;
    }
    ta_actor_address view = ta_address_view(request->destination.as.named);
    return view.family.name.len == strlen(family) && memcmp(view.family.name.ptr, family, view.family.name.len) == 0 &&
           view.name.len == strlen(name) && memcmp(view.name.ptr, name, view.name.len) == 0;
}

/* Entries live: taken and not freed. */
static uint32_t live_entries(fixture *f) {
    return f->system->run.waiting.items.used;
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* Claim 1. */
static void test_mail_waits_on_one_entry_per_address(void) {
    fixture f = open_fixture();
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_ERR_NOT_FOUND);
    CHECK(deliver(&f, f.sender, "one", 0) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 1 && envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == 0);
    CHECK(live_entries(&f) == 1);
    CHECK(ta_system_load_needed(f.system, &request) == TA_OK);
    CHECK(names_address(&request, "gone", "g") && request.hint == TA_ERR_NOT_FOUND && request.waiting == 1);
    CHECK(ta_trace_context_empty(&request.trace.context));
    ta_load_request again = { 0 };
    CHECK(ta_system_load_needed(f.system, &again) == TA_ERR_NOT_FOUND);
    /* More mail for the address joins the entry and asks nothing more. */
    CHECK(deliver(&f, f.sender, "two", 0) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 3 && live_entries(&f) == 1);
    CHECK(ta_system_load_needed(f.system, &again) == TA_ERR_NOT_FOUND);
    close_fixture(&f);
}

/* Claim 2. */
static void test_settling_delivers_the_mail_in_order(void) {
    fixture f = open_fixture();
    CHECK(deliver(&f, f.sender, "one", 0) == TA_OK);
    CHECK(deliver(&f, f.sender, "two", 0) == TA_OK);
    run_all(&f, 0, NULL);
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_OK && request.waiting == 3);
    ta_actor_id loaded = spawn_named(&f, f.receiver_definition, "gone", "g");
    CHECK(ta_system_load_settled(f.system, &(ta_load_settled_params){ .request = request.request,
                                                                       .actor = loaded }) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 0 && live_entries(&f) == 0);
    CHECK(ta_system_load_settled(f.system, &(ta_load_settled_params){ .request = request.request,
                                                                       .actor = loaded }) == TA_ERR_STALE);
    taken_list taken = { 0 };
    run_all(&f, loaded, &taken);
    CHECK(taken.count == 3 && strcmp(taken.names[0], "e1") == 0 && strcmp(taken.names[1], "e2") == 0 &&
          strcmp(taken.names[2], "e3") == 0);
    CHECK(in_state(&f, loaded, "r3"));
    /* Resident now: later mail goes to it, and nothing is asked. */
    CHECK(deliver(&f, f.sender, "one", 0) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(ta_system_load_needed(f.system, &request) == TA_ERR_NOT_FOUND && live_entries(&f) == 0);
    close_fixture(&f);
}

/* Claim 3. */
static void test_failing_tells_each_sender(void) {
    fixture f = open_fixture();
    CHECK(deliver(&f, f.sender, "one", 0) == TA_OK);
    CHECK(deliver(&f, f.sender, "two", 0) == TA_OK);
    run_all(&f, 0, NULL);
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_OK);
    uint64_t before = communication_errors(&f);
    counted_value value = { .references = 1 };
    static const char text[] = "no loader";
    ta_fault fault = { .code = TA_ERR_NOT_FOUND, .message = STR(text),
                       .data = { .self = &value, .v = &counted_vtable } };
    CHECK(ta_system_load_failed(f.system, &(ta_load_failed_params){ .request = request.request, .fault = &fault }) ==
          TA_OK);
    CHECK(value.references == 1 + 3); /* each queued error holds the data */
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 0 && live_entries(&f) == 0);
    CHECK(ta_system_load_failed(f.system, &(ta_load_failed_params){ .request = request.request, .fault = &fault }) ==
          TA_ERR_STALE);
    run_all(&f, 0, NULL);
    CHECK(communication_errors(&f) == before + 3);
    CHECK(value.references == 1);
    /* Later mail for the address asks again. */
    CHECK(deliver(&f, f.sender, "one", 0) == TA_OK);
    run_all(&f, 0, NULL);
    ta_load_request next = { 0 };
    CHECK(ta_system_load_needed(f.system, &next) == TA_OK && next.request != request.request && next.waiting == 1);
    close_fixture(&f);
}

/* Claim 4. */
static void test_an_abandoned_macrostep_asks_for_nothing(void) {
    fixture f = open_fixture();
    ta_actor_id loop = spawn_named(&f, define(f.system, &looper, "loop"), "loop", "l");
    run_all(&f, 0, NULL);
    CHECK(deliver(&f, loop, "go", 0) == TA_OK);
    /* Paused between microsteps, after a's entry mailed once. */
    for (uint32_t index = 0; index < f.count; index += 1u) {
        ta_event_release(&f.outcomes[index].event);
    }
    f.count = 0;
    CHECK(ta_run(f.system, &(ta_run_params){ .budget = 2, .session = loop, .outcomes = f.outcomes,
                                              .outcome_capacity = 2, .outcome_count = &f.count,
                                              .status = &f.status }) == TA_OK &&
          f.status == TA_RUN_PAUSED);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) >= 1 && live_entries(&f) == 1);
    CHECK(ta_actor_abandon(f.system, loop) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_OUTGOING) == 0 && live_entries(&f) == 0);
    run_all(&f, 0, NULL);
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_ERR_NOT_FOUND);
    close_fixture(&f);
}

/* Claim 5. */
static void test_mail_whose_target_goes_is_an_error(void) {
    fixture f = open_fixture();
    uint64_t before = communication_errors(&f);
    /* Destroyed after the mail was sent, before it is taken. */
    CHECK(deliver(&f, f.sender, "near", 0) == TA_OK);
    CHECK(run_once(&f, 0) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == 1);
    CHECK(ta_actor_destroy(f.system, f.receiver) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(communication_errors(&f) == before + 1);
    close_fixture(&f);

    /* Finished after the mail was sent, before it is taken. */
    f = open_fixture();
    before = communication_errors(&f);
    CHECK(deliver(&f, f.sender, "near", 0) == TA_OK);
    CHECK(deliver(&f, f.receiver, "fin", 0) == TA_OK);
    CHECK(run_once(&f, 0) == TA_OK); /* the sender's macrostep: e1 queued behind fin */
    CHECK(run_once(&f, 0) == TA_OK); /* the receiver finishes */
    run_all(&f, 0, NULL);
    CHECK(communication_errors(&f) == before + 1);
    close_fixture(&f);
}

/* Claim 6. */
static void test_mail_for_an_unpublished_session_waits_for_its_publish(void) {
    fixture f = open_fixture();
    ta_actor_id prepared = 0;
    ta_actor_address address = { .family = { .name = STR("gone") }, .name = STR("p") };
    CHECK(ta_actor_prepare(f.system, &(ta_actor_prepare_params){ .definition = f.receiver_definition,
                                                                 .address = &address,
                                                                 .out = &prepared }) == TA_OK);
    run_all(&f, 0, NULL);
    ta_actor *target = ta_actor_lookup(f.system, prepared);
    CHECK(target != NULL && !target->published);
    ta_event_name_id name = 0;
    CHECK(ta_trie_intern_dotted(&f.system->event_names, STR("e1"), &name) == TA_OK);
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_record_capacity_resource short_of = (ta_record_capacity_resource)0;
    CHECK(ta_run_post_mail(f.system,
                           &(ta_run_mail){ .target = target, .event = &event,
                                           .from = ta_actor_lookup(f.system, f.sender)->session_id,
                                           .source = TA_RECORD_INPUT_SOURCE_MAIL },
                           &short_of) == TA_OK);
    taken_list taken = { 0 };
    run_all(&f, prepared, &taken);
    CHECK(taken.count == 0 && !ta_list_empty(&target->deferred) && in_state(&f, prepared, "r0"));
    CHECK(ta_actor_publish(f.system, &(ta_actor_publish_params){ .actor = prepared }) == TA_OK);
    run_all(&f, prepared, &taken);
    CHECK(taken.count == 1 && strcmp(taken.names[0], "e1") == 0 && in_state(&f, prepared, "r1"));
    close_fixture(&f);
}

/* Claim 7. */
static void test_withdrawal_reads_the_index(void) {
    fixture f = open_fixture();
    CHECK(deliver(&f, f.receiver, "e1", 7) == TA_OK);
    CHECK(deliver(&f, f.receiver, "e2", 7) == TA_OK);
    CHECK(deliver(&f, f.receiver, "e3", 9) == TA_OK);
    CHECK(ta_system_withdraw_input(f.system, 0) == TA_ERR_INVALID);
    CHECK(ta_system_withdraw_input(f.system, 8) == TA_ERR_NOT_FOUND);
    /* The first with 7 goes: e1, so the receiver takes e2 and e3 only. */
    CHECK(ta_system_withdraw_input(f.system, 7) == TA_OK);
    CHECK(run_once(&f, 0) == TA_OK); /* takes e2 (7) and completes it */
    CHECK(ta_system_withdraw_input(f.system, 7) == TA_ERR_NOT_FOUND); /* taken already */
    CHECK(ta_system_withdraw_input(f.system, 9) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(in_state(&f, f.receiver, "r0")); /* e2 alone moves nothing */
    close_fixture(&f);
}

/* Claim 8. */
static void test_teardown_releases_waiting_mail(void) {
    fixture f = open_fixture();
    CHECK(deliver(&f, f.sender, "one", 0) == TA_OK);
    run_all(&f, 0, NULL);
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_OK);
    CHECK(deliver(&f, f.sender, "two", 0) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 3);
    close_fixture(&f); /* checks heap.live == 0 */
}

/* Claim 9. */
static void test_a_send_no_host_can_answer_fails_at_once(void) {
    fixture f = open_fixture();
    ta_actor_id probe = spawn_named(&f, define(f.system, &prober, "probe"), "probe", "p");
    run_all(&f, 0, NULL);
    CHECK(in_state(&f, probe, "pass"));
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_ERR_NOT_FOUND && live_entries(&f) == 0);
    close_fixture(&f);
}

/* Posts `text` to gone/g by address, with `correlation`. */
static ta_result post_by_address(fixture *f, const char *text, uint64_t correlation) {
    ta_event_name_id name = 0;
    CHECK(ta_trie_intern_dotted(&f->system->event_names, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) },
                                &name) == TA_OK);
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_actor_address address = { .family = { .name = STR("gone") }, .name = STR("g") };
    return ta_actor_deliver(f->system, &(ta_actor_deliver_params){ .address = &address, .event = &event,
                                                                   .time = ta_system_time(f->system),
                                                                   .correlation = correlation });
}

/* Claim 10. */
static void test_posts_by_address_wait_like_mail(void) {
    fixture f = open_fixture();
    CHECK(post_by_address(&f, "e1", 11) == TA_OK);
    CHECK(deliver(&f, f.sender, "two", 0) == TA_OK); /* chart mail for the same address */
    CHECK(post_by_address(&f, "e1", 12) == TA_OK);
    run_all(&f, 0, NULL);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 4 && live_entries(&f) == 1);
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(f.system, &request) == TA_OK && request.waiting == 4);
    CHECK(ta_system_load_needed(f.system, &(ta_load_request){ 0 }) == TA_ERR_NOT_FOUND);
    /* A waiting post is withdrawn; the request stays owed. */
    CHECK(ta_system_withdraw_input(f.system, 12) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 3 && live_entries(&f) == 1);
    ta_actor_id loaded = spawn_named(&f, f.receiver_definition, "gone", "g");
    CHECK(ta_system_load_settled(f.system, &(ta_load_settled_params){ .request = request.request,
                                                                       .actor = loaded }) == TA_OK);
    bool reported = false;
    for (uint32_t call = 0; call < 100u && !reported; call += 1u) {
        CHECK(run_once(&f, 0) == TA_OK);
        for (uint32_t at = 0; at < f.count; at += 1u) {
            reported = reported || (f.outcomes[at].correlation == 11u && f.outcomes[at].actor == loaded &&
                                    f.outcomes[at].kind == TA_INPUT_COMPLETED);
        }
    }
    CHECK(reported);
    run_all(&f, 0, NULL);
    CHECK(in_state(&f, loaded, "r3"));
    /* Resident now: a post by address goes straight to it. */
    CHECK(post_by_address(&f, "fin", 13) == TA_OK);
    CHECK(envelopes_in(&f, TA_ENVELOPE_WAITING) == 0 && live_entries(&f) == 0);
    run_all(&f, 0, NULL);
    close_fixture(&f);

    /* A failed load refuses the post, under its correlation, with the code. */
    f = open_fixture();
    CHECK(post_by_address(&f, "e1", 21) == TA_OK);
    CHECK(ta_system_load_needed(f.system, &request) == TA_OK && request.hint == TA_ERR_NOT_FOUND);
    ta_fault fault = { .code = TA_ERR_UNLOADED };
    CHECK(ta_system_load_failed(f.system, &(ta_load_failed_params){ .request = request.request, .fault = &fault }) ==
          TA_OK);
    CHECK(run_once(&f, 0) == TA_OK);
    CHECK(f.count == 1 && f.outcomes[0].kind == TA_INPUT_REFUSED && f.outcomes[0].correlation == 21u &&
          f.outcomes[0].code == TA_ERR_UNLOADED);
    CHECK(envelopes_in(&f, TA_ENVELOPE_RUNNABLE) == 0 && live_entries(&f) == 0);
    /* A post with no actor and no usable address is a caller error. */
    ta_event event = { .name = 0, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_actor_address empty = { 0 };
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .address = &empty, .event = &event,
                                                                 .time = ta_system_time(f.system) }) ==
          TA_ERR_INVALID);
    close_fixture(&f);
}

int main(void) {
    test_mail_waits_on_one_entry_per_address();
    test_settling_delivers_the_mail_in_order();
    test_failing_tells_each_sender();
    test_an_abandoned_macrostep_asks_for_nothing();
    test_mail_whose_target_goes_is_an_error();
    test_mail_for_an_unpublished_session_waits_for_its_publish();
    test_withdrawal_reads_the_index();
    test_teardown_releases_waiting_mail();
    test_a_send_no_host_can_answer_fails_at_once();
    test_posts_by_address_wait_like_mail();
    if (failures != 0) {
        fprintf(stderr, "waiting_test: %d failures\n", failures);
        return 1;
    }
    printf("waiting_test: ok\n");
    return 0;
}
