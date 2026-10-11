/* End to end: SCXML files on disk -> manifest compile with src resolution ->
 * growable runtime system -> simulated time -> golden trace. */
#include "../harness/harness.h"
#include "../support/datamodel_test.h"
#include "../support/file_util.h"
#include <tinyactors/runtime.h>

#include <stdlib.h>

static const char *SUPERVISOR =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='run'>\n"
    " <datamodel><data id='done' expr='0'/><data id='sum' expr='0'/></datamodel>\n"
    " <state id='run'>\n"
    "  <invoke id='w1' src='worker.scxml'><param name='n' expr='3'/></invoke>\n"
    "  <invoke id='w2' src='worker.scxml'><param name='n' expr='5'/></invoke>\n"
    "  <transition event='tick'><log label='tick' expr=\"_event.invokeid + '@' + "
    "_event.data.i\"/></transition>\n"
    "  <transition event='done.invoke.*'><assign location='done' expr='done + 1'/>\n"
    "   <assign location='sum' expr='sum + _event.data.total'/></transition>\n"
    "  <transition cond='done == 2' target='finished'/>\n"
    " </state>\n"
    " <final id='finished'><onentry><log label='sum' expr='sum'/></onentry>\n"
    "  <donedata><content expr='sum'/></donedata></final>\n"
    "</scxml>\n";

static const char *WORKER =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='loop'>\n"
    " <datamodel><data id='n' expr='1'/><data id='i' expr='0'/><data id='total' "
    "expr='0'/></datamodel>\n"
    " <state id='loop'>\n"
    "  <onentry><send event='step' delay='100ms'/></onentry>\n"
    "  <transition event='step' cond='i &lt; n' target='loop'><assign location='i' expr='i + 1'/>\n"
    "   <assign location='total' expr='total + i'/>\n"
    "   <send event='tick' target='#_parent'><param name='i' expr='i'/></send></transition>\n"
    "  <transition event='step' target='end'/>\n"
    " </state>\n"
    " <final id='end'><donedata><param name='total' expr='total'/></donedata></final>\n"
    "</scxml>\n";

static const char *MANIFEST = "<system><chart name='supervisor' src='supervisor.scxml'/>"
                              "<actor name='boss' chart='supervisor'/></system>";

/* Run the system to completion under simulated time; returns the trace. */
static void run_trace(const ta_image *image, uint32_t quantum, char *trace, size_t capacity,
                      uint64_t *end_time) {
    datamodel_test *datamodel = datamodel_test_new();
    ta_datamodel_binding binding = datamodel_test_binding(datamodel);
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.ticks_per_second = 1000; /* ms */
    config.quantum = quantum;
    config.max_actors = 2; /* grows to 4 on demand */
    config.max_events = 4;
    ta_system *system;
    CHECK_EQUAL(
        ta_runtime_system_new(&(ta_system_setup){{0, 0}, image, &config, &binding, 1, 0}, &system),
        TA_OK);
    CHECK_EQUAL(ta_system_boot(system), TA_OK);
    trace[0] = 0;
    uint64_t now = 0;
    for (int guard = 0; guard < 100000; guard++) {
        ta_run result = ta_runtime_run(&system, 1000);
        const ta_effect *effect;
        uint32_t n = ta_system_effects(system, &effect);
        for (uint32_t i = 0; i < n; i++) {
            size_t l = strlen(trace);
            if (effect[i].kind == TA_EFFECT_LOG)
                snprintf(trace + l, capacity - l, "%llu:%.*s=%.*s;", (unsigned long long)now,
                         (int)effect[i].auxiliary.length, effect[i].auxiliary.pointer,
                         (int)effect[i].event.length, effect[i].event.pointer);
            else if (effect[i].kind == TA_EFFECT_DONE)
                snprintf(trace + l, capacity - l, "DONE(%.*s)=%s;", (int)effect[i].target.length,
                         effect[i].target.pointer,
                         datamodel_test_payload_string(datamodel, effect[i].payload));
        }
        ta_system_effects_ack(system, n);
        if (result.status == TA_RUN_IDLE) {
            if (result.next_deadline == UINT64_MAX) break;
            now = result.next_deadline; /* host event loop: sleep until the deadline */
            ta_system_advance_time(system, now);
        }
    }
    *end_time = now;
    ta_runtime_system_free(system);
    datamodel_test_free(datamodel);
}

static void test_supervisor_workers(void) {
    CHECK(fixture_write("supervisor.scxml", SUPERVISOR));
    CHECK(fixture_write("worker.scxml", WORKER));
    CHECK(fixture_write("sys.xml", MANIFEST));
    ta_compile_result *result;
    ta_status status = ta_runtime_compile_file(fixture_path("sys.xml"), 1, &result);
    CHECK_EQUAL(status, TA_OK);
    if (status != TA_OK) {
        const ta_diagnostic *diagnostic;
        uint32_t nd = ta_compile_diagnostics(result, &diagnostic);
        for (uint32_t i = 0; i < nd; i++)
            fprintf(stderr, "  %.*s:%u: %.*s\n", (int)diagnostic[i].file.length,
                    diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "",
                    diagnostic[i].line, (int)diagnostic[i].message.length,
                    diagnostic[i].message.pointer);
        ta_compile_free(result);
        return;
    }
    ta_string_view bytes = ta_compile_image(result);
    void *copy = aligned_alloc(8, (bytes.length + 7) & ~(size_t)7);
    memcpy(copy, bytes.pointer, bytes.length);
    ta_compile_free(result);
    ta_image image;
    CHECK_EQUAL(ta_image_open(copy, bytes.length, &image), TA_OK);

    static char t1[8192], t2[8192];
    uint64_t end1, end2;
    run_trace(&image, 1000, t1, sizeof t1, &end1);
    const char *golden =
        "100:tick=w1@1;100:tick=w2@1;200:tick=w1@2;200:tick=w2@2;300:tick=w1@3;300:tick=w2@3;"
        "400:tick=w2@4;500:tick=w2@5;600:sum=21;DONE(boss)=21;";
    if (strcmp(t1, golden) != 0) fprintf(stderr, "  trace : %s\n  golden: %s\n", t1, golden);
    CHECK(strcmp(t1, golden) == 0);
    CHECK_EQUAL(end1, 600);
    /* same image, tiny quantum: scheduling changes, observable trace does not */
    run_trace(&image, 3, t2, sizeof t2, &end2);
    CHECK(strcmp(t1, t2) == 0);
    free(copy);
}

int main(void) {
    RUN(test_supervisor_workers);
    TEST_MAIN_END();
}
