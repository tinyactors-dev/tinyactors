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
 *   7. Through the interpreter: the <send> of a chart is an envelope that
 *      waits for the end of its macrostep; a fanout that spends the macrostep
 *      reserve is error.execution at exactly the send after it, on a cold and
 *      on a warm system; a send to a name a finished session holds is
 *      error.communication in the sender's own macrostep.
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
    ta_definition_id talker, chain, finisher, spinner, timed;
    ta_event_name_id go, e1, e2, e3, end, tick;
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
    /* Sends itself `tick` after a second and then goes to `ticked`: a due send that is mail to a resident session. */
    ta_scxml_state_spec timed[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("waiting"),
          .onentry = (const ta_scxml_handler_spec[]){
              { .actions = (const ta_scxml_action_spec[]){
                    { .kind = TA_SCXML_ACTION_SEND,
                      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("tick") },
                                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("1s") } } },
                    END } },
              END },
          .transitions = (const ta_scxml_transition_spec[]){ ON("tick", "ticked"), END } },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("ticked") },
        END,
    };
    w->timed = define(w->system, &(ta_scxml_spec){ .name = STR("timed"), .states = timed });
    w->tick = event_name(w->system, "tick");
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
        .in_macrostep = true,
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

/* Runs ta_run once with report_taken and `capacity` notices; answers the status. */
static ta_run_status run_notices(world *w, ta_run_notice *notices, uint32_t capacity, uint32_t *count) {
    ta_run_status status = TA_RUN_IDLE;
    CHECK(ta_run(w->system, &(ta_run_params){ .budget = 1000, .report_taken = true, .status = &status,
                                              .notices = notices, .notice_capacity = capacity,
                                              .notice_count = count }) == TA_OK);
    return status;
}

static uint32_t count_kind(const ta_run_notice *notices, uint32_t count, ta_run_notice_kind kind) {
    uint32_t n = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        n += notices[index].kind == kind ? 1u : 0u;
    }
    return n;
}

static ta_address *intern_address(world *w, const char *family, const char *name) {
    ta_address *address = NULL;
    CHECK(ta_address_intern(w->system, &(ta_actor_address){ .family = { { .ptr = family, .len = (uint32_t)strlen(family) } },
                                                            .name = { .ptr = name, .len = (uint32_t)strlen(name) } },
                            &address) == TA_OK);
    return address;
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

    /* A macrostep's mail needs a macrostep to pay it: none runs, so there is no envelope for it. */
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e1, 0) == TA_MAIL_FULL);

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

    /* An ID no session has, and the ID of a destroyed or a finished one, with no family to serve it, are undeliverable. */
    CHECK(send_to(&w, sender, runtime_ref(0), w.e1, 0) == TA_MAIL_UNDELIVERABLE);
    CHECK(send_to(&w, sender, runtime_ref(done), w.e1, 0) == TA_MAIL_UNDELIVERABLE);
    CHECK(send_to(&w, sender, runtime_ref(destroyed), w.e1, 0) == TA_MAIL_UNDELIVERABLE);
    CHECK(w.system->envelopes.pool.used == used);
    CHECK(ta_ilist_is_empty(&w.system->runner.slot.outgoing));
    CHECK(ta_ilist_is_empty(&w.system->runner.slot.outgoing_waiting));

    /* A named destination is the session the directory says is resident. */
    ta_address *address = NULL;
    CHECK(ta_address_intern(w.system, &(ta_actor_address){ .family = { STR("chain") }, .name = STR("n") }, &address) ==
          TA_OK);
    ta_actor_ref named = { .kind = TA_ACTOR_REF_NAMED, .as.named = address };
    w.directory.resident = receiver;
    CHECK(send_to(&w, sender, named, w.e1, 0) == TA_MAIL_QUEUED);
    w.directory.resident = done; /* a name that a finished session holds is known to be undeliverable */
    CHECK(send_to(&w, sender, named, w.e2, 0) == TA_MAIL_UNDELIVERABLE);
    w.directory.resident = 0;
    ta_address_release(address);

    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(in_state(&w, receiver, "s1"));
    quiescent(&w);
    world_close(&w);
}


/* ------------------------------------------------------------------------
 * 2b. Destinations that wait: a name nobody holds, and a session that is gone
 */

static ta_actor_ref family_ref(world *w, ta_actor_id session, const char *family) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(w->system, (ta_str){ .ptr = family, .len = (uint32_t)strlen(family) }, &atom) == TA_OK);
    return (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .family = atom, .as.runtime = session };
}

/* The key address of a gone session: its family, and the session's text as the name. */
static ta_address *session_key(world *w, ta_actor_id session, const char *family) {
    char text[96];
    uint32_t needed = 0;
    CHECK(ta_session_id_format(w->system, &(ta_session_id_format_params){ .session = session,
                                                                         .family = { .ptr = family,
                                                                                     .len = (uint32_t)strlen(family) },
                                                                         .buffer = text, .capacity = sizeof text,
                                                                         .needed = &needed }) == TA_OK);
    ta_address *key = NULL;
    CHECK(ta_address_intern(w->system,
                            &(ta_actor_address){ .family = { { .ptr = family, .len = (uint32_t)strlen(family) } },
                                                 .name = { .ptr = text, .len = needed - 1u } },
                            &key) == TA_OK);
    return key;
}

/*
 * A name that nobody holds, a session that was destroyed and one that
 * finished (both with a family that may serve them) wait for an address: the
 * mail is on `outgoing_waiting` until the macrostep ends, then on the
 * address's list, invisible to every session, and the host is asked once for
 * each address. A failed load comes back to the sender as an error.
 */
static void test_waiting_destinations(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id done = spawn(&w, w.finisher);
    ta_actor_id destroyed = spawn(&w, w.chain);
    settle(&w);
    CHECK(post(&w, done, w.end) == TA_OK);
    CHECK(ta_actor_destroy(w.system, destroyed) == TA_OK);
    settle(&w);
    ta_address *named = intern_address(&w, "chain", "n");
    ta_address *key_gone = session_key(&w, destroyed, "chain");
    ta_address *key_done = session_key(&w, done, "finisher");
    CHECK(key_gone != key_done);

    pause_in_macrostep(&w, sender);
    uint64_t used = w.system->envelopes.pool.used;
    ta_actor_ref by_name = { .kind = TA_ACTOR_REF_NAMED, .as.named = named };
    CHECK(send_to(&w, sender, by_name, w.e1, 41) == TA_MAIL_WAITING);
    CHECK(send_to(&w, sender, by_name, w.e2, 42) == TA_MAIL_WAITING);
    CHECK(send_to(&w, sender, family_ref(&w, destroyed, "chain"), w.e1, 43) == TA_MAIL_WAITING);
    CHECK(send_to(&w, sender, family_ref(&w, done, "finisher"), w.e1, 44) == TA_MAIL_WAITING);
    CHECK(w.system->envelopes.pool.used == used + 4u);
    CHECK(ta_ilist_is_empty(&w.system->runner.slot.outgoing) && !ta_ilist_is_empty(&w.system->runner.slot.outgoing_waiting));
    CHECK(ta_address_waiting(w.system, named) == 0u); /* nothing is visible before the macrostep ends */
    consistent(&w);

    /* The macrostep ends: each mail joins its list, in order, and each list asks once. */
    ta_run_notice notices[16];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    CHECK(ta_address_waiting(w.system, named) == 2u);
    CHECK(ta_address_waiting(w.system, key_gone) == 1u && ta_address_waiting(w.system, key_done) == 1u);
    CHECK(count_kind(notices, count, TA_NOTICE_LOAD_NEEDED) == 3u);
    for (uint32_t index = 0; index < count; index += 1u) {
        if (notices[index].kind != TA_NOTICE_LOAD_NEEDED) {
            continue;
        }
        CHECK(notices[index].source == TA_SOURCE_MAIL);
        CHECK(notices[index].session == (notices[index].address != named));
    }
    consistent(&w);

    /* The host cannot load any of them: the sender hears an error for each mail, in the order sent. */
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = named,
                                                                            .reason = TA_REFUSED_LOAD_NOT_FOUND }) == TA_OK);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = key_gone,
                                                                            .reason = TA_REFUSED_LOAD_NOT_FOUND }) == TA_OK);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = key_done,
                                                                            .reason = TA_REFUSED_LOAD_NOT_FOUND }) == TA_OK);
    CHECK(queued_of(&w, sender) == 4u);
    settle(&w);
    CHECK(in_state(&w, sender, "failed"));
    ta_address_release(named);
    ta_address_release(key_gone);
    ta_address_release(key_done);
    quiescent(&w);
    world_close(&w);
}

/*
 * A session of another incarnation parses as session 0 with its family
 * (scxml_uri.c), and its family's loader may serve it: the mail waits under
 * the key "<this incarnation>.0.<family>", and one failed load answers it.
 */
static void test_other_incarnation_session_waits(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    settle(&w);
    char text[64];
    int length = snprintf(text, sizeof text, "%llu.0.chain", (unsigned long long)ta_system_incarnation(w.system));
    CHECK(length > 0 && (size_t)length < sizeof text);
    ta_address *key = NULL;
    CHECK(ta_address_intern(w.system,
                            &(ta_actor_address){ .family = { { .ptr = "chain", .len = 5 } },
                                                 .name = { .ptr = text, .len = (uint32_t)length } },
                            &key) == TA_OK);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, family_ref(&w, 0, "chain"), w.e1, 51) == TA_MAIL_WAITING);
    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 8, &count) == TA_RUN_IDLE);
    CHECK(ta_address_waiting(w.system, key) == 1u);
    CHECK(count_kind(notices, count, TA_NOTICE_LOAD_NEEDED) == 1u);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = key,
                                                                            .reason = TA_REFUSED_LOAD_NOT_FOUND }) == TA_OK);
    CHECK(queued_of(&w, sender) == 1u);
    settle(&w);
    CHECK(in_state(&w, sender, "failed"));
    ta_address_release(key);
    quiescent(&w);
    world_close(&w);
}

/* A name that becomes resident during the sender's macrostep gets its mail at the end of it. */
static void test_name_resident_by_the_end(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    ta_address *named = intern_address(&w, "chain", "n");
    pause_in_macrostep(&w, sender);
    ta_actor_ref by_name = { .kind = TA_ACTOR_REF_NAMED, .as.named = named };
    CHECK(send_to(&w, sender, by_name, w.e1, 0) == TA_MAIL_WAITING); /* nobody holds the name yet */
    w.directory.resident = receiver;                                 /* and now somebody does */
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(ta_address_waiting(w.system, named) == 0u && in_state(&w, receiver, "s1"));
    ta_address_release(named);
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

/* ------------------------------------------------------------------------
 * 7. Through the interpreter
 */

enum { MAX_SENDS = 8 };

/*
 * Defines a chart that, on entry, sends `sends` events (e1, e2, e3, then e1
 * again) to `target`, and goes to `refused` on error.execution and to `failed`
 * on error.communication.
 */
static ta_definition_id define_sender(world *w, const char *target, uint32_t sends, const char *delay) {
    static const char *const names[MAX_SENDS] = { "e1", "e2", "e3", "e1", "e1", "e1", "e1", "e1" };
    ta_scxml_action_spec actions[MAX_SENDS + 1];
    memset(actions, 0, sizeof actions);
    for (uint32_t index = 0; index < sends && index < MAX_SENDS; index += 1u) {
        actions[index] = (ta_scxml_action_spec){
            .kind = TA_SCXML_ACTION_SEND,
            .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME,
                                    .as.name = { .ptr = names[index], .len = (uint32_t)strlen(names[index]) } },
                         .target = { .kind = TA_SCXML_STRING_LITERAL,
                                     .as.literal = { .ptr = target, .len = (uint32_t)strlen(target) } },
                         .delay = delay == NULL ? (ta_scxml_string_spec){0}
                                                : (ta_scxml_string_spec){ .kind = TA_SCXML_STRING_LITERAL,
                                                                          .as.literal = { .ptr = delay,
                                                                                          .len = (uint32_t)strlen(delay) } } },
        };
    }
    ta_scxml_handler_spec onentry[] = { { .actions = actions }, END };
    ta_scxml_transition_spec transitions[] = {
        ON("error.execution", "refused"),
        ON("error.communication", "failed"),
        END,
    };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .onentry = onentry, .transitions = transitions },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("refused") },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
        END,
    };
    return define(w->system, &(ta_scxml_spec){ .name = STR("sender"), .states = states });
}

/* The session text of `actor`, as a <send> target: "#_scxml_" and the number the session ID formats to. */
static void session_target(world *w, ta_actor_id actor, char *out, size_t capacity) {
    char text[64];
    uint32_t needed = 0;
    ta_actor_inspection view = {0};
    CHECK(ta_actor_inspect(w->system, actor, &view) == TA_OK);
    CHECK(ta_session_id_format(w->system, &(ta_session_id_format_params){ .session = actor,
                                                                         .family = view.definition.family.name,
                                                                         .buffer = text,
                                                                         .capacity = sizeof text,
                                                                         .needed = &needed }) == TA_OK);
    snprintf(out, capacity, "#_scxml_%s", text);
}

/* A chart's sends wait for the end of its macrostep, arrive in order, and need no router. */
static void test_interpreter_mail(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id receiver = spawn(&w, w.chain);
    char target[96];
    session_target(&w, receiver, target, sizeof target);
    ta_definition_id sender_chart = define_sender(&w, target, 3, NULL);
    settle(&w);
    CHECK(in_state(&w, receiver, "s0"));
    ta_actor_id sender = spawn(&w, sender_chart);

    CHECK(test_run_input(w.system) == TA_OK); /* the sender's bootstrap: three sends */
    CHECK(in_state(&w, receiver, "s0"));
    CHECK(ta_run_pending(w.system));
    CHECK(queued_of(&w, receiver) == 3u);
    CHECK(in_state(&w, sender, "start"));
    settle(&w);
    CHECK(in_state(&w, receiver, "s3"));
    CHECK(queued_of(&w, receiver) == 0u);
    quiescent(&w);
    world_close(&w);
}

/*
 * A fanout that spends its reserve: with macrostepReserve 3, the fourth send
 * of one input finds no envelope and is error.execution, counted as a quota
 * refusal, in the same macrostep. A warm system fails at the same send.
 */
static void test_reserve_exhausted(void) {
    ta_param_value params[] = { { TA_PARAM_MACROSTEP_RESERVE, 3 } };
    for (uint32_t round = 0; round < 2u; round += 1u) {
        world w;
        world_init(&w, params, 1);
        ta_actor_id receiver = spawn(&w, w.chain);
        char target[96];
        session_target(&w, receiver, target, sizeof target);
        ta_definition_id sender_chart = define_sender(&w, target, 4, NULL);
        settle(&w);
        if (round == 1u) {
            /* Warm: the pool has served other inputs first. */
            ta_actor_id warmer = spawn(&w, w.talker);
            settle(&w);
            CHECK(post(&w, warmer, w.go) == TA_OK);
            settle(&w);
        }
        uint64_t before = w.system->counters.quota_refusals;
        ta_actor_id sender = spawn(&w, sender_chart);
        settle(&w);
        CHECK(w.system->counters.quota_refusals == before + 1u);
        CHECK(in_state(&w, sender, "refused"));
        CHECK(in_state(&w, receiver, "s3")); /* the three sends that fitted arrived, in order */
        quiescent(&w);
        world_close(&w);
    }
}

/* A name that a finished session holds is undeliverable: the sender hears error.communication at once. */
static void test_finished_name(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id done = spawn(&w, w.finisher);
    ta_definition_id sender_chart = define_sender(&w, "#_actor_finisher/f", 1, NULL);
    settle(&w);
    CHECK(post(&w, done, w.end) == TA_OK);
    settle(&w);
    w.directory.resident = done;
    ta_actor_id sender = spawn(&w, sender_chart);
    settle(&w);
    CHECK(in_state(&w, sender, "failed"));
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 8. Waiting for an address (ADR 0099 sections 4 and 6)
 */

/* Spawns `definition` at `address`; the directory then says it is resident. */
static ta_actor_id spawn_at(world *w, ta_definition_id definition, const char *family, const char *name) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(w->system,
                         &(ta_actor_spawn_params){ .definition = definition,
                                                   .address = &(ta_actor_address){
                                                       .family = { { .ptr = family, .len = (uint32_t)strlen(family) } },
                                                       .name = { .ptr = name, .len = (uint32_t)strlen(name) } },
                                                   .out = &actor }) == TA_OK);
    w->directory.resident = actor;
    return actor;
}

static ta_result post_to(world *w, ta_address *address, ta_event_name_id name, uint64_t *token) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .address = address, .event = &event,
                                                                   .time = ta_system_time(w->system),
                                                                   .input = token });
}

static ta_result post_to_at(world *w, ta_address *address, ta_event_name_id name, ta_time time) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_actor_deliver(w->system, &(ta_actor_deliver_params){ .address = address, .event = &event, .time = time });
}

/*
 * Makes the mail envelope `sender` would have sent for `name` to `address` and
 * puts it on that address's waiting list, as the macrostep end will once
 * mail to a name is placed here. Safe point: the pool is refilled first.
 */
static void wait_mail(world *w, ta_actor_id sender, ta_address *address, ta_event_name_id name, ta_send_id send_id) {
    ta_system *system = w->system;
    CHECK(ta_envelopes_refill(&system->envelopes, 1u) == TA_OK);
    uint32_t index = ta_envelope_make(&system->envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    CHECK(index != 0);
    ta_envelope *envelope = ta_envelope_at(&system->envelopes, index);
    envelope->source = TA_SOURCE_MAIL;
    envelope->from = sender;
    envelope->due = ta_system_time(system);
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .send_id = send_id, .invoke_id = TA_INVOKE_ID_NONE,
                       .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = ta_actor_lookup(system, sender)->record.self } };
    ta_event_retain(&event, &envelope->event);
    ta_mail_post_waiting(system, index, address);
}

/*
 * Host input for an address with no session waits in arrival order and raises
 * one LOAD_NEEDED notice; a session that becomes resident there takes it, in
 * order, after its bootstrap, and each input completes with its own token.
 */
static void test_host_input_waits_for_the_address(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_address *address = intern_address(&w, "chain", "n1");
    uint64_t tokens[3] = {0};
    CHECK(post_to(&w, address, w.e1, &tokens[0]) == TA_OK);
    CHECK(post_to(&w, address, w.e2, &tokens[1]) == TA_OK);
    CHECK(post_to(&w, address, w.e3, &tokens[2]) == TA_OK);
    CHECK(ta_address_waiting(w.system, address) == 3u);
    CHECK(tokens[0] != 0 && tokens[1] != tokens[0] && tokens[2] != tokens[1]);
    CHECK(w.system->envelopes.pool.used == 3u);
    consistent(&w);
    CHECK(ta_run_pending(w.system)); /* the cue is work */

    ta_run_notice notices[16];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    CHECK(count == 1u && notices[0].kind == TA_NOTICE_LOAD_NEEDED && notices[0].address == address);
    CHECK(!ta_run_pending(w.system)); /* the cue is reported once, however many inputs joined */
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE && count == 0u);
    CHECK(ta_address_waiting(w.system, address) == 3u);

    ta_actor_id actor = spawn_at(&w, w.chain, "chain", "n1");
    CHECK(ta_address_waiting(w.system, address) == 0u);
    CHECK(queued_of(&w, actor) == 3u);
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    uint32_t completed = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (notices[index].kind == TA_NOTICE_INPUT_COMPLETED && notices[index].source == TA_SOURCE_HOST) {
            CHECK(notices[index].actor == actor && notices[index].input == tokens[completed]);
            completed += 1u;
        }
    }
    CHECK(completed == 3u);
    CHECK(in_state(&w, actor, "s3")); /* e1, e2, e3 in the order they arrived */
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE); /* frees the lent events */
    ta_address_release(address);
    quiescent(&w);
    world_close(&w);
}

/* Once the address is resident, a post by address is an admission to its session. */
static void test_post_by_address_to_a_resident_session(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_address *address = intern_address(&w, "chain", "n2");
    ta_actor_id actor = spawn_at(&w, w.chain, "chain", "n2");
    settle(&w);
    uint64_t token = 0;
    CHECK(post_to(&w, address, w.e1, &token) == TA_OK);
    CHECK(ta_address_waiting(w.system, address) == 0u && queued_of(&w, actor) == 1u);
    settle(&w);
    CHECK(in_state(&w, actor, "s1"));
    /* Neither an actor nor an address: nothing to name. */
    ta_event event = { .name = w.e1, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(w.system, &(ta_actor_deliver_params){ .event = &event, .time = ta_system_time(w.system) }) ==
          TA_ERR_NOT_FOUND);
    /* A time that is not now cannot be admitted to an address either (ADR 0094). */
    CHECK(ta_actor_deliver(w.system, &(ta_actor_deliver_params){ .address = address, .event = &event,
                                                                 .time = ta_system_time(w.system) + 1u }) ==
          TA_ERR_INVALID);
    ta_address_release(address);
    quiescent(&w);
    world_close(&w);
}

/*
 * The host cannot load the address: its inputs are refused with the reason,
 * in order, each with its own token and event, and the cue is dropped when
 * the answer comes before it was reported.
 */
static void test_load_failed_refuses_host_input(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_address *address = intern_address(&w, "chain", "n3");
    uint64_t tokens[2] = {0};
    /* An input for an address is for now, as an input for a session is (ADR 0094). */
    CHECK(post_to_at(&w, address, w.e1, ta_system_time(w.system) + 1u) == TA_ERR_INVALID);
    CHECK(post_to(&w, address, w.e2, &tokens[0]) == TA_OK);
    CHECK(post_to(&w, address, w.e3, &tokens[1]) == TA_OK);

    /* Answered before the cue was reported: no LOAD_NEEDED notice at all. */
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_REJECTED }) ==
          TA_OK);
    CHECK(ta_address_waiting(w.system, address) == 0u);
    CHECK(ta_run_pending(w.system));
    ta_run_notice notices[16];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    CHECK(count == 2u && count_kind(notices, count, TA_NOTICE_LOAD_NEEDED) == 0u);
    for (uint32_t index = 0; index < 2u; index += 1u) {
        CHECK(notices[index].kind == TA_NOTICE_INPUT_REFUSED && notices[index].source == TA_SOURCE_HOST);
        CHECK(notices[index].input == tokens[index] && notices[index].reason == TA_REFUSED_LOAD_REJECTED);
        CHECK(notices[index].event != NULL && notices[index].event->name == (index == 0 ? w.e2 : w.e3));
    }
    CHECK(!ta_run_pending(w.system));
    CHECK(w.system->envelopes.pool.used == 2u); /* the events are lent until the next call */
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE && count == 0u);
    CHECK(w.system->envelopes.pool.used == 0u);

    /* A late or repeated answer finds nothing; a bad one is refused. */
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_FAILED }) ==
          TA_ERR_NOT_FOUND);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_DESTROYED }) ==
          TA_ERR_INVALID);
    CHECK(ta_address_load_failed(w.system, NULL) == TA_ERR_INVALID);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .reason = TA_REFUSED_LOAD_FAILED }) ==
          TA_ERR_INVALID);

    /* The next input for the address starts a new list, and a new cue. */
    CHECK(post_to(&w, address, w.e1, NULL) == TA_OK);
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    CHECK(count == 1u && notices[0].kind == TA_NOTICE_LOAD_NEEDED);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_TIMED_OUT }) ==
          TA_OK);
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    CHECK(count == 1u && notices[0].reason == TA_REFUSED_LOAD_TIMED_OUT);
    CHECK(run_notices(&w, notices, 16, &count) == TA_RUN_IDLE);
    ta_address_release(address);
    quiescent(&w);
    world_close(&w);
}

/*
 * Mail that waits and whose load fails comes back to each sender as a
 * data-free error.communication with the send's ID, in the order the mail was
 * sent, using the mail's own envelope; mail whose sender is gone is freed.
 */
static void test_load_failed_recycles_mail(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id first = spawn(&w, w.talker);
    ta_actor_id second = spawn(&w, w.talker);
    ta_actor_id gone = spawn(&w, w.talker);
    settle(&w);
    ta_address *address = intern_address(&w, "chain", "n4");
    wait_mail(&w, first, address, w.e1, 11);
    wait_mail(&w, gone, address, w.e1, 12);
    wait_mail(&w, second, address, w.e2, 13);
    wait_mail(&w, first, address, w.e3, 14);
    CHECK(ta_actor_destroy(w.system, gone) == TA_OK);
    CHECK(ta_address_waiting(w.system, address) == 4u);
    CHECK(queued_of(&w, first) == 0u); /* waiting mail is counted by no session */
    uint64_t pool = w.system->envelopes.pool.used;

    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_NOT_FOUND }) ==
          TA_OK);
    /* The dead sender's mail is freed; the other three are errors on the run queue, in order. */
    CHECK(w.system->envelopes.pool.used == pool - 1u);
    const ta_ilink *link = ta_envelopes_link(&w.system->envelopes);
    ta_send_id expected[3] = { 11, 13, 14 };
    ta_actor_id receivers[3] = { first, second, first };
    uint32_t seen = 0;
    for (uint32_t index = w.system->envelopes.run.head; index != 0; index = ta_ilist_next(link, index)) {
        const ta_envelope *envelope = ta_envelope_at(&w.system->envelopes, index);
        if (seen < 3u) {
            CHECK(envelope->source == TA_SOURCE_ERROR && envelope->target == receivers[seen]);
            CHECK(envelope->event.name == TA_EVENT_NAME_ERROR_COMMUNICATION && envelope->event.send_id == expected[seen]);
            CHECK(envelope->event.data.v == NULL && envelope->from == 0);
        }
        seen += 1u;
    }
    CHECK(seen == 3u);
    CHECK(queued_of(&w, first) == 2u && queued_of(&w, second) == 1u);
    consistent(&w);
    settle(&w);
    CHECK(in_state(&w, first, "failed") && in_state(&w, second, "failed"));
    ta_address_release(address);
    quiescent(&w);
    world_close(&w);
}

/* Mail that waited for an address is delivered, in order, when a session becomes resident there. */
static void test_waiting_mail_is_delivered(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    settle(&w);
    ta_address *address = intern_address(&w, "chain", "n5");
    wait_mail(&w, sender, address, w.e1, 21);
    wait_mail(&w, sender, address, w.e2, 22);
    wait_mail(&w, sender, address, w.e3, 23);
    ta_run_notice notices[4];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 4, &count) == TA_RUN_IDLE);
    CHECK(count == 1u && notices[0].kind == TA_NOTICE_LOAD_NEEDED && notices[0].address == address);
    CHECK(notices[0].source == TA_SOURCE_MAIL);
    ta_actor_id receiver = spawn_at(&w, w.chain, "chain", "n5");
    CHECK(queued_of(&w, receiver) == 3u && ta_address_waiting(w.system, address) == 0u);
    settle(&w);
    CHECK(in_state(&w, receiver, "s3"));
    CHECK(in_state(&w, sender, "a")); /* no error came back */
    ta_address_release(address);
    quiescent(&w);
    world_close(&w);
}

/* More cues than the caller's array holds: the rest are reported by the next call. */
static void test_cues_wait_for_room(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_address *addresses[4];
    const char *names[4] = { "q1", "q2", "q3", "q4" };
    for (uint32_t index = 0; index < 4u; index += 1u) {
        addresses[index] = intern_address(&w, "chain", names[index]);
        CHECK(post_to(&w, addresses[index], w.e1, NULL) == TA_OK);
    }
    ta_run_notice notices[3];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 3, &count) == TA_RUN_NOTICES_FULL && count == 3u);
    for (uint32_t index = 0; index < 3u; index += 1u) {
        CHECK(notices[index].kind == TA_NOTICE_LOAD_NEEDED && notices[index].address == addresses[index]);
    }
    CHECK(ta_run_pending(w.system));
    CHECK(run_notices(&w, notices, 3, &count) == TA_RUN_IDLE);
    CHECK(count == 1u && notices[0].address == addresses[3]);
    for (uint32_t index = 0; index < 4u; index += 1u) {
        CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = addresses[index],
                                                                                .reason = TA_REFUSED_LOAD_FAILED }) ==
              TA_OK);
        ta_address_release(addresses[index]);
    }
    CHECK(run_notices(&w, notices, 3, &count) == TA_RUN_NOTICES_FULL && count == 3u);
    CHECK(run_notices(&w, notices, 3, &count) == TA_RUN_IDLE && count == 1u);
    CHECK(run_notices(&w, notices, 3, &count) == TA_RUN_IDLE && count == 0u);
    quiescent(&w);
    world_close(&w);
}

/*
 * A session unloaded with mail on its way: the mail finds it out when it comes
 * to the head of the run queue, waits for its address (one LOAD_NEEDED), and
 * the load delivers it to the session's new runtime ID.
 */
static void test_unloaded_receiver_is_waited_for(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn_at(&w, w.chain, "chain", "n6");
    settle(&w);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(w.system, &(ta_actor_capture_params){ .actor = receiver, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(w.system, &(ta_actor_unload_params){ .actor = receiver, .image = image }) == TA_OK);
    w.directory.resident = 0;

    /* Mail addressed to the old runtime ID, as a sender that resolved it earlier would have left it. */
    CHECK(ta_envelopes_refill(&w.system->envelopes, 1u) == TA_OK);
    uint32_t index = ta_envelope_make(&w.system->envelopes, NULL, TA_ENVELOPE_RUNNABLE);
    ta_envelope *envelope = ta_envelope_at(&w.system->envelopes, index);
    envelope->source = TA_SOURCE_MAIL;
    envelope->from = sender;
    envelope->target = receiver;
    envelope->due = ta_system_time(w.system);
    ta_event event = { .name = w.e1, .type = TA_EVENT_EXTERNAL, .send_id = 31, .invoke_id = TA_INVOKE_ID_NONE,
                       .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = ta_actor_lookup(w.system, sender)->record.self } };
    ta_event_retain(&event, &envelope->event);
    ta_envelopes_run_push(&w.system->envelopes, index);

    ta_run_notice notices[8];
    uint32_t count = 0;
    CHECK(run_notices(&w, notices, 8, &count) == TA_RUN_IDLE);
    CHECK(count == 1u && notices[0].kind == TA_NOTICE_LOAD_NEEDED);
    ta_address *address = intern_address(&w, "chain", "n6");
    CHECK(notices[0].address == address && ta_address_waiting(w.system, address) == 1u);

    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(w.system, &(ta_actor_load_params){ .image = image, .out = &loaded }) == TA_OK);
    w.directory.resident = loaded;
    CHECK(ta_address_waiting(w.system, address) == 0u && queued_of(&w, loaded) == 1u);
    settle(&w);
    CHECK(in_state(&w, loaded, "s1"));
    ta_session_image_destroy(image);
    ta_address_release(address);
    quiescent(&w);
    world_close(&w);
}

/* A replacement takes the incumbent's queued inputs with the address, in order. */
static void test_retarget_follows_the_address(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id old_one = spawn(&w, w.chain);
    ta_actor_id new_one = spawn(&w, w.chain);
    settle(&w);
    ta_event_name_id names[3] = { w.e1, w.e2, w.e3 };
    for (uint32_t index = 0; index < 3u; index += 1u) {
        CHECK(post(&w, old_one, names[index]) == TA_OK);
    }
    CHECK(post(&w, new_one, w.e1) == TA_OK); /* its own input stays */
    CHECK(queued_of(&w, old_one) == 3u && queued_of(&w, new_one) == 1u);
    ta_mail_retarget(w.system, old_one, new_one);
    CHECK(queued_of(&w, old_one) == 0u && queued_of(&w, new_one) == 4u);
    consistent(&w);
    settle(&w);
    CHECK(in_state(&w, old_one, "s0"));
    CHECK(in_state(&w, new_one, "s3")); /* the incumbent's e1, e2, e3 in order, then its own e1 (ignored in s3) */
    quiescent(&w);
    world_close(&w);
}

/*
 * The waiting paths never allocate once the pool has grown: joining a list,
 * moving it, and recycling it call the allocator zero times.
 */
static void test_waiting_makes_no_allocator_call(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    settle(&w);
    ta_address *address = intern_address(&w, "chain", "n7");
    ta_address *other = intern_address(&w, "chain", "n8");
    CHECK(post_to(&w, address, w.e1, NULL) == TA_OK); /* grows the pool and the table */
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_FAILED }) ==
          TA_OK);
    ta_run_notice notices[16];
    uint32_t count = 0;
    (void)run_notices(&w, notices, 16, &count);
    (void)run_notices(&w, notices, 16, &count);
    uint64_t attempts = heap.stats.attempts;
    for (uint32_t round = 0; round < 50u; round += 1u) {
        wait_mail(&w, sender, address, w.e1, (ta_send_id)(100 + round));
        wait_mail(&w, sender, other, w.e2, (ta_send_id)(200 + round));
    }
    CHECK(ta_address_waiting(w.system, address) == 50u);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = address,
                                                                            .reason = TA_REFUSED_LOAD_FAILED }) ==
          TA_OK);
    CHECK(ta_address_load_failed(w.system, &(ta_address_load_failed_params){ .address = other,
                                                                            .reason = TA_REFUSED_LOAD_UNLOADED }) ==
          TA_OK);
    CHECK(heap.stats.attempts == attempts);
    ta_address_release(address);
    ta_address_release(other);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* ------------------------------------------------------------------------
 * 9. Safe-point mail (due sends, activation sends): the envelope is made from
 * the pool's free quota and joins the run queue at once.
 */

/* Mail sent at a safe point is on the run queue in send order and the receiver runs it in that order. */
static void test_safe_point_order(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    ta_mail_params mail = { .sender = sender, .destination = runtime_ref(receiver),
                            .event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE } };
    ta_event_name_id names[3] = { w.e1, w.e2, w.e3 };
    CHECK(ta_mail_prepare(w.system, 3u) == TA_OK);
    for (uint32_t index = 0; index < 3u; index += 1u) {
        mail.event.name = names[index];
        CHECK(ta_mail_send(w.system, &mail) == TA_MAIL_QUEUED);
    }
    consistent(&w);
    CHECK(!ta_ilist_is_empty(&w.system->envelopes.run)); /* visible at once: no macrostep to wait for */
    CHECK(queued_of(&w, receiver) == 3u);
    CHECK(in_state(&w, receiver, "s0"));
    settle(&w);
    CHECK(in_state(&w, receiver, "s3"));
    quiescent(&w);
    world_close(&w);
}

/*
 * A macrostep paused by a budget keeps the slot while safe points run. Mail
 * made at a safe point then goes to the run queue and is paid from the free
 * quota: the paused macrostep's outgoing list and claim are untouched by it,
 * and its own mail still waits for its end, behind the safe-point mail.
 */
static void test_safe_point_while_paused(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id sender = spawn(&w, w.talker);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    pause_in_macrostep(&w, sender);
    CHECK(send_to(&w, sender, runtime_ref(receiver), w.e2, 0) == TA_MAIL_QUEUED); /* the paused macrostep's own */
    uint64_t held = w.system->runner.slot.claim.held;
    uint32_t outgoing = w.system->runner.slot.outgoing.head;
    ta_mail_params mail = { .sender = sender, .destination = runtime_ref(receiver),
                            .event = { .name = w.e1, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE } };
    CHECK(ta_mail_prepare(w.system, 1u) == TA_OK);
    CHECK(ta_mail_send(w.system, &mail) == TA_MAIL_QUEUED);
    CHECK(w.system->runner.slot.claim.held == held); /* the claim paid nothing for it */
    CHECK(w.system->runner.slot.outgoing.head == outgoing && outgoing == w.system->runner.slot.outgoing.tail);
    CHECK(!ta_ilist_is_empty(&w.system->envelopes.run));
    consistent(&w);
    /* e1 (safe point) reaches the receiver before e2 (sent by the macrostep that was paused). */
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(in_state(&w, receiver, "s2"));
    quiescent(&w);
    world_close(&w);
}

/* ta_mail_prepare grows the pool at a safe point, answers FULL at the bound and changes nothing then. */
static void test_prepare_bounds(void) {
    ta_param_value params[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_MACROSTEP_RESERVE, 2 } };
    world w;
    world_init(&w, params, 2);
    ta_actor_id receiver = spawn(&w, w.chain);
    settle(&w);
    uint32_t bound = w.system->envelopes.bound;
    CHECK(bound == 8u);
    CHECK(ta_mail_prepare(w.system, bound + 1u) == TA_ERR_FULL);
    CHECK(ta_mail_prepare(w.system, bound) == TA_OK);
    ta_mail_params mail = { .sender = 0, .destination = runtime_ref(receiver),
                            .event = { .name = w.e1, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE } };
    for (uint32_t index = 0; index < bound; index += 1u) {
        CHECK(ta_mail_send(w.system, &mail) == TA_MAIL_QUEUED);
    }
    CHECK(ta_mail_send(w.system, &mail) == TA_MAIL_FULL); /* no envelope left, and nothing changed */
    CHECK(queued_of(&w, receiver) == bound);
    CHECK(ta_mail_prepare(w.system, 1u) == TA_ERR_FULL);
    consistent(&w);
    settle(&w);
    quiescent(&w);
    world_close(&w);
}

/* One ta_system_dispatch_due call; answers dispatched, and fills `fired` when it is not NULL. */
static uint32_t dispatch(world *w, ta_due_fired *fired, uint32_t capacity, uint32_t *fired_count, bool *more) {
    uint32_t dispatched = 0;
    CHECK(ta_system_dispatch_due(w->system, &(ta_due_dispatch_params){ .budget = 100, .dispatched = &dispatched,
                                                                      .more_due = more, .fired = fired,
                                                                      .fired_capacity = capacity,
                                                                      .fired_count = fired_count }) == TA_OK);
    return dispatched;
}

/*
 * A due send to a resident session is an envelope: dispatch reports it in
 * `fired`, the macrostep paused meanwhile keeps its claim and outgoing list,
 * and the receiver runs the mail.
 */
static void test_due_send_is_mail(void) {
    world w;
    world_init(&w, NULL, 0);
    ta_actor_id timed = spawn(&w, w.timed);
    ta_actor_id sender = spawn(&w, w.talker);
    settle(&w);
    CHECK(in_state(&w, timed, "waiting"));
    pause_in_macrostep(&w, sender);
    uint64_t held = w.system->runner.slot.claim.held;
    CHECK(ta_system_set_time(w.system, 1000) == TA_OK);
    ta_due_fired fired[4];
    uint32_t fired_count = 99;
    bool more = true;
    CHECK(dispatch(&w, fired, 4, &fired_count, &more) == 1u);
    CHECK(!more && fired_count == 1u && fired[0].delivered && fired[0].due == 1000);
    CHECK(fired[0].sender.kind == TA_ACTOR_REF_RUNTIME && fired[0].sender.as.runtime == timed);
    CHECK(w.system->runner.slot.claim.held == held && ta_ilist_is_empty(&w.system->runner.slot.outgoing));
    CHECK(!ta_ilist_is_empty(&w.system->envelopes.run) && queued_of(&w, timed) == 1u);
    consistent(&w);
    CHECK(run_for(&w, 1000) == TA_RUN_IDLE);
    CHECK(in_state(&w, timed, "ticked"));
    quiescent(&w);
    world_close(&w);
}

/*
 * Dispatch stops, without losing a send, when the pool has no envelope left or
 * the `fired` array is full, and goes on at the next call.
 */
static void test_due_dispatch_stops_when_full(void) {
    ta_param_value params[] = { { TA_PARAM_POOL_ENVELOPES, 8 }, { TA_PARAM_MACROSTEP_RESERVE, 1 } };
    world w;
    world_init(&w, params, 2);
    ta_actor_id actors[10];
    for (uint32_t index = 0; index < 10u; index += 1u) {
        actors[index] = spawn(&w, w.timed);
    }
    settle(&w);
    CHECK(ta_system_set_time(w.system, 1000) == TA_OK);
    ta_due_fired fired[2];
    uint32_t fired_count = 0;
    bool more = false;
    CHECK(dispatch(&w, fired, 2, &fired_count, &more) == 2u && fired_count == 2u && more); /* the array was full */
    uint32_t total = 2u;
    /* Without a `fired` array the pool's bound (8 envelopes, two in use) stops it at six more. */
    uint32_t next = dispatch(&w, NULL, 0, NULL, &more);
    CHECK(next == 6u && more);
    total += next;
    settle(&w); /* the mail is taken, which frees its envelopes */
    total += dispatch(&w, NULL, 0, NULL, &more);
    CHECK(total == 10u && !more);
    settle(&w);
    for (uint32_t index = 0; index < 10u; index += 1u) {
        CHECK(in_state(&w, actors[index], "ticked"));
    }
    quiescent(&w);
    world_close(&w);
}

int main(void) {
    test_interpreter_mail();
    test_reserve_exhausted();
    test_finished_name();
    test_order_and_visibility();
    test_destinations();
    test_waiting_destinations();
    test_other_incarnation_session_waits();
    test_name_resident_by_the_end();
    test_claim();
    test_abandon();
    test_runaway();
    test_receiver_destroyed();
    test_receiver_finished();
    test_sender_gone();
    test_destroyed_in_macrostep();
    test_no_allocation();
    test_host_input_waits_for_the_address();
    test_post_by_address_to_a_resident_session();
    test_load_failed_refuses_host_input();
    test_load_failed_recycles_mail();
    test_waiting_mail_is_delivered();
    test_cues_wait_for_room();
    test_unloaded_receiver_is_waited_for();
    test_retarget_follows_the_address();
    test_waiting_makes_no_allocator_call();
    test_safe_point_order();
    test_safe_point_while_paused();
    test_prepare_bounds();
    test_due_send_is_mail();
    test_due_dispatch_stops_when_full();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "mail_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("mail_test: ok");
    return 0;
}
