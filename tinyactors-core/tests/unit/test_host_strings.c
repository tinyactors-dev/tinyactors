/* Strings an input carries are at most TA_SYSTEM_MAX_STRING bytes (system.h):
 * a longer one is refused with TA_ERROR_INVALID at the call, before anything
 * is allocated, and the host does not grow a segment for it. A chart's
 * expression that gives a longer string (an eventexpr, a targetexpr, a
 * cancel's sendidexpr, an invoke's srcexpr) is error.execution; a <log>'s
 * text is cut at a character instead. A segment
 * that cannot grow any more stops the host's retries. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_config.h"

#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"

/* Logs every event it gets; `long:<what>` makes it evaluate a long string
 * where the chart keeps one. */
#define CHART                                                                                      \
    SC("echo",                                                                                     \
       "initial='s'><datamodel><data id='big' expr=\"'x'.repeat(5000)\"/>"                         \
       "<data id='edge' expr=\"'y'.repeat(4087)\"/></datamodel>"                                  \
       "<state id='s'>"                                                                            \
       "<transition event='long.event'><send eventexpr='big' target='#_internal'/></transition>"   \
       "<transition event='long.target'><send event='t' targetexpr='big'/></transition>"           \
       "<transition event='long.cancel'><cancel sendidexpr='big'/></transition>"                   \
       "<transition event='long.invoke' target='i'/>"                                              \
       "<transition event='later'><send eventexpr='edge' delay='100s'/></transition>"               \
       "<transition event='log.long'><log expr='big'/></transition>"                               \
       "<transition event='log.edge'><log expr='edge'/></transition>"                              \
       "<transition event='log.two'><log expr=\"'a'.repeat(4086) + '\\u00e9'\"/></transition>"      \
       "<transition event='log.four'><log expr=\"'a'.repeat(4085) + '\\u{1F600}'\"/></transition>"  \
       "<transition event='*'><log expr=\"'got:' + _event.name.length\"/></transition>"            \
       "</state>"                                                                                  \
       "<state id='i'><invoke type='scxml' srcexpr='big'/>"                                        \
       "<transition event='*' target='s'><log expr=\"'got:' + _event.name\"/></transition>"        \
       "</state>")

/* The host's allocator, with a ceiling: without the limit, a long string
 * grew a segment until the allocator refused (the process was killed at 6 GB
 * before), so a test that regresses ends here instead. */
#define CEILING ((size_t)256 << 20)
typedef struct capped {
    size_t live, peak;
    uint32_t allocations;
} capped;
static capped memory;

static void *capped_alloc(void *context, size_t size, size_t align) {
    (void)context;
    if (memory.live + size > CEILING) return NULL;
    ta_allocator c = ta_runtime_malloc();
    void *p = c.alloc(c.context, size, align);
    if (p) {
        memory.live += size;
        memory.allocations++;
        if (memory.live > memory.peak) memory.peak = memory.live;
    }
    return p;
}
static void capped_free(void *context, void *pointer, size_t size) {
    (void)context;
    memory.live -= size;
    ta_allocator c = ta_runtime_malloc();
    c.free(c.context, pointer, size);
}

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *image;
    uint64_t session;
    char log[1024];
    char last[TA_SYSTEM_MAX_STRING + 1]; /* the last log's text */
    uint32_t last_length;
} world;

static char text[TA_SYSTEM_MAX_STRING + 2]; /* 'a' repeated, no terminator needed */

static ta_string_view sized(uint32_t length) {
    return (ta_string_view){text, length};
}

static void drain(world *w) {
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w->host, &effects);
    for (uint32_t i = 0; i < n; i++)
        if (effects[i].kind == TA_EFFECT_LOG) {
            size_t l = strlen(w->log);
            snprintf(w->log + l, sizeof w->log - l, "%.*s;", (int)effects[i].event.length,
                     effects[i].event.pointer);
            w->last_length = effects[i].event.length;
            if (w->last_length <= TA_SYSTEM_MAX_STRING)
                memcpy(w->last, effects[i].event.pointer, w->last_length);
        }
    ta_host_effects_ack(w->host);
}

static ta_run run(world *w) {
    ta_run result = {0};
    for (int i = 0; i < 20; i++) {
        result = ta_host_run(w->host, 100000);
        drain(w);
        if (result.status != TA_RUN_EFFECTS && result.status != TA_RUN_EFFECTS_FULL &&
            result.status != TA_RUN_YIELDED)
            break;
    }
    return result;
}

static void world_open(world *w) {
    memset(w, 0, sizeof *w);
    memset(text, 'a', sizeof text);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 4;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){NULL, capped_alloc, capped_free};
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length = 0;
    w->image = compile_chart("echo.scxml", CHART, NULL, &length, 0);
    CHECK(w->image != NULL);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){w->image, length}, &segment), TA_OK);
    ta_spawn spawn = {segment, 0, {0, 0}, 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->session), TA_OK);
    run(w);
    w->log[0] = 0;
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image);
    CHECK_EQUAL(memory.live, 0u);
}

static ta_event_in event_for(uint64_t session, ta_string_view name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = name;
    event.session = session;
    return event;
}

/* Each string field of ta_event_in, at the limit and one past it. */
enum { NAME, SENDID, ORIGIN, ORIGINTYPE, RAW, FIELDS };
static const char *const field_names[FIELDS] = {"name", "sendid", "origin", "origintype", "raw"};

static ta_event_in with_field(uint64_t session, uint32_t field, uint32_t length) {
    ta_event_in event = event_for(session, (ta_string_view){"e", 1});
    switch (field) {
    case NAME: event.name = sized(length); break;
    case SENDID: event.sendid = sized(length); break;
    case ORIGIN: event.origin = sized(length); break;
    case ORIGINTYPE: event.origintype = sized(length); break;
    default: event.raw = sized(length); break;
    }
    return event;
}

static void test_send_refuses_a_string_past_the_limit(void) {
    for (uint32_t field = 0; field < FIELDS; field++) {
        world w;
        world_open(&w);
        uint32_t before = memory.allocations;
        ta_event_in over = with_field(w.session, field, TA_SYSTEM_MAX_STRING + 1);
        ta_status status = ta_host_send(w.host, &over);
        CHECK_EQUAL(status, TA_ERROR_INVALID);
        if (status != TA_ERROR_INVALID)
            fprintf(stderr, "  %s: %s\n", field_names[field], ta_status_name(status));
        CHECK_EQUAL(memory.allocations, before); /* no segment grew for it */
        ta_event_in at = with_field(w.session, field, TA_SYSTEM_MAX_STRING);
        CHECK_EQUAL(ta_host_send(w.host, &at), TA_OK);
        run(&w);
        /* the event at the limit arrived; the name's length is its own */
        CHECK(strstr(w.log, field == NAME ? "got:4087;" : "got:1;") != NULL);
        world_close(&w);
    }
}

static void test_send_many_stops_at_a_string_past_the_limit(void) {
    world w;
    world_open(&w);
    ta_event_in events[3] = {event_for(w.session, (ta_string_view){"a", 1}),
                             event_for(w.session, sized(TA_SYSTEM_MAX_STRING + 1)),
                             event_for(w.session, (ta_string_view){"c", 1})};
    uint32_t before = memory.allocations;
    CHECK_EQUAL(ta_host_send_many(w.host, events, 3), 1u); /* the accepted prefix */
    CHECK_EQUAL(memory.allocations, before);
    run(&w);
    CHECK(strcmp(w.log, "got:1;") == 0);
    world_close(&w);
}

/* The system's own call refuses it too, as the host's datamodels reach it. */
static void test_system_send_refuses_a_string_past_the_limit(void) {
    world w;
    world_open(&w);
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(w.host, w.session, &location), TA_OK);
    ta_event_in over = event_for(0, sized(TA_SYSTEM_MAX_STRING + 1));
    over.target = location.actor;
    CHECK_EQUAL(ta_system_send(ta_host_system(w.host, location.segment), &over), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_system_send_many(ta_host_system(w.host, location.segment), &over, 1), 0u);
    world_close(&w);
}

static void test_spawn_refuses_a_name_past_the_limit(void) {
    world w;
    world_open(&w);
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w.host, (ta_string_view){"echo", 4}, &ref), TA_OK);
    uint64_t session = 0;
    uint32_t before = memory.allocations;
    ta_spawn over = {ref.segment, ref.chart, sized(TA_SYSTEM_MAX_STRING + 1), 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w.host, &over, &session), TA_ERROR_INVALID);
    CHECK_EQUAL(session, 0u);
    CHECK_EQUAL(memory.allocations, before);
    ta_spawn at = {ref.segment, ref.chart, sized(TA_SYSTEM_MAX_STRING), 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w.host, &at, &session), TA_OK);
    CHECK_EQUAL(ta_host_actor_by_name(w.host, sized(TA_SYSTEM_MAX_STRING)), session);
    /* a long traceparent is parsed, not kept: it starts a new trace */
    ta_spawn traced = {ref.segment, ref.chart, {0, 0}, 0, sized(TA_SYSTEM_MAX_STRING + 1)};
    CHECK_EQUAL(ta_host_spawn(w.host, &traced, &session), TA_OK);
    world_close(&w);
}

/* An exported session named at the limit, with a delayed send whose event
 * name is at the limit, is adopted again: a blob's strings are the system's
 * own, so they fit. A blob that names a longer one (the session's name or
 * the timer's event) is malformed: TA_ERROR_INVALID, and nothing grows. */
static void test_a_blob_with_a_string_past_the_limit_is_invalid(void) {
    world w;
    world_open(&w);
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w.host, (ta_string_view){"echo", 4}, &ref), TA_OK);
    uint64_t session = 0;
    ta_spawn named = {ref.segment, ref.chart, sized(TA_SYSTEM_MAX_STRING), 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w.host, &named, &session), TA_OK);
    ta_event_in later = event_for(session, (ta_string_view){"later", 5});
    CHECK_EQUAL(ta_host_send(w.host, &later), TA_OK);
    run(&w);
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(w.host, session, &location), TA_OK);
    ta_system *system = ta_host_system(w.host, location.segment);
    static unsigned char bytes[65536] __attribute__((aligned(8)));
    ta_buffer blob = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_actor_export(system, location.actor, &blob), TA_OK);
    /* the lengths of the name and of the timer's event, each one past the
     * limit in turn (the blob pads a text to 4 bytes, so its bytes are there) */
    uint32_t want = TA_SYSTEM_MAX_STRING, longer = TA_SYSTEM_MAX_STRING + 1;
    uint32_t found = 0;
    for (size_t at = 0; at + 4 <= blob.length; at++) {
        if (memcmp(bytes + at, &want, 4) != 0) continue;
        found++;
        memcpy(bytes + at, &longer, 4);
        uint32_t actor = 0, before = memory.allocations;
        CHECK_EQUAL(ta_system_adopt(system, (ta_bytes){bytes, blob.length}, &actor),
                    TA_ERROR_INVALID);
        CHECK_EQUAL(memory.allocations, before);
        memcpy(bytes + at, &want, 4);
    }
    CHECK_EQUAL(found, 2u);
    uint32_t actor = 0;
    CHECK_EQUAL(ta_system_adopt(system, (ta_bytes){bytes, blob.length}, &actor), TA_OK);
    CHECK_EQUAL(ta_host_actor_by_name(w.host, sized(TA_SYSTEM_MAX_STRING)), session);
    world_close(&w);
}

/* A chart's expression whose string is past the limit: error.execution, and
 * the run ends (it used to be EXHAUSTED, and the host grew the segment again
 * and again). */
static void test_an_expression_past_the_limit_is_error_execution(void) {
    static const char *const triggers[] = {"long.event", "long.target", "long.cancel",
                                           "long.invoke"};
    for (uint32_t i = 0; i < sizeof triggers / sizeof *triggers; i++) {
        world w;
        world_open(&w);
        ta_event_in go = event_for(w.session, (ta_string_view){triggers[i], (uint32_t)strlen(triggers[i])});
        CHECK_EQUAL(ta_host_send(w.host, &go), TA_OK);
        CHECK_EQUAL(run(&w).status, TA_RUN_IDLE);
        const char *want = i == 3 ? "got:error.execution;" : "got:15;"; /* "error.execution" */
        CHECK(strstr(w.log, want) != NULL);
        if (!strstr(w.log, want)) fprintf(stderr, "  %s: log \"%s\"\n", triggers[i], w.log);
        CHECK(memory.peak < CEILING / 4); /* nothing grew without bound */
        world_close(&w);
    }
}

/* A <log> text past the limit comes out cut to the longest text that fits,
 * before a UTF-8 continuation byte; one at the limit is whole. */
static void test_a_long_log_is_cut_at_a_character(void) {
    static const struct {
        const char *event;
        uint32_t length; /* what comes out: 'a's or 'y's, then nothing of the character */
    } cases[] = {{"log.long", TA_SYSTEM_MAX_STRING},
                 {"log.edge", TA_SYSTEM_MAX_STRING},
                 {"log.two", TA_SYSTEM_MAX_STRING - 1},   /* é at 4086..4087 */
                 {"log.four", TA_SYSTEM_MAX_STRING - 2}}; /* U+1F600 at 4085..4088 */
    for (uint32_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        world w;
        world_open(&w);
        ta_event_in go = event_for(w.session, (ta_string_view){cases[i].event, (uint32_t)strlen(cases[i].event)});
        CHECK_EQUAL(ta_host_send(w.host, &go), TA_OK);
        CHECK_EQUAL(run(&w).status, TA_RUN_IDLE);
        CHECK_EQUAL(w.last_length, cases[i].length);
        char want = i == 1 ? 'y' : i == 0 ? 'x' : 'a';
        uint32_t same = 0;
        while (same < w.last_length && w.last[same] == want)
            same++;
        CHECK_EQUAL(same, cases[i].length);
        world_close(&w);
    }
}

int main(void) {
    RUN(test_send_refuses_a_string_past_the_limit);
    RUN(test_send_many_stops_at_a_string_past_the_limit);
    RUN(test_system_send_refuses_a_string_past_the_limit);
    RUN(test_spawn_refuses_a_name_past_the_limit);
    RUN(test_a_blob_with_a_string_past_the_limit_is_invalid);
    RUN(test_an_expression_past_the_limit_is_error_execution);
    RUN(test_a_long_log_is_cut_at_a_character);
    TEST_MAIN_END();
}
