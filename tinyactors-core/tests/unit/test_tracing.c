/* The system's tracing (docs/adr/0006 in the repository root, section 5):
 * tracing on by default, contexts across sends between two and three actors,
 * a delayed send, an invocation, a full context pool, sampling and seeds,
 * lifecycle records, the trace on effects, and draining the journal. */
#include "../harness/harness.h"
#include "../support/system_util.h"
#include "system/system_internal.h"

/* ---- reading records ------------------------------------------------------------------- */

typedef struct records {
    ta_trace_record *at[512];
    char names[512][64]; /* a record's event name (macrostep_begin, raised, sent), if any */
    uint32_t count;
} records;

/* The default policy, sampling parts / whole. */
static const ta_trace_policy *sampling(uint32_t parts, uint32_t whole) {
    static ta_trace_policy policy;
    ta_trace_policy_default(&policy);
    policy.sample_parts = parts;
    policy.sample_whole = whole;
    return &policy;
}

static records seen;

static void name_of(const test_system *t, const ta_trace_view *view, uint32_t reference,
                    char *out) {
    ta_string_view s = reference & TA_TRACE_STRING_DYNAMIC
                           ? ta_trace_dynamic_string(view, reference)
                           : ta_image_string(&t->image, reference);
    snprintf(out, 64, "%.*s", (int)s.length, s.pointer ? s.pointer : "");
}

/* Read and acknowledge every record waiting. */
static void collect(test_system *t) {
    ta_trace_view view;
    while (ta_system_trace(t->system, &view)) {
        uint32_t cursor = 0;
        const ta_trace_record *record;
        while ((record = ta_trace_next(&view, &cursor))) {
            if (seen.count == 512) break;
            seen.at[seen.count] = malloc(record->size);
            memcpy(seen.at[seen.count], record, record->size);
            char *name = seen.names[seen.count];
            name[0] = 0;
            if (record->kind == TA_TRACE_MACROSTEP_BEGIN) {
                uint32_t n = ((const ta_trace_macrostep_begin *)ta_trace_body(record))->name;
                if (n != TA_NIL) name_of(t, &view, n, name);
            } else if (record->kind == TA_TRACE_SENT) {
                name_of(t, &view, ((const ta_trace_sent *)ta_trace_body(record))->name, name);
            } else if (record->kind == TA_TRACE_SPAWNED) {
                name_of(t, &view, ((const ta_trace_spawned *)ta_trace_body(record))->name, name);
            }
            seen.count++;
        }
        CHECK_EQUAL(cursor, view.length); /* every byte is a record */
        ta_system_trace_ack(t->system, view.length);
    }
}

static void forget(void) {
    for (uint32_t i = 0; i < seen.count; i++)
        free(seen.at[i]);
    seen.count = 0;
}

/* The first record of `kind` for `session` (and `macrostep` unless ~0, and
 * named `name` unless NULL), or NULL. */
static const ta_trace_record *find(uint32_t kind, uint64_t session, uint64_t macrostep,
                                   const char *name) {
    for (uint32_t i = 0; i < seen.count; i++) {
        const ta_trace_record *r = seen.at[i];
        if (r->kind == kind && r->key.session == session &&
            (macrostep == ~(uint64_t)0 || r->key.macrostep == macrostep) &&
            (!name || !strcmp(seen.names[i], name)))
            return r;
    }
    return NULL;
}

static uint32_t count_kind(uint32_t kind) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < seen.count; i++)
        n += seen.at[i]->kind == kind;
    return n;
}

static uint64_t session_of(test_system *t, uint32_t actor) {
    ta_actor_info info;
    info.size = sizeof info;
    return ta_actor_info_get(t->system, actor, &info) == TA_OK ? info.serial : 0;
}

static ta_span_id span_of(uint64_t session, uint64_t macrostep, uint32_t microstep) {
    return ta_step_key_span_id((ta_step_key){session, macrostep, microstep, 0});
}

static ta_status send_traced(test_system *t, uint32_t actor, const char *name,
                             const char *traceparent) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = actor;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    if (traceparent)
        event.traceparent = (ta_string_view){traceparent, (uint32_t)strlen(traceparent)};
    return ta_system_send(t->system, &event);
}

#define SC(body)                                                                                   \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' " body "</scxml>"

static const char *const PARENT = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

/* ---- tests ------------------------------------------------------------------------------- */

/* With the default configuration every system traces at full detail; off
 * carves nothing and leaves events untraced. */
static void test_on_by_default(void) {
    const char *chart = SC("initial='a'><state id='a'><transition event='e' target='b'/></state>"
                           "<state id='b'/>");
    test_system t;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t, chart, NULL, &config));
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.trace_bytes_capacity, 256 * TA_TRACE_BYTES_PER_EVENT); /* max_events 256 */
    CHECK_EQUAL(stats.trace_contexts_capacity, 256);                         /* max_events */
    size_t on = t.memory_length;
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 10000, 5);
    CHECK_EQUAL(send_traced(&t, a, "e", NULL), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    CHECK_EQUAL(count_kind(TA_TRACE_SPAWNED), 1);
    CHECK_EQUAL(count_kind(TA_TRACE_MACROSTEP_BEGIN), 2);
    CHECK_EQUAL(count_kind(TA_TRACE_MICROSTEP), 2);
    CHECK_EQUAL(count_kind(TA_TRACE_MACROSTEP_END), 2);
    uint64_t s = session_of(&t, a);
    const ta_trace_record *boot = find(TA_TRACE_MACROSTEP_BEGIN, s, 1, NULL),
                          *e = find(TA_TRACE_MACROSTEP_BEGIN, s, 2, "e");
    CHECK(boot && e);
    if (boot && e) { /* each host input starts a trace of its own */
        CHECK(boot->trace.low != e->trace.low || boot->trace.high != e->trace.high);
        CHECK_EQUAL(boot->parent, 0);
        CHECK(boot->flags & TA_TRACE_FLAG_SAMPLED);
    }
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.trace_bytes, 0); /* read */
    CHECK(stats.trace_contexts == 1);  /* only the pinned _event's: the rest are let go */
    CHECK(stats.fuel_clock > 0);
    forget();
    tsys_free(&t);

    test_system untraced;
    config.tracing = TA_TRACING_DISABLED;
    CHECK(tsys_init(&untraced, chart, NULL, &config));
    CHECK(untraced.memory_length < on);
    ta_system_stats_get(untraced.system, &stats);
    CHECK(stats.trace_bytes_capacity == 0 && stats.trace_contexts_capacity == 0);
    CHECK_EQUAL(ta_system_spawn(untraced.system,
                                &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0,
                                            (ta_string_view){PARENT, (uint32_t)strlen(PARENT)}},
                                &a),
                TA_OK);
    tsys_run(&untraced, 10000, 5);
    ta_trace_view view;
    CHECK_EQUAL(ta_system_trace(untraced.system, &view), 0);
    CHECK_EQUAL(ta_system_trace_set(untraced.system, sampling(1, 1)), TA_ERROR_STATE);
    tsys_free(&untraced);
}

/* One input with a traceparent is one trace across three actors: a host
 * span, A's macrostep, B's (caused by A's microstep), C's, and the replies. */
static const char *const CHAIN = SC(
    "initial='wait' name='a'><state id='wait'>"
    "<invoke id='b'><content><scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' "
    "initial='idle' name='b'><state id='idle'>"
    "<invoke id='c'><content><scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' "
    "initial='idle' name='c'><state id='idle'><transition event='ping'><send target='#_parent' "
    "event='pong'/></transition></state></scxml></content></invoke>"
    "<transition event='ping'><send target='#_c' event='ping'/></transition>"
    "<transition event='pong'><send target='#_parent' event='pong'/></transition>"
    "</state></scxml></content></invoke>"
    "<transition event='go'><send target='#_b' event='ping'/></transition>"
    "<transition event='pong' target='done'/></state><final id='done'/>");

static void test_three_actors(void) {
    test_system t;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t, CHAIN, NULL, &config));
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 100000, 10);
    collect(&t);
    uint64_t sa = session_of(&t, a), sb = sa + 1, sc = sa + 2;
    /* the invocations: B's bootstrap is a child of the step of A that invoked
     * it (A's bootstrap's last microstep), in A's bootstrap trace */
    const ta_trace_record *a_boot = find(TA_TRACE_MACROSTEP_BEGIN, sa, 1, NULL),
                          *b_boot = find(TA_TRACE_MACROSTEP_BEGIN, sb, 1, NULL),
                          *c_boot = find(TA_TRACE_MACROSTEP_BEGIN, sc, 1, NULL),
                          *b_spawned = find(TA_TRACE_SPAWNED, sb, ~(uint64_t)0, "b");
    CHECK(a_boot && b_boot && c_boot && b_spawned);
    if (a_boot && b_boot && c_boot && b_spawned) {
        CHECK(b_boot->trace.low == a_boot->trace.low && c_boot->trace.low == a_boot->trace.low);
        CHECK_EQUAL(b_boot->parent, span_of(sa, 1, 1));
        CHECK_EQUAL(c_boot->parent, span_of(sb, 1, 1));
        const ta_trace_macrostep_begin *cause = ta_trace_body(b_boot);
        CHECK(cause->cause.session == sa && cause->cause.macrostep == 1 &&
              cause->cause.microstep == 1);
        CHECK_EQUAL(b_spawned->parent, span_of(sa, 1, 1));
        CHECK_EQUAL(b_spawned->trace.low, a_boot->trace.low);
        CHECK_EQUAL(b_spawned->key.microstep, TA_TRACE_STEP_SPAWNED);
    }
    const ta_trace_record *invoked = find(TA_TRACE_INVOKED, sa, 1, NULL);
    CHECK(invoked != NULL);
    if (invoked) {
        const ta_trace_invoked *body = ta_trace_body(invoked);
        CHECK_EQUAL(body->child_session, sb);
        CHECK_EQUAL(invoked->parent, span_of(sa, 1, 0));
    }
    forget();

    /* go: the caller's span is the root */
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    tsys_run(&t, 100000, 10);
    collect(&t);
    const ta_trace_record *a_go = find(TA_TRACE_MACROSTEP_BEGIN, sa, 2, "go"),
                          *b_ping = find(TA_TRACE_MACROSTEP_BEGIN, sb, 2, "ping"),
                          *c_ping = find(TA_TRACE_MACROSTEP_BEGIN, sc, 2, "ping"),
                          *b_pong = find(TA_TRACE_MACROSTEP_BEGIN, sb, 3, "pong"),
                          *a_pong = find(TA_TRACE_MACROSTEP_BEGIN, sa, 3, "pong");
    CHECK(a_go && b_ping && c_ping && b_pong && a_pong);
    if (a_go && b_ping && c_ping && b_pong && a_pong) {
        const ta_trace_record *chain[] = {a_go, b_ping, c_ping, b_pong, a_pong};
        for (int i = 0; i < 5; i++)
            CHECK(chain[i]->trace.high == 0x0af7651916cd43ddull &&
                  chain[i]->trace.low == 0x8448eb211c80319cull);
        CHECK_EQUAL(a_go->parent, 0xb7ad6b7169203331ull);
        const ta_trace_macrostep_begin *cause = ta_trace_body(a_go);
        CHECK(cause->cause.session == 0 && cause->cause.macrostep == 0xb7ad6b7169203331ull);
        CHECK_EQUAL(b_ping->parent, span_of(sa, 2, 1));
        CHECK_EQUAL(c_ping->parent, span_of(sb, 2, 1));
        CHECK_EQUAL(b_pong->parent, span_of(sc, 2, 1));
        CHECK_EQUAL(a_pong->parent, span_of(sb, 3, 1));
        cause = ta_trace_body(a_pong);
        CHECK(cause->cause.session == sb && cause->cause.macrostep == 3 &&
              cause->cause.microstep == 1);
        CHECK_EQUAL(cause->origin_session, sb);
    }
    /* A finished: its end is in the same trace; then everyone is destroyed */
    const ta_trace_record *finished = find(TA_TRACE_FINISHED, sa, ~(uint64_t)0, NULL);
    CHECK(finished != NULL);
    if (finished) {
        CHECK_EQUAL(finished->trace.low, 0x8448eb211c80319cull);
        CHECK_EQUAL(finished->key.microstep, TA_TRACE_STEP_FINISHED);
        CHECK_EQUAL(((const ta_trace_finished *)ta_trace_body(finished))->final_state,
                    ta_image_state_by_name(&t.image, 0, (ta_string_view){"done", 4}));
    }
    CHECK(find(TA_TRACE_DESTROYED, sa, ~(uint64_t)0, NULL) != NULL);
    CHECK(find(TA_TRACE_DESTROYED, sb, ~(uint64_t)0, NULL) != NULL);
    CHECK(find(TA_TRACE_DESTROYED, sc, ~(uint64_t)0, NULL) != NULL);
    CHECK_EQUAL(t.effect_count[TA_EFFECT_DONE], 1);
    CHECK_EQUAL(t.last[TA_EFFECT_DONE].trace.trace.low, 0x8448eb211c80319cull);
    CHECK_EQUAL(t.last[TA_EFFECT_DONE].trace.parent, span_of(sa, 3, 1)); /* its last step */
    forget();
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.trace_contexts, 0);
    tsys_free(&t);
}

/* A delayed send keeps its cause while it waits; a delayed send of an
 * untraced macrostep starts a trace when it fires. */
static void test_delayed_send(void) {
    test_system t;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='go'>"
                       "<send event='tick' delay='1s'/></transition>"
                       "<transition event='tick' target='b'/></state><state id='b'/>"),
                    NULL, &config));
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 10000, 5);
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    tsys_run(&t, 10000, 5);
    ta_system_advance_time(t.system, 2000);
    tsys_run(&t, 10000, 5);
    collect(&t);
    uint64_t s = session_of(&t, a);
    const ta_trace_record *tick = find(TA_TRACE_MACROSTEP_BEGIN, s, 3, "tick"),
                          *sent = find(TA_TRACE_SENT, s, 2, "tick");
    CHECK(tick && sent);
    if (tick && sent) {
        CHECK_EQUAL(tick->trace.low, 0x8448eb211c80319cull);
        CHECK_EQUAL(tick->parent, span_of(s, 2, 1));
        CHECK_EQUAL(sent->span, span_of(s, 2, 1));
        CHECK_EQUAL(((const ta_trace_sent *)ta_trace_body(sent))->delay, 1000000000ull);
        CHECK(tick->now == 2000 && tick->fuel > sent->fuel);
    }
    forget();
    tsys_free(&t);

    /* untraced: nothing sampled, but a traceparent keeps its caller's flag */
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='go'>"
                       "<send event='tick' delay='1s'/></transition>"
                       "<transition event='tick' target='b'/></state><state id='b'/>"),
                    NULL, &config));
    CHECK_EQUAL(ta_system_trace_set(t.system, sampling(0, 1)), TA_OK); /* sample nothing */
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 10000, 5);
    CHECK_EQUAL(send_traced(&t, a, "go", NULL), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    s = session_of(&t, a);
    CHECK_EQUAL(count_kind(TA_TRACE_MACROSTEP_BEGIN), 0); /* unsampled */
    CHECK_EQUAL(count_kind(TA_TRACE_SPAWNED), 1);         /* lifecycle: whatever the sampling */
    const ta_trace_record *spawned = find(TA_TRACE_SPAWNED, s, ~(uint64_t)0, NULL);
    if (spawned) /* in its unsampled trace, which has an ID of its own (docs/adr/0007) */
        CHECK((spawned->trace.high || spawned->trace.low) &&
              !(spawned->flags & TA_TRACE_FLAG_SAMPLED));
    forget();
    CHECK_EQUAL(ta_system_trace_set(t.system, sampling(1, 1)), TA_OK); /* sample everything again */
    ta_system_advance_time(t.system, 2000);
    tsys_run(&t, 10000, 5);
    collect(&t);
    /* the delayed send kept its unsampled trace (docs/adr/0007): the ratio
     * decides when a trace starts, not when its events arrive */
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, s, 3, "tick") == NULL);
    forget();
    CHECK_EQUAL(ta_system_trace_set(t.system, sampling(0, 1)), TA_OK);
    const char *sampled = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01",
               *unsampled = "00-0af7651916cd43dd8448eb211c80319d-b7ad6b7169203331-00";
    CHECK_EQUAL(send_traced(&t, a, "x", sampled), TA_OK);
    CHECK_EQUAL(send_traced(&t, a, "y", unsampled), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, s, 4, "x") != NULL);
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, s, 5, "y") == NULL);
    forget();
    tsys_free(&t);
}

/* A full context pool leaves events untraced and counts them; the run goes
 * on as if nothing happened. */
static void test_full_pool(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.trace_contexts = 2;
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='e' target='b'/></state>"
                       "<state id='b'/>"),
                    NULL, &config));
    uint32_t actors[6];
    for (int i = 0; i < 6; i++)
        CHECK_EQUAL(
            ta_system_spawn(t.system,
                            &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}},
                            &actors[i]),
            TA_OK);
    for (int i = 0; i < 6; i++)
        CHECK_EQUAL(send_traced(&t, actors[i], "e", NULL), TA_OK);
    CHECK_EQUAL(tsys_run(&t, 100000, 10), TA_RUN_IDLE);
    for (int i = 0; i < 6; i++)
        CHECK(strcmp(tsys_config(&t, actors[i]), "b") == 0);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK(stats.trace_contexts_dropped > 0);
    CHECK_EQUAL(stats.trace_contexts, 0);
    collect(&t);
    CHECK_EQUAL(count_kind(TA_TRACE_SPAWNED), 6);
    CHECK(count_kind(TA_TRACE_MACROSTEP_BEGIN) < 12);
    forget();
    tsys_free(&t);
}

/* The trace an I/O processor gets: a SEND effect names the sending
 * microstep's span, so a traceparent made from it continues the trace. */
static void test_effects(void) {
    static const ta_io_processor_descriptor processors[] = {
        {{"http://example.com/out", 22}, {"", 0}, 0, 0}};
    test_system t;
    ta_system_config config = test_system_config();
    config.io_processors = processors;
    config.io_processor_count = 1;
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='go'>"
                       "<log label='l' expr='1'/>"
                       "<send type='http://example.com/out' target='x' event='out'/>"
                       "</transition></state>"),
                    NULL, &config));
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 10000, 5);
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    ta_run run = ta_system_run(t.system, 10000);
    CHECK_EQUAL(run.effect_count, 2);
    const ta_effect *effects;
    CHECK_EQUAL(ta_system_effects(t.system, &effects), 2);
    uint64_t s = session_of(&t, a);
    for (int i = 0; i < 2; i++) {
        CHECK(effects[i].kind == TA_EFFECT_LOG || effects[i].kind == TA_EFFECT_SEND);
        CHECK_EQUAL(effects[i].trace.trace.low, 0x8448eb211c80319cull);
        CHECK_EQUAL(effects[i].trace.parent, span_of(s, 2, 1));
        CHECK_EQUAL(effects[i].trace.flags & TA_TRACE_FLAG_SAMPLED, TA_TRACE_FLAG_SAMPLED);
    }
    char text[TA_TRACEPARENT_LENGTH + 1] = {0};
    CHECK_EQUAL(
        ta_traceparent_format(&effects[1].trace, &(ta_buffer){text, TA_TRACEPARENT_LENGTH, 0}),
        TA_OK);
    ta_trace_context back;
    CHECK_EQUAL(ta_traceparent_parse((ta_string_view){text, TA_TRACEPARENT_LENGTH}, &back), TA_OK);
    CHECK_EQUAL(back.parent, span_of(s, 2, 1));
    ta_system_effects_ack(t.system, 2);
    collect(&t);
    const ta_trace_record *log = find(TA_TRACE_LOG, s, 2, NULL);
    CHECK(log != NULL);
    if (log) CHECK_EQUAL(log->key.microstep, 1);
    forget();
    tsys_free(&t);
}

/* The same seed gives the same trace IDs; another seed others. */
static void test_seed(void) {
    ta_trace_id first[2][2];
    for (int round = 0; round < 2; round++) {
        for (int seeded = 0; seeded < 2; seeded++) {
            test_system t;
            ta_system_config config = test_system_config();
            config.trace_seed = seeded ? 42 : 7;
            CHECK(tsys_init(&t, SC("initial='a'><state id='a'/>"), NULL, &config));
            uint32_t a = 0;
            ta_system_spawn(t.system,
                            &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}},
                            &a);
            tsys_run(&t, 10000, 5);
            collect(&t);
            const ta_trace_record *boot =
                find(TA_TRACE_MACROSTEP_BEGIN, session_of(&t, a), 1, NULL);
            CHECK(boot != NULL);
            if (boot) {
                if (round == 0)
                    first[seeded][0] = boot->trace;
                else
                    CHECK(first[seeded][0].high == boot->trace.high &&
                          first[seeded][0].low == boot->trace.low);
            }
            forget();
            tsys_free(&t);
        }
    }
    CHECK(first[0][0].low != first[1][0].low);
}

/* Acknowledging part of what is waiting keeps the rest, whole records. */
static void test_partial_ack(void) {
    test_system t;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t, SC("initial='a'><state id='a'><transition event='e' target='a'/></state>"),
                    NULL, &config));
    uint32_t a = 0;
    ta_system_spawn(t.system,
                    &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a);
    tsys_run(&t, 10000, 5);
    ta_trace_view view;
    uint32_t length = ta_system_trace(t.system, &view), cursor = 0;
    CHECK(length > 0);
    const ta_trace_record *first = ta_trace_next(&view, &cursor);
    CHECK(first && first->kind == TA_TRACE_SPAWNED);
    ta_system_trace_ack(t.system, cursor + 4); /* past the first record: rounds down */
    CHECK_EQUAL(ta_system_trace(t.system, &view), length - cursor);
    cursor = 0;
    CHECK(ta_trace_next(&view, &cursor)->kind == TA_TRACE_MACROSTEP_BEGIN);
    ta_system_trace_ack(t.system, view.length);
    CHECK_EQUAL(ta_system_trace(t.system, &view), 0);
    /* the fuel clock only goes forward */
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    uint64_t clock = stats.fuel_clock;
    ta_system_trace_clock(t.system, clock - 1);
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.fuel_clock, clock);
    ta_system_trace_clock(t.system, clock + 1000);
    send_traced(&t, a, "e", NULL);
    tsys_run(&t, 10000, 5);
    collect(&t);
    const ta_trace_record *e = find(TA_TRACE_MACROSTEP_BEGIN, session_of(&t, a), 2, "e");
    CHECK(e && e->fuel >= clock + 1000);
    forget();
    tsys_free(&t);
}

/* Relocation keeps the journal, the contexts and their references. */
static void test_relocation(void) {
    test_system t;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='go'>"
                       "<send event='tick' delay='1s'/></transition>"
                       "<transition event='tick' target='b'/></state><state id='b'/>"),
                    NULL, &config));
    uint32_t a = 0;
    ta_system_spawn(t.system,
                    &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a);
    tsys_run(&t, 10000, 5);
    send_traced(&t, a, "go", PARENT);
    tsys_run(&t, 10000, 5); /* the tick waits, with its context, and records wait */
    ta_system_config bigger = config;
    bigger.max_events = 512;
    size_t need;
    CHECK_EQUAL(ta_system_layout(&t.image, &bigger, &need), TA_OK);
    void *memory = malloc(need);
    ta_system *moved = t.system;
    CHECK_EQUAL(ta_system_relocate(&moved, (ta_memory){memory, need}, &bigger), TA_OK);
    free(t.memory);
    t.memory = memory;
    t.system = moved;
    ta_system_advance_time(t.system, 2000);
    tsys_run(&t, 10000, 5);
    collect(&t);
    uint64_t s = session_of(&t, a);
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, s, 2, "go") != NULL); /* written before the move */
    const ta_trace_record *tick = find(TA_TRACE_MACROSTEP_BEGIN, s, 3, "tick");
    CHECK(tick && tick->parent == span_of(s, 2, 1) && tick->trace.low == 0x8448eb211c80319cull);
    forget();
    tsys_free(&t);
}

/* ---- durability (docs/adr/0006, section 6) ------------------------------------------- */

#define NULLSC(body)                                                                               \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' " body "</scxml>"

/* a sender (session 1) that pings session 2 on go, and sends itself a tick
 * in a second on later; a receiver (session 2) */
static const char *const SENDER = NULLSC(
    "initial='a'><state id='a'><transition event='go'><send target='#_scxml_2' event='ping'/>"
    "</transition><transition event='later'><send event='tick' delay='1s'/></transition>"
    "<transition event='tick' target='b'/></state><state id='b'/>");

static uint32_t wake_requests(test_system *t) {
    const ta_effect *effect;
    uint32_t n = ta_system_effects(t->system, &effect), wakes = 0;
    for (uint32_t i = 0; i < n; i++)
        wakes += effect[i].kind == TA_EFFECT_WAKE;
    ta_system_effects_ack(t->system, n);
    return wakes;
}

/* An event waits for its hibernated receiver with its context: the
 * receiver's macrostep is still a child of the sending microstep. */
static void test_hibernated_receiver(void) {
    test_system t;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t, SENDER, NULL, &config));
    uint32_t a = 0, b = 0;
    ta_system_spawn(t.system,
                    &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a);
    ta_system_spawn(t.system,
                    &(ta_spawn){0, 0, (ta_string_view){"b", 1}, 0, (ta_string_view){0, 0}}, &b);
    tsys_run(&t, 10000, 5);
    static unsigned char blob[4096];
    size_t length = 0;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_hibernate(t.system, b, &written), TA_OK);
    length = written.length;
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    ta_system_run(t.system, 10000);
    CHECK_EQUAL(wake_requests(&t), 1); /* ping waits in b's mailbox */
    /* the waiting event keeps its context across a snapshot and a restore */
    size_t snapshot_length = 0;
    written = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &written), TA_ERROR_NO_MEMORY);
    snapshot_length = written.length;
    void *snapshot = malloc(snapshot_length), *memory = malloc(t.memory_length);
    written = (ta_buffer){snapshot, snapshot_length, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &written), TA_OK);
    snapshot_length = written.length;
    ta_system *restored = NULL;
    CHECK_EQUAL(
        ta_system_restore(
            &(ta_system_setup){{memory, t.memory_length}, &t.image, &config, &t.binding, 1, 0},
            (ta_bytes){snapshot, snapshot_length}, &restored),
        TA_OK);
    free(snapshot);
    free(t.memory); /* the restored system owns the actors now */
    t.memory = memory;
    t.system = restored;
    CHECK_EQUAL(ta_actor_wake(t.system, b, (ta_bytes){blob, length}), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    const ta_trace_record *ping = find(TA_TRACE_MACROSTEP_BEGIN, 2, ~(uint64_t)0, "ping"),
                          *go = find(TA_TRACE_MACROSTEP_BEGIN, 1, ~(uint64_t)0, "go");
    CHECK(ping && go);
    if (ping && go) {
        CHECK_EQUAL(ping->trace.low, 0x8448eb211c80319cull);
        CHECK_EQUAL(ping->parent, span_of(1, go->key.macrostep, 1));
        CHECK_EQUAL(ping->key.macrostep, 2); /* the counters came back with the blob */
    }
    const ta_trace_record *unloaded = find(TA_TRACE_UNLOADED, 2, ~(uint64_t)0, NULL),
                          *loaded = find(TA_TRACE_LOADED, 2, ~(uint64_t)0, NULL);
    CHECK(unloaded && loaded);
    if (unloaded && loaded) {
        CHECK_EQUAL(unloaded->key.microstep, TA_TRACE_STEP_UNLOADED);
        CHECK_EQUAL(loaded->key.macrostep, 1);
        CHECK(loaded->span != unloaded->span);
    }
    forget();
    tsys_free(&t);
}

/* A delayed send leaves with its exported sender and keeps its context
 * where the sender is adopted. */
static void test_export_adopt(void) {
    test_system t, u;
    ta_system_config config = test_system_config();
    CHECK(tsys_init(&t, SENDER, NULL, &config));
    CHECK(tsys_init(&u, SENDER, NULL, &config));
    uint32_t a = 0;
    ta_system_spawn(t.system,
                    &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a);
    tsys_run(&t, 10000, 5);
    CHECK_EQUAL(send_traced(&t, a, "later", PARENT), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    const ta_trace_record *later = find(TA_TRACE_MACROSTEP_BEGIN, 1, ~(uint64_t)0, "later");
    uint64_t macrostep = later ? later->key.macrostep : 0;
    forget();
    static unsigned char blob[8192];
    size_t length = 0;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(t.system, a, &written), TA_OK);
    length = written.length;
    uint32_t adopted = 0;
    CHECK_EQUAL(ta_system_adopt(u.system, (ta_bytes){blob, length}, &adopted), TA_OK);
    ta_system_advance_time(u.system, 2000);
    tsys_run(&u, 10000, 5);
    collect(&u);
    const ta_trace_record *tick = find(TA_TRACE_MACROSTEP_BEGIN, 1, ~(uint64_t)0, "tick");
    CHECK(tick != NULL);
    if (tick) {
        CHECK_EQUAL(tick->trace.low, 0x8448eb211c80319cull);
        CHECK_EQUAL(tick->parent, span_of(1, macrostep, 1));
        CHECK_EQUAL(tick->key.macrostep, macrostep + 1);
    }
    CHECK(find(TA_TRACE_LOADED, 1, ~(uint64_t)0, NULL) != NULL);
    forget();
    collect(&t);
    CHECK(find(TA_TRACE_UNLOADED, 1, ~(uint64_t)0, NULL) != NULL);
    CHECK(find(TA_TRACE_DESTROYED, 1, ~(uint64_t)0, NULL) == NULL); /* it left, it lives on */
    forget();
    tsys_free(&t);
    tsys_free(&u);
}

/* Detail classes (docs/adr/0007): a sampled trace is TA_TRACE_DETAIL_SAMPLED
 * and writes that class's kinds; the class is in a record's flags and in an
 * effect's context, never in traceparent text. */
static void test_detail_classes(void) {
    static const ta_io_processor_descriptor processors[] = {
        {{"http://example.com/out", 22}, {"", 0}, 0, 0}};
    ta_trace_policy policy;
    ta_trace_policy_default(&policy);
    policy.kinds[TA_TRACE_DETAIL_SAMPLED] = TA_TRACE_SUMMARY; /* no microsteps, no sends */
    test_system t;
    ta_system_config config = test_system_config();
    config.io_processors = processors;
    config.io_processor_count = 1;
    config.trace_policy = &policy;
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='go'>"
                       "<send type='http://example.com/out' target='x' event='out'/>"
                       "</transition></state>"),
                    NULL, &config));
    policy.kinds[TA_TRACE_DETAIL_SAMPLED] = TA_TRACE_FULL; /* copied at init: no effect */
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 10000, 5);
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    ta_system_run(t.system, 10000);
    const ta_effect *effects;
    CHECK_EQUAL(ta_system_effects(t.system, &effects), 1);
    CHECK_EQUAL(TA_TRACE_DETAIL(effects[0].trace.flags), TA_TRACE_DETAIL_SAMPLED);
    char text[TA_TRACEPARENT_LENGTH + 1] = {0};
    CHECK_EQUAL(
        ta_traceparent_format(&effects[0].trace, &(ta_buffer){text, TA_TRACEPARENT_LENGTH, 0}),
        TA_OK);
    CHECK(strcmp(text + 53, "01") == 0); /* the W3C flags alone */
    ta_system_effects_ack(t.system, 1);
    collect(&t);
    uint64_t s = session_of(&t, a);
    const ta_trace_record *begin = find(TA_TRACE_MACROSTEP_BEGIN, s, 2, "go");
    CHECK(begin != NULL);
    if (begin) CHECK_EQUAL(TA_TRACE_DETAIL(begin->flags), TA_TRACE_DETAIL_SAMPLED);
    CHECK_EQUAL(count_kind(TA_TRACE_MICROSTEP), 0);
    CHECK_EQUAL(count_kind(TA_TRACE_SENT), 0);
    CHECK(count_kind(TA_TRACE_MACROSTEP_END) >= 1);
    forget();
    /* the same system, another policy: the class now writes every kind */
    ta_trace_policy_default(&policy);
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    CHECK_EQUAL(count_kind(TA_TRACE_MICROSTEP), 1);
    CHECK_EQUAL(count_kind(TA_TRACE_SENT), 1);
    forget();
    /* a class that writes nothing: no step record, lifecycle as its class says */
    policy.kinds[TA_TRACE_DETAIL_SAMPLED] = 0;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    CHECK_EQUAL(send_traced(&t, a, "go", PARENT), TA_OK);
    tsys_run(&t, 10000, 5);
    collect(&t);
    CHECK_EQUAL(seen.count, 0);
    forget();
    tsys_free(&t);
}

/* An unsampled trace keeps its ID (docs/adr/0007): what its steps send
 * carries it with the sampled flag clear, so a service downstream continues
 * the caller's decision. */
static void test_unsampled_propagates(void) {
    static const ta_io_processor_descriptor processors[] = {
        {{"http://example.com/out", 22}, {"", 0}, 0, 0}};
    test_system t;
    ta_system_config config = test_system_config();
    config.io_processors = processors;
    config.io_processor_count = 1;
    config.trace_policy = sampling(0, 1);
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition event='go'>"
                       "<send type='http://example.com/out' target='x' event='out'/>"
                       "</transition></state>"),
                    NULL, &config));
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t.system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(&t, 10000, 5);
    const char *unsampled = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00";
    CHECK_EQUAL(send_traced(&t, a, "go", unsampled), TA_OK);
    ta_system_run(t.system, 10000);
    const ta_effect *effects;
    CHECK_EQUAL(ta_system_effects(t.system, &effects), 1);
    CHECK_EQUAL(effects[0].trace.trace.low, 0x8448eb211c80319cull); /* the caller's trace */
    CHECK_EQUAL(effects[0].trace.flags & TA_TRACE_FLAG_SAMPLED, 0);
    CHECK_EQUAL(TA_TRACE_DETAIL(effects[0].trace.flags), TA_TRACE_DETAIL_UNSAMPLED);
    char text[TA_TRACEPARENT_LENGTH + 1] = {0};
    CHECK_EQUAL(
        ta_traceparent_format(&effects[0].trace, &(ta_buffer){text, TA_TRACEPARENT_LENGTH, 0}),
        TA_OK);
    CHECK(strcmp(text + 53, "00") == 0);
    ta_system_effects_ack(t.system, 1);
    collect(&t);
    CHECK_EQUAL(count_kind(TA_TRACE_MACROSTEP_BEGIN), 0); /* nothing of it recorded */
    forget();
    tsys_free(&t);
}

/* A round of CHAIN: spawn A (which invokes B, which invokes C), send it
 * `go` without a traceparent, run until it is done. Returns A's session. */
static uint64_t chain_round(test_system *t) {
    uint32_t a = 0;
    CHECK_EQUAL(
        ta_system_spawn(t->system,
                        &(ta_spawn){0, 0, (ta_string_view){"a", 1}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    tsys_run(t, 100000, 10);
    uint64_t session = session_of(t, a);
    CHECK_EQUAL(send_traced(t, a, "go", NULL), TA_OK);
    tsys_run(t, 100000, 10);
    collect(t);
    return session;
}

/* Rules (docs/adr/0007): with nothing sampled, a rule at take samples B's
 * steps, in full, and what they cause (C's, the replies); a rate limit lets
 * one trace through per second of logical time; a rule at start samples a
 * host input by its event; a rule drops one chart's steps from sampled
 * traces. */
static void test_rules(void) {
    ta_trace_rule watch;
    memset(&watch, 0, sizeof watch);
    watch.size = sizeof watch;
    watch.at = TA_TRACE_AT_TAKE;
    watch.actor = (ta_string_view){"b", 1};
    watch.sample = TA_TRACE_SAMPLE;
    watch.detail = TA_TRACE_DETAIL_WATCHED;
    ta_trace_policy policy;
    ta_trace_policy_default(&policy);
    policy.sample_parts = 0; /* nothing sampled ... */
    policy.kinds[TA_TRACE_DETAIL_SAMPLED] = TA_TRACE_SUMMARY;
    policy.rules = &watch; /* ... but B */
    policy.rule_count = 1;
    test_system t;
    ta_system_config config = test_system_config();
    config.trace_policy = &policy;
    CHECK(tsys_init(&t, CHAIN, NULL, &config));
    uint64_t sa = chain_round(&t), sb = sa + 1, sc = sa + 2;
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, sa, 2, "go") == NULL); /* A's input: unsampled */
    const ta_trace_record *b = find(TA_TRACE_MACROSTEP_BEGIN, sb, ~(uint64_t)0, "ping"),
                          *c = find(TA_TRACE_MACROSTEP_BEGIN, sc, ~(uint64_t)0, "ping"),
                          *back = find(TA_TRACE_MACROSTEP_BEGIN, sa, ~(uint64_t)0, "pong");
    CHECK(b && c && back);
    if (b && c && back) {
        CHECK_EQUAL(TA_TRACE_DETAIL(b->flags), TA_TRACE_DETAIL_WATCHED);
        CHECK_EQUAL(b->parent, 0); /* a new trace: what sent it was not traced */
        CHECK(c->trace.low == b->trace.low && back->trace.low == b->trace.low);
        CHECK_EQUAL(TA_TRACE_DETAIL(c->flags), TA_TRACE_DETAIL_WATCHED); /* caused: watched too */
        CHECK_EQUAL(c->parent, span_of(sb, b->key.macrostep, 1));
    }
    CHECK(count_kind(TA_TRACE_MICROSTEP) >= 3); /* the watched class writes every kind */
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK(stats.traces_sampled >= 1 && stats.traces_started > stats.traces_sampled);
    forget();

    /* at most one trace a second (logical time) */
    watch.per_second = 1;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    chain_round(&t);
    CHECK(count_kind(TA_TRACE_MACROSTEP_BEGIN) > 0);
    forget();
    chain_round(&t);
    CHECK_EQUAL(count_kind(TA_TRACE_MACROSTEP_BEGIN), 0); /* the bucket is empty */
    forget();
    ta_system_advance_time(t.system, 2 * config.ticks_per_second);
    chain_round(&t);
    CHECK(count_kind(TA_TRACE_MACROSTEP_BEGIN) > 0);
    forget();

    /* at start, by event: A's `go` is sampled, at the default class */
    memset(&watch, 0, sizeof watch);
    watch.size = sizeof watch;
    watch.at = TA_TRACE_AT_START;
    watch.event = (ta_string_view){"go.*", 4};
    watch.sample = TA_TRACE_SAMPLE;
    watch.detail = TA_NIL;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    sa = chain_round(&t);
    const ta_trace_record *go = find(TA_TRACE_MACROSTEP_BEGIN, sa, 2, "go");
    CHECK(go != NULL);
    if (go) CHECK_EQUAL(TA_TRACE_DETAIL(go->flags), TA_TRACE_DETAIL_SAMPLED);
    CHECK_EQUAL(count_kind(TA_TRACE_MICROSTEP), 0); /* the sampled class: summary */
    forget();

    /* everything sampled, but C's steps dropped */
    memset(&watch, 0, sizeof watch);
    watch.size = sizeof watch;
    watch.at = TA_TRACE_AT_TAKE;
    watch.chart = (ta_string_view){"c", 1};
    watch.sample = TA_TRACE_DROP;
    watch.detail = TA_NIL;
    policy.sample_parts = 1;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    sa = chain_round(&t);
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, sa + 1, ~(uint64_t)0, "ping") != NULL);
    CHECK(find(TA_TRACE_MACROSTEP_BEGIN, sa + 2, ~(uint64_t)0, "ping") == NULL);
    forget();

    /* malformed rules change nothing */
    watch.detail = TA_TRACE_DETAILS;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_ERROR_INVALID);
    watch.detail = TA_NIL;
    ta_trace_rule many[17];
    for (int i = 0; i < 17; i++) {
        many[i] = watch;
        many[i].actor = (ta_string_view){"x*", 2};
    }
    policy.rules = many;
    policy.rule_count = 17;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_ERROR_NO_MEMORY); /* 16 by default */
    policy.rule_count = 16;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    tsys_free(&t);
}

int main(void) {
    RUN(test_on_by_default);
    RUN(test_three_actors);
    RUN(test_delayed_send);
    RUN(test_full_pool);
    RUN(test_effects);
    RUN(test_seed);
    RUN(test_partial_ack);
    RUN(test_relocation);
    RUN(test_hibernated_receiver);
    RUN(test_export_adopt);
    RUN(test_detail_classes);
    RUN(test_rules);
    RUN(test_unsampled_propagates);
    TEST_MAIN_END();
}
