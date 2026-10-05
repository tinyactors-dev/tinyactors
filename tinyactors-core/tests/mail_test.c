/* test: standalone asan */
/*
 * Package mail (src/mail.h; ADR 0099 sections 4 and 6), driven directly. The
 * interpreter does not call ta_mail_send in this step, so the tests pause a
 * session inside its macrostep with a one-microstep budget (the slot is then
 * occupied, as it is when a chart runs a <send>) and call ta_mail_send as the
 * interpreter will:
 *
 *   1. Order and visibility: mail sent in a macrostep keeps its order, is not
 *      on the run queue and not taken while the macrostep is paused, joins
 *      the queue when it ends, and the receiver runs it in the order sent.
 *   2. A named destination resolves through the host's directory; a runtime
 *      destination that is gone, finished, or unknown is not placed here.
 *   3. The claim: the macrostep reserve is the number of envelopes one input
 *      may take, the one after it is TA_MAIL_FULL, and a refused send changes
 *      nothing.
 *   4. Abandon and runaway: the macrostep's mail is freed unseen, the
 *      receivers' queued counts and the pool return to their baseline, and no
 *      receiver ever runs it.
 *   5. Recycling (ADR 0099 section 6): mail whose receiver is destroyed, or
 *      has finished, comes back to its sender as a data-free
 *      error.communication carrying the send's ID, using the same envelope; a
 *      sender that is gone has it freed. Every case ends with an empty pool.
 *   6. No allocation: a round of mail between two sessions makes no allocator
 *      call once the pool has grown.
 *
 * Includes the amalgamation, because the package is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_allocator.h"
#include "run_support.h"

static int failures;

#define CHECK(condition)                                                                                      \
    do {                                                                                                      \
        if (!(condition)) {                                                                                   \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);                    \
            failures += 1;                                                                                    \
        }                                                                                                     \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }
#define END {0}

static ta_test_allocator heap = { .strict = true };
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* ------------------------------------------------------------------------
 * A one-entry directory that always says the same session is resident.
 */

typedef struct {
    ta_actor_id resident;
} fixed_directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    fixed_directory *dir = self;
    *params->out = dir->resident != 0
                       ? (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = dir->resident }
                       : (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/* ------------------------------------------------------------------------
 * The world: a sender `talker` that leaves `a` for `b` on `go` and for
 * `failed` on error.communication; a receiver `chain` that goes s0 -> s1 ->
 * s2 -> s3 on e1, e2, e3 (so only the order e1 e2 e3 reaches s3); and a
 * `finisher` that is final after `end`; and a `spinner` that never settles
 * (an eventless loop), for the microstep limit.
 */

typedef struct {
    ta_system *system;
    fixed_directory directory;
    ta_definition_id talker, chain, finisher, spinner;
    ta_event_name_id go, e1, e2, e3, end;
} world;

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

static ta_definition_id define(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = spec,
                                                                              .datamodel = ta_null_datamodel(),
                                                                              .allocator = allocator,
                                                                              .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

#define ON(event_text, target_text)                                                                 \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text),                               \
      .targets = (const ta_str[]){ STR(target_text), END } }

static void world_init(world *w, const ta_param_value *params, uint32_t count) {
    *w = (world){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count },
                           &w->system) == TA_OK);
    CHECK(ta_system_set_actor_directory(w->system, (ta_actor_directory){ .self = &w->directory, .v = &dir_vtable }) ==
          TA_OK);
    ta_scxml_state_spec talker[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("go", "b"), ON("error.communication", "failed"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("go", "a"), ON("error.communication", "failed"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
        END,
    };
    ta_scxml_state_spec chain[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s0"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("e1", "s1"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s1"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("e2", "s2"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s2"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("e3", "s3"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s3") },
        END,
    };
    ta_scxml_state_spec finisher[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("wait"),
          .transitions = (const ta_scxml_transition_spec[]){ ON("end", "over"), END } },
        { .kind = TA_SCXML_STATE_FINAL, .id = STR("over") },
        END,
    };
    ta_scxml_state_spec spinner[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("x"),
          .transitions = (const ta_scxml_transition_spec[]){
              { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("y"), END } }, END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("y"),
          .transitions = (const ta_scxml_transition_spec[]){
              { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("x"), END } }, END } },
        END,
    };
    w->talker = define(w->system, &(ta_scxml_spec){ .name = STR("talker"), .states = talker });
    w->chain = define(w->system, &(ta_scxml_spec){ .name = STR("chain"), .states = chain });
    w->finisher = define(w->system, &(ta_scxml_spec){ .name = STR("finisher"), .states = finisher });
    w->spinner = define(w->system, &(ta_scxml_spec){ .name = STR("spinner"), .states = spinner });
    w->go = event_name(w->system, "go");
    w->e1 = event_name(w->system, "e1");
    w->e2 = event_name(w->system, "e2");
    w->e3 = event_name(w->system, "e3");
    w->end = event_name(w->system, "end");
}

static void world_close(world *w) {
    ta_system_destroy(w->system);
    CHECK(heap.stats.live_blocks == 0);
}

static ta_actor_id spawn(world *w, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(w->system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    return actor;
}

static ta_result post(world *w, ta_actor_id actor, ta_event_name_id name) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .actor = actor, .event = &event,
                                                                   .time = ta_system_time(w->system) });
}

/* One ta_run call with `budget` microsteps; answers its status and ignores the notices. */
static ta_run_status run_for(world *w, uint32_t budget) {
    ta_run_status status = TA_RUN_IDLE;
    ta_run_notice notices[16];
    uint32_t count = 0;
    CHECK(ta_run(w->system, &(ta_run_params){ .budget = budget, .status = &status, .notices = notices,
                                              .notice_capacity = 16, .notice_count = &count }) == TA_OK);
    return status;
}

static void settle(world *w) {
    CHECK(test_run_idle(w->system) == TA_OK);
}

static bool in_state(world *w, ta_actor_id actor, const char *id) {
    ta_state_id active[8];
    uint32_t count = 0;
    ta_actor_inspection inspection = {0};
    CHECK(ta_actor_inspect(w->system, actor, &inspection) == TA_OK);
    CHECK(ta_actor_active_states(w->system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                             .capacity = 8, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count; index += 1) {
        ta_state_info info = {0};
        CHECK(ta_definition_state(w->system, &(ta_definition_state_params){ .definition = inspection.definition_id,
                                                                            .state = active[index], .out = &info }) ==
              TA_OK);
        if (info.id.len == strlen(id) && memcmp(info.id.ptr, id, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

/* I1 and the lists: what must hold between any two calls (envelope.h). */
static void consistent(world *w) {
    const ta_envelopes *envelopes = &w->system->envelopes;
    CHECK(envelopes->quota.used == envelopes->pool.used);
    CHECK(envelopes->quota.used + envelopes->quota.reserved <= envelopes->quota.capacity);
}

/* The pool and the claims are back to nothing in use. */
static void quiescent(world *w) {
    const ta_envelopes *envelopes = &w->system->envelopes;
    CHECK(envelopes->pool.used == 0);
    CHECK(envelopes->quota.used == 0 && envelopes->quota.reserved == 0);
    CHECK(ta_ilist_is_empty(&envelopes->run));
    CHECK(!w->system->runner.slot.occupied);
    consistent(w);
}

static ta_actor_ref runtime_ref(ta_actor_id id) {
    return (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = id };
}

/* Sends `name` from `sender` to `destination` as the interpreter will, inside the running macrostep. */
static ta_mail_status send_to(world *w, ta_actor_id sender, ta_actor_ref destination, ta_event_name_id name,
                              ta_send_id send_id) {
    const ta_actor *from = ta_actor_lookup(w->system, sender);
    ta_mail_params mail = {
        .sender = sender,
        .destination = destination,
        .event = { .name = name, .type = TA_EVENT_EXTERNAL, .send_id = send_id, .invoke_id = TA_INVOKE_ID_NONE,
                   .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = from->record.self } },
    };
    return ta_mail_send(w->system, &mail);
}

/*
 * Puts `sender` inside a macrostep that has not ended: it takes `go` and a
 * budget of one microstep pauses it. The slot is then occupied by `sender`.
 */
static void pause_in_macrostep(world *w, ta_actor_id sender) {
    CHECK(post(w, sender, w->go) == TA_OK);
    CHECK(run_for(w, 1) == TA_RUN_PAUSED);
    CHECK(w->system->runner.slot.occupied && w->system->runner.slot.actor == sender);
}

static uint32_t queued_of(world *w, ta_actor_id id) {
    const ta_actor *actor = ta_actor_lookup(w->system, id);
    return actor == NULL ? 0xffffffffu : actor->queued;
}

/* ------------------------------------------------------------------------
 * 1. Order and visibility
 */

static void test_order_and_visibility(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    CHECK(in_state(&w, receiver, "s0"));

    /* Nothing is sent outside a macrostep: no input runs, the answer is "not here". */
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_ROUTER);

    pause_in_macrostep(&w, sender);
    CHECK(w.system->envelopes.quota.reserved > 0); /* the reserve is held by the running input */
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e2, 0) == TA_MAIL_QUEUED);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e3, 0) == TA_MAIL_QUEUED);
    consistent(&w);
    /* Invisible: not on the run queue, counted for the receiver, not taken. */
    CHECK(ta_ilist_is_empty(&w.system->envelopes.run));
    CHECK(!ta_ilist_is_empty(&w.system->runner.slot.outgoing));
    CHECK(queued_of(&w, receiver) == 3u);
    CHECK(in_state(&w, receiver, "s0"));

    /* The macrostep ends: one splice, and the receiver runs the mail in the order it was sent. */
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(in_state(&w, sender, "b"));
    CHECK(in_state(&w, receiver, "s3"));
    CHECK(queued_of(&w, receiver) == 0u);
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 2. Where a destination is placed
 */

static void test_destinations(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    ta_actor_id done = spawn(&w, w.finisher);
    ta_actor_id destroyed = spawn(&w, w.chain);
    settle(&w);
    CHECK(post(&w, done, w.end) == TA_OK);
    CHECK(ta_actor_destroy(w.system, destroyed) == TA_OK);
    settle(&w);
    pause_in_macrostep(&w, sender);
    uint64_t used = w.system->envelopes.pool.used;

    /* An ID no session has, a finished session, and the ID of a destroyed one are not placed. */
    CHECK(send_to(&w, sender, runtime_ref(0), w.e1, 0) == TA_MAIL_ROUTER);
    CHECK(send_to(&w, sender, runtime_ref(done), w.e1, 0) == TA_MAIL_ROUTER);
    CHECK(send_to(&w, sender, runtime_ref(destroyed), w.e1, 0) == TA_MAIL_ROUTER);
    CHECK(w.system->envelopes.pool.used == used);
    CHECK(ta_ilist_is_empty(&w.system->runner.slot.outgoing));

    /* A named destination is the session the directory says is resident. */
    ta_address *address = NULL;
    CHECK(ta_address_intern(w.system, &(ta_actor_address){ .family = { STR("chain") }, .name = STR("n") }, &address) ==
          TA_OK);
    ta_actor_ref named = { .kind = TA_ACTOR_REF_NAMED, .as.named = address };
    CHECK(send_to(&w, sender, named, w.e1, 0) == TA_MAIL_ROUTER); /* the directory knows no one yet */
    w.directory.resident = receiver;
    CHECK(send_to(&w, sender, named, w.e1, 0) == TA_MAIL_QUEUED);
    w.directory.resident = done; /* a finished session is not a receiver */
    CHECK(send_to(&w, sender, named, w.e2, 0) == TA_MAIL_ROUTER);
    w.directory.resident = 0;
    ta_address_release(address);

    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(in_state(&w, receiver, "s1"));
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 3. The claim
 */

static void test_claim(void) {
    ta_param_value params[] = { { TA_PARAM_MACROSTEP_RESERVE, 3 } };
    world w;
    world_init(&w, params, 1);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(w.system->runner.slot.claim.held == 3u);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e2, 0) == TA_MAIL_QUEUED);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e3, 0) == TA_MAIL_QUEUED);
    uint64_t used = w.system->envelopes.pool.used;
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_FULL); /* the fourth: the claim is spent */
    CHECK(w.system->envelopes.pool.used == used);
    CHECK(queued_of(&w, receiver) == 3u);
    consistent(&w);
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(in_state(&w, receiver, "s3"));
    quiescent(&w);

    /* The reserve is a fresh claim for the next input, and the same size on a warm system. */
    pause_in_macrostep(&w, sender);
    CHECK(w.system->runner.slot.claim.held == 3u);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 4. Abandon and runaway
 */

static void test_abandon(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e2, 0) == TA_MAIL_QUEUED);
    CHECK(queued_of(&w, receiver) == 2u);
    CHECK(ta_actor_abandon(w.system, sender) == TA_OK);
    /* The mail is freed at once, unseen; only the abandoned input's own envelope waits to be reported. */
    CHECK(queued_of(&w, receiver) == 0u);
    CHECK(w.system->envelopes.pool.used == 1u);
    CHECK(w.system->envelopes.quota.reserved == 0);
    consistent(&w);
    settle(&w);
    CHECK(in_state(&w, receiver, "s0"));
    quiescent(&w);
    world_close(&w);
}

static void test_runaway(void) {
    ta_param_value params[] = { { TA_PARAM_MICROSTEP_LIMIT, 20 } };
    world w;
    world_init(&w, params, 1);
    ta_actor_id sender = spawn(&w, w.spinner);
    ta_actor_id receiver = spawn(&w, w.chain);
    /* The spinner's bootstrap loops: one microstep, then pause inside it. */
    CHECK(run_for(&w, 1) == TA_RUN_PAUSED);
    CHECK(w.system->runner.slot.occupied && w.system->runner.slot.actor == sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(queued_of(&w, receiver) == 1u);
    /* It reaches the microstep limit and is abandoned: the mail it sent is dropped. */
    settle(&w);
    CHECK(queued_of(&w, receiver) == 0u);
    CHECK(in_state(&w, receiver, "s0"));
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 5. Recycling
 */

/*
 * Ends the sender's macrostep with the mail still on the run queue, not taken:
 * a bystander's input is queued before the mail, and core is stopped inside
 * the bystander's macrostep by the one-microstep budget.
 */
static void leave_mail_queued(world *w, ta_actor_id bystander) {
    CHECK(post(w, bystander, w->go) == TA_OK);
    for (uint32_t calls = 0; calls < 10u && !(w->system->runner.slot.occupied &&
                                              w->system->runner.slot.actor == bystander);
         calls += 1u) {
        (void)run_for(w, 1);
    }
    CHECK(w->system->runner.slot.occupied && w->system->runner.slot.actor == bystander);
    CHECK(!ta_ilist_is_empty(&w->system->envelopes.run)); /* the mail, behind the bystander */
}

/* The receiver is destroyed while the mail waits: the sender hears error.communication, once. */
static void test_receiver_destroyed(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    ta_actor_id bystander = spawn(&w, w.talker);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 77) == TA_MAIL_QUEUED);
    leave_mail_queued(&w, bystander);
    CHECK(ta_actor_destroy(w.system, receiver) == TA_OK);
    uint64_t before = w.system->counters.errors_communication;
    /* The error is the same envelope, now for the sender: its name, its type and the send's ID, and no data. */
    ta_run_status status = TA_RUN_MORE;
    ta_run_notice notices[16];
    uint32_t count = 0;
    CHECK(ta_run(w.system, &(ta_run_params){ .budget = 1000, .report_taken = true, .status = &status,
                                             .notices = notices, .notice_capacity = 16, .notice_count = &count }) ==
          TA_OK);
    uint32_t taken = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (notices[index].kind == TA_NOTICE_INPUT_TAKEN) {
            taken += 1u;
            CHECK(notices[index].actor == sender);
            CHECK(notices[index].event->name == TA_EVENT_NAME_ERROR_COMMUNICATION);
            CHECK(notices[index].event->type == TA_EVENT_PLATFORM);
            CHECK(notices[index].event->send_id == 77);
            CHECK(notices[index].event->data.v == NULL);
        }
    }
    CHECK(taken == 1u);
    settle(&w);
    CHECK(w.system->counters.errors_communication == before + 1u);
    CHECK(in_state(&w, sender, "failed"));
    quiescent(&w);
    world_close(&w);
}

/* The receiver finishes on the first of two mails: the second comes back as an error. */
static void test_receiver_finished(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.finisher);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.end, 5) == TA_MAIL_QUEUED);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.end, 6) == TA_MAIL_QUEUED);
    settle(&w);
    CHECK(ta_actor_lookup(w.system, receiver) != NULL && ta_actor_lookup(w.system, receiver)->finished);
    CHECK(in_state(&w, sender, "failed")); /* the second end found a finished session */
    quiescent(&w);
    world_close(&w);
}

/* The sender is gone too: nobody is told, and everything is freed. */
static void test_sender_gone(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    ta_actor_id bystander = spawn(&w, w.talker);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    leave_mail_queued(&w, bystander);
    CHECK(ta_actor_destroy(w.system, receiver) == TA_OK);
    CHECK(ta_actor_destroy(w.system, sender) == TA_OK);
    uint64_t before = w.system->counters.errors_communication;
    settle(&w);
    CHECK(w.system->counters.errors_communication == before);
    quiescent(&w);
    world_close(&w);
}

/* A session destroyed inside its macrostep drops the mail it sent. */
static void test_destroyed_in_macrostep(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(ta_actor_destroy(w.system, sender) == TA_OK);
    CHECK(queued_of(&w, receiver) == 0u);
    settle(&w);
    CHECK(in_state(&w, receiver, "s0"));
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 6. No allocation once the pool has grown
 */

static void test_no_allocation(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_QUEUED);
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    unsigned long before = heap.stats.attempts;
    for (uint32_t round = 0; round < 200u; round += 1u) {
        pause_in_macrostep(&w, sender);
        CHECK(send_to(&w, sender, runtime_ref(receiver), w.e2, 0) == TA_MAIL_QUEUED);
        CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    }
    CHECK(heap.stats.attempts == before);
    quiescent(&w);
    world_close(&w);
}

int main(void) {
    test_order_and_visibility();
    test_destinations();
    test_claim();
    test_abandon();
    test_runaway();
    test_receiver_destroyed();
    test_receiver_finished();
    test_sender_gone();
    test_destroyed_in_macrostep();
    test_no_allocation();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "mail_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("mail_test: ok");
    return 0;
}
