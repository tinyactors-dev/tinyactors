/* test: standalone */
/*
 * Event descriptors (ADR 0060 section 5): a `*` segment anywhere matches
 * zero or more event segments, descriptors match by prefix, and matching is
 * iterative and polynomial, so a descriptor of thousands of `*` segments
 * still matches at once.
 *
 * Includes the amalgamation, as export_test does; it uses the C library,
 * the core does not.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator heap;
static const ta_allocator test_allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

static ta_system *system_under_test;

static ta_str text(const char *value) {
    return (ta_str){ .ptr = value, .len = (uint32_t)strlen(value) };
}

/* Compiles a one-state chart whose one transition has `descriptor`, and
 * asks whether its first descriptor token matches `event`. */
static bool matches(const char *descriptor, const char *event) {
    ta_scxml_transition_spec transitions[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = text(descriptor) },
        {0},
    };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = text("s"), .transitions = transitions },
                                     {0} };
    ta_scxml_spec spec = { .name = text("patterns"), .states = states };
    ta_definition *definition = NULL;
    ta_result compiled = ta_definition_compile_spec(system_under_test,
                                                    &(ta_definition_compile_params){
                                                        .spec = &spec,
                                                        .datamodel = ta_null_datamodel(),
                                                        .allocator = test_allocator,
                                                        .out = &definition,
                                                    });
    CHECK(compiled == TA_OK);
    if (compiled != TA_OK) {
        return false;
    }
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern_dotted(&system_under_test->event_names, text(event), &name) == TA_OK);
    const ta_transition_def *transition = &definition->transitions[0];
    CHECK(transition->event_patterns.length == 1);
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    ta_event_atoms atoms = { .atoms = segments,
                             .count = ta_trie_segments(&system_under_test->event_names.trie, name, segments) };
    bool result = ta_event_pattern_matches(definition, &definition->event_patterns[transition->event_patterns.offset],
                                           atoms);
    ta_definition_destroy(definition);
    return result;
}

#define MATCHES(descriptor, event) CHECK(matches((descriptor), (event)))
#define MISSES(descriptor, event) CHECK(!matches((descriptor), (event)))

static void test_adr_examples(void) {
    /* `*` matches every event. */
    MATCHES("*", "a");
    MATCHES("*", "a.b.c");
    /* `a.*` matches a and every event below it. */
    MATCHES("a.*", "a");
    MATCHES("a.*", "a.b");
    MATCHES("a.*", "a.b.c");
    MISSES("a.*", "b");
    MISSES("a.*", "ab");
    /* `a.*.c` matches a.c, a.b.c, a.b.x.c, and by prefix a.b.c.d. */
    MATCHES("a.*.c", "a.c");
    MATCHES("a.*.c", "a.b.c");
    MATCHES("a.*.c", "a.b.x.c");
    MATCHES("a.*.c", "a.b.c.d");
    MISSES("a.*.c", "a.b");
    MISSES("a.*.c", "a");
    MISSES("a.*.c", "b.c");
    /* `*.error` matches error, x.error and x.y.error.detail. */
    MATCHES("*.error", "error");
    MATCHES("*.error", "x.error");
    MATCHES("*.error", "x.y.error.detail");
    MISSES("*.error", "x.errors");
    MISSES("*.error", "x.y");
}

static void test_prefix_and_plain(void) {
    MATCHES("a", "a");
    MATCHES("a", "a.b");
    MATCHES("a.b", "a.b.c");
    MISSES("a.b", "a");
    MISSES("a.b", "a.c");
    /* Several stars, and a star that must take more than one segment. */
    MATCHES("*.b.*.d", "a.b.c.d");
    MATCHES("*.b.*.d", "b.d");
    MATCHES("a.*.*.d", "a.d");
    MATCHES("a.*.c.*.e", "a.x.c.y.z.e");
    MISSES("a.*.c.*.e", "a.x.c.y.z");
    /* The first `c` is not the right one: backtrack to the star. */
    MATCHES("a.*.c.d", "a.c.x.c.d");
}

static void test_partial_stars_are_ordinary(void) {
    MATCHES("a*", "a*");
    MISSES("a*", "a");
    MISSES("a*", "ab");
    MATCHES("*b", "*b");
    MISSES("*b", "b");
    MISSES("*b", "ab");
}

/* Thousands of `*` segments before a segment the event lacks. The old
 * backtracking matcher took exponential time here. */
static void test_many_stars(void) {
    enum { STARS = 5000 };
    char *descriptor = malloc(STARS * 2u + 2u);
    CHECK(descriptor != NULL);
    if (descriptor == NULL) {
        return;
    }
    for (uint32_t index = 0; index < STARS; index += 1u) {
        descriptor[index * 2u] = '*';
        descriptor[index * 2u + 1u] = '.';
    }
    descriptor[STARS * 2u] = 'z';
    descriptor[STARS * 2u + 1u] = 0;
    char event[256];
    size_t length = 0;
    for (uint32_t index = 0; index < 60u; index += 1u) {
        length += (size_t)snprintf(event + length, sizeof(event) - length, index == 0 ? "e" : ".e");
    }
    clock_t start = clock();
    MISSES(descriptor, event);
    memcpy(event + length, ".z", 3);
    MATCHES(descriptor, event);
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    CHECK(seconds < 1.0);
    free(descriptor);
}

int main(void) {
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system_under_test) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    test_adr_examples();
    test_prefix_and_plain();
    test_partial_stars_are_ordinary();
    test_many_stars();
    ta_system_destroy(system_under_test);
    if (failures != 0) {
        fprintf(stderr, "event pattern test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core event pattern test passed");
    return 0;
}
