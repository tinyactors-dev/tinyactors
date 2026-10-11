/* ta_runtime: allocator, files, resolver, clock, grow-by-relocation. */
#include "../harness/harness.h"
#include "../support/datamodel_test.h"
#include "../support/file_util.h"
#include <tinyactors/runtime.h>

#include <stdlib.h>

static void test_alloc_files_clock(void) {
    ta_allocator allocator = ta_runtime_malloc();
    void *p = allocator.alloc(allocator.context, 100, 8);
    CHECK(p != NULL && ((uintptr_t)p & 7u) == 0);
    allocator.free(allocator.context, p, 100);

    void *bytes;
    size_t length;
    CHECK_EQUAL(ta_runtime_load_file(fixture_path("does-not-exist"), &bytes, &length),
                TA_ERROR_INVALID);
    CHECK(fixture_write("hello.txt", "hello"));
    CHECK_EQUAL(ta_runtime_load_file(fixture_path("hello.txt"), &bytes, &length), TA_OK);
    CHECK(length == 5 && memcmp(bytes, "hello", 5) == 0 && ((uintptr_t)bytes & 7u) == 0);
    ta_runtime_free_file(bytes, length);

    ta_time t0 = ta_runtime_now_ns(), t1 = ta_runtime_now_ns();
    CHECK(t1 >= t0 && t0 > 0);
}

static void test_compile_file_with_source(void) {
    CHECK(fixture_write(
        "child.scxml",
        "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='c'><final id='c'/></scxml>"));
    CHECK(fixture_write("parent.scxml",
                        "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='p'>"
                        "<state id='p'><invoke src='child.scxml'/></state></scxml>"));
    ta_compile_result *result;
    CHECK_EQUAL(ta_runtime_compile_file(fixture_path("parent.scxml"), 0, &result), TA_OK);
    ta_string_view image_view = ta_compile_image(result);
    CHECK(image_view.length > 0);
    void *copy = aligned_alloc(8, (image_view.length + 7) & ~(size_t)7);
    memcpy(copy, image_view.pointer, image_view.length);
    ta_image image;
    CHECK_EQUAL(ta_image_open(copy, image_view.length, &image), TA_OK);
    ta_image_info info;
    ta_image_get_info(&image, &info);
    CHECK_EQUAL(info.chart_count, 2); /* child resolved relative to the parent */
    free(copy);
    ta_compile_free(result);
    CHECK(ta_runtime_compile_file(fixture_path("missing.scxml"), 0, &result) != TA_OK);
    ta_compile_free(result);
}

static void test_growable_system(void) {
    CHECK(fixture_write(
        "burst.scxml",
        "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='a'>"
        "<datamodel><data id='n' expr='0'/></datamodel>"
        "<state id='a'><onentry>"
        "<foreach "
        "array='[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,"
        "31,32,33,34,35,36,37,38,39,40]' item='i'>"
        "<send eventexpr=\"'dynamic.name.number.' + i\"/></foreach></onentry>"
        "<transition event='dynamic'><assign location='n' expr='n + 1'/></transition>"
        "<transition cond='n == 40' target='b'/></state>"
        "<state id='b'><onentry><log label='n' expr='n'/></onentry></state></scxml>"));
    ta_compile_result *result;
    CHECK_EQUAL(ta_runtime_compile_file(fixture_path("burst.scxml"), 0, &result), TA_OK);
    ta_string_view bytes = ta_compile_image(result);
    void *copy = aligned_alloc(8, (bytes.length + 7) & ~(size_t)7);
    memcpy(copy, bytes.pointer, bytes.length);
    ta_compile_free(result);
    ta_image image;
    CHECK_EQUAL(ta_image_open(copy, bytes.length, &image), TA_OK);
    datamodel_test *datamodel = datamodel_test_new();
    ta_datamodel_binding binding = datamodel_test_binding(datamodel);
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.max_events = 8;    /* far too small: 40 queued sends */
    config.string_bytes = 64; /* far too small: 40 dynamic event names */
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
    ta_system_effects_ack(system, n);
    ta_runtime_system_free(system);
    datamodel_test_free(datamodel);
    free(copy);
}

/* Limits: counts clamp to the handle widths before byte budgets are derived
 * from them; the generic grow targets the pools that are filling up. */
static void test_limits_and_targeted_grow(void) {
    CHECK(fixture_write("tiny.scxml", "<scxml xmlns='http://www.w3.org/2005/07/scxml' "
                                      "datamodel='null' initial='a'><state id='a'/></scxml>"));
    ta_compile_result *result;
    CHECK_EQUAL(ta_runtime_compile_file(fixture_path("tiny.scxml"), 0, &result), TA_OK);
    ta_string_view bytes = ta_compile_image(result);
    void *copy = aligned_alloc(8, (bytes.length + 7) & ~(size_t)7);
    memcpy(copy, bytes.pointer, bytes.length);
    ta_compile_free(result);
    ta_image image;
    CHECK_EQUAL(ta_image_open(copy, bytes.length, &image), TA_OK);

    ta_system_config huge, max;
    memset(&huge, 0, sizeof huge);
    huge.size = sizeof huge;
    max = huge;
    huge.max_actors = huge.max_events = huge.max_timers = 0xFFFFFFFFu;
    max.max_actors = TA_SYSTEM_MAX_ACTORS;
    max.max_events = max.max_timers = TA_SYSTEM_MAX_POOL;
    size_t need_huge = 0, need_max = 0;
    CHECK_EQUAL(ta_system_layout(&image, &huge, &need_huge), TA_OK);
    CHECK_EQUAL(ta_system_layout(&image, &max, &need_max), TA_OK);
    CHECK(need_huge == need_max && need_max > (size_t)TA_SYSTEM_MAX_ACTORS * 48);

    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.max_actors = 2;
    config.max_events = 64;
    config.max_timers = 64;
    ta_system *system;
    CHECK_EQUAL(
        ta_runtime_system_new(&(ta_system_setup){{0, 0}, &image, &config, NULL, 0, 0}, &system),
        TA_OK);
    CHECK_EQUAL(ta_system_boot(system), TA_OK);
    uint32_t actor = 0;
    ta_status status = TA_OK;
    for (int i = 0; i < 3 && status == TA_OK; i++)
        status = ta_system_spawn(
            system, &(ta_spawn){0, 0, (ta_string_view){"", 0}, 0, (ta_string_view){0, 0}}, &actor);
    CHECK_EQUAL(status, TA_ERROR_NO_MEMORY); /* two actors fit */
    CHECK_EQUAL(ta_runtime_system_grow(&system), TA_OK);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(system, &stats);
    CHECK_EQUAL(stats.actors_capacity, 4);  /* the full pool doubled ... */
    CHECK_EQUAL(stats.events_capacity, 64); /* ... the idle ones did not */
    CHECK_EQUAL(stats.timers_capacity, 64);
    CHECK_EQUAL(
        ta_system_spawn(
            system, &(ta_spawn){0, 0, (ta_string_view){"", 0}, 0, (ta_string_view){0, 0}}, &actor),
        TA_OK);
    ta_runtime_system_free(system);
    free(copy);
}

int main(void) {
    RUN(test_alloc_files_clock);
    RUN(test_compile_file_with_source);
    RUN(test_growable_system);
    RUN(test_limits_and_targeted_grow);
    TEST_MAIN_END();
}
