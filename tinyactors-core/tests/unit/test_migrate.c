/* Live migration and moving actors between systems (hot reload):
 * ta_actor_export -> ta_blob_migrate -> ta_system_adopt, sessions addressable
 * across systems sharing one base URI (TA_IO_PROCESSOR_SCXML_SHARED), and the
 * error.platform.upgrade event carrying the previous bindings. */
#include "../harness/harness.h"
#include "system/system_internal.h"
#include "../support/system_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "../support/payload_text.h"

#define CHECK_STR(got, want)                                                                       \
    do {                                                                                           \
        const char *ta__g = (got), *ta__w = (want);                                                \
        CHECK(strcmp(ta__g, ta__w) == 0);                                                          \
        if (strcmp(ta__g, ta__w)) fprintf(stderr, "  got \"%s\", want \"%s\"\n", ta__g, ta__w);    \
    } while (0)

#define BASE "tinyactors://t/"
#define ESSC(body)                                                                                 \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='w' " body         \
    "</scxml>"
#define NULLSC(body)                                                                               \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' name='w' " body "</scxml>"

/* Two systems (old and new version) sharing a base URI, one datamodel host
 * and a clock; the "host" routes TA_EFFECT_SEND between them. */
typedef struct world {
    void *bytes[2];
    ta_image image[2];
    ta_datamodel_ecmascript *ecmascript;
    datamodel_test *test_datamodel;
    ta_datamodel_binding binding;
    void *memory[2];
    ta_system *system[2];
    ta_io_processor_descriptor io_processors;
    char log[8192];
    ta_time now;
    int communication_errors;
} world;

static ta_system_config config_for(world *w, uint64_t first_serial, uint32_t max_timers) {
    ta_system_config config = test_system_config();
    config.first_serial = first_serial;
    config.max_timers = max_timers;
    config.io_processor_count = 1;
    config.io_processors = &w->io_processors;
    return config;
}

static int world_open(world *w, const char *v1, const char *v2, int ecmascript, uint32_t timers2) {
    memset(w, 0, sizeof *w);
    w->io_processors.type = (ta_string_view){"tinyactors", 10};
    w->io_processors.location = (ta_string_view){BASE, (uint32_t)strlen(BASE)};
    w->io_processors.flags = TA_IO_PROCESSOR_SCXML_REMOTE | TA_IO_PROCESSOR_SCXML_SHARED;
    if (ecmascript) {
        w->ecmascript = ta_datamodel_ecmascript_new(NULL);
        w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    } else {
        w->test_datamodel = datamodel_test_new();
        w->binding = datamodel_test_binding(w->test_datamodel);
    }
    const char *sources[2] = {v1, v2};
    for (int i = 0; i < 2; i++) {
        size_t length;
        w->bytes[i] = compile_chart("w.scxml", sources[i], NULL, &length, 0);
        if (!w->bytes[i] || ta_image_open(w->bytes[i], length, &w->image[i]) != TA_OK) return 0;
        ta_system_config config = config_for(w, (uint64_t)i << 32, i ? timers2 : 0);
        size_t ml;
        if (ta_system_layout(&w->image[i], &config, &ml) != TA_OK) return 0;
        w->memory[i] = malloc(ml);
        if (ta_system_init(
                &(ta_system_setup){{w->memory[i], ml}, &w->image[i], &config, &w->binding, 1, 0},
                &w->system[i]) != TA_OK)
            return 0;
    }
    return 1;
}

static void world_close(world *w) {
    for (int i = 0; i < 2; i++) {
        if (w->system[i]) ta_system_deinit(w->system[i]);
        free(w->memory[i]);
        free(w->bytes[i]);
    }
    if (w->ecmascript) ta_datamodel_ecmascript_free(w->ecmascript);
    if (w->test_datamodel) datamodel_test_free(w->test_datamodel);
}

/* Which system has session `serial`: 0/1, or -1 (with its handle in *actor). */
static int find(world *w, uint64_t serial, uint32_t *actor) {
    for (int i = 0; i < 2; i++)
        if ((*actor = ta_system_actor_by_session(w->system[i], serial)) != 0) return i;
    return -1;
}

static void route(world *w, int from, const ta_effect *effect) {
    ta_string_view t = effect->target;
    const char *hash = memchr(t.pointer, '#', t.length);
    uint64_t serial = 0;
    for (const char *p = hash ? hash + 8 : t.pointer + t.length; p < t.pointer + t.length; p++)
        serial = serial * 10 + (uint64_t)(*p - '0');
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    char origin[64] = "";
    if (ta_actor_info_get(w->system[from], effect->actor, &actor_info) == TA_OK)
        snprintf(origin, sizeof origin, BASE "#_scxml_%llu", (unsigned long long)actor_info.serial);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    uint32_t actor;
    int to = find(w, serial, &actor);
    if (to < 0) { /* nobody has it: the send failed */
        w->communication_errors++;
        event.target = effect->actor;
        event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
        event.sendid = effect->sendid;
        ta_system_send(w->system[from], &event);
        return;
    }
    event.target = actor;
    event.name = effect->event;
    event.origin = (ta_string_view){origin, (uint32_t)strlen(origin)};
    event.origintype =
        (ta_string_view){TA_SCXML_IO_PROCESSOR, (uint32_t)strlen(TA_SCXML_IO_PROCESSOR)};
    if (effect->payload && w->ecmascript) { /* the datamodel host is shared: keep a reference */
        w->binding.vtable->retain(w->binding.datamodel, effect->payload);
        event.data = effect->payload;
    }
    CHECK_EQUAL(ta_system_send(w->system[to], &event), TA_OK);
}

static void run(world *w) {
    for (int round = 0; round < 50; round++) {
        int busy = 0;
        for (int i = 0; i < 2; i++) {
            ta_run result = ta_system_run(w->system[i], 100000);
            const ta_effect *effect;
            uint32_t n = ta_system_effects(w->system[i], &effect);
            for (uint32_t k = 0; k < n; k++) {
                if (effect[k].kind == TA_EFFECT_LOG) {
                    size_t l = strlen(w->log);
                    snprintf(w->log + l, sizeof w->log - l, "%.*s;", (int)effect[k].event.length,
                             effect[k].event.pointer ? effect[k].event.pointer : "");
                }
                if (effect[k].kind == TA_EFFECT_SEND) {
                    CHECK_EQUAL(effect[k].io_processor, 0);
                    route(w, i, &effect[k]);
                    busy = 1;
                }
            }
            ta_system_effects_ack(w->system[i], n);
            if (result.status != TA_RUN_IDLE) busy = 1;
        }
        if (!busy) return;
    }
}

static void advance(world *w, ta_time ms) {
    w->now += ms;
    ta_system_advance_time(w->system[0], w->now);
    ta_system_advance_time(w->system[1], w->now);
    run(w);
}

static const char *config_of(world *w, int i, uint32_t actor) {
    static char buffer[512];
    uint32_t states[64];
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    buffer[0] = 0;
    if (ta_actor_info_get(w->system[i], actor, &actor_info) != TA_OK) return "<gone>";
    uint32_t n = ta_actor_config(w->system[i], actor, (ta_uint32_array){states, 64});
    for (uint32_t k = 0; k < n && k < 64; k++) {
        ta_string_view nm = ta_image_state_name(&w->image[i], actor_info.chart, states[k]);
        size_t l = strlen(buffer);
        snprintf(buffer + l, sizeof buffer - l, "%s%.*s", l ? "," : "", (int)nm.length, nm.pointer);
    }
    return buffer;
}

static const char *eval(world *w, int i, uint32_t actor, const char *expression) {
    static char buffer[512];
    uint32_t index = ta_system_resolve_actor(w->system[i], actor);
    if (index == TA_NIL) return "<gone>";
    uint64_t instance =
        ta_actor_instance(w->system[i], ta_actor_at(w->system[i], index))->datamodel;
    if (eval_text(
            w->ecmascript, instance, (ta_string_view){expression, (uint32_t)strlen(expression)},
            buffer, sizeof buffer) == (size_t)-1)
        return "<error>";
    return buffer;
}

static unsigned char blob[1 << 16];
static uint32_t mig[1 << 14]; /* 4-aligned */

/* export from system 0, translate to image 1's chart 0, adopt into system 1 */
static ta_status move_and_upgrade(world *w, uint32_t actor, uint32_t *out) {
    size_t length, ml;
    ta_buffer migrated;
    ta_buffer written = {blob, sizeof blob, 0};
    ta_status status = ta_actor_export(w->system[0], actor, &written);
    length = written.length;
    if (status != TA_OK) return status;
    migrated = (ta_buffer){mig, sizeof mig, 0};
    status = ta_blob_migrate(&(ta_blob_migration){&w->image[0], &w->image[1], 0, 0, {blob, length}},
                             &migrated);
    ml = migrated.length;
    if (status != TA_OK) return status;
    return ta_system_adopt(w->system[1], (ta_bytes){mig, ml}, out);
}

/* ---- the whole path, with the ECMAScript datamodel ---- */

#define COMMON                                                                                     \
    "<transition event='ask'><send targetexpr='_event.data.to' event='ping'/></transition>"        \
    "<transition event='ping'><send targetexpr='_event.origin' event='pong'/></transition>"        \
    "<transition event='pong'><log expr=\"'pong@' + _sessionid\"/></transition>"                   \
    "<transition event='error.communication'><log expr=\"'comm-error'\"/></transition>"

static const char *V1 =
    ESSC("initial='p'><datamodel><data id='count' expr='0'/><data id='gone' expr='\"bye\"'/>"
         "</datamodel>"
         "<parallel id='p'>" COMMON
         "<state id='r1' initial='a'><state id='a'><transition event='go' target='b'/></state>"
         "<state id='b'><onentry><assign location='count' expr='count + 1'/>"
         "<send event='tick' delay='100ms'><param name='v' expr='count'/></send>"
         "<send event='late' delay='300ms' id='late'/></onentry></state></state>"
         "<state id='r2' initial='x'><state id='x'><transition event='go' target='y'/></state>"
         "<state id='y'/></state>"
         "</parallel>");

/* v2: `gone` removed, `added` new, region r3 new, state y removed */
static const char *V2 = ESSC(
    "initial='p'><datamodel><data id='count' expr='0'/><data id='added' expr='count + 100'/>"
    "</datamodel>"
    "<parallel id='p'>" COMMON
    "<transition event='error.platform.upgrade'><log expr=\"'up:' + _event.type + ':' + "
    "_event.data.count + ':' + _event.data.gone + ':' + added + ':' + typeof gone\"/>"
    "</transition>"
    "<transition event='tick'><log expr=\"'tick:' + _event.data.v + ':' + count\"/></transition>"
    "<transition event='late'><log expr=\"'late'\"/></transition>"
    "<state id='r1' initial='a'><state id='a'/><state id='b'/></state>"
    "<state id='r2' initial='x'><state id='x'/></state>"
    "<state id='r3' initial='z'><state id='z'/></state>"
    "</parallel>");

static void test_upgrade_roundtrip(void) {
    world w;
    CHECK(world_open(&w, V1, V2, 1, 0));
    uint32_t a, peer;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){"peer", 4}, 0, (ta_string_view){0, 0}},
                        &peer),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"go", 2};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    CHECK_STR(config_of(&w, 0, a), "p,r1,b,r2,y");
    char sid[32];
    snprintf(sid, sizeof sid, "%s", eval(&w, 0, a, "_sessionid"));
    CHECK(strcmp(sid, "\"1\"") == 0);

    /* not while busy, not for invoked children (none here), not twice */
    uint32_t b;
    CHECK_EQUAL(move_and_upgrade(&w, a, &b), TA_OK);
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    CHECK_EQUAL(ta_actor_info_get(w.system[0], a, &actor_info), TA_ERROR_STALE); /* it left */
    ta_system_stats s0;
    s0.size = sizeof s0;
    ta_system_stats_get(w.system[0], &s0);
    CHECK_EQUAL(s0.actors, 1);
    CHECK_EQUAL(s0.timers, 0); /* its delayed sends left with it */
    CHECK_EQUAL(ta_actor_info_get(w.system[1], b, &actor_info), TA_OK);
    CHECK_EQUAL(actor_info.serial, 1); /* same session id, same address */
    {                                  /* enumeration finds it whatever its session range */
        uint32_t list[4], cursor = 0, n = 0, got;
        while ((got = ta_system_actors(w.system[1], &cursor, (ta_uint32_array){list + n, 1})) != 0)
            n += got;
        CHECK(n == 1 && list[0] == b);
        cursor = 0;
        CHECK_EQUAL(ta_system_actors(w.system[0], &cursor, (ta_uint32_array){list, 4}),
                    1); /* peer */
        CHECK_EQUAL(ta_system_actors(w.system[0], &cursor, (ta_uint32_array){list, 4}), 0);
    }

    run(&w);
    /* states mapped by id, y gone -> r2's initial x, new region entered */
    CHECK(strcmp(config_of(&w, 1, b), "p,r1,b,r2,x,r3,z") == 0);
    /* the upgrade event: platform type, previous bindings, new data initialized
     * from the migrated value, removed data gone */
    CHECK(strstr(w.log, "up:platform:1:bye:101:undefined;") != NULL);
    CHECK(strcmp(eval(&w, 1, b, "_sessionid"), sid) == 0);
    CHECK(strcmp(eval(&w, 1, b, "_event.name"), "\"error.platform.upgrade\"") == 0);

    /* addresses keep working across systems: peer (old system) -> b (new) -> peer */
    w.log[0] = 0;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = peer;
    event.name = (ta_string_view){"ask", 3};
    event.data = ta_datamodel_ecmascript_payload_eval(
        w.ecmascript, 0, (ta_string_view){"{\"to\": \"" BASE "#_scxml_1\"}",
                                       (uint32_t)strlen("{\"to\": \"" BASE "#_scxml_1\"}")});
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    CHECK(strcmp(w.log, "pong@2;") == 0);

    /* an address nobody has: error.communication through the host */
    w.log[0] = 0;
    event.data = ta_datamodel_ecmascript_payload_eval(
        w.ecmascript, 0, (ta_string_view){"{\"to\": \"" BASE "#_scxml_99\"}",
                                       (uint32_t)strlen("{\"to\": \"" BASE "#_scxml_99\"}")});
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    CHECK_EQUAL(w.communication_errors, 1);
    CHECK(strcmp(w.log, "comm-error;") == 0);

    /* delayed sends moved along, payload intact, on the shared clock */
    w.log[0] = 0;
    advance(&w, 100);
    CHECK(strcmp(w.log, "tick:1:1;") == 0);
    advance(&w, 200);
    CHECK(strcmp(w.log, "tick:1:1;late;") == 0);

    /* the session exists: adopting the same blob again is refused */
    size_t ml;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[1], b, &written), TA_OK);
    ml = written.length;
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){blob, ml}, &b),
                TA_OK); /* back, unmigrated */
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){blob, ml}, &b), TA_ERROR_STATE);
    CHECK_EQUAL(ta_system_adopt(w.system[0], (ta_bytes){blob, ml}, &b),
                TA_ERROR_INVALID); /* other image */
    world_close(&w);
}

/* A failed adoption leaves the blob intact: re-adopt it where it came from. */

static void test_failed_adopt_keeps_blob(void) {
    world w;
    CHECK(world_open(&w, V1, V2, 1, 1)); /* the new system has room for one timer */
    uint32_t a, b;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"go", 2};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    size_t length, ml;
    ta_buffer migrated;
    { /* hibernation keeps the data too (declared variables are configurable) */
        size_t hl;
        ta_buffer written = {blob, sizeof blob, 0};
        CHECK_EQUAL(ta_actor_hibernate(w.system[0], a, &written), TA_OK);
        hl = written.length;
        CHECK_EQUAL(ta_actor_wake(w.system[0], a, (ta_bytes){blob, hl}), TA_OK);
        CHECK(strcmp(eval(&w, 0, a, "JSON.stringify([count, gone])"), "\"[1,\\\"bye\\\"]\"") == 0);
    }
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_OK);
    length = written.length;
    migrated = (ta_buffer){mig, 8, 0};
    CHECK_EQUAL(
        ta_blob_migrate(&(ta_blob_migration){&w.image[0], &w.image[1], 0, 0, {blob, length}},
                        &migrated),
        TA_ERROR_NO_MEMORY);
    ml = migrated.length;
    CHECK(ml > 8);
    migrated = (ta_buffer){(unsigned char *)mig + 1, sizeof mig - 4, 0};
    CHECK_EQUAL(
        ta_blob_migrate(&(ta_blob_migration){&w.image[0], &w.image[1], 0, 0, {blob, length}},
                        &migrated),
        TA_ERROR_INVALID);
    ml = migrated.length; /* unaligned output */
    migrated = (ta_buffer){mig, sizeof mig, 0};
    CHECK_EQUAL(
        ta_blob_migrate(&(ta_blob_migration){&w.image[0], &w.image[1], 0, 0, {blob, length}},
                        &migrated),
        TA_OK);
    ml = migrated.length;
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){mig, ml}, &b),
                TA_ERROR_NO_MEMORY); /* two timers */
    ta_system_stats s1;
    s1.size = sizeof s1;
    ta_system_stats_get(w.system[1], &s1);
    CHECK(s1.actors == 0 && s1.timers == 0 && s1.events == 0);
    /* the original blob goes back home; the payload of the tick is still valid */
    CHECK_EQUAL(ta_system_adopt(w.system[0], (ta_bytes){blob, length}, &a), TA_OK);
    CHECK_STR(config_of(&w, 0, a), "p,r1,b,r2,y");
    CHECK(strcmp(eval(&w, 0, a, "count"), "1") == 0);
    advance(&w, 100);
    CHECK(strcmp(eval(&w, 0, a, "_event.name + ':' + _event.data.v"), "\"tick:1\"") == 0);
    world_close(&w);
}

/* Export preconditions. */
static void test_export_preconditions(void) {
    world w;
    CHECK(world_open(&w,
                     ESSC("initial='a'><state id='a'><invoke id='kid'><content><scxml "
                          "xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' "
                          "initial='k'><state id='k'/></scxml></content></invoke></state>"),
                     V2, 1, 0));
    uint32_t a;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_ERROR_STATE); /* runnable */
    run(&w);
    uint32_t index = ta_system_resolve_actor(w.system[0], a);
    uint32_t kid = ta_core_invoke_child(
        &w.system[0]->core, ta_actor_instance(w.system[0], ta_actor_at(w.system[0], index)), 0);
    CHECK(kid != 0);
    written = (ta_buffer){blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], kid, &written), TA_ERROR_STATE); /* parent */
    /* a session in the tree is busy: the whole tree stays */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = kid;
    event.name = (ta_string_view){"poke", 4};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    written = (ta_buffer){blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_ERROR_STATE);
    run(&w);
    written = (ta_buffer){blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_OK); /* with its child */
    CHECK_EQUAL(ta_actor_export(w.system[0], 12345, &written), TA_ERROR_STALE);
    world_close(&w);
}

/* ---- session trees: actors with running invocations move whole ---- */

#define TREE                                                                                       \
    ESSC("initial='a'><datamodel><data id='n' expr='0'/></datamodel>"                              \
         "<state id='a'>"                                                                          \
         "<invoke id='kid'><content><scxml xmlns='http://www.w3.org/2005/07/scxml' "               \
         "datamodel='ecmascript' name='kid' initial='k'>"                                          \
         "<datamodel><data id='c' expr='0'/><data id='g' expr='0'/></datamodel>"                   \
         "<state id='k'>"                                                                          \
         "<invoke id='gk'><content><scxml xmlns='http://www.w3.org/2005/07/scxml' "                \
         "datamodel='ecmascript' name='gk' initial='q'><state id='q'>"                             \
         "<transition event='ping'><send target='#_parent' event='gpong'>"                         \
         "<param name='sid' expr='_sessionid'/></send></transition>"                               \
         "</state></scxml></content></invoke>"                                                     \
         "<transition event='ping'><assign location='c' expr='c + 1'/>"                            \
         "<send target='#_gk' event='ping'/>"                                                      \
         "<send target='#_parent' event='pong'><param name='v' expr='c'/></send></transition>"     \
         "<transition event='gpong'><assign location='g' expr='g + 1'/></transition>"              \
         "<transition event='finish' target='end'/></state>"                                       \
         "<final id='end'><donedata><param name='c' expr='c'/><param name='g' expr='g'/>"          \
         "</donedata></final></scxml></content></invoke>"                                          \
         "<transition event='ping'><send target='#_kid' event='ping'/></transition>"               \
         "<transition event='later'><send target='#_kid' event='ping' delay='5s'>"                 \
         "<param name='x' expr='1'/></send></transition>"                                          \
         "<transition event='finish'><send target='#_kid' event='finish'/></transition>"           \
         "<transition event='pong'><assign location='n' expr='n + 1'/>"                            \
         "<log expr=\"'pong ' + _event.data.v\"/></transition>"                                    \
         "<transition event='done.invoke.kid' target='b'>"                                         \
         "<log expr=\"'done ' + _event.data.c + ' ' + _event.data.g\"/></transition></state>"      \
         "<state id='b'/>")

static void send_to(world *w, int i, uint32_t actor, const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = actor;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    CHECK_EQUAL(ta_system_send(w->system[i], &event), TA_OK);
    run(w);
}

static uint32_t actors_in(world *w, int i) {
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(w->system[i], &stats);
    return stats.actors;
}

/* the session ids of the tree rooted at `actor`: root, kid, grandchild */
static void tree_serials(world *w, int i, uint32_t actor, uint64_t out[3]) {
    ta_system *system = w->system[i];
    uint32_t index = ta_system_resolve_actor(system, actor);
    out[0] = ta_actor_at(system, index)->serial;
    uint32_t kid = ta_core_invoke_child(&system->core,
                                        ta_actor_instance(system, ta_actor_at(system, index)), 0);
    uint32_t ki = ta_system_resolve_actor(system, kid);
    out[1] = ki == TA_NIL ? 0 : ta_actor_at(system, ki)->serial;
    uint32_t gk = ki == TA_NIL
                      ? 0
                      : ta_core_invoke_child(&system->core,
                                             ta_actor_instance(system, ta_actor_at(system, ki)), 0);
    uint32_t gi = gk ? ta_system_resolve_actor(system, gk) : TA_NIL;
    out[2] = gi == TA_NIL ? 0 : ta_actor_at(system, gi)->serial;
}

static void test_session_tree(void) {
    world w;
    CHECK(world_open(&w, TREE, TREE, 1, 8));
    uint32_t a;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    CHECK_EQUAL(actors_in(&w, 0), 3u); /* parent, kid, grandchild */
    send_to(&w, 0, a, "ping");
    CHECK_STR(w.log, "pong 1;");
    send_to(&w, 0, a, "later"); /* a delayed send to #_kid, with a payload */
    uint64_t before[3];
    tree_serials(&w, 0, a, before);
    CHECK(before[1] && before[2]);

    size_t length = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    CHECK_EQUAL(actors_in(&w, 0), 3u); /* nothing changed */
    written = (ta_buffer){blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_OK);
    length = written.length;
    CHECK_EQUAL(actors_in(&w, 0), 0u); /* the whole tree left */
    uint32_t sessions;                 /* the export header: magic, version, number of sessions */
    memcpy(&sessions, blob + 8, 4);
    CHECK_EQUAL(sessions, 3u);
    ta_buffer migrated;
    migrated = (ta_buffer){mig, sizeof mig, 0};
    CHECK_EQUAL(
        ta_blob_migrate(&(ta_blob_migration){&w.image[0], &w.image[1], 0, 0, {blob, length}},
                        &migrated),
        TA_ERROR_UNSUPPORTED);

    uint32_t b;
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){blob, length}, &b), TA_OK);
    CHECK_EQUAL(actors_in(&w, 1), 3u);
    uint32_t again;
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){blob, length}, &again),
                TA_ERROR_STATE); /* its sessions exist */
    CHECK_EQUAL(again, 0u);
    uint64_t after[3];
    tree_serials(&w, 1, b, after);
    CHECK(after[0] == before[0] && after[1] == before[1] && after[2] == before[2]);
    CHECK_STR(config_of(&w, 1, b), "a");
    CHECK_STR(eval(&w, 1, b, "n"), "1");

    /* messages both ways, the grandchild included; state carried on */
    w.log[0] = 0;
    send_to(&w, 1, b, "ping");
    CHECK_STR(w.log, "pong 2;");
    /* the delayed send armed before the move reaches the moved child */
    advance(&w, 5000);
    CHECK_STR(w.log, "pong 2;pong 3;");
    /* the grandchild's _sessionid is its old one */
    uint32_t kid = ta_system_actor_by_session(w.system[1], after[1]);
    CHECK(kid != 0);
    CHECK_STR(eval(&w, 1, kid, "g"), "3");
    /* done.invoke reaches the parent, with donedata */
    send_to(&w, 1, b, "finish");
    CHECK_STR(w.log, "pong 2;pong 3;done 3 3;");
    CHECK_STR(config_of(&w, 1, b), "b");
    CHECK_EQUAL(actors_in(&w, 1), 1u); /* the kid and its child finished */
    /* now a single session again: it can be migrated */
    written = (ta_buffer){blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[1], b, &written), TA_OK);
    length = written.length;
    memcpy(&sessions, blob + 8, 4);
    CHECK_EQUAL(sessions, 1u);
    CHECK_EQUAL(ta_system_adopt(w.system[0], (ta_bytes){blob, length}, &a), TA_OK);
    world_close(&w);
}

/* A tree adoption that fails part way (here: restoring the k-th session)
 * leaves the system as it was and the blob intact, payloads included. */
static const ta_datamodel_vtable *real_vt;
static int deser_calls, deser_fail_at;
static ta_status failing_deserialize(void *datamodel, void *chart_context,
                                     const ta_datamodel_system_variables *variables,
                                     const void *buffer, size_t length, uint64_t *out) {
    if (++deser_calls == deser_fail_at) return TA_ERROR_NO_MEMORY;
    return real_vt->deserialize(datamodel, chart_context, variables, buffer, length, out);
}

static void test_session_tree_rollback(void) {
    world w;
    CHECK(world_open(&w, TREE, TREE, 1, 8));
    uint32_t a;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    send_to(&w, 0, a, "later"); /* a timer with a payload, in the root */
    size_t length = 0;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_OK);
    length = written.length;
    static ta_datamodel_vtable vtable;
    real_vt = w.binding.vtable;
    vtable = *w.binding.vtable;
    vtable.deserialize = failing_deserialize;
    ta_datamodel_binding fb = {&vtable, w.binding.datamodel};
    for (int k = 1; k <= 4; k++) { /* fail at the root, the kid, the grandchild; then succeed */
        ta_system_config config = config_for(&w, 1u << 20, 8);
        size_t need;
        CHECK_EQUAL(ta_system_layout(&w.image[1], &config, &need), TA_OK);
        void *memory = malloc(need);
        ta_system *system;
        CHECK_EQUAL(
            ta_system_init(&(ta_system_setup){{memory, need}, &w.image[1], &config, &fb, 1, 0},
                           &system),
            TA_OK);
        deser_calls = 0;
        deser_fail_at = k;
        uint32_t b;
        ta_status status = ta_system_adopt(system, (ta_bytes){blob, length}, &b);
        ta_system_stats stats;
        stats.size = sizeof stats;
        ta_system_stats_get(system, &stats);
        if (k < 4) {
            CHECK_EQUAL(status, TA_ERROR_NO_MEMORY);
            CHECK_EQUAL(stats.actors, 0u);
            CHECK_EQUAL(stats.timers, 0u);
            CHECK_EQUAL(deser_calls, k);
        } else {
            CHECK_EQUAL(status, TA_OK);
            CHECK_EQUAL(stats.actors, 3u);
            CHECK_EQUAL(stats.timers, 1u);
        }
        deser_fail_at = 0;
        if (status == TA_OK)
            CHECK_EQUAL(ta_actor_export(system, b, &(ta_buffer){blob, sizeof blob, 0}), TA_OK);
        ta_system_deinit(system);
        free(memory);
    }
    /* the blob is still whole: it goes home, and the timer fires there */
    uint32_t b;
    CHECK_EQUAL(ta_system_adopt(w.system[0], (ta_bytes){blob, length}, &b), TA_OK);
    w.log[0] = 0;
    advance(&w, 5000);
    CHECK_STR(w.log, "pong 1;");
    world_close(&w);
}

/* a child that vanished without done.invoke (terminated by the host, or
 * faulted) is not part of the tree: the actor exports alone */
static void test_vanished_child(void) {
    world w;
    CHECK(world_open(&w, TREE, TREE, 1, 8));
    uint32_t a;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    CHECK_EQUAL(ta_actor_info_get(w.system[0], a, &actor_info), TA_OK);
    CHECK_EQUAL(actor_info.invocations, 1u);
    uint32_t index = ta_system_resolve_actor(w.system[0], a);
    uint32_t kid = ta_core_invoke_child(
        &w.system[0]->core, ta_actor_instance(w.system[0], ta_actor_at(w.system[0], index)), 0);
    CHECK_EQUAL(ta_system_terminate(w.system[0], kid), TA_OK); /* no done.invoke */
    run(&w);
    CHECK_EQUAL(actors_in(&w, 0), 1u); /* the kid and its grandchild are gone */
    CHECK_EQUAL(ta_actor_info_get(w.system[0], a, &actor_info), TA_OK);
    CHECK_EQUAL(actor_info.invocations, 0u);
    size_t length;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_OK);
    length = written.length;
    uint32_t sessions;
    memcpy(&sessions, blob + 8, 4);
    CHECK_EQUAL(sessions, 1u);
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){blob, length}, &a), TA_OK);
    /* a smaller declared size gets exactly that much */
    unsigned char part[64];
    memset(part, 0xAB, sizeof part);
    ((ta_actor_info *)(void *)part)->size = 48;
    CHECK_EQUAL(ta_actor_info_get(w.system[1], a, (ta_actor_info *)(void *)part), TA_OK);
    CHECK(part[48] == 0xAB && part[55] == 0xAB);
    world_close(&w);
}

/* ---- history and late binding, null datamodel ---- */

#define HISTORY_CHART                                                                              \
    "<state id='s' initial='s1'><history id='h'><transition target='s1'/></history>"               \
    "<state id='s1'><transition event='two' target='s2'/></state><state id='s2'/>"                 \
    "<transition event='out' target='o'/></state>"                                                 \
    "<state id='o'><transition event='back' target='h'/></state>"

static void history_case(const char *v2, const char *expect_after_back) {
    world w;
    CHECK(world_open(&w, NULLSC("initial='s'>" HISTORY_CHART), v2, 0, 0));
    uint32_t a, b;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    const char *events[] = {"two", "out"};
    for (int i = 0; i < 2; i++) {
        ta_event_in event;
        memset(&event, 0, sizeof event);
        event.size = sizeof event;
        event.target = a;
        event.name = (ta_string_view){events[i], (uint32_t)strlen(events[i])};
        CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
        run(&w);
    }
    CHECK(strcmp(config_of(&w, 0, a), "o") == 0);
    CHECK_EQUAL(move_and_upgrade(&w, a, &b), TA_OK);
    run(&w);
    CHECK(strstr(w.log, "upgraded;") != NULL); /* null datamodel: the event, no data */
    CHECK(strcmp(config_of(&w, 1, b), "o") == 0);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = b;
    event.name = (ta_string_view){"back", 4};
    CHECK_EQUAL(ta_system_send(w.system[1], &event), TA_OK);
    run(&w);
    CHECK(strcmp(config_of(&w, 1, b), expect_after_back) == 0);
    world_close(&w);
}

#define UPG "<transition event='error.platform.upgrade'><log expr=\"'upgraded'\"/></transition>"

static void test_history(void) {
    /* recorded history kept: s3 added, s2 still under s */
    history_case(NULLSC("initial='s'><state id='s' initial='s1'>" UPG
                        "<history id='h'><transition target='s1'/></history>"
                        "<state id='s1'/><state id='s3'/><state id='s2'/>"
                        "<transition event='out' target='o'/></state>"
                        "<state id='o'>" UPG "<transition event='back' target='h'/></state>"),
                 "s,s2");
    /* s2 removed: the recording is dropped, the history default applies */
    history_case(NULLSC("initial='s'><state id='s' initial='s1'>"
                        "<history id='h'><transition target='s3'/></history>"
                        "<state id='s1'/><state id='s3'/>"
                        "<transition event='out' target='o'/></state>"
                        "<state id='o'>" UPG "<transition event='back' target='h'/></state>"),
                 "s,s3");
}

/* The active atomic state is removed: its parent enters its initial state;
 * late binding: data of newly active states is initialized, others wait. */
static void test_removed_state_late_binding(void) {
    world w;
    CHECK(world_open(
        &w,
        ESSC("binding='late' initial='m'><state id='m' initial='m1'>"
             "<datamodel><data id='keep' expr='1'/></datamodel>"
             "<state id='m1'><transition event='n' target='m2'/></state>"
             "<state id='m2'><datamodel><data id='inner' expr='2'/></datamodel>"
             "<onentry><assign location='inner' expr='inner + 40'/></onentry></state></state>"),
        ESSC("binding='late' initial='m'><state id='m' initial='m3'>"
             "<datamodel><data id='keep' expr='1'/><data id='fresh' expr='keep * 10'/></datamodel>"
             "<state id='m1'/>"
             "<state id='m3'><datamodel><data id='m3v' expr='\"m3\"'/></datamodel></state>"
             "<state id='later'><datamodel><data id='lv' expr='5'/></datamodel></state></state>"),
        1, 0));
    uint32_t a, b;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"n", 1};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    CHECK(strcmp(config_of(&w, 0, a), "m,m2") == 0);
    CHECK(strcmp(eval(&w, 0, a, "inner"), "42") == 0);
    CHECK_EQUAL(move_and_upgrade(&w, a, &b), TA_OK);
    run(&w);
    CHECK(strcmp(config_of(&w, 1, b), "m,m3") == 0);
    CHECK(strcmp(eval(&w, 1, b, "keep + ':' + fresh + ':' + m3v"), "\"1:10:m3\"") == 0);
    CHECK(strcmp(eval(&w, 1, b, "typeof inner + ':' + typeof lv"), "\"undefined:undefined\"") == 0);
    CHECK(strcmp(eval(&w, 1, b, "_event.data.inner"), "42") == 0);
    world_close(&w);
}

/* A chart whose scripts declare globals (isolated sessions) is upgraded too:
 * names the new chart declares keep their values, function declarations are
 * re-run as the new chart has them, the rest (a let binding, an implicit
 * global) is in the previous bindings and dropped. */
static void test_upgrade_declarations(void) {
    world w;
    CHECK(world_open(
        &w,
        ESSC("initial='a'><datamodel><data id='count' expr='1'/></datamodel>"
             "<script>var hits = 2; let note = 'x'; extra = [3]; "
             "function f() { return hits * 10; }</script>"
             "<state id='a'><transition event='go' target='b'/></state><state id='b'/>"),
        ESSC("initial='a'><datamodel><data id='count' expr='1'/></datamodel>"
             "<script>var hits = 0; function f() { return hits * 100 + count; }</script>"
             "<state id='a'/><state id='b'><transition event='error.platform.upgrade'>"
             "<log expr=\"'up:' + _event.data.hits + ':' + _event.data.note + ':' + "
             "_event.data.extra[0] + ':' + typeof note + ':' + typeof extra + ':' + f()\"/>"
             "</transition></state>"),
        1, 0));
    uint32_t a, b;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"go", 2};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    CHECK_STR(eval(&w, 0, a, "f()"), "20");
    CHECK_EQUAL(move_and_upgrade(&w, a, &b), TA_OK);
    run(&w);
    CHECK_STR(w.log, "up:2:x:3:undefined:undefined:201;");
    CHECK_STR(eval(&w, 1, b, "hits + ':' + count"), "\"2:1\"");
    world_close(&w);
}

/* A failed send leaves the payload with the caller (any status), so hosts can
 * grow the system and retry with the same handle. */
static void test_failed_send_keeps_payload(void) {
    world w;
    CHECK(world_open(&w, V1, V2, 1, 0));
    uint32_t a;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"noise", 5};
    ta_status status = TA_OK;
    int sent = 0;
    while (status == TA_OK && sent < 100000) { /* fill the event pool (the actor does not run) */
        event.data = ta_datamodel_ecmascript_payload_eval(w.ecmascript, 0,
                                                               (ta_string_view){"{\"k\": 1}", 8});
        status = ta_system_send(w.system[0], &event);
        sent++;
    }
    CHECK_EQUAL(status, TA_ERROR_NO_MEMORY);
    char buffer[64];
    size_t n =
        payload_text(w.ecmascript, event.data, buffer, sizeof buffer);
    CHECK(n != (size_t)-1 && n < sizeof buffer &&
          strstr(buffer, "\"k\""));                /* still ours, still valid */
    ta_system_config config = w.system[0]->config; /* grow, retry with the same handle */
    config.max_events *= 2;
    size_t nl;
    CHECK_EQUAL(ta_system_layout(&w.image[0], &config, &nl), TA_OK);
    void *nm = malloc(nl);
    ta_system *new_system = w.system[0];
    CHECK_EQUAL(ta_system_relocate(&new_system, (ta_memory){nm, nl}, &config), TA_OK);
    free(w.memory[0]);
    w.memory[0] = nm;
    w.system[0] = new_system;
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    /* stale target: also not consumed */
    event.target = 12345;
    event.data =
        ta_datamodel_ecmascript_payload_eval(w.ecmascript, 0, (ta_string_view){"{\"k\": 2}", 8});
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_ERROR_STALE);
    n = payload_text(w.ecmascript, event.data, buffer, sizeof buffer);
    CHECK(n != (size_t)-1 && strstr(buffer, "2"));
    ta_datamodel_ecmascript_vtable()->release(w.ecmascript, event.data);
    world_close(&w);
}

/* Generated ids (idlocation sendids) are unique for a session's whole life,
 * also after it moved to a system that generated none yet: the counter is
 * session state (instance header), carried with the instance. */
static const char *IDS = ESSC("initial='s'><datamodel><data id='sid' expr='null'/></datamodel>"
                              "<state id='s'><transition event='go'>"
                              "<send event='later' delay='1s' idlocation='sid'/><log expr='sid'/>"
                              "</transition></state>");

static void test_generated_ids_unique_across_adopt(void) {
    world w;
    CHECK(world_open(&w, IDS, IDS, 1, 4));
    uint32_t a, b;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"go", 2};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    size_t length;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_export(w.system[0], a, &written), TA_OK);
    length = written.length;
    CHECK_EQUAL(ta_system_adopt(w.system[1], (ta_bytes){blob, length}, &b), TA_OK);
    event.target = b;
    CHECK_EQUAL(ta_system_send(w.system[1], &event), TA_OK);
    run(&w);
    /* two different ids, though system 1 had generated none */
    const char *semi = strchr(w.log, ';');
    CHECK(semi != NULL);
    if (semi) {
        size_t n1 = (size_t)(semi - w.log);
        CHECK(n1 > 0 && strncmp(w.log, semi + 1, n1) != 0);
        if (n1 == 0 || strncmp(w.log, semi + 1, n1) == 0) fprintf(stderr, "  log: %s\n", w.log);
    }
    world_close(&w);
}

/* An isolated session (its scripts declare globals) after an upgrade: the new
 * chart's declarations stay (var, function, let, const), and the previous
 * bindings (error.platform.upgrade's data) hold no functions — they are code
 * the new chart re-creates, and a payload with one could not be stored (the
 * session's _event, snapshots). */
static const char *ISO =
    ESSC("initial='s'><datamodel><data id='box' expr='({n: 0})'/></datamodel>"
         "<script>var total = 0; let seen = []; const unit = 2;"
         " function bump(x) { total = total + x * unit; seen.push(x); return total; }</script>"
         "<state id='s'><transition event='a'><script>bump(1); box.n = box.n + 1</script>"
         "<log expr='total + \" \" + seen.length + \" \" + box.n'/></transition>"
         "<transition event='error.platform.upgrade'>"
         "<log expr='Object.keys(_event.data).sort().join(\",\")'/></transition>"
         "<transition event='error.execution'><log expr='\"error\"'/></transition></state>");

static void test_isolated_session_upgrade(void) {
    world w;
    CHECK(world_open(&w, ISO, ISO, 1, 4));
    uint32_t a, b;
    CHECK_EQUAL(
        ta_system_spawn(w.system[0],
                        &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &a),
        TA_OK);
    run(&w);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"a", 1};
    CHECK_EQUAL(ta_system_send(w.system[0], &event), TA_OK);
    run(&w);
    CHECK_EQUAL(move_and_upgrade(&w, a, &b), TA_OK);
    run(&w);
    CHECK_EQUAL(ta_actor_export(w.system[1], b, &(ta_buffer){NULL, 0, 0}),
                TA_ERROR_NO_MEMORY); /* storable */
    event.target = b;
    CHECK_EQUAL(ta_system_send(w.system[1], &event), TA_OK);
    run(&w);
    CHECK_STR(w.log, "2 1 1;box,total;4 2 2;"); /* lexical bindings are re-declared, not bindings */
    world_close(&w);
}

int main(void) {
    RUN(test_isolated_session_upgrade);
    RUN(test_generated_ids_unique_across_adopt);
    RUN(test_failed_send_keeps_payload);
    RUN(test_upgrade_roundtrip);
    RUN(test_failed_adopt_keeps_blob);
    RUN(test_export_preconditions);
    RUN(test_session_tree);
    RUN(test_session_tree_rollback);
    RUN(test_vanished_child);
    RUN(test_history);
    RUN(test_removed_state_late_binding);
    RUN(test_upgrade_declarations);
    TEST_MAIN_END();
}
