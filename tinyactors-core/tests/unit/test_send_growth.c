/* A <send> that waits for the slab to grow before it evaluates anything:
 * its expressions run once however often the slab grows. On quickjs, whose
 * expressions can count their own runs; apart from test_runtime, which the
 * release also runs against the packed runtime library alone. */
#include "../harness/harness.h"
#include "../support/file_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/runtime.h>

#include <stdlib.h>

/* A send whose event name an expression makes needs a string of the slab:
 * it waits for the slab to grow before it evaluates anything, so the name's
 * expression runs once per send however often the slab grows (each name
 * takes a page of its own: 40 of them beside a slab of a few pages). */
static void test_sends_evaluate_their_names_once(void) {
    CHECK(fixture_write(
        "names.scxml",
        "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='a'>"
        "<datamodel><data id='n' expr='0'/><data id='c' expr='0'/></datamodel>"
        "<state id='a'><onentry><foreach array='Array.from({length: 40})' item='i'>"
        "<send eventexpr=\"'dynamic.' + 'x'.repeat(3000) + (n++)\"/></foreach></onentry>"
        "<transition event='dynamic'><assign location='c' expr='c + 1'/></transition>"
        "<transition cond='c == 40' target='b'/></state>"
        "<state id='b'><onentry><log label='n' expr='n'/></onentry></state></scxml>"));
    ta_compile_result *result;
    CHECK_EQUAL(ta_runtime_compile_file(fixture_path("names.scxml"), 0, &result), TA_OK);
    ta_string_view bytes = ta_compile_image(result);
    void *copy = aligned_alloc(8, (bytes.length + 7) & ~(size_t)7);
    memcpy(copy, bytes.pointer, bytes.length);
    ta_compile_free(result);
    ta_image image;
    CHECK_EQUAL(ta_image_open((ta_bytes){copy, bytes.length}, &image), TA_OK);
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.max_events = 200; /* room for every event: only the slab fills */
    config.string_bytes = 64;
    config.max_actors = 2;
    ta_system *system;
    CHECK_EQUAL(
        ta_runtime_system_new(&(ta_system_setup){{0, 0}, &image, &config, &binding, 1, 0}, &system),
        TA_OK);
    CHECK_EQUAL(ta_system_boot(system), TA_OK);
    ta_run run = ta_runtime_run(&system, 1000000);
    CHECK_EQUAL(run.status, TA_RUN_IDLE);
    const ta_effect *effect;
    uint32_t n = ta_system_effects(system, &effect);
    CHECK_EQUAL(n, 1);
    CHECK(n == 1 && effect[0].event.length == 2 && memcmp(effect[0].event.pointer, "40", 2) == 0);
    if (n == 1 && effect[0].event.length != 2)
        fprintf(stderr, "  n = %.*s\n", (int)effect[0].event.length, effect[0].event.pointer);
    ta_system_effects_ack(system, n);
    ta_runtime_system_free(system);
    ta_datamodel_ecmascript_free(ecmascript);
    free(copy);
}

int main(void) {
    RUN(test_sends_evaluate_their_names_once);
    TEST_MAIN_END();
}
