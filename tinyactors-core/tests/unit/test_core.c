/* ta_core: kernel semantics driven directly (no ta_system). */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "../support/datamodel_test.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "core/core_internal.h"

/* ---- minimal driver ------------------------------------------------------------------ */

#define MAXI 4
typedef struct driver {
    ta_core core;
    ta_pool events, workspace;
    ta_slab slab;
    void *event_memory, *workspace_memory, *slab_memory, *slab_metadata;
    unsigned char *event_generations, *workspace_generations;
    void *bytes;
    ta_image image;
    datamodel_test *datamodel;
    ta_datamodel_ecmascript
        *ecmascript; /* the real ECMAScript datamodel instead of datamodel_test */
    ta_datamodel_binding binding[2];
    uint8_t chart_datamodel[8];
    void *chart_context[8];
    ta_instance *instances[MAXI];
    int instance_count;
    uint32_t external_queue[MAXI][64];
    int external_count[MAXI];
    char log[8192];
    int sends, invokes, cancels, dones;
    ta_payload done_payload;
} driver;

static uint64_t serial_of(void *system, uint32_t actor) {
    (void)system;
    return actor;
}

static int driver_init_datamodel(driver *d, const char *chart, uint32_t workspace_count,
                                 uint32_t event_count, int ecmascript) {
    memset(d, 0, sizeof *d);
    size_t length;
    d->bytes = compile_chart("t.scxml", chart, NULL, &length, 0);
    if (!d->bytes || ta_image_open(d->bytes, length, &d->image) != TA_OK) return 0;
    if (ecmascript) {
        d->ecmascript = ta_datamodel_ecmascript_new(NULL);
        d->binding[0] = ta_datamodel_ecmascript_binding(d->ecmascript);
    } else {
        d->datamodel = datamodel_test_new();
        d->binding[0] = datamodel_test_binding(d->datamodel);
    }
    d->binding[1].vtable = ta_datamodel_null();
    d->binding[1].datamodel = NULL;
    const ta_image_view *view = ta_image_view_of(&d->image);
    uint32_t wsz = 0;
    for (uint32_t c = 0; c < view->chart_count; c++) {
        ta_string_view dmn = ta_image_chart_datamodel(&d->image, c);
        d->chart_datamodel[c] = (dmn.length == 4 && !memcmp(dmn.pointer, "null", 4)) ? 1 : 0;
        if (d->binding[d->chart_datamodel[c]].vtable->chart_attach) {
            ta_datamodel_chart datamodel_chart = {sizeof datamodel_chart, c, &d->image,
                                                  view->charts[c].binding, 0};
            d->binding[d->chart_datamodel[c]].vtable->chart_attach(
                d->binding[d->chart_datamodel[c]].datamodel, &datamodel_chart,
                &d->chart_context[c]);
        }
        uint32_t s = (uint32_t)ta_core_workspace_size(&d->image, c);
        if (s > wsz) wsz = s;
    }
    d->event_memory = aligned_alloc(8, ta_pool_slots_bytes(sizeof(ta_event), event_count));
    d->event_generations = (unsigned char *)malloc(event_count);
    ta_pool_init(&d->events, d->event_memory, d->event_generations, sizeof(ta_event), event_count);
    d->workspace_memory = aligned_alloc(8, ta_pool_slots_bytes(wsz, workspace_count));
    d->workspace_generations = (unsigned char *)malloc(workspace_count);
    ta_pool_init(&d->workspace, d->workspace_memory, d->workspace_generations, wsz,
                 workspace_count);
    d->slab_memory = aligned_alloc(8, ta_slab_pages_bytes(16));
    d->slab_metadata = malloc(ta_slab_metadata_bytes(16));
    ta_slab_init(&d->slab, d->slab_memory, (ta_slab_page *)d->slab_metadata, 16);
    d->core.image_view = *view;
    d->core.image = &d->image;
    d->core.events = &d->events;
    d->core.workspaces = &d->workspace;
    d->core.slab = &d->slab;
    d->core.datamodels = d->binding;
    d->core.datamodel_count = 2;
    d->core.reserve = 2;
    d->core.chart_datamodel = d->chart_datamodel;
    d->core.chart_context = d->chart_context;
    d->core.serial_of = serial_of;
    return 1;
}

static int drv_init(driver *d, const char *chart, uint32_t workspace_count, uint32_t event_count) {
    return driver_init_datamodel(d, chart, workspace_count, event_count, 0);
}

static ta_instance *drv_spawn(driver *d, uint32_t chart) {
    ta_instance *instance =
        (ta_instance *)aligned_alloc(8, (ta_core_chart_instance_size(&d->core, chart) + 7) & ~7u);
    char sid[16];
    snprintf(sid, sizeof sid, "%d", d->instance_count + 1);
    ta_io_processor_entry io = {{TA_SCXML_IO_PROCESSOR, (uint32_t)strlen(TA_SCXML_IO_PROCESSOR)},
                                {"#_scxml_1", 9}};
    ta_datamodel_system_variables variables = {
        sizeof variables, 1, {sid, (uint32_t)strlen(sid)}, {"t", 1}, &io};
    CHECK_EQUAL(ta_core_instance_init(&d->core, instance, chart, (uint32_t)d->instance_count + 1,
                                      &variables, 0),
                TA_OK);
    d->instances[d->instance_count++] = instance;
    return instance;
}

static void logf_(driver *d, const char *s) {
    size_t l = strlen(d->log);
    snprintf(d->log + l, sizeof d->log - l, "%s;", s);
}

/* Step instance i until it is stable/done or `max_calls` step calls happened.
 * Returns the last step result. */
static uint32_t drv_run(driver *d, int i, uint32_t fuel_per_call, int max_calls) {
    uint32_t r = TA_STEP_YIELD;
    for (int calls = 0; calls < max_calls; calls++) {
        uint32_t fuel = fuel_per_call;
        r = ta_core_step(&d->core, d->instances[i], &fuel);
        ta_core_out *out = &d->core.out;
        switch (r) {
        case TA_STEP_SEND:
            d->sends++;
            if (out->route == TA_ROUTE_SELF && d->external_count[i] < 64)
                d->external_queue[i][d->external_count[i]++] = out->event;
            else
                ta_event_release(&d->core, out->event);
            break;
        case TA_STEP_LOG: {
            char buffer[256];
            ta_string_view label = ta_shared_string_get(&d->core, out->label);
            snprintf(buffer, sizeof buffer, "%.*s%s%.*s", (int)label.length,
                     label.pointer ? label.pointer : "", label.length ? "=" : "",
                     (int)out->text.length, out->text.pointer ? out->text.pointer : "");
            logf_(d, buffer);
            break;
        }
        case TA_STEP_INVOKE:
            d->invokes++;
            if (out->payload) {
            }
            break;
        case TA_STEP_CANCEL:
            d->cancels++;
            break;
        case TA_STEP_STABLE:
            if (d->external_count[i]) { /* deliver next external event */
                uint32_t event = d->external_queue[i][0];
                memmove(d->external_queue[i], d->external_queue[i] + 1,
                        sizeof(uint32_t) * (size_t)--d->external_count[i]);
                CHECK_EQUAL(ta_core_deliver(&d->core, d->instances[i], event), TA_OK);
                break;
            }
            return r;
        case TA_STEP_DONE:
            d->dones++;
            d->done_payload = out->payload;
            return r;
        case TA_STEP_YIELD:
        case TA_STEP_NEED_WORKSPACE:
        case TA_STEP_UNINVOKE:
        case TA_STEP_AUTOFORWARD:
            break;
        default:
            return r;
        }
    }
    return r;
}

static void drv_send(driver *d, int i, const char *name, const char *data) {
    uint32_t event_index = ta_event_new(&d->core, 0);
    ta_event *event = ta_event_at(&d->core, event_index);
    ta_event_set_name(&d->core, event, name, (uint32_t)strlen(name));
    event->type = TA_EVENT_TYPE_EXTERNAL;
    if (data) {
        event->payload = datamodel_test_payload(d->datamodel, data);
        event->datamodel = 0;
    }
    d->external_queue[i][d->external_count[i]++] = event_index;
}

/* Active states as "a,b,c" in document order. */
static const char *drv_config(driver *d, int i) {
    static char buffer[512];
    buffer[0] = 0;
    const ta_chart_record *chart = &d->core.image_view.charts[d->instances[i]->chart];
    for (uint32_t s = 1; s < chart->state_count; s++)
        if (ta_core_in_state(&d->core, d->instances[i], s)) {
            ta_string_view n = ta_image_state_name(&d->image, d->instances[i]->chart, s);
            size_t l = strlen(buffer);
            snprintf(buffer + l, sizeof buffer - l, "%s%.*s", l ? "," : "", (int)n.length,
                     n.pointer);
        }
    return buffer;
}

static void drv_free(driver *d) {
    for (int i = 0; i < d->instance_count; i++) {
        for (int k = 0; k < d->external_count[i]; k++)
            ta_event_release(&d->core, d->external_queue[i][k]);
        ta_core_instance_deinit(&d->core, d->instances[i]);
        free(d->instances[i]);
    }
    ta_core_release_out(&d->core);
    CHECK_EQUAL(d->events.used, 0);    /* no leaked events */
    CHECK_EQUAL(d->workspace.used, 0); /* no leaked workspaces */
    CHECK_EQUAL(d->slab.used_pages, 0);
    if (d->ecmascript)
        ta_datamodel_ecmascript_free(d->ecmascript); /* JS_FreeRuntime asserts nothing leaked */
    else
        datamodel_test_free(d->datamodel);
    free(d->event_memory);
    free(d->event_generations);
    free(d->workspace_memory);
    free(d->workspace_generations);
    free(d->slab_memory);
    free(d->slab_metadata);
    free(d->bytes);
}

#define SC(body)                                                                                   \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' " body "</scxml>"

/* ---- tests ------------------------------------------------------------------------------- */

static void test_basic_transition(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='a'><datamodel><data id='n' expr='0'/></datamodel>"
                      "<state id='a'><onentry><log label='enter' expr=\"'a'\"/></onentry>"
                      "<onexit><log label='exit' expr=\"'a'\"/></onexit>"
                      "<transition event='go' target='b'><assign location='n' expr='n + 1'/>"
                      "<log label='n' expr='n'/></transition></state>"
                      "<state id='b'><onentry><log label='enter' expr=\"'b'\"/></onentry></state>"),
                   2, 32));
    drv_spawn(&d, 0);
    CHECK_EQUAL(drv_run(&d, 0, 1000, 100), TA_STEP_STABLE);
    CHECK(strcmp(drv_config(&d, 0), "a") == 0);
    drv_send(&d, 0, "go", NULL);
    CHECK_EQUAL(drv_run(&d, 0, 1000, 100), TA_STEP_STABLE);
    CHECK(strcmp(drv_config(&d, 0), "b") == 0);
    CHECK(strcmp(d.log, "enter=a;exit=a;n=1;enter=b;") == 0);
    CHECK(d.instances[0]->workspace == TA_NIL); /* stable actors hold no workspace */
    drv_free(&d);
}

static void test_eventless_and_internal(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='s1'><datamodel><data id='x' expr='0'/></datamodel>"
                      "<state id='s1'><onentry><raise event='i1'/></onentry>"
                      "<transition cond='x &lt; 3' target='s1'><assign location='x' expr='x + "
                      "1'/></transition>"
                      "<transition event='i1' target='s2'/></state>"
                      "<state id='s2'><onentry><log label='x' expr='x'/></onentry></state>"),
                   1, 32));
    drv_spawn(&d, 0);
    CHECK_EQUAL(drv_run(&d, 0, 1000, 100), TA_STEP_STABLE);
    /* eventless transitions run first (x -> 3, re-entering s1 raises i1 each time),
     * then internal events: the first i1 moves to s2. */
    CHECK(strcmp(drv_config(&d, 0), "s2") == 0);
    CHECK(strcmp(d.log, "x=3;") == 0);
    drv_free(&d);
}

static void test_infinite_loop_yields(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='a'><datamodel><data id='k' expr='0'/></datamodel>"
                      "<state id='a'><transition target='b'><assign location='k' expr='k + "
                      "1'/></transition></state>"
                      "<state id='b'><transition target='a'/></state>"),
                   1, 16));
    /* two looping instances share ONE workspace: both must progress */
    drv_spawn(&d, 0);
    drv_spawn(&d, 0);
    int boundary_yields = 0;
    for (int round = 0; round < 50; round++)
        for (int i = 0; i < 2; i++) {
            uint32_t fuel = 37;
            uint32_t r = ta_core_step(&d.core, d.instances[i], &fuel);
            CHECK(r == TA_STEP_YIELD || r == TA_STEP_NEED_WORKSPACE);
            if (r == TA_STEP_YIELD && d.instances[i]->workspace == TA_NIL) boundary_yields++;
        }
    int k0 = atoi(datamodel_test_variable(d.datamodel, d.instances[0]->datamodel, "k")),
        k1 = atoi(datamodel_test_variable(d.datamodel, d.instances[1]->datamodel, "k"));
    fprintf(stderr, "  k0=%d k1=%d boundary_yields=%d\n", k0, k1, boundary_yields);
    CHECK(k0 > 30 && k1 > 30);
    CHECK(k0 - k1 <= 2 && k1 - k0 <= 2); /* round-robin, not starvation */
    CHECK(boundary_yields > 0);          /* yields between microsteps release the workspace */
    drv_free(&d);
}

static void test_event_survives_yield(void) {
    driver d;
    /* E triggers eventless processing whose guards read _event; then an internal
     * event replaces _event. fuel=1 per call forces yields everywhere. */
    CHECK(drv_init(&d,
                   SC("initial='idle'><datamodel><data id='seen' expr=\"''\"/><data id='c' "
                      "expr='0'/></datamodel>"
                      "<state id='idle'><transition event='E' target='work'/></state>"
                      "<state id='work'>"
                      "<transition cond=\"c &lt; 5 &amp;&amp; _event.name == 'E'\" target='work'>"
                      "<assign location='c' expr='c + 1'/><assign location='seen' expr='seen + "
                      "_event.name'/></transition>"
                      "<transition cond='c == 5' target='raise'/></state>"
                      "<state id='raise'><onentry><raise event='I'/></onentry>"
                      "<transition event='I' target='done'><assign location='seen' expr=\"seen + "
                      "'|' + _event.name + ':' + _event.type\"/></transition></state>"
                      "<state id='done'/>"),
                   1, 16));
    drv_spawn(&d, 0);
    CHECK_EQUAL(drv_run(&d, 0, 1, 10000), TA_STEP_STABLE);
    drv_send(&d, 0, "E", NULL);
    uint32_t r = drv_run(&d, 0, 1, 100000);
    CHECK_EQUAL(r, TA_STEP_STABLE);
    CHECK(strcmp(drv_config(&d, 0), "done") == 0);
    CHECK(strcmp(datamodel_test_variable(d.datamodel, d.instances[0]->datamodel, "seen"),
                 "EEEEE|I:internal") == 0);
    drv_free(&d);
}

static void test_foreach_yield_and_errors(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='a'><datamodel><data id='arr' expr='[1,2,3,4,5,6,7,8,9,10]'/>"
                      "<data id='sum' expr='0'/><data id='after' expr='0'/></datamodel>"
                      "<state id='a'><onentry>"
                      "<foreach array='arr' item='v' index='i'><assign location='sum' expr='sum + "
                      "v'/></foreach>"
                      "<log label='sum' expr='sum'/>"
                      "</onentry><onentry>"
                      "<assign location='nosuchvar' expr='1'/><assign location='after' expr='1'/>"
                      "</onentry><onentry><log label='after' expr='after'/></onentry>"
                      "<transition event='error.execution' target='b'/></state><state id='b'/>"),
                   1, 16));
    drv_spawn(&d, 0);
    CHECK_EQUAL(drv_run(&d, 0, 2, 10000), TA_STEP_STABLE);
    CHECK(strcmp(d.log, "sum=55;after=0;") == 0); /* failed block aborted; next block ran */
    CHECK(strcmp(drv_config(&d, 0), "b") == 0);   /* error.execution raised */
    drv_free(&d);
}

static void test_history(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='p'>"
                      "<state id='p' initial='a'>"
                      "<history id='hs'><transition target='a'/></history>"
                      "<history id='hd' type='deep'><transition target='a'/></history>"
                      "<state id='a' initial='a1'><state id='a1'><transition event='n' "
                      "target='a2'/></state><state id='a2'/></state>"
                      "<state id='b'/>"
                      "<transition event='tob' target='b'/>"
                      "<transition event='out' target='o'/></state>"
                      "<state id='o'><transition event='shallow' target='hs'/><transition "
                      "event='deep' target='hd'/></state>"),
                   1, 32));
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "p,a,a1") == 0);
    drv_send(&d, 0, "n", NULL);
    drv_send(&d, 0, "out", NULL);
    drv_send(&d, 0, "shallow", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "p,a,a1") == 0); /* shallow: a re-entered by default */
    drv_send(&d, 0, "n", NULL);
    drv_send(&d, 0, "out", NULL);
    drv_send(&d, 0, "deep", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "p,a,a2") == 0); /* deep restores a2 */
    drv_send(&d, 0, "tob", NULL);
    drv_send(&d, 0, "out", NULL);
    drv_send(&d, 0, "shallow", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "p,b") == 0);
    drv_free(&d);
}

static void test_parallel_done_and_final(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='p'><datamodel><data id='r' expr='0'/></datamodel>"
                      "<parallel id='p'>"
                      "<state id='r1' initial='r1a'><state id='r1a'><transition event='a' "
                      "target='r1f'/></state><final id='r1f'/></state>"
                      "<state id='r2' initial='r2a'><state id='r2a'><transition event='b' "
                      "target='r2f'/></state><final id='r2f'>"
                      "<donedata><param name='v' expr='42'/></donedata></final></state>"
                      "<transition event='done.state.r2' cond='_event.data.v == 42'><log "
                      "label='r2done' expr='_event.data.v'/></transition>"
                      "<transition event='done.state.p' target='end'/></parallel>"
                      "<final id='end'><donedata><content expr=\"'bye'\"/></donedata></final>"),
                   1, 32));
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "p,r1,r1a,r2,r2a") == 0);
    drv_send(&d, 0, "b", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(d.log, "r2done=42;") == 0);
    drv_send(&d, 0, "a", NULL);
    CHECK_EQUAL(drv_run(&d, 0, 1000, 100), TA_STEP_DONE);
    CHECK_EQUAL(d.dones, 1);
    CHECK(strcmp(datamodel_test_payload_string(d.datamodel, d.done_payload), "bye") == 0);
    CHECK(strcmp(drv_config(&d, 0), "") == 0); /* exitInterpreter left no active states */
    drv_free(&d);
}

static void test_conflicts_and_preemption(void) {
    driver d;
    /* both regions react to 'e'; r1's transition exits the whole parallel and
     * conflicts with r2's: r1 comes first in document order and wins. A
     * targetless transition never conflicts. */
    CHECK(drv_init(&d,
                   SC("initial='p'><parallel id='p'>"
                      "<state id='r1'><transition event='e' target='x'><log label='t' "
                      "expr=\"'r1'\"/></transition></state>"
                      "<state id='r2'><transition event='e' target='y'><log label='t' "
                      "expr=\"'r2'\"/></transition></state>"
                      "<state id='r3'><transition event='e'><log label='t' "
                      "expr=\"'r3'\"/></transition></state>"
                      "</parallel><state id='x'/><state id='y'/>"),
                   1, 32));
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    drv_send(&d, 0, "e", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "x") == 0);
    CHECK(strcmp(d.log, "t=r1;t=r3;") == 0);
    drv_free(&d);
}

static void test_late_binding_and_null_datamodel(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("binding='late' initial='a'>"
                      "<state id='a'><onentry><log label='v' expr='typeof v'/></onentry>"
                      "<transition event='go' target='b'/></state>"
                      "<state id='b'><datamodel><data id='v' expr='7'/></datamodel>"
                      "<onentry><log label='v' expr='v'/></onentry></state>"),
                   1, 16));
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    drv_send(&d, 0, "go", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(d.log, "v=undefined;v=7;") == 0);
    drv_free(&d);

    CHECK(drv_init(&d,
                   "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='p'>"
                   "<parallel id='p'><state id='a'><transition event='go' cond=\"In('b')\" "
                   "target='c'/></state>"
                   "<state id='b'/></parallel><state id='c'/></scxml>",
                   1, 16));
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    drv_send(&d, 0, "go", NULL);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(drv_config(&d, 0), "c") == 0);
    drv_free(&d);
}

static void test_send_self_and_cancel(void) {
    driver d;
    CHECK(drv_init(
        &d,
        SC("initial='a'><datamodel><data id='sid' expr=\"''\"/></datamodel>"
           "<state id='a'><onentry><send event='ping' idlocation='sid'><param name='x' "
           "expr='5'/></send>"
           "<cancel sendidexpr='sid'/></onentry>"
           "<transition event='ping' cond='_event.data.x == 5' target='b'>"
           "<log label='origin' expr='typeof _event.origin'/></transition></state><state id='b'/>"),
        1, 16));
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    CHECK_EQUAL(d.sends, 1);
    CHECK_EQUAL(d.cancels, 1);
    CHECK(strcmp(drv_config(&d, 0), "b") == 0);
    CHECK(strcmp(d.log, "origin=string;") == 0);
    CHECK(strncmp(datamodel_test_variable(d.datamodel, d.instances[0]->datamodel, "sid"), "send.",
                  5) == 0);
    drv_free(&d);
}

static void test_toplevel_script(void) {
    driver d;
    CHECK(drv_init(&d,
                   SC("initial='s0'><script>var Var1 = 1</script>"
                      "<state id='s0'><onentry><log label='v' expr='Var1'/></onentry></state>"),
                   1, 16));
    const ta_chart_record *chart = &d.core.image_view.charts[0];
    CHECK(chart->script != TA_NIL);
    drv_spawn(&d, 0);
    drv_run(&d, 0, 1000, 100);
    CHECK(strcmp(d.log, "v=1;") == 0);
    if (strcmp(d.log, "v=1;"))
        fprintf(stderr, "  log: %s Var1=%s phase=%u\n", d.log,
                datamodel_test_variable(d.datamodel, d.instances[0]->datamodel, "Var1"),
                d.instances[0]->phase);
    drv_free(&d);
}

/* Late binding (SCXML 5.3): every <data> exists from the start; each gets its
 * value when its state is first entered. The document is entered at start, so
 * top-level <data> is assigned before the top-level <script> runs; nested
 * <data> on its state's first entry, never again (also not when history
 * re-enters the state). Both datamodels; fuel 1 yields inside every phase. */
static void test_late_binding_entry(void) {
    static const char *const chart =
        SC("binding='late' initial='a'>"
           "<datamodel><data id='t' expr='5'/><data id='u' expr='t + 1'/></datamodel>"
           "<script>t = t + 1</script>"
           "<state id='a'><onentry><log label='a' expr=\"typeof v + ' ' + t + ' ' + u\"/>"
           "</onentry><transition event='go' target='p'/></state>"
           "<state id='p' initial='c1'><datamodel><data id='v' expr='t * 10'/></datamodel>"
           "<history id='h' type='deep'><transition target='c1'/></history>"
           "<state id='c1'><datamodel><data id='w' expr='v + 1'/></datamodel>"
           "<onentry><log label='c1' expr=\"v + ' ' + w\"/></onentry>"
           "<transition event='next' target='c2'/></state>"
           "<state id='c2'><datamodel><data id='x' expr='w * 2'/></datamodel>"
           "<onentry><log label='c2' expr='x'/></onentry></state>"
           "<transition event='out' target='o'><assign location='v' expr='v + 1'/>"
           "<assign location='w' expr='w + 1'/></transition></state>"
           "<state id='o'><transition event='back' target='h'/>"
           "<transition event='again' target='p'/></state>");
    static const char *const events[] = {"go",  "out",  "back", "next",
                                         "out", "back", "out",  "again"};
    for (int ecmascript = 0; ecmascript < 2; ecmascript++)
        for (uint32_t fuel = 1; fuel <= 1000; fuel += 999) {
            driver d;
            CHECK(driver_init_datamodel(&d, chart, 1, 32, ecmascript));
            drv_spawn(&d, 0);
            CHECK_EQUAL(drv_run(&d, 0, fuel, 100000), TA_STEP_STABLE);
            for (size_t i = 0; i < sizeof events / sizeof *events; i++) {
                drv_send(&d, 0, events[i], NULL);
                CHECK_EQUAL(drv_run(&d, 0, fuel, 100000), TA_STEP_STABLE);
            }
            CHECK(strcmp(drv_config(&d, 0), "p,c1") == 0);
            /* top-level data before the script (u from t = 5); v and w once (history
             * and default re-entry keep the assigned values); x on c2's first entry */
            const char *want = "a=undefined 6 6;c1=60 61;c1=61 62;c2=124;c2=124;c1=63 64;";
            CHECK(strcmp(d.log, want) == 0);
            if (strcmp(d.log, want))
                fprintf(stderr, "  es=%d fuel=%u log: %s\n", ecmascript, fuel, d.log);
            drv_free(&d);
        }
}

/* <send delayexpr>: durations beyond 64 bits of nanoseconds saturate ("never") */
static void test_duration_saturates(void) {
    uint64_t ns = 0;
    CHECK(ta_core_parse_duration((ta_string_view){"1.5s", 4}, &ns) && ns == 1500000000ull);
    CHECK(ta_core_parse_duration((ta_string_view){"300000000000s", 13}, &ns) && ns == UINT64_MAX);
}

int main(void) {
    RUN(test_duration_saturates);
    RUN(test_basic_transition);
    RUN(test_eventless_and_internal);
    RUN(test_infinite_loop_yields);
    RUN(test_event_survives_yield);
    RUN(test_foreach_yield_and_errors);
    RUN(test_history);
    RUN(test_parallel_done_and_final);
    RUN(test_conflicts_and_preemption);
    RUN(test_late_binding_and_null_datamodel);
    RUN(test_late_binding_entry);
    RUN(test_send_self_and_cancel);
    RUN(test_toplevel_script);
    TEST_MAIN_END();
}
