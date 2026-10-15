/* Inputs during a run (host.h, "During a run"; docs/adr/0009 in the
 * repository root, decision 8): a datamodel the host calls while it runs may
 * spawn, send, send many and terminate. Each <script> of these charts runs
 * one C action against the host from inside the run, through a datamodel
 * that wraps the ECMAScript one. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"
#include "../support/host_config.h"

#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"

/* <script> enters `busy`, then an internal event leaves it in the same
 * macrostep; an external `poke` is counted and logged. */
#define CALLER                                                                                     \
    SC("caller",                                                                                   \
       "initial='idle'><datamodel><data id='n' expr='0'/></datamodel>"                             \
       "<state id='idle'><transition event='go' target='busy'/>"                                   \
       "<transition event='poke'><assign location='n' expr='n + 1'/>"                              \
       "<log expr=\"'poke:' + n\"/></transition></state>"                                          \
       "<state id='busy'><onentry><script>1</script><raise event='internal'/></onentry>"           \
       "<transition event='internal' target='idle'><log expr=\"'internal'\"/></transition>"        \
       "</state>")
#define LISTENER                                                                                   \
    SC("listener", "initial='l'><state id='l'><onentry><log expr=\"'listening'\"/></onentry>"      \
                   "<transition event='*'><log expr=\"'got:' + _event.name\"/></transition>"       \
                   "</state>")

#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        CHECK(strcmp((a), (b)) == 0);                                                              \
        if (strcmp((a), (b)) != 0) fprintf(stderr, "  got \"%s\", want \"%s\"\n", (a), (b));      \
    } while (0)

/* Both charts in one image: one segment, one system. */
static void *compile_both(size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source sources[2] = {{{"caller.scxml", 12}, {CALLER, sizeof CALLER - 1}},
                            {{"listener.scxml", 14}, {LISTENER, sizeof LISTENER - 1}}};
    ta_compile_result *result = NULL;
    void *copy = NULL;
    if (ta_compile_charts(&options, (ta_source_list){sources, 2}, &result) == TA_OK) {
        ta_string_view image = ta_compile_image(result);
        copy = aligned_alloc(8, (image.length + 7) & ~(size_t)7);
        memcpy(copy, image.pointer, image.length);
        *length = image.length;
    } else {
        print_diagnostics(result);
    }
    ta_compile_free(result);
    return copy;
}

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_vtable vtable; /* the ECMAScript one, with `script` wrapped */
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    size_t image_length;
    uint32_t segment;
    char log[4096];
    uint64_t caller, listener;
    void (*action)(struct world *); /* run by the next <script>, once */
    int actions;
} world;

static world *current;

/* The host's allocator, counted (the C library's): a hook reads the count
 * before and after its calls to show that nothing grew while it ran. */
static size_t allocations;
static void *counting_alloc(void *context, size_t size, size_t align) {
    (void)context;
    allocations++;
    ta_allocator c = ta_runtime_malloc();
    return c.alloc(c.context, size, align);
}
static void counting_free(void *context, void *pointer, size_t size) {
    (void)context;
    ta_allocator c = ta_runtime_malloc();
    c.free(c.context, pointer, size);
}

static ta_status script(const ta_datamodel_call *call, ta_expression e) {
    if (current->action) {
        void (*action)(world *) = current->action;
        current->action = NULL;
        action(current);
        current->actions++;
    }
    return ta_datamodel_ecmascript_vtable()->script(call, e);
}

static void world_open(world *w, uint32_t max_events) {
    memset(w, 0, sizeof *w);
    current = w;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    w->vtable = *w->binding.vtable;
    w->vtable.script = script;
    w->binding.vtable = &w->vtable;
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    config.system.max_events = max_events;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){NULL, counting_alloc, counting_free};
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_both(&length);
    CHECK(w->image_bytes != NULL);
    w->image_length = length;
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){w->image_bytes, length}, &w->segment), TA_OK);
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image_bytes);
    current = NULL;
}

static void drain(world *w) {
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w->host, &effects);
    for (uint32_t i = 0; i < n; i++)
        if (effects[i].kind == TA_EFFECT_LOG) {
            size_t l = strlen(w->log);
            snprintf(w->log + l, sizeof w->log - l, "%.*s;", (int)effects[i].event.length,
                     effects[i].event.pointer);
        }
    ta_host_effects_ack(w->host);
}

/* One ta_host_run with a budget for everything, then its effects. */
static ta_run run_once(world *w) {
    ta_run result = ta_host_run(w->host, 100000);
    drain(w);
    return result;
}

static uint64_t spawn(world *w, const char *chart, const char *name) {
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w->host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref),
                TA_OK);
    uint64_t session = 0;
    ta_spawn spawn = {ref.segment, ref.chart, {name, (uint32_t)strlen(name)}, 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &session), TA_OK);
    return session;
}

static ta_event_in event_for(uint64_t session, const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.session = session;
    return event;
}

/* The two actors, idle, and an action for the caller's next <script>. */
static void start(world *w, void (*action)(world *)) {
    w->caller = spawn(w, "caller", "caller");
    w->listener = spawn(w, "listener", "listener");
    run_once(w);
    w->log[0] = 0;
    w->action = action;
    ta_event_in go = event_for(w->caller, "go");
    CHECK_EQUAL(ta_host_send(w->host, &go), TA_OK);
}

/* ---- an event for the actor whose expression sent it ---------------------------------- */

static void poke_self(world *w) {
    ta_event_in poke = event_for(w->caller, "poke");
    CHECK_EQUAL(ta_host_send(w->host, &poke), TA_OK);
}

static void test_send_to_self_waits_for_the_macrostep(void) {
    world w;
    world_open(&w, 0);
    start(&w, poke_self);
    CHECK_EQUAL(run_once(&w).status, TA_RUN_IDLE);
    CHECK_EQUAL(w.actions, 1);
    /* the internal event of the macrostep first, then the external poke */
    CHECK_STR(w.log, "internal;poke:1;");
    world_close(&w);
}

/* ---- events for another actor, one or many ------------------------------------------------ */

static void send_to_listener(world *w) {
    ta_event_in one = event_for(w->listener, "one");
    CHECK_EQUAL(ta_host_send(w->host, &one), TA_OK);
    ta_event_in many[2] = {event_for(w->listener, "two"), event_for(w->listener, "three")};
    CHECK_EQUAL(ta_host_send_many(w->host, many, 2), 2u);
}

static void test_send_and_send_many_to_another_actor(void) {
    world w;
    world_open(&w, 0);
    start(&w, send_to_listener);
    CHECK_EQUAL(run_once(&w).status, TA_RUN_IDLE); /* the same run, in order */
    CHECK_STR(w.log, "internal;got:one;got:two;got:three;");
    world_close(&w);
}

/* ---- spawn ------------------------------------------------------------------------------ */

static void spawn_child(world *w) {
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w->host, (ta_string_view){"listener", 8}, &ref), TA_OK);
    uint64_t session = 0;
    ta_spawn spawn = {ref.segment, ref.chart, {"child", 5}, 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &session), TA_OK);
    CHECK(session != 0);
    ta_event_in hello = event_for(session, "hello"); /* to the actor spawned a moment ago */
    CHECK_EQUAL(ta_host_send(w->host, &hello), TA_OK);
}

static void test_spawn(void) {
    world w;
    world_open(&w, 0);
    start(&w, spawn_child);
    CHECK_EQUAL(run_once(&w).status, TA_RUN_IDLE);
    CHECK(ta_host_actor_by_name(w.host, (ta_string_view){"child", 5}) != 0);
    CHECK_STR(w.log, "internal;listening;got:hello;");
    world_close(&w);
}

/* ---- terminate ---------------------------------------------------------------------------- */

static void terminate_self(world *w) { CHECK_EQUAL(ta_host_terminate(w->host, w->caller), TA_OK); }

static void terminate_listener(world *w) {
    CHECK_EQUAL(ta_host_terminate(w->host, w->listener), TA_OK);
}

static void test_terminate_self_ends_after_the_microstep(void) {
    world w;
    world_open(&w, 0);
    start(&w, terminate_self);
    run_once(&w);
    /* it exits at the microstep boundary, as a cancelled invoke does: the
     * transition on `internal` is not taken */
    CHECK_STR(w.log, "");
    ta_host_location where;
    CHECK(ta_host_locate(w.host, w.caller, &where) != TA_OK);
    CHECK_EQUAL(ta_host_locate(w.host, w.listener, &where), TA_OK);
    world_close(&w);
}

static void test_terminate_another_actor(void) {
    world w;
    world_open(&w, 0);
    start(&w, terminate_listener);
    run_once(&w);
    ta_host_location where;
    CHECK(ta_host_locate(w.host, w.listener, &where) != TA_OK);
    CHECK_EQUAL(ta_host_locate(w.host, w.caller, &where), TA_OK);
    world_close(&w);
}

/* ---- what stays refused --------------------------------------------------------------------- */

static void refused(world *w) {
    unsigned char bytes[64];
    ta_buffer room = {bytes, sizeof bytes, 0};
    ta_bytes out;
    CHECK_EQUAL(ta_host_hibernate(w->host, w->listener, &out), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_snapshot(w->host, &room), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_post(w->host, (ta_bytes){"\xa1\x62op\x65stats", 9}),
                TA_ERROR_STATE);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){w->image_bytes, w->image_length}, &segment),
                TA_ERROR_STATE);
    ta_resume resume;
    memset(&resume, 0, sizeof resume);
    CHECK_EQUAL(ta_host_resume(w->host, &resume), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_boot(w->host, w->segment), TA_ERROR_STATE);
    ta_run nested = ta_host_run(w->host, 10);
    CHECK_EQUAL(nested.status, TA_RUN_FAULT);
    CHECK_EQUAL(nested.detail, (uint32_t)TA_ERROR_STATE);
    const ta_host_effect *effects;
    CHECK_EQUAL(ta_host_effects(w->host, &effects), 0u);
    /* lookups answer */
    ta_host_location where;
    CHECK_EQUAL(ta_host_locate(w->host, w->listener, &where), TA_OK);
    CHECK_EQUAL(ta_host_actor_by_name(w->host, (ta_string_view){"listener", 8}), w->listener);
}

static void test_other_calls_are_refused(void) {
    world w;
    world_open(&w, 0);
    start(&w, refused);
    CHECK_EQUAL(run_once(&w).status, TA_RUN_IDLE);
    CHECK_EQUAL(w.actions, 1);
    CHECK_STR(w.log, "internal;");
    world_close(&w);
}

/* ---- lookups and free ------------------------------------------------------------------ */

/* The lookups answer, an address resolves, and ta_host_free does nothing:
 * the run goes on with the host it had. */
static void lookups_and_free(world *w) {
    ta_host_segment_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    CHECK_EQUAL(ta_host_segment_info_get(w->host, w->segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 2u);
    CHECK(ta_host_system(w->host, w->segment) != NULL);
    uint64_t resolved = 0;
    CHECK_EQUAL(ta_host_resolve(w->host, (ta_string_view){"tinyactors://h/@listener", 24}, &resolved),
                TA_OK);
    CHECK_EQUAL(resolved, w->listener);
    CHECK_EQUAL(ta_host_migration_pending(w->host, 1, (ta_uint64_array){NULL, 0}), 0u);
    CHECK_EQUAL(ta_host_effect_number(w->host, 0, TA_EFFECT_FIELD_KIND), 0u); /* no batch */
    CHECK_EQUAL(ta_host_effect_text(w->host, 0, TA_EFFECT_FIELD_EVENT).length, 0u);
    ta_host_free(w->host);
}

static void test_lookups_answer_and_free_does_nothing(void) {
    world w;
    world_open(&w, 0);
    start(&w, lookups_and_free);
    CHECK_EQUAL(run_once(&w).status, TA_RUN_IDLE);
    CHECK_EQUAL(w.actions, 1);
    CHECK_STR(w.log, "internal;"); /* the macrostep went on */
    ta_host_location where; /* the host is still there */
    CHECK_EQUAL(ta_host_locate(w.host, w.listener, &where), TA_OK);
    world_close(&w);
}

/* ---- full pools: no memory, nothing grows ------------------------------------------------ */

/* A hook never calls ta_system_* (datamodel.h): it shows that nothing grew
 * by the host's allocator, which is not called while it runs. */
static size_t grew; /* allocations during a hook's calls */
static uint32_t spawned;
static uint64_t refused_session;

static void fill_actors(world *w) {
    size_t before = allocations;
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w->host, (ta_string_view){"listener", 8}, &ref), TA_OK);
    spawned = 0;
    for (int i = 0; i < 100; i++) {
        uint64_t session = 0;
        ta_spawn spawn = {ref.segment, ref.chart, {0, 0}, 0, {0, 0}};
        ta_status status = ta_host_spawn(w->host, &spawn, &session);
        if (status == TA_ERROR_NO_MEMORY) {
            refused_session = session;
            break;
        }
        CHECK_EQUAL(status, TA_OK);
        spawned++;
    }
    grew = allocations - before;
}

static void test_spawn_into_a_full_pool_is_no_memory(void) {
    world w;
    world_open(&w, 0);
    refused_session = 99;
    start(&w, fill_actors);
    for (int i = 0; i < 20 && run_once(&w).status != TA_RUN_IDLE; i++) {
    }
    CHECK_EQUAL(w.actions, 1);
    CHECK(spawned > 0 && spawned < 100);
    CHECK_EQUAL(refused_session, 0u); /* no session for the refusal */
    CHECK_EQUAL(grew, 0u);            /* nothing grew while the pool filled */
    ta_host_segment_info info;        /* the spawned ones live, no more */
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    CHECK_EQUAL(ta_host_segment_info_get(w.host, w.segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 2u + spawned);
    world_close(&w);
}

static uint32_t accepted;

static void flood(world *w) {
    size_t before = allocations;
    accepted = 0;
    for (int i = 0; i < 1000; i++) {
        ta_event_in flood = event_for(w->listener, "f");
        ta_status status = ta_host_send(w->host, &flood);
        if (status == TA_ERROR_NO_MEMORY) break;
        CHECK_EQUAL(status, TA_OK);
        accepted++;
    }
    grew = allocations - before;
}

static void test_a_full_pool_is_no_memory(void) {
    world w;
    world_open(&w, 16);
    start(&w, flood);
    for (int i = 0; i < 20 && run_once(&w).status != TA_RUN_IDLE; i++) {
    }
    CHECK(accepted > 0 && accepted < 1000);
    CHECK_EQUAL(grew, 0u); /* nothing grew during the run */
    uint32_t got = 0;
    for (const char *p = w.log; (p = strstr(p, "got:f;")) != NULL; p++)
        got++;
    CHECK_EQUAL(got, accepted); /* each accepted event once, the refused one never */
    ta_event_in after = event_for(w.listener, "after"); /* after the run, the host grows */
    CHECK_EQUAL(ta_host_send(w.host, &after), TA_OK);
    world_close(&w);
}

int main(void) {
    RUN(test_send_to_self_waits_for_the_macrostep);
    RUN(test_send_and_send_many_to_another_actor);
    RUN(test_spawn);
    RUN(test_terminate_self_ends_after_the_microstep);
    RUN(test_terminate_another_actor);
    RUN(test_other_calls_are_refused);
    RUN(test_lookups_answer_and_free_does_nothing);
    RUN(test_spawn_into_a_full_pool_is_no_memory);
    RUN(test_a_full_pool_is_no_memory);
    TEST_MAIN_END();
}
