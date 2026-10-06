/*
 * ta_trie: paths of atoms interned as nodes keyed by (parent, segment), on
 * a growing hash table (src/trie.h). Event names are one user of it.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and
 * counts the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses a max_depth of 0 or above TA_TRIE_MAX_DEPTH with
 *    TA_ERR_INVALID and an init whose allocation is refused with
 *    TA_ERR_NOMEM, leaking nothing either way; a new trie holds only the
 *    root, node 0, of depth 0, whose path is empty;
 * 2. one node: find in a trie with only the root answers
 *    TA_ERR_NOT_FOUND; child adds node 1 below the root, find then
 *    answers it, and child again answers it without allocating or adding;
 *    a parent that is not a node or a segment that is not an atom answers
 *    TA_ERR_INVALID to find and child, adding nothing;
 * 3. node IDs follow the order nodes are added, across many growths of
 *    the nodes and of the hash table, and every node stays found with its
 *    parent, segment and depth;
 * 4. find, valid, has_prefix and path never allocate, even with every
 *    allocation refused, and neither do child, intern_path and
 *    intern_dotted for a path the trie holds, even when its hash table
 *    is full;
 * 5. a refused reservation adds nothing: child, intern_path and
 *    intern_dotted refused at each allocation in turn answer TA_ERR_NOMEM,
 *    add no node, keep every node found and leak nothing, and the next
 *    success gets the next ID; intern_path makes room for the whole path
 *    first, so it adds all of a path or none of it;
 * 6. depth: child answers TA_ERR_INVALID below a node at max_depth, and
 *    intern_path and intern_dotted take paths of exactly max_depth
 *    segments and refuse one more, adding nothing;
 * 7. intern_dotted gives the node of its segments' atoms, as intern_path
 *    does; an empty text or an empty segment answers TA_ERR_INVALID before
 *    it interns any atom or adds any node;
 * 8. has_prefix answers whether a node is its prefix or one of its
 *    ancestors, false for any ID that is not a node; path writes the
 *    segments root first;
 * 9. the system's event-name trie holds the built-in names with the IDs
 *    of the TA_EVENT_NAME_* constants and the system's eventNameDepth;
 * 10. destroy frees everything the trie allocated.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_str S(const char *text) {
    return (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) };
}

/* An atom table and a trie on it, over one test allocator. */
typedef struct {
    test_allocator heap;
    ta_atom_table atoms;
    ta_trie trie;
} fixture;

static void fixture_open(fixture *f, uint32_t max_depth) {
    memset(f, 0, sizeof *f);
    ta_allocator allocator = test_allocator_of(&f->heap);
    CHECK(ta_atom_table_init(&f->atoms, allocator) == TA_OK);
    CHECK(ta_trie_init(&f->trie, &(ta_trie_params){ .allocator = allocator,
                                                    .atoms = &f->atoms,
                                                    .max_depth = max_depth }) == TA_OK);
}

static void fixture_close(fixture *f) {
    ta_trie_destroy(&f->trie);
    ta_atom_table_destroy(&f->atoms);
    CHECK(f->heap.live == 0);
    CHECK(test_allocator_finish(&f->heap) == 0);
}

static ta_atom atom_of(fixture *f, const char *text) {
    ta_atom atom = TA_ATOM_NONE;
    CHECK(ta_atom_intern(&f->atoms, S(text), &atom) == TA_OK);
    return atom;
}

static ta_trie_edge edge(ta_trie_id parent, ta_atom segment) {
    return (ta_trie_edge){ .parent = parent, .segment = segment };
}

/* Claim 1. */
static void test_init(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    ta_atom_table atoms;
    CHECK(ta_atom_table_init(&atoms, allocator) == TA_OK);
    uint32_t live = heap.live;
    ta_trie trie;
    CHECK(ta_trie_init(&trie, &(ta_trie_params){ .allocator = allocator, .atoms = &atoms, .max_depth = 0 }) ==
          TA_ERR_INVALID);
    ta_trie_destroy(&trie);
    CHECK(ta_trie_init(&trie, &(ta_trie_params){ .allocator = allocator,
                                                 .atoms = &atoms,
                                                 .max_depth = TA_TRIE_MAX_DEPTH + 1u }) == TA_ERR_INVALID);
    ta_trie_destroy(&trie);
    CHECK(heap.live == live);

    for (uint32_t refused = 1;; refused += 1u) {
        heap.attempts = 0;
        heap.policy.fail_at = refused;
        ta_result result = ta_trie_init(&trie, &(ta_trie_params){ .allocator = allocator,
                                                                  .atoms = &atoms,
                                                                  .max_depth = 8 });
        heap.policy.fail_at = 0;
        if (result == TA_OK) {
            CHECK(heap.attempts < refused);
            break;
        }
        CHECK(result == TA_ERR_NOMEM);
        CHECK(heap.live == live);
        ta_trie_destroy(&trie);
        CHECK(heap.live == live);
    }
    CHECK(trie.nodes.count == 1);
    CHECK(ta_trie_valid(&trie, TA_TRIE_ROOT));
    CHECK(!ta_trie_valid(&trie, 1));
    ta_atom out[TA_TRIE_MAX_DEPTH];
    CHECK(ta_trie_path(&trie, TA_TRIE_ROOT, out) == 0);
    ta_trie_destroy(&trie);
    ta_trie_destroy(&trie);
    ta_atom_table_destroy(&atoms);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 2. */
static void test_one_node(void) {
    fixture f;
    fixture_open(&f, 8);
    ta_atom a = atom_of(&f, "a");
    ta_trie_id id = 99;
    CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, a), &id) == TA_ERR_NOT_FOUND);
    CHECK(id == 99);
    CHECK(ta_trie_child(&f.trie, edge(TA_TRIE_ROOT, a), &id) == TA_OK);
    CHECK(id == 1);
    ta_trie_id found = 0;
    CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, a), &found) == TA_OK && found == 1);
    uint32_t attempts = f.heap.attempts;
    CHECK(ta_trie_child(&f.trie, edge(TA_TRIE_ROOT, a), &found) == TA_OK && found == 1);
    CHECK(f.heap.attempts == attempts);
    CHECK(f.trie.nodes.count == 2);

    CHECK(ta_trie_find(&f.trie, edge(2, a), &found) == TA_ERR_INVALID);
    CHECK(ta_trie_child(&f.trie, edge(2, a), &found) == TA_ERR_INVALID);
    CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, 12345), &found) == TA_ERR_INVALID);
    CHECK(ta_trie_child(&f.trie, edge(TA_TRIE_ROOT, 12345), &found) == TA_ERR_INVALID);
    CHECK(ta_trie_child(&f.trie, edge(TA_TRIE_ROOT, TA_ATOM_NONE), &found) == TA_ERR_INVALID);
    CHECK(f.trie.nodes.count == 2 && f.trie.index.count == 1);
    fixture_close(&f);
}

/* Claim 3. */
static void test_order_across_growth(void) {
    enum { SEGMENTS = 40, NODES = 1 + SEGMENTS + SEGMENTS * SEGMENTS };
    fixture f;
    fixture_open(&f, 8);
    ta_atom segments[SEGMENTS];
    char text[16];
    for (uint32_t index = 0; index < SEGMENTS; index += 1u) {
        snprintf(text, sizeof text, "s%u", index);
        segments[index] = atom_of(&f, text);
    }
    uint32_t slot_counts = 0;
    uint32_t last_slots = f.trie.index.slot_count;
    ta_trie_id next = 1;
    for (uint32_t first = 0; first < SEGMENTS; first += 1u) {
        ta_trie_id parent = 0;
        CHECK(ta_trie_child(&f.trie, edge(TA_TRIE_ROOT, segments[first]), &parent) == TA_OK);
        CHECK(parent == next);
        next += 1u;
        for (uint32_t second = 0; second < SEGMENTS; second += 1u) {
            ta_trie_id id = 0;
            CHECK(ta_trie_child(&f.trie, edge(parent, segments[second]), &id) == TA_OK);
            CHECK(id == next);
            next += 1u;
            if (f.trie.index.slot_count != last_slots) {
                slot_counts += 1u;
                last_slots = f.trie.index.slot_count;
            }
        }
    }
    CHECK(slot_counts >= 4);
    CHECK(f.trie.nodes.count == NODES && f.trie.index.count == NODES - 1u);
    /* Every node is found again with its parent, segment and depth. */
    for (uint32_t first = 0; first < SEGMENTS; first += 1u) {
        ta_trie_id parent = 0;
        CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, segments[first]), &parent) == TA_OK);
        CHECK(parent == 1u + first * (SEGMENTS + 1u));
        for (uint32_t second = 0; second < SEGMENTS; second += 1u) {
            ta_trie_id id = 0;
            CHECK(ta_trie_find(&f.trie, edge(parent, segments[second]), &id) == TA_OK);
            CHECK(id == parent + 1u + second);
            const ta_trie_node *node = ta_vec_at(&f.trie.nodes, id);
            CHECK(node->parent == parent && node->segment == segments[second] && node->depth == 2);
        }
    }
    fixture_close(&f);
}

/* Interns nodes until the next child needs the hash table to grow. */
static void fill_to_growth(fixture *f) {
    while (f->trie.index.count < f->trie.index.limit) {
        ta_atom atom = 0;
        ta_trie_id id = 0;
        char text[16];
        snprintf(text, sizeof text, "f%u", f->trie.index.count);
        CHECK(ta_atom_intern(&f->atoms, S(text), &atom) == TA_OK);
        CHECK(ta_trie_child(&f->trie, edge(TA_TRIE_ROOT, atom), &id) == TA_OK);
    }
}

/* Claim 4. */
static void test_lookups_never_allocate(void) {
    fixture f;
    fixture_open(&f, 8);
    ta_trie_id id = 0;
    CHECK(ta_trie_intern_dotted(&f.trie, S("order.paid.late"), &id) == TA_OK);
    ta_atom order = atom_of(&f, "order");
    ta_atom paid = atom_of(&f, "paid");
    ta_atom unpaid = atom_of(&f, "unpaid");
    f.heap.policy.refuse = true;
    uint32_t attempts = f.heap.attempts;
    ta_trie_id parent = 0;
    ta_trie_id found = 0;
    CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, order), &parent) == TA_OK);
    CHECK(ta_trie_find(&f.trie, edge(parent, paid), &found) == TA_OK);
    CHECK(ta_trie_find(&f.trie, edge(parent, unpaid), &found) == TA_ERR_NOT_FOUND);
    CHECK(ta_trie_valid(&f.trie, id) && !ta_trie_valid(&f.trie, id + 1u));
    CHECK(ta_trie_has_prefix(&f.trie, id, parent));
    ta_atom out[TA_TRIE_MAX_DEPTH];
    CHECK(ta_trie_path(&f.trie, id, out) == 3);
    /* An existing node is answered without making room. */
    CHECK(ta_trie_child(&f.trie, edge(parent, paid), &found) == TA_OK);
    CHECK(f.heap.attempts == attempts);
    f.heap.policy.refuse = false;

    /* With the hash table full, a path the trie holds still needs no room. */
    fill_to_growth(&f);
    CHECK(f.trie.index.count == f.trie.index.limit);
    f.heap.policy.refuse = true;
    attempts = f.heap.attempts;
    CHECK(ta_trie_intern_dotted(&f.trie, S("order.paid.late"), &found) == TA_OK && found == id);
    CHECK(ta_trie_intern_dotted(&f.trie, S("order.paid"), &found) == TA_OK && found == id - 1u);
    ta_atom late = TA_ATOM_NONE;
    CHECK(ta_atom_find(&f.atoms, S("late"), &late) == TA_OK);
    ta_atom path[3] = { order, paid, late };
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 3 }, &found) == TA_OK);
    CHECK(found == id);
    CHECK(ta_trie_child(&f.trie, edge(parent, paid), &found) == TA_OK && found == id - 1u);
    CHECK(f.heap.attempts == attempts);
    CHECK(ta_trie_child(&f.trie, edge(parent, unpaid), &found) == TA_ERR_NOMEM);
    f.heap.policy.refuse = false;
    fixture_close(&f);
}

/* Claim 5. */
static void test_refused_reservation(void) {
    fixture f;
    fixture_open(&f, 8);
    ta_atom fresh = atom_of(&f, "fresh");
    fill_to_growth(&f);
    uint32_t nodes = f.trie.nodes.count;
    uint32_t refusals = 0;
    for (uint32_t refused = 1;; refused += 1u) {
        f.heap.attempts = 0;
        f.heap.policy.fail_at = refused;
        ta_trie_id id = 0;
        ta_result result = ta_trie_child(&f.trie, edge(TA_TRIE_ROOT, fresh), &id);
        f.heap.policy.fail_at = 0;
        if (result == TA_OK) {
            CHECK(id == nodes);
            break;
        }
        refusals += 1u;
        CHECK(result == TA_ERR_NOMEM);
        CHECK(f.trie.nodes.count == nodes && f.trie.index.count == nodes - 1u);
        CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, fresh), &id) == TA_ERR_NOT_FOUND);
    }
    CHECK(refusals >= 1);
    for (ta_trie_id id = 1; id < nodes; id += 1u) {
        const ta_trie_node *node = ta_vec_at(&f.trie.nodes, id);
        ta_trie_id found = 0;
        CHECK(ta_trie_find(&f.trie, edge(node->parent, node->segment), &found) == TA_OK && found == id);
    }

    /* A whole path, every allocation refused in turn: all of it or none. */
    ta_atom path[6];
    char text[16];
    for (uint32_t index = 0; index < 6; index += 1u) {
        snprintf(text, sizeof text, "p%u", index);
        path[index] = atom_of(&f, text);
    }
    fill_to_growth(&f);
    nodes = f.trie.nodes.count;
    refusals = 0;
    for (uint32_t refused = 1;; refused += 1u) {
        f.heap.attempts = 0;
        f.heap.policy.fail_at = refused;
        ta_trie_id id = 0;
        ta_result result = ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 6 }, &id);
        f.heap.policy.fail_at = 0;
        if (result == TA_OK) {
            CHECK(id == nodes + 5u);
            break;
        }
        refusals += 1u;
        CHECK(result == TA_ERR_NOMEM);
        CHECK(f.trie.nodes.count == nodes && f.trie.index.count == nodes - 1u);
        CHECK(ta_trie_find(&f.trie, edge(TA_TRIE_ROOT, path[0]), &id) == TA_ERR_NOT_FOUND);
    }
    CHECK(refusals >= 1);
    CHECK(f.trie.nodes.count == nodes + 6u);

    /* A dotted path whose atoms are all new, refused in turn. */
    fill_to_growth(&f);
    nodes = f.trie.nodes.count;
    refusals = 0;
    for (uint32_t refused = 1;; refused += 1u) {
        f.heap.attempts = 0;
        f.heap.policy.fail_at = refused;
        ta_trie_id id = 0;
        ta_result result = ta_trie_intern_dotted(&f.trie, S("q.r.s"), &id);
        f.heap.policy.fail_at = 0;
        if (result == TA_OK) {
            CHECK(id == nodes + 2u);
            break;
        }
        refusals += 1u;
        CHECK(result == TA_ERR_NOMEM);
        CHECK(f.trie.nodes.count == nodes && f.trie.index.count == nodes - 1u);
    }
    CHECK(refusals >= 1);
    fixture_close(&f);
}

/* Claim 6. */
static void test_depth(void) {
    fixture f;
    fixture_open(&f, 3);
    ta_atom a = atom_of(&f, "a");
    ta_atom path[4] = { a, a, a, a };
    ta_trie_id id = 0;
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 4 }, &id) == TA_ERR_INVALID);
    CHECK(ta_trie_intern_dotted(&f.trie, S("a.a.a.a"), &id) == TA_ERR_INVALID);
    CHECK(f.trie.nodes.count == 1);
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 3 }, &id) == TA_OK);
    CHECK(id == 3);
    ta_trie_id deeper = 0;
    CHECK(ta_trie_child(&f.trie, edge(id, a), &deeper) == TA_ERR_INVALID);
    CHECK(ta_trie_intern_dotted(&f.trie, S("a.a.a"), &deeper) == TA_OK && deeper == id);
    CHECK(f.trie.nodes.count == 4);
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 0 }, &id) == TA_ERR_INVALID);
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = NULL, .count = 1 }, &id) == TA_ERR_INVALID);
    CHECK(ta_trie_intern_path(&f.trie, NULL, &id) == TA_ERR_INVALID);
    path[1] = 4242;
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 2 }, &id) == TA_ERR_INVALID);
    CHECK(f.trie.nodes.count == 4);
    fixture_close(&f);

    fixture_open(&f, TA_TRIE_MAX_DEPTH);
    char text[2u * TA_TRIE_MAX_DEPTH + 2u];
    for (uint32_t index = 0; index < TA_TRIE_MAX_DEPTH; index += 1u) {
        text[2u * index] = 'x';
        text[2u * index + 1u] = '.';
    }
    text[2u * TA_TRIE_MAX_DEPTH - 1u] = '\0';
    CHECK(ta_trie_intern_dotted(&f.trie, S(text), &id) == TA_OK && id == TA_TRIE_MAX_DEPTH);
    text[2u * TA_TRIE_MAX_DEPTH - 1u] = '.';
    text[2u * TA_TRIE_MAX_DEPTH] = 'x';
    text[2u * TA_TRIE_MAX_DEPTH + 1u] = '\0';
    CHECK(ta_trie_intern_dotted(&f.trie, S(text), &id) == TA_ERR_INVALID);
    CHECK(f.trie.nodes.count == TA_TRIE_MAX_DEPTH + 1u);
    fixture_close(&f);
}

/* Claim 7. */
static void test_dotted(void) {
    fixture f;
    fixture_open(&f, 8);
    ta_trie_id dotted = 0;
    CHECK(ta_trie_intern_dotted(&f.trie, S("error.send.failed"), &dotted) == TA_OK);
    ta_atom path[3] = { atom_of(&f, "error"), atom_of(&f, "send"), atom_of(&f, "failed") };
    ta_trie_id id = 0;
    uint32_t attempts = f.heap.attempts;
    CHECK(ta_trie_intern_path(&f.trie, &(ta_atom_list){ .atoms = path, .count = 3 }, &id) == TA_OK);
    CHECK(id == dotted && f.heap.attempts == attempts);
    ta_trie_id single = 0;
    CHECK(ta_trie_intern_dotted(&f.trie, S("error"), &single) == TA_OK && single == 1);

    uint32_t atoms = f.atoms.entries.count;
    uint32_t nodes = f.trie.nodes.count;
    static const char *const bad[] = { "", ".", "new..x", ".new", "new.", "new.x." };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(bad); index += 1u) {
        CHECK(ta_trie_intern_dotted(&f.trie, S(bad[index]), &id) == TA_ERR_INVALID);
    }
    CHECK(ta_trie_intern_dotted(&f.trie, (ta_str){ .ptr = NULL, .len = 0 }, &id) == TA_ERR_INVALID);
    CHECK(f.atoms.entries.count == atoms && f.trie.nodes.count == nodes);
    fixture_close(&f);
}

/* Claim 8. */
static void test_prefix_and_path(void) {
    fixture f;
    fixture_open(&f, 8);
    ta_trie_id error = 0;
    ta_trie_id send = 0;
    ta_trie_id failed = 0;
    ta_trie_id errors = 0;
    CHECK(ta_trie_intern_dotted(&f.trie, S("error"), &error) == TA_OK);
    CHECK(ta_trie_intern_dotted(&f.trie, S("error.send"), &send) == TA_OK);
    CHECK(ta_trie_intern_dotted(&f.trie, S("error.send.failed"), &failed) == TA_OK);
    CHECK(ta_trie_intern_dotted(&f.trie, S("errors.send"), &errors) == TA_OK);
    CHECK(ta_trie_has_prefix(&f.trie, failed, failed));
    CHECK(ta_trie_has_prefix(&f.trie, failed, send));
    CHECK(ta_trie_has_prefix(&f.trie, failed, error));
    CHECK(ta_trie_has_prefix(&f.trie, failed, TA_TRIE_ROOT));
    CHECK(!ta_trie_has_prefix(&f.trie, send, failed));
    CHECK(!ta_trie_has_prefix(&f.trie, errors, error));
    CHECK(!ta_trie_has_prefix(&f.trie, 1000, error));
    CHECK(!ta_trie_has_prefix(&f.trie, failed, 1000));

    ta_atom out[TA_TRIE_MAX_DEPTH];
    CHECK(ta_trie_path(&f.trie, failed, out) == 3);
    CHECK(out[0] == atom_of(&f, "error") && out[1] == atom_of(&f, "send") && out[2] == atom_of(&f, "failed"));
    CHECK(ta_trie_path(&f.trie, 1000, out) == 0);
    fixture_close(&f);
}

/* Claim 9. */
static void test_system_event_names(void) {
    test_allocator heap = {0};
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator_of(&heap) }, &system) == TA_OK);
    static const struct {
        const char *text;
        ta_event_name_id id;
    } builtins[] = {
        { "error", TA_EVENT_NAME_ERROR },
        { "error.execution", TA_EVENT_NAME_ERROR_EXECUTION },
        { "error.communication", TA_EVENT_NAME_ERROR_COMMUNICATION },
        { "error.platform", TA_EVENT_NAME_ERROR_PLATFORM },
        { "done", TA_EVENT_NAME_DONE },
        { "done.state", TA_EVENT_NAME_DONE_STATE },
        { "done.invoke", TA_EVENT_NAME_DONE_INVOKE },
        { "error.execution.capacity", TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY },
        { "error.execution.capacity.pool", TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY_POOL },
        { "error.execution.capacity.names", TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY_NAMES },
        { "error.execution.capacity.internal", TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY_INTERNAL },
    };
    uint32_t nodes = system->event_names.nodes.count;
    CHECK(nodes == 12);
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(builtins); index += 1u) {
        CHECK(builtins[index].id == index + 1u);
        ta_event_name_id id = 0;
        CHECK(ta_trie_intern_dotted(&system->event_names, S(builtins[index].text), &id) == TA_OK);
        CHECK(id == builtins[index].id);
    }
    CHECK(system->event_names.nodes.count == nodes);
    CHECK(system->event_names.max_depth == TA_PARAM_DEFAULT_EVENT_NAME_DEPTH);
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 10. */
static void test_destroy(void) {
    fixture f;
    fixture_open(&f, 8);
    uint32_t live = f.heap.live;
    ta_trie_id id = 0;
    CHECK(ta_trie_intern_dotted(&f.trie, S("a.b.c"), &id) == TA_OK);
    ta_trie_destroy(&f.trie);
    CHECK(f.trie.nodes.count == 0 && f.trie.index.slot_count == 0);
    CHECK(f.heap.live < live);
    fixture_close(&f);
}

int main(void) {
    test_init();
    test_one_node();
    test_order_across_growth();
    test_lookups_never_allocate();
    test_refused_reservation();
    test_depth();
    test_dotted();
    test_prefix_and_path();
    test_system_event_names();
    test_destroy();
    if (failures != 0) {
        fprintf(stderr, "trie_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("trie_test: ok\n");
    return 0;
}
