/* test: standalone asan */
/*
 * Block B7 (src/type_registry.h): the registry of declared type names.
 *
 * Checks, in order: the built-ins and every alias of both specs (and that
 * near misses are not declared), declare then declared (idempotent, alias
 * resolves to its built-in, invalid names), a lookup of an unknown name
 * leaves the atom table and the heap alone, declared_atom, growth past the
 * first index page, a refused grow only postpones, an allocation failure
 * sweep over declare and over init (strict allocator: sized frees, no leak),
 * and a zeroed registry.
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

#define IO_URI "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"
#define INVOKE_URI "http://www.w3.org/TR/scxml/"

static ta_str S(const char *text) {
    return ta_str_from_cstr(text);
}

typedef struct fixture {
    ta_atom_table atoms;
    ta_type_registry registry;
} fixture;

static ta_result fixture_open(fixture *f, ta_type_builtin_set builtins) {
    ta_allocator allocator = ta_test_allocator_get(&heap);
    ta_result result = ta_atom_table_init(&f->atoms, allocator);
    if (result != TA_OK) {
        return result;
    }
    result = ta_type_registry_init(&f->registry, allocator, &(ta_type_registry_params){ .atoms = &f->atoms, .builtins = builtins });
    if (result != TA_OK) {
        ta_atom_table_destroy(&f->atoms);
    }
    return result;
}

static void fixture_close(fixture *f) {
    ta_type_registry_destroy(&f->registry);
    ta_atom_table_destroy(&f->atoms);
}

static bool resolves(const fixture *f, const char *name, ta_atom expected) {
    ta_atom canonical = 0;
    return ta_type_registry_declared(&f->registry, S(name), &canonical) && canonical == expected;
}

static void test_builtins(void) {
    fixture f;
    CHECK(fixture_open(&f, ta_type_registry_io_builtins) == TA_OK);
    ta_atom canonical = TA_ATOM_NONE;
    CHECK(ta_type_registry_count(&f.registry) == 2u);
    CHECK(ta_type_registry_declared(&f.registry, S(IO_URI), &canonical));
    CHECK(ta_str_equal(ta_atom_text(&f.atoms, canonical), S(IO_URI)));
    CHECK(resolves(&f, "scxml", canonical));
    CHECK(resolves(&f, IO_URI, canonical));
    /* Forms core does not accept for I/O processors, or near misses. */
    CHECK(!ta_type_registry_declared(&f.registry, S("SCXML"), &canonical) && canonical == TA_ATOM_NONE);
    CHECK(!ta_type_registry_declared(&f.registry, S(INVOKE_URI), NULL));
    CHECK(!ta_type_registry_declared(&f.registry, S(IO_URI "/"), NULL));
    CHECK(!ta_type_registry_declared(&f.registry, S(""), NULL));
    fixture_close(&f);

    CHECK(fixture_open(&f, ta_type_registry_invoker_builtins) == TA_OK);
    CHECK(ta_type_registry_count(&f.registry) == 3u);
    CHECK(ta_type_registry_declared(&f.registry, S(INVOKE_URI), &canonical));
    CHECK(ta_str_equal(ta_atom_text(&f.atoms, canonical), S(INVOKE_URI)));
    CHECK(resolves(&f, "scxml", canonical));
    CHECK(resolves(&f, "http://www.w3.org/TR/scxml", canonical));
    CHECK(resolves(&f, INVOKE_URI, canonical));
    CHECK(!ta_type_registry_declared(&f.registry, S("SCXML"), NULL));
    CHECK(!ta_type_registry_declared(&f.registry, S("http://www.w3.org/TR/scxml//"), NULL));
    CHECK(!ta_type_registry_declared(&f.registry, S(IO_URI), NULL));
    fixture_close(&f);

    CHECK(fixture_open(&f, (ta_type_builtin_set){ 0 }) == TA_OK && ta_type_registry_count(&f.registry) == 0u);
    CHECK(!ta_type_registry_declared(&f.registry, S("scxml"), NULL));
    fixture_close(&f);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

static void test_declare(void) {
    fixture f;
    CHECK(fixture_open(&f, ta_type_registry_invoker_builtins) == TA_OK);
    ta_atom canonical = TA_ATOM_NONE;
    ta_atom builtin = TA_ATOM_NONE;
    CHECK(ta_type_registry_declared(&f.registry, S(INVOKE_URI), &builtin));

    CHECK(!ta_type_registry_declared(&f.registry, S("mqtt"), NULL));
    CHECK(ta_type_registry_declare(&f.registry, S("mqtt"), &canonical) == TA_OK);
    CHECK(ta_type_registry_count(&f.registry) == 4u);
    CHECK(ta_str_equal(ta_atom_text(&f.atoms, canonical), S("mqtt")));
    CHECK(resolves(&f, "mqtt", canonical));

    /* Idempotent: nothing is added, not even an atom. */
    uint32_t atoms = f.atoms.entries.count;
    ta_atom again = TA_ATOM_NONE;
    CHECK(ta_type_registry_declare(&f.registry, S("mqtt"), &again) == TA_OK && again == canonical);
    CHECK(ta_type_registry_count(&f.registry) == 4u && f.atoms.entries.count == atoms);

    /* A built-in or an alias returns its canonical atom and adds nothing. */
    CHECK(ta_type_registry_declare(&f.registry, S("scxml"), &again) == TA_OK && again == builtin);
    CHECK(ta_type_registry_declare(&f.registry, S(INVOKE_URI), NULL) == TA_OK);
    CHECK(ta_type_registry_count(&f.registry) == 4u && f.atoms.entries.count == atoms);

    ta_atom untouched = 12345u;
    CHECK(ta_type_registry_declare(&f.registry, S(""), &untouched) == TA_ERR_INVALID && untouched == 12345u);
    CHECK(ta_type_registry_declare(&f.registry, (ta_str){ .ptr = NULL, .len = 3 }, NULL) == TA_ERR_INVALID);
    CHECK(ta_type_registry_declare(&f.registry, (ta_str){ .ptr = NULL, .len = 0 }, NULL) == TA_ERR_INVALID);
    CHECK(ta_type_registry_count(&f.registry) == 4u);
    fixture_close(&f);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

static void test_lookup_is_read_only(void) {
    fixture f;
    CHECK(fixture_open(&f, ta_type_registry_io_builtins) == TA_OK);
    ta_atom interned = TA_ATOM_NONE;
    CHECK(ta_atom_intern(&f.atoms, S("interned-not-declared"), &interned) == TA_OK);

    uint32_t atoms = f.atoms.entries.count;
    size_t atom_bytes = ta_vec_bytes(&f.atoms.entries) + ta_table_bytes(&f.atoms.index);
    uint32_t entries = ta_type_registry_count(&f.registry);
    ta_test_allocator_reset_counts(&heap);
    ta_test_allocator_refuse_all(&heap, true); /* any allocation would fail loudly */

    CHECK(!ta_type_registry_declared(&f.registry, S("never-seen"), NULL));
    CHECK(!ta_type_registry_declared(&f.registry, S("interned-not-declared"), NULL));
    CHECK(!ta_type_registry_declared_atom(&f.registry, interned, NULL));
    CHECK(ta_type_registry_declared(&f.registry, S("scxml"), NULL));
    CHECK(heap.stats.attempts == 0);

    ta_test_allocator_refuse_all(&heap, false);
    CHECK(f.atoms.entries.count == atoms);
    CHECK(ta_vec_bytes(&f.atoms.entries) + ta_table_bytes(&f.atoms.index) == atom_bytes);
    CHECK(ta_type_registry_count(&f.registry) == entries);
    CHECK(ta_atom_find(&f.atoms, S("never-seen"), &interned) == TA_ERR_NOT_FOUND);
    fixture_close(&f);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

static void test_declared_atom(void) {
    fixture f;
    CHECK(fixture_open(&f, ta_type_registry_io_builtins) == TA_OK);
    ta_atom canonical = TA_ATOM_NONE;
    ta_atom alias = TA_ATOM_NONE;
    ta_atom other = TA_ATOM_NONE;
    CHECK(ta_type_registry_declared(&f.registry, S(IO_URI), &canonical));
    CHECK(ta_atom_find(&f.atoms, S("scxml"), &alias) == TA_OK);
    CHECK(ta_atom_intern(&f.atoms, S("other"), &other) == TA_OK);

    ta_atom out = 0;
    CHECK(ta_type_registry_declared_atom(&f.registry, canonical, &out) && out == canonical);
    CHECK(ta_type_registry_declared_atom(&f.registry, alias, &out) && out == canonical);
    CHECK(ta_type_registry_declared_atom(&f.registry, alias, NULL));
    CHECK(!ta_type_registry_declared_atom(&f.registry, other, &out) && out == TA_ATOM_NONE);
    CHECK(!ta_type_registry_declared_atom(&f.registry, TA_ATOM_NONE, &out) && out == TA_ATOM_NONE);
    CHECK(!ta_type_registry_declared_atom(&f.registry, f.atoms.entries.count + 100u, NULL));
    CHECK(!ta_type_registry_declared_atom(&f.registry, 0u, NULL)); /* the empty text */
    fixture_close(&f);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

static void name_of(char *buffer, uint32_t number) {
    int length = snprintf(buffer, 32, "x-type-%u", number);
    (void)length;
}

static void test_growth(void) {
    fixture f;
    CHECK(fixture_open(&f, ta_type_registry_invoker_builtins) == TA_OK);
    uint32_t pages = f.registry.index.pages.count;
    size_t slots = f.registry.index.slots;
    enum { total = 400 };
    for (uint32_t i = 0; i < total; i += 1) {
        char name[32];
        name_of(name, i);
        ta_atom canonical = TA_ATOM_NONE;
        CHECK(ta_type_registry_declare(&f.registry, S(name), &canonical) == TA_OK);
        CHECK(ta_str_equal(ta_atom_text(&f.atoms, canonical), S(name)));
    }
    CHECK(ta_type_registry_count(&f.registry) == 3u + total);
    CHECK(f.registry.index.slots > slots && f.registry.index.pages.count > pages); /* past the first page */
    for (uint32_t i = 0; i < total; i += 1) {
        char name[32];
        name_of(name, i);
        ta_atom canonical = TA_ATOM_NONE;
        CHECK(ta_type_registry_declared(&f.registry, S(name), &canonical));
        CHECK(ta_str_equal(ta_atom_text(&f.atoms, canonical), S(name)));
    }
    CHECK(ta_type_registry_declared(&f.registry, S("scxml"), NULL));
    CHECK(!ta_type_registry_declared(&f.registry, S("x-type-400"), NULL));
    fixture_close(&f);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* A grow that is refused only postpones: declare still succeeds and every name stays found. */
static void test_refused_grow(void) {
    fixture f;
    CHECK(fixture_open(&f, ta_type_registry_io_builtins) == TA_OK);
    /* The first index page is 16 slots (128 bytes); refuse anything from 200 bytes, which a grow or a vector doubling asks. */
    for (uint32_t i = 0; i < 40; i += 1) {
        char name[32];
        name_of(name, i);
        ta_test_allocator_refuse_from(&heap, 200u);
        ta_result result = ta_type_registry_declare(&f.registry, S(name), NULL);
        ta_test_allocator_refuse_from(&heap, 0);
        CHECK(result == TA_OK || result == TA_ERR_NOMEM);
        if (result == TA_ERR_NOMEM) {
            CHECK(!ta_type_registry_declared(&f.registry, S(name), NULL));
            CHECK(ta_type_registry_declare(&f.registry, S(name), NULL) == TA_OK);
        }
        CHECK(ta_type_registry_declared(&f.registry, S(name), NULL));
    }
    for (uint32_t i = 0; i < 40; i += 1) {
        char name[32];
        name_of(name, i);
        CHECK(ta_type_registry_declared(&f.registry, S(name), NULL));
    }
    fixture_close(&f);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

/* Every declared name, in order, still resolves to what it did before the failed call. */
static bool same_as(const fixture *f, const ta_type_entry *before, uint32_t count) {
    if (ta_type_registry_count(&f->registry) != count) {
        return false;
    }
    for (uint32_t i = 0; i < count; i += 1) {
        ta_atom canonical = TA_ATOM_NONE;
        const ta_type_entry *entry = ta_vec_at(&f->registry.entries, i);
        if (entry->name != before[i].name || entry->canonical != before[i].canonical) {
            return false;
        }
        if (!ta_type_registry_declared_atom(&f->registry, before[i].name, &canonical) || canonical != before[i].canonical) {
            return false;
        }
    }
    return true;
}

static void test_declare_sweep(void) {
    /* Start with enough names that the sweep crosses a vector doubling and an index grow. */
    for (uint32_t population = 0; population < 40; population += 13) {
        fixture f;
        CHECK(fixture_open(&f, ta_type_registry_invoker_builtins) == TA_OK);
        for (uint32_t i = 0; i < population; i += 1) {
            char name[32];
            name_of(name, i);
            CHECK(ta_type_registry_declare(&f.registry, S(name), NULL) == TA_OK);
        }
        for (uint32_t step = 0; step < 4; step += 1) {
            ta_type_entry before[64];
            uint32_t count = ta_type_registry_count(&f.registry);
            for (uint32_t i = 0; i < count; i += 1) {
                before[i] = *(const ta_type_entry *)ta_vec_at(&f.registry.entries, i);
            }
            char name[32];
            name_of(name, 1000u + population * 10u + step);
            for (uint64_t nth = 1;; nth += 1) {
                ta_test_allocator_fail_nth(&heap, nth);
                ta_result result = ta_type_registry_declare(&f.registry, S(name), NULL);
                ta_test_allocator_disarm(&heap);
                if (result == TA_OK) {
                    CHECK(ta_type_registry_declared(&f.registry, S(name), NULL));
                    break;
                }
                CHECK(result == TA_ERR_NOMEM);
                CHECK(same_as(&f, before, count));
                CHECK(!ta_type_registry_declared(&f.registry, S(name), NULL));
                if (nth > 20) {
                    CHECK(false); /* a declare makes a handful of allocations */
                    break;
                }
            }
            /* The same name declares when retried with the allocator disarmed. */
            CHECK(ta_type_registry_declare(&f.registry, S(name), NULL) == TA_OK);
            CHECK(ta_type_registry_declared(&f.registry, S(name), NULL));
        }
        fixture_close(&f);
        CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
    }
}

static void test_init_sweep(void) {
    ta_type_builtin_set sets[2] = { ta_type_registry_io_builtins, ta_type_registry_invoker_builtins };
    for (uint32_t s = 0; s < 2; s += 1) {
        for (uint64_t nth = 1;; nth += 1) {
            ta_atom_table atoms;
            ta_type_registry registry;
            ta_allocator allocator = ta_test_allocator_get(&heap);
            CHECK(ta_atom_table_init(&atoms, allocator) == TA_OK);
            ta_test_allocator_fail_nth(&heap, nth);
            ta_result result = ta_type_registry_init(&registry, allocator, &(ta_type_registry_params){ .atoms = &atoms, .builtins = sets[s] });
            ta_test_allocator_disarm(&heap);
            if (result == TA_OK) {
                CHECK(ta_type_registry_count(&registry) == (s == 0 ? 2u : 3u));
                ta_type_registry_destroy(&registry);
                ta_atom_table_destroy(&atoms);
                CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
                break;
            }
            CHECK(result == TA_ERR_NOMEM);
            CHECK(registry.entries.items == NULL && registry.index.pages.items == NULL && registry.atoms == NULL); /* zeroed */
            ta_type_registry_destroy(&registry);                                                                    /* a no-op */
            ta_atom_table_destroy(&atoms);
            CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0); /* nothing leaked */
            if (nth > 40) {
                CHECK(false);
                break;
            }
        }
    }
}

static void test_zeroed_and_invalid(void) {
    ta_type_registry zero = { 0 };
    ta_type_registry_destroy(&zero);
    CHECK(ta_type_registry_count(&zero) == 0u);
    CHECK(!ta_type_registry_declared(&zero, S("scxml"), NULL));
    CHECK(!ta_type_registry_declared_atom(&zero, 0u, NULL));

    ta_type_registry registry;
    CHECK(ta_type_registry_init(&registry, ta_test_allocator_get(&heap), &(ta_type_registry_params){ 0 }) == TA_ERR_INVALID);
    CHECK(heap.stats.bad_frees == 0 && ta_test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_builtins();
    test_declare();
    test_lookup_is_read_only();
    test_declared_atom();
    test_growth();
    test_refused_grow();
    test_declare_sweep();
    test_init_sweep();
    test_zeroed_and_invalid();
    if (failures != 0) {
        fprintf(stderr, "type_registry_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("type_registry_test: ok\n");
    return 0;
}
