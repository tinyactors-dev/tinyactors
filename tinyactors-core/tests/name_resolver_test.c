/* test: standalone asan */
/*
 * Block D4 (src/name_resolver.h): the name resolver.
 *
 * Checks: the worked examples of docs/proposals/ring-proto/reference/names.md
 * (resolved segments and the match marks of the four descriptors), every edge
 * case (empty name, leading, trailing and doubled dots, a lone dot, null
 * pointers, capacity 0, a literal '*' segment), the depth boundary with a guard
 * word after the output, agreement with ta_event_name_intern_dotted about
 * which names are valid and which segments are atoms, that the matcher gives
 * the same answer over resolved atoms as over interned ones, and that
 * resolving leaves the atom table and the trie unchanged and never allocates.
 *
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

#define REGISTERED 9u
static uint32_t known_atoms; /* atoms in a freshly opened world */
static const char *const registered[REGISTERED] = {
    "order", "paid", "error", "communication", "invoke", "payment", "a", "b", "c"
};

typedef struct {
    ta_atom_table atoms;
    ta_event_name_table names;
} world;

static ta_str str(const char *text) {
    uint32_t length = 0;
    while (text[length] != 0) {
        length += 1u;
    }
    return (ta_str){ text, length };
}

static void world_open(world *w, uint32_t depth) {
    CHECK(ta_atom_table_init(&w->atoms, ta_test_allocator_get(&heap)) == TA_OK);
    CHECK(ta_event_name_table_init(&w->names, ta_test_allocator_get(&heap), &w->atoms) == TA_OK);
    w->names.trie.max_depth = depth;
    for (uint32_t i = 0; i < REGISTERED; i += 1u) {
        ta_atom atom;
        CHECK(ta_atom_intern(&w->atoms, str(registered[i]), &atom) == TA_OK);
    }
}

static void world_note_known(const world *w) {
    known_atoms = (uint32_t)w->atoms.entries.count;
}

static void world_close(world *w) {
    ta_event_name_table_destroy(&w->names);
    ta_atom_table_destroy(&w->atoms);
}

static ta_result resolve(const world *w, ta_str name, ta_atom *out, uint32_t capacity, uint32_t *count) {
    ta_name_resolve_params params = {
        .atoms = &w->atoms, .name = name, .out = out, .capacity = capacity, .count = count,
    };
    return ta_name_resolve(&params);
}

static ta_atom atom_of(const world *w, const char *text) {
    ta_atom atom = TA_ATOM_NONE;
    CHECK(ta_atom_find(&w->atoms, str(text), &atom) == TA_OK);
    return atom;
}

typedef struct {
    const ta_event_pattern_segment *segments;
    uint32_t count;
} descriptor;

static bool matches(const descriptor *d, const ta_atom *event, uint32_t count) {
    ta_pattern_match match = {
        .event = event, .event_count = count, .pattern = d->segments, .pattern_count = d->count,
    };
    return ta_pattern_match_run(&match);
}

static void test_worked_examples(void) {
    world w;
    world_open(&w, 8);
    ta_atom order = atom_of(&w, "order"), paid = atom_of(&w, "paid"), error = atom_of(&w, "error");
    ta_atom comm = atom_of(&w, "communication"), invoke = atom_of(&w, "invoke");
    ta_atom payment = atom_of(&w, "payment");
    const uint32_t wc = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
    ta_event_pattern_segment d_order[] = { { order, 0 } };
    ta_event_pattern_segment d_paid[] = { { order, 0 }, { TA_ATOM_NONE, wc }, { paid, 0 } };
    ta_event_pattern_segment d_err[] = { { error, 0 }, { comm, 0 }, { invoke, 0 }, { payment, 0 } };
    ta_event_pattern_segment d_star[] = { { TA_ATOM_NONE, wc } };
    descriptor ds[4] = { { d_order, 1 }, { d_paid, 3 }, { d_err, 4 }, { d_star, 1 } };
    const ta_atom U = TA_NAME_UNKNOWN;
    struct {
        const char *name;
        uint32_t count;
        ta_atom segments[6];
        bool marks[4];
    } rows[] = {
        { "order.123.paid", 3, { order, U, paid }, { true, true, false, true } },
        { "order.124", 2, { order, U }, { true, false, false, true } },
        { "orders.1", 2, { U, U }, { false, false, false, true } },
        { "error.communication.invoke.payment.timeout", 5, { error, comm, invoke, payment, U },
          { false, false, true, true } },
        { "error.communication.invoke.s3.7.unavailable", 6, { error, comm, invoke, U, U, U },
          { false, false, false, true } },
        { "x9f2.qq.zz", 3, { U, U, U }, { false, false, false, true } },
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r += 1u) {
        ta_atom out[8];
        uint32_t count = 99;
        CHECK(resolve(&w, str(rows[r].name), out, 8, &count) == TA_OK);
        CHECK(count == rows[r].count);
        for (uint32_t i = 0; i < rows[r].count && i < count; i += 1u) {
            CHECK(out[i] == rows[r].segments[i]);
        }
        for (size_t d = 0; d < 4; d += 1u) {
            CHECK(matches(&ds[d], out, count) == rows[r].marks[d]);
        }
    }
    world_close(&w);
}

static void test_edges(void) {
    world w;
    world_open(&w, 8);
    ta_atom out[8];
    uint32_t count = 7;
    CHECK(resolve(&w, str("order"), out, 8, &count) == TA_OK && count == 1u && out[0] == atom_of(&w, "order"));
    CHECK(resolve(&w, str("nothing"), out, 8, &count) == TA_OK && count == 1u && out[0] == TA_NAME_UNKNOWN);
    /* A literal '*' is ordinary text: UNKNOWN when no definition interned it. */
    CHECK(resolve(&w, str("a.*"), out, 8, &count) == TA_OK && count == 2u && out[1] == TA_NAME_UNKNOWN);

    const char *bad[] = { "", ".", "..", ".a", "a.", "a..b", "a.b.", "...", "order.paid." };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i += 1u) {
        count = 7;
        CHECK(resolve(&w, str(bad[i]), out, 8, &count) == TA_ERR_INVALID);
        CHECK(count == 0);
    }
    count = 7;
    CHECK(resolve(&w, (ta_str){ NULL, 0 }, out, 8, &count) == TA_ERR_INVALID && count == 0);
    count = 7;
    CHECK(resolve(&w, (ta_str){ NULL, 3 }, out, 8, &count) == TA_ERR_INVALID && count == 0);
    count = 7;
    CHECK(resolve(&w, str("a"), out, 0, &count) == TA_ERR_INVALID && count == 0);
    count = 7;
    CHECK(resolve(&w, str("a"), NULL, 8, &count) == TA_ERR_INVALID && count == 0);
    ta_name_resolve_params no_atoms = { .atoms = NULL, .name = str("a"), .out = out, .capacity = 8, .count = &count };
    count = 7;
    CHECK(ta_name_resolve(&no_atoms) == TA_ERR_INVALID && count == 0);
    ta_name_resolve_params no_count = { .atoms = &w.atoms, .name = str("a"), .out = out, .capacity = 8, .count = NULL };
    CHECK(ta_name_resolve(&no_count) == TA_ERR_INVALID);
    CHECK(ta_name_resolve(NULL) == TA_ERR_INVALID);
    world_close(&w);
}

static void test_depth(void) {
    static const uint32_t capacities[] = { 1, 8, TA_EVENT_NAME_MAX_DEPTH };
    world w;
    world_open(&w, TA_EVENT_NAME_MAX_DEPTH);
    for (size_t c = 0; c < 3; c += 1u) {
        uint32_t capacity = capacities[c];
        ta_atom out[TA_EVENT_NAME_MAX_DEPTH + 1u];
        char name[2 * (TA_EVENT_NAME_MAX_DEPTH + 1u) + 1u];
        for (uint32_t segments = capacity; segments <= capacity + 1u; segments += 1u) {
            uint32_t length = 0;
            for (uint32_t i = 0; i < segments; i += 1u) {
                if (i > 0) {
                    name[length++] = '.';
                }
                name[length++] = 'a';
            }
            name[length] = 0;
            for (uint32_t i = 0; i <= capacity; i += 1u) {
                out[i] = 0xDEADBEEFu;
            }
            uint32_t count = 7;
            ta_result result = resolve(&w, str(name), out, capacity, &count);
            CHECK(result == (segments == capacity ? TA_OK : TA_ERR_INVALID));
            CHECK(count == (segments == capacity ? capacity : 0u));
            CHECK(out[capacity] == 0xDEADBEEFu); /* the guard word after the output */
        }
    }
    world_close(&w);
}

static uint32_t random_name(char *buffer, uint32_t max_length) {
    static const char alphabet[] = { 'a', 'b', 'c', 'x', 'y', 'o', 'r', 'd', 'e', '.', '.', '*' };
    uint32_t length = random32() % (max_length + 1u);
    for (uint32_t i = 0; i < length; i += 1u) {
        buffer[i] = alphabet[random32() % sizeof alphabet];
    }
    buffer[length] = 0;
    return length;
}

/* Words, so that random names are often whole known atoms per segment. */
static uint32_t random_dotted(char *buffer) {
    static const char *const words[] = { "a", "b", "c", "order", "paid", "x", "yy", "*", "error", "" };
    uint32_t segments = random32() % 7u;
    uint32_t length = 0;
    for (uint32_t i = 0; i < segments; i += 1u) {
        if (i > 0) {
            buffer[length++] = '.';
        }
        const char *word = words[random32() % (sizeof words / sizeof words[0])];
        for (; *word; word += 1) {
            buffer[length++] = (char)*word;
        }
    }
    buffer[length] = 0;
    return length;
}

static void test_agreement_and_matching(void) {
    world reader; /* registered atoms only; the resolver reads it */
    world reference; /* the interner adds to it */
    world_open(&reader, 8);
    world_open(&reference, 8);
    world_note_known(&reader);
    CHECK(known_atoms == (uint32_t)reference.atoms.entries.count);
    const uint32_t wc = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
    for (uint32_t round = 0; round < 4000u; round += 1u) {
        char buffer[64];
        uint32_t length = (round & 1u) ? random_name(buffer, 20) : random_dotted(buffer);
        ta_str name = (ta_str){ buffer, length };
        ta_atom out[8];
        uint32_t count = 0;
        ta_result resolved = resolve(&reader, name, out, 8, &count);
        ta_event_name_id id = 0;
        ta_result interned = ta_event_name_intern_dotted(&reference.names, name, &id);
        CHECK((resolved == TA_OK) == (interned == TA_OK));
        if (resolved != TA_OK || interned != TA_OK) {
            CHECK(count == 0);
            continue;
        }
        ta_atom truth[TA_EVENT_NAME_MAX_DEPTH];
        uint32_t truth_count = ta_trie_segments(&reference.names.trie, id, truth);
        CHECK(truth_count == count);
        for (uint32_t i = 0; i < count && i < truth_count; i += 1u) {
            /* Both tables hold the same first known_atoms atoms (the empty text, the built-in names, the registered ones), numbered alike. */
            CHECK((out[i] == TA_NAME_UNKNOWN) == (truth[i] >= known_atoms));
            if (out[i] != TA_NAME_UNKNOWN) {
                CHECK(out[i] == truth[i]);
            }
        }
        for (uint32_t k = 0; k < 6u; k += 1u) {
            ta_event_pattern_segment segments[4];
            uint32_t n = 1u + random32() % 4u;
            for (uint32_t i = 0; i < n; i += 1u) {
                bool star = random32() % 3u == 0;
                segments[i].atom = star ? TA_ATOM_NONE : random32() % (known_atoms - 1u) + 1u;
                segments[i].flags = star ? wc : 0u;
            }
            descriptor d = { segments, n };
            CHECK(matches(&d, out, count) == matches(&d, truth, truth_count));
        }
    }
    world_close(&reader);
    world_close(&reference);
}

static void test_read_only_and_no_allocation(void) {
    world w;
    world_open(&w, 8);
    uint32_t atoms_count = (uint32_t)w.atoms.entries.count;
    uint32_t index_count = ta_table_count(&w.atoms.index);
    size_t text_bytes = ta_arena_allocated_bytes(&w.atoms.strings);
    uint32_t nodes = (uint32_t)w.names.trie.nodes.count;
    uint32_t trie_index = ta_table_count(&w.names.trie.index);
    ta_test_allocator_reset_counts(&heap);
    ta_test_allocator_fail_from_nth(&heap, 1); /* any allocation would be refused, and counted */
    uint32_t ok = 0;
    for (uint32_t round = 0; round < 5000u; round += 1u) {
        char buffer[64];
        uint32_t length = random_dotted(buffer);
        ta_atom out[8];
        uint32_t count = 0;
        if (resolve(&w, (ta_str){ buffer, length }, out, 8, &count) == TA_OK) {
            ok += 1u;
        }
    }
    CHECK(ok > 100u);
    CHECK(heap.stats.attempts == 0);
    ta_test_allocator_fail_from_nth(&heap, 0);
    CHECK(w.atoms.entries.count == atoms_count);
    CHECK(ta_table_count(&w.atoms.index) == index_count);
    CHECK(ta_arena_allocated_bytes(&w.atoms.strings) == text_bytes);
    CHECK(w.names.trie.nodes.count == nodes);
    CHECK(ta_table_count(&w.names.trie.index) == trie_index);
    world_close(&w);
}

int main(void) {
    test_worked_examples();
    test_edges();
    test_depth();
    test_agreement_and_matching();
    test_read_only_and_no_allocation();
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "name_resolver_test: %d failures\n", failures);
        return 1;
    }
    puts("name_resolver_test: ok");
    return 0;
}
