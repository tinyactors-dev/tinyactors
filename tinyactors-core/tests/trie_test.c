/* test: standalone asan */
/*
 * Block D7 (src/trie.h): the trie of atoms.
 *
 * Checks: the root and the depth limit, that a path interned twice has one
 * node, that find never makes one, that node IDs are dense in the order
 * paths were first interned (they appear in traces), segments, depth and
 * under against a reference model over thousands of random paths, growth of
 * the index past many pages, and an allocation failure sweep over
 * ta_trie_child that leaves no node behind and leaks nothing.
 *
 * The trie takes atoms as plain numbers, so the test needs no atom table.
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator heap = { .strict = true };

static uint64_t rng_state = 0x2545F4914F6CDD1Dull;

static uint32_t random32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static void test_basics(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_atom_trie trie;
    CHECK(ta_trie_init(&trie, ta_test_allocator_get(&heap), 0) == TA_ERR_INVALID);
    CHECK(ta_trie_init(&trie, ta_test_allocator_get(&heap), TA_TRIE_MAX_DEPTH + 1u) == TA_ERR_INVALID);
    CHECK(heap.stats.attempts == 0);
    CHECK(ta_trie_init(&trie, ta_test_allocator_get(&heap), 8) == TA_OK);
    CHECK(ta_trie_valid(&trie, TA_TRIE_ROOT) && !ta_trie_valid(&trie, 1u));
    ta_atom segments[TA_TRIE_MAX_DEPTH];
    CHECK(ta_trie_segments(&trie, TA_TRIE_ROOT, segments) == 0);
    CHECK(ta_trie_segments(&trie, 99u, segments) == 0);
    CHECK(ta_trie_under(&trie, TA_TRIE_ROOT, TA_TRIE_ROOT));
    CHECK(!ta_trie_under(&trie, 99u, TA_TRIE_ROOT) && !ta_trie_under(&trie, TA_TRIE_ROOT, 99u));

    uint32_t first = 0;
    uint32_t again = 0;
    uint32_t child = 0;
    CHECK(ta_trie_find(&trie, &(ta_trie_key){ TA_TRIE_ROOT, 5u }, &first) == TA_ERR_NOT_FOUND);
    CHECK(trie.nodes.count == 1u && ta_table_count(&trie.index) == 0); /* find made nothing */
    CHECK(ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, 5u }, &first) == TA_OK && first == 1u);
    CHECK(ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, 5u }, &again) == TA_OK && again == first);
    CHECK(ta_trie_find(&trie, &(ta_trie_key){ TA_TRIE_ROOT, 5u }, &again) == TA_OK && again == first);
    CHECK(ta_trie_child(&trie, &(ta_trie_key){ first, 5u }, &child) == TA_OK && child == 2u && child != first);
    CHECK(trie.nodes.count == 3u && ta_table_count(&trie.index) == 2u);
    CHECK(ta_trie_segments(&trie, child, segments) == 2u && segments[0] == 5u && segments[1] == 5u);
    CHECK(ta_trie_under(&trie, child, first) && ta_trie_under(&trie, child, child) && ta_trie_under(&trie, child, TA_TRIE_ROOT));
    CHECK(!ta_trie_under(&trie, first, child));

    /* Refusals change nothing. */
    CHECK(ta_trie_child(&trie, &(ta_trie_key){ 99u, 1u }, &again) == TA_ERR_INVALID);
    CHECK(ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, TA_ATOM_NONE }, &again) == TA_ERR_INVALID);
    CHECK(ta_trie_find(&trie, &(ta_trie_key){ 99u, 1u }, &again) == TA_ERR_INVALID);
    CHECK(ta_trie_find(&trie, &(ta_trie_key){ TA_TRIE_ROOT, TA_ATOM_NONE }, &again) == TA_ERR_INVALID);
    CHECK(trie.nodes.count == 3u);

    /* The depth limit: eight segments are fine, the ninth is not, and finding an existing path still works. */
    uint32_t node = TA_TRIE_ROOT;
    for (uint32_t depth = 1; depth <= 8u; depth += 1u) {
        CHECK(ta_trie_child(&trie, &(ta_trie_key){ node, 100u + depth }, &node) == TA_OK);
    }
    uint32_t count = trie.nodes.count;
    CHECK(ta_trie_child(&trie, &(ta_trie_key){ node, 1u }, &again) == TA_ERR_INVALID);
    CHECK(trie.nodes.count == count);
    CHECK(ta_trie_segments(&trie, node, segments) == 8u && segments[7] == 108u && segments[0] == 101u);

    ta_trie_destroy(&trie);
    ta_trie_destroy(&trie); /* idempotent */
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* A random path over a small alphabet, so that many paths share prefixes. */
enum { ALPHABET = 6, MAX_PATH = 6, PATHS = 5000 };

typedef struct {
    ta_atom atoms[MAX_PATH];
    uint32_t length;
} model_path;

static bool is_prefix(const model_path *prefix, const model_path *path) {
    if (prefix->length > path->length) {
        return false;
    }
    for (uint32_t index = 0; index < prefix->length; index += 1u) {
        if (prefix->atoms[index] != path->atoms[index]) {
            return false;
        }
    }
    return true;
}

static void test_random_paths(void) {
    static model_path model[PATHS * MAX_PATH + 1];
    static bool known[PATHS * MAX_PATH + 1];
    ta_test_allocator_reset_counts(&heap);
    ta_atom_trie trie;
    CHECK(ta_trie_init(&trie, ta_test_allocator_get(&heap), 16) == TA_OK);
    known[0] = true;
    model[0].length = 0;
    for (uint32_t round = 0; round < PATHS; round += 1u) {
        model_path path = { .length = 1u + random32() % MAX_PATH };
        uint32_t node = TA_TRIE_ROOT;
        for (uint32_t index = 0; index < path.length; index += 1u) {
            path.atoms[index] = random32() % ALPHABET;
            uint32_t expected = trie.nodes.count;
            uint32_t found = 0;
            bool existed = ta_trie_find(&trie, &(ta_trie_key){ node, path.atoms[index] }, &found) == TA_OK;
            CHECK(ta_trie_child(&trie, &(ta_trie_key){ node, path.atoms[index] }, &node) == TA_OK);
            /* A node that is new is the next one in order: IDs are dense. */
            CHECK(existed ? node == found : node == expected && trie.nodes.count == expected + 1u);
            /* The node of a prefix is the node of exactly that prefix, whichever path reached it. */
            model_path prefix = { .length = index + 1u };
            for (uint32_t at = 0; at <= index; at += 1u) {
                prefix.atoms[at] = path.atoms[at];
            }
            if (!known[node]) {
                known[node] = true;
                model[node] = prefix;
            }
            CHECK(model[node].length == prefix.length && is_prefix(&prefix, &model[node]));
        }
    }
    uint32_t nodes = trie.nodes.count;
    CHECK(ta_table_count(&trie.index) == nodes - 1u);
    CHECK(nodes > 2000u && trie.index.pages.count > 4u); /* the index has grown through many splits */
    for (uint32_t node = 0; node < nodes; node += 1u) {
        ta_atom segments[TA_TRIE_MAX_DEPTH];
        uint32_t depth = ta_trie_segments(&trie, node, segments);
        CHECK(depth == model[node].length);
        for (uint32_t index = 0; index < depth; index += 1u) {
            CHECK(segments[index] == model[node].atoms[index]);
        }
    }
    for (uint32_t pair = 0; pair < 200000u; pair += 1u) {
        uint32_t left = random32() % nodes;
        uint32_t right = random32() % nodes;
        CHECK(ta_trie_under(&trie, left, right) == is_prefix(&model[right], &model[left]));
    }
    ta_trie_destroy(&trie);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* Every allocation a run of child calls makes is refused in turn: the failed call leaves no node, a retry works. */
static void test_allocation_failures(void) {
    enum { RUN = 300 };
    ta_test_allocator_reset_counts(&heap);
    ta_atom_trie trie;
    CHECK(ta_trie_init(&trie, ta_test_allocator_get(&heap), 8) == TA_OK);
    uint32_t node = TA_TRIE_ROOT;
    for (uint32_t index = 0; index < RUN; index += 1u) {
        node = TA_TRIE_ROOT;
        CHECK(ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, index }, &node) == TA_OK);
    }
    ta_trie_destroy(&trie);
    uint64_t init_and_run = heap.stats.allocations;
    CHECK(init_and_run > 4u);
    CHECK(ta_test_allocator_finish(&heap) == 0);

    for (uint64_t nth = 1; nth <= init_and_run; nth += 1u) {
        ta_test_allocator_reset_counts(&heap);
        ta_test_allocator_fail_nth(&heap, nth);
        ta_result result = ta_trie_init(&trie, ta_test_allocator_get(&heap), 8);
        uint32_t refused = result == TA_OK ? 0u : 1u;
        if (result != TA_OK) {
            CHECK(result == TA_ERR_NOMEM);
            CHECK(trie.nodes.count == 0 && trie.index.pages.count == 0);
            ta_trie_destroy(&trie);
            ta_test_allocator_disarm(&heap);
            CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
            continue;
        }
        for (uint32_t index = 0; index < RUN; index += 1u) {
            uint32_t count = trie.nodes.count;
            uint32_t entries = ta_table_count(&trie.index);
            result = ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, index }, &node);
            if (result != TA_OK) {
                refused += 1u;
                CHECK(result == TA_ERR_NOMEM);
                CHECK(trie.nodes.count == count && ta_table_count(&trie.index) == entries);
                uint32_t missing = 0;
                CHECK(ta_trie_find(&trie, &(ta_trie_key){ TA_TRIE_ROOT, index }, &missing) == TA_ERR_NOT_FOUND);
                CHECK(ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, index }, &node) == TA_OK);
            }
            CHECK(node == index + 1u);
        }
        /* A refused page for the index is absorbed (growth only postpones), so there may be no failure at all. */
        CHECK(refused <= 1u);
        for (uint32_t index = 0; index < RUN; index += 1u) {
            uint32_t found = 0;
            CHECK(ta_trie_find(&trie, &(ta_trie_key){ TA_TRIE_ROOT, index }, &found) == TA_OK && found == index + 1u);
        }
        ta_trie_destroy(&trie);
        ta_test_allocator_disarm(&heap);
        CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
    }
}

static bool name_is(const ta_event_name_table *names, ta_event_name_id name, const char *expected) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_trie_segments(&names->trie, name, segments);
    uint32_t at = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_str text = ta_atom_text(names->atoms, segments[index]);
        if (index > 0) {
            if (expected[at] != '.') {
                return false;
            }
            at += 1u;
        }
        for (uint32_t byte = 0; byte < text.len; byte += 1u, at += 1u) {
            if (expected[at] != text.ptr[byte]) {
                return false;
            }
        }
    }
    return expected[at] == '\0';
}

/* Event names are the trie's nodes: the built-ins have the IDs of the constants, in order. */
static void test_event_names(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_atom_table atoms;
    ta_event_name_table names;
    CHECK(ta_atom_table_init(&atoms, ta_test_allocator_get(&heap)) == TA_OK);
    CHECK(ta_event_name_table_init(&names, ta_test_allocator_get(&heap), &atoms) == TA_OK);
    CHECK(names.trie.nodes.count == 8u);
    CHECK(name_is(&names, TA_EVENT_NAME_ROOT, ""));
    CHECK(name_is(&names, TA_EVENT_NAME_ERROR, "error"));
    CHECK(name_is(&names, TA_EVENT_NAME_ERROR_EXECUTION, "error.execution"));
    CHECK(name_is(&names, TA_EVENT_NAME_ERROR_COMMUNICATION, "error.communication"));
    CHECK(name_is(&names, TA_EVENT_NAME_ERROR_PLATFORM, "error.platform"));
    CHECK(name_is(&names, TA_EVENT_NAME_DONE, "done"));
    CHECK(name_is(&names, TA_EVENT_NAME_DONE_STATE, "done.state"));
    CHECK(name_is(&names, TA_EVENT_NAME_DONE_INVOKE, "done.invoke"));

    ta_event_name_id name = 0;
    ta_event_name_id again = 0;
    CHECK(ta_event_name_intern_dotted(&names, (ta_str){ "done.invoke.x", 13 }, &name) == TA_OK);
    CHECK(name == 8u && name_is(&names, name, "done.invoke.x"));
    CHECK(ta_event_name_intern_dotted(&names, (ta_str){ "done.invoke.x", 13 }, &again) == TA_OK && again == name);
    CHECK(ta_trie_under(&names.trie, name, TA_EVENT_NAME_DONE_INVOKE) && !ta_trie_under(&names.trie, name, TA_EVENT_NAME_ERROR));
    CHECK(ta_event_name_intern_dotted(&names, (ta_str){ "done..x", 7 }, &again) == TA_ERR_INVALID);
    CHECK(ta_event_name_child(&names, name, atoms.entries.count, &again) == TA_ERR_INVALID); /* not an atom */
    CHECK(ta_event_name_child(&names, 1000u, 0, &again) == TA_ERR_INVALID);              /* not a name */

    /* The system lowers the limit after init (eventNameDepth). */
    names.trie.max_depth = 3u;
    CHECK(ta_event_name_intern_dotted(&names, (ta_str){ "a.b.c", 5 }, &again) == TA_OK);
    CHECK(ta_event_name_intern_dotted(&names, (ta_str){ "a.b.c.d", 7 }, &again) == TA_ERR_INVALID);
    ta_atom list[4] = { 1u, 1u, 1u, 1u };
    CHECK(ta_event_name_intern_atoms_internal(&names, &(ta_atom_list){ .atoms = list, .count = 4u }, &again) == TA_ERR_INVALID);
    CHECK(ta_event_name_intern_atoms_internal(&names, &(ta_atom_list){ .atoms = list, .count = 3u }, &again) == TA_OK);

    ta_event_name_table_destroy(&names);
    ta_atom_table_destroy(&atoms);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_event_names();
    test_basics();
    test_random_paths();
    test_allocation_failures();
    if (failures != 0) {
        fprintf(stderr, "trie_test: %d checks failed\n", failures);
        return 1;
    }
    printf("trie_test: ok\n");
    return 0;
}
