/*
 * ta_type_registry: the set of type names a chart may name in a
 * `<send type>` or an `<invoke type>` (src/type_registry.h), with the
 * built-in entries core owns and their aliases.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not.
 *
 * Claims proven here, in order:
 *
 * 1. init with the I/O processor set and with the invoker set holds
 *    exactly their built-ins and aliases, interned in the atom table, and
 *    each name, alias or not, is declared and answers its canonical
 *    entry; a set of no built-ins makes an empty registry;
 * 2. a type the registry does not hold, TA_ATOM_NONE and the empty atom
 *    are not declared (TA_ERR_NOT_FOUND) and leave the canonical
 *    untouched; a NULL canonical is allowed;
 * 3. declare adds a name that stands for itself and is interned as the
 *    atom a chart's literal type gets, whether or not it was an atom
 *    before; names are compared byte for byte;
 * 4. declaring a declared name answers TA_OK, changes nothing and
 *    allocates nothing;
 * 5. declaring a built-in name or an alias answers TA_ERR_UNSUPPORTED and
 *    changes nothing;
 * 6. an empty name and a NULL pointer with a nonzero length answer
 *    TA_ERR_INVALID and change nothing;
 * 7. a declaration whose allocation is refused, at every allocation it
 *    makes, answers TA_ERR_NOMEM, adds no entry, leaves every lookup as it
 *    was, and leaks nothing; the same declaration succeeds afterwards;
 * 8. declared never allocates and never inserts: with an allocator that
 *    refuses everything, every lookup still answers, and the atom table
 *    and the registry keep their sizes;
 * 9. init answers TA_ERR_NOMEM at every allocation it makes, and
 *    TA_ERR_INVALID for each malformed built-in set, leaving an empty
 *    registry that may be destroyed, leaking nothing;
 * 10. two registries over one atom table, made from the two built-in
 *     sets, stand side by side: `scxml` stands for a different canonical
 *     entry in each, a name built into one may be declared in the other,
 *     and a declaration in one is not seen by the other;
 * 11. many declarations, growing the entries and the hash table well past
 *     their first size, all stay declared and answer themselves;
 *     destroying twice is allowed and the atoms stay.
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

#define SCXML_IO "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"
#define SCXML_INVOKE "http://www.w3.org/TR/scxml/"
#define SCXML_INVOKE_BARE "http://www.w3.org/TR/scxml"

static ta_str text(const char *cstr) {
    return (ta_str){ .ptr = cstr, .len = (uint32_t)strlen(cstr) };
}

/* The atom of `cstr`, or TA_ATOM_NONE when it is not one; interns nothing. */
static ta_atom atom_of(const ta_atom_table *atoms, const char *cstr) {
    ta_atom atom = TA_ATOM_NONE;
    if (ta_atom_find(atoms, text(cstr), &atom) != TA_OK) {
        return TA_ATOM_NONE;
    }
    return atom;
}

/* The canonical atom `cstr` stands for in `registry`, or TA_ATOM_NONE. */
static ta_atom canonical_of(const ta_type_registry *registry, const char *cstr) {
    ta_atom name = atom_of(registry->atoms, cstr);
    ta_atom canonical = TA_ATOM_NONE;
    if (ta_type_registry_declared(registry, name, &canonical) != TA_OK) {
        return TA_ATOM_NONE;
    }
    return canonical;
}

typedef struct {
    test_allocator heap;
    ta_allocator allocator;
    ta_atom_table atoms;
} fixture;

static void fixture_init(fixture *f) {
    memset(f, 0, sizeof *f);
    f->allocator = test_allocator_of(&f->heap);
    CHECK(ta_atom_table_init(&f->atoms, f->allocator) == TA_OK);
}

static void fixture_destroy(fixture *f) {
    ta_atom_table_destroy(&f->atoms);
    CHECK(f->heap.live == 0);
    CHECK(test_allocator_finish(&f->heap) == 0);
}

static ta_result registry_init(fixture *f, ta_type_registry *registry, const ta_type_registry_builtins *builtins) {
    return ta_type_registry_init(registry, &(ta_type_registry_params){ .allocator = f->allocator,
                                                                        .atoms = &f->atoms,
                                                                        .builtins = builtins });
}

/* Claim 1. */
static void test_builtins(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    ta_type_registry invoker;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    CHECK(registry_init(&f, &invoker, &TA_TYPE_REGISTRY_INVOKER_BUILTINS) == TA_OK);

    CHECK(io.entries.count == 2u);
    CHECK(io.builtin_count == 2u);
    ta_atom io_canonical = atom_of(&f.atoms, SCXML_IO);
    CHECK(io_canonical != TA_ATOM_NONE);
    CHECK(canonical_of(&io, SCXML_IO) == io_canonical);
    CHECK(canonical_of(&io, "scxml") == io_canonical);

    CHECK(invoker.entries.count == 3u);
    CHECK(invoker.builtin_count == 3u);
    ta_atom invoke_canonical = atom_of(&f.atoms, SCXML_INVOKE);
    CHECK(invoke_canonical != TA_ATOM_NONE);
    CHECK(canonical_of(&invoker, SCXML_INVOKE) == invoke_canonical);
    CHECK(canonical_of(&invoker, "scxml") == invoke_canonical);
    CHECK(canonical_of(&invoker, SCXML_INVOKE_BARE) == invoke_canonical);

    ta_type_registry none;
    CHECK(registry_init(&f, &none, &(ta_type_registry_builtins){ .names = NULL, .count = 0 }) == TA_OK);
    CHECK(none.entries.count == 0u);
    CHECK(none.builtin_count == 0u);
    CHECK(canonical_of(&none, "scxml") == TA_ATOM_NONE);
    CHECK(ta_type_registry_declare(&none, text("console")) == TA_OK);
    CHECK(canonical_of(&none, "console") == atom_of(&f.atoms, "console"));

    ta_type_registry_destroy(&none);
    ta_type_registry_destroy(&invoker);
    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 2. */
static void test_not_declared(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    ta_atom other = TA_ATOM_NONE;
    CHECK(ta_atom_intern(&f.atoms, text("http"), &other) == TA_OK);

    ta_atom canonical = 1234u;
    CHECK(ta_type_registry_declared(&io, other, &canonical) == TA_ERR_NOT_FOUND);
    CHECK(canonical == 1234u);
    CHECK(ta_type_registry_declared(&io, TA_ATOM_NONE, &canonical) == TA_ERR_NOT_FOUND);
    CHECK(canonical == 1234u);
    CHECK(ta_type_registry_declared(&io, 0u, &canonical) == TA_ERR_NOT_FOUND);
    CHECK(canonical == 1234u);
    CHECK(ta_type_registry_declared(&io, other, NULL) == TA_ERR_NOT_FOUND);
    CHECK(ta_type_registry_declared(&io, atom_of(&f.atoms, "scxml"), NULL) == TA_OK);

    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 3. */
static void test_declare(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);

    /* A name that was no atom is interned. */
    CHECK(atom_of(&f.atoms, "console") == TA_ATOM_NONE);
    CHECK(ta_type_registry_declare(&io, text("console")) == TA_OK);
    ta_atom console = atom_of(&f.atoms, "console");
    CHECK(console != TA_ATOM_NONE);
    CHECK(canonical_of(&io, "console") == console);
    CHECK(io.entries.count == 3u);
    CHECK(io.builtin_count == 2u);

    /* A name a chart interned first gets the same atom. */
    ta_atom http = TA_ATOM_NONE;
    CHECK(ta_atom_intern(&f.atoms, text("http"), &http) == TA_OK);
    CHECK(ta_type_registry_declare(&io, text("http")) == TA_OK);
    ta_atom canonical = TA_ATOM_NONE;
    CHECK(ta_type_registry_declared(&io, http, &canonical) == TA_OK);
    CHECK(canonical == http);

    /* Byte for byte: case, a prefix and an extension are other names. */
    CHECK(canonical_of(&io, "Console") == TA_ATOM_NONE);
    CHECK(canonical_of(&io, "consol") == TA_ATOM_NONE);
    CHECK(canonical_of(&io, "consoles") == TA_ATOM_NONE);
    /* A name read within its length from a larger buffer. */
    const char buffer[] = "consoles";
    CHECK(ta_type_registry_declare(&io, (ta_str){ .ptr = buffer, .len = 7 }) == TA_OK);
    CHECK(io.entries.count == 4u);
    /* An embedded NUL is part of the name. */
    CHECK(ta_type_registry_declare(&io, (ta_str){ .ptr = "a\0b", .len = 3 }) == TA_OK);
    CHECK(io.entries.count == 5u);
    CHECK(canonical_of(&io, "a") == TA_ATOM_NONE);

    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 4. */
static void test_idempotent(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    CHECK(ta_type_registry_declare(&io, text("console")) == TA_OK);
    ta_atom console = atom_of(&f.atoms, "console");
    uint32_t count = io.entries.count;
    uint32_t atoms = f.atoms.entries.count;
    uint32_t attempts = f.heap.attempts;
    f.heap.policy.refuse = true;
    for (int round = 0; round < 3; round += 1) {
        CHECK(ta_type_registry_declare(&io, text("console")) == TA_OK);
    }
    f.heap.policy.refuse = false;
    CHECK(f.heap.attempts == attempts);
    CHECK(io.entries.count == count);
    CHECK(f.atoms.entries.count == atoms);
    CHECK(canonical_of(&io, "console") == console);

    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 5. */
static void test_reserved(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    ta_type_registry invoker;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    CHECK(registry_init(&f, &invoker, &TA_TYPE_REGISTRY_INVOKER_BUILTINS) == TA_OK);
    uint32_t attempts = f.heap.attempts;

    CHECK(ta_type_registry_declare(&io, text(SCXML_IO)) == TA_ERR_UNSUPPORTED);
    CHECK(ta_type_registry_declare(&io, text("scxml")) == TA_ERR_UNSUPPORTED);
    CHECK(ta_type_registry_declare(&invoker, text(SCXML_INVOKE)) == TA_ERR_UNSUPPORTED);
    CHECK(ta_type_registry_declare(&invoker, text("scxml")) == TA_ERR_UNSUPPORTED);
    CHECK(ta_type_registry_declare(&invoker, text(SCXML_INVOKE_BARE)) == TA_ERR_UNSUPPORTED);

    CHECK(f.heap.attempts == attempts);
    CHECK(io.entries.count == 2u);
    CHECK(invoker.entries.count == 3u);
    CHECK(canonical_of(&io, "scxml") == atom_of(&f.atoms, SCXML_IO));
    CHECK(canonical_of(&invoker, "scxml") == atom_of(&f.atoms, SCXML_INVOKE));

    ta_type_registry_destroy(&invoker);
    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 6. */
static void test_invalid_names(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    uint32_t atoms = f.atoms.entries.count;
    uint32_t attempts = f.heap.attempts;

    CHECK(ta_type_registry_declare(&io, (ta_str){ .ptr = "", .len = 0 }) == TA_ERR_INVALID);
    CHECK(ta_type_registry_declare(&io, (ta_str){ .ptr = NULL, .len = 0 }) == TA_ERR_INVALID);
    CHECK(ta_type_registry_declare(&io, (ta_str){ .ptr = NULL, .len = 4 }) == TA_ERR_INVALID);

    CHECK(f.heap.attempts == attempts);
    CHECK(io.entries.count == 2u);
    CHECK(f.atoms.entries.count == atoms);
    CHECK(ta_type_registry_declared(&io, 0u, NULL) == TA_ERR_NOT_FOUND);

    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Declares type-0 .. type-(count - 1) in `registry`. */
static void declare_many(ta_type_registry *registry, uint32_t count) {
    char name[32];
    for (uint32_t at = 0; at < count; at += 1u) {
        snprintf(name, sizeof name, "type-%u", at);
        CHECK(ta_type_registry_declare(registry, text(name)) == TA_OK);
    }
}

/* Claim 7. The sweep covers the declarations that grow the registry's
 * entries, its hash table and the atom table, each refused at every
 * allocation it makes. */
static void test_declare_refused(void) {
    enum { DECLARATIONS = 40 };
    uint32_t refusals = 0;
    char name[32];
    for (uint32_t before = 0; before < DECLARATIONS; before += 1u) {
        snprintf(name, sizeof name, "type-%u", before);
        /* How many allocations this declaration makes. */
        uint32_t needed = 0;
        {
            fixture f;
            fixture_init(&f);
            ta_type_registry io;
            CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
            declare_many(&io, before);
            uint32_t attempts = f.heap.attempts;
            CHECK(ta_type_registry_declare(&io, text(name)) == TA_OK);
            needed = f.heap.attempts - attempts;
            ta_type_registry_destroy(&io);
            fixture_destroy(&f);
        }
        for (uint32_t refused = 1; refused <= needed; refused += 1u) {
            fixture f;
            fixture_init(&f);
            ta_type_registry io;
            CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
            declare_many(&io, before);
            uint32_t count = io.entries.count;
            f.heap.policy.fail_at = f.heap.attempts + refused;
            CHECK(ta_type_registry_declare(&io, text(name)) == TA_ERR_NOMEM);
            f.heap.policy.fail_at = 0;
            refusals += 1u;
            CHECK(io.entries.count == count);
            /* No entry for the name, whether or not its atom was made. */
            CHECK(canonical_of(&io, name) == TA_ATOM_NONE);
            CHECK(canonical_of(&io, "scxml") == atom_of(&f.atoms, SCXML_IO));
            CHECK(canonical_of(&io, SCXML_IO) == atom_of(&f.atoms, SCXML_IO));
            char earlier[32];
            for (uint32_t at = 0; at < before; at += 1u) {
                snprintf(earlier, sizeof earlier, "type-%u", at);
                CHECK(canonical_of(&io, earlier) == atom_of(&f.atoms, earlier));
            }
            /* The same declaration succeeds once memory is there. */
            CHECK(ta_type_registry_declare(&io, text(name)) == TA_OK);
            CHECK(canonical_of(&io, name) == atom_of(&f.atoms, name));
            CHECK(io.entries.count == count + 1u);
            ta_type_registry_destroy(&io);
            fixture_destroy(&f);
        }
    }
    /* The registry's entries grow at 8, 16 and 32 names, and its hash
     * table at 6, 12 and 24: each is one refusal at least. */
    CHECK(refusals >= 6u);

    /* Refusing every allocation from the first. */
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    declare_many(&io, 6u); /* the next declaration grows the entries */
    uint32_t count = io.entries.count;
    f.heap.policy.refuse = true;
    CHECK(ta_type_registry_declare(&io, text("console")) == TA_ERR_NOMEM);
    f.heap.policy.refuse = false;
    CHECK(io.entries.count == count);
    CHECK(canonical_of(&io, "console") == TA_ATOM_NONE);
    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 8. */
static void test_declared_never_allocates(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    CHECK(ta_type_registry_declare(&io, text("console")) == TA_OK);
    ta_atom other = TA_ATOM_NONE;
    CHECK(ta_atom_intern(&f.atoms, text("not-declared"), &other) == TA_OK);
    uint32_t atoms = f.atoms.entries.count;
    uint32_t count = io.entries.count;
    uint32_t attempts = f.heap.attempts;
    f.heap.policy.refuse = true;

    ta_atom console = atom_of(&f.atoms, "console");
    ta_atom io_canonical = atom_of(&f.atoms, SCXML_IO);
    for (int round = 0; round < 100; round += 1) {
        ta_atom canonical = TA_ATOM_NONE;
        CHECK(ta_type_registry_declared(&io, console, &canonical) == TA_OK && canonical == console);
        CHECK(ta_type_registry_declared(&io, atom_of(&f.atoms, "scxml"), &canonical) == TA_OK &&
              canonical == io_canonical);
        CHECK(ta_type_registry_declared(&io, other, NULL) == TA_ERR_NOT_FOUND);
        CHECK(ta_type_registry_declared(&io, TA_ATOM_NONE, NULL) == TA_ERR_NOT_FOUND);
        CHECK(ta_type_registry_declared(&io, (ta_atom)round, NULL) == TA_ERR_NOT_FOUND ||
              (ta_atom)round == console || (ta_atom)round == io_canonical ||
              (ta_atom)round == atom_of(&f.atoms, "scxml"));
    }

    f.heap.policy.refuse = false;
    CHECK(f.heap.attempts == attempts);
    CHECK(f.atoms.entries.count == atoms);
    CHECK(io.entries.count == count);
    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 9. */
static void test_init_failures(void) {
    /* Every allocation init makes, refused in turn. */
    uint32_t needed = 0;
    {
        fixture f;
        fixture_init(&f);
        ta_type_registry invoker;
        uint32_t before = f.heap.attempts;
        CHECK(registry_init(&f, &invoker, &TA_TYPE_REGISTRY_INVOKER_BUILTINS) == TA_OK);
        needed = f.heap.attempts - before;
        ta_type_registry_destroy(&invoker);
        fixture_destroy(&f);
    }
    CHECK(needed >= 2u);
    for (uint32_t refused = 1; refused <= needed; refused += 1u) {
        fixture f;
        fixture_init(&f);
        ta_type_registry invoker;
        f.heap.policy.fail_at = f.heap.attempts + refused;
        CHECK(registry_init(&f, &invoker, &TA_TYPE_REGISTRY_INVOKER_BUILTINS) == TA_ERR_NOMEM);
        f.heap.policy.fail_at = 0;
        CHECK(invoker.entries.count == 0u);
        CHECK(invoker.builtin_count == 0u);
        CHECK(ta_type_registry_declared(&invoker, atom_of(&f.atoms, "scxml"), NULL) == TA_ERR_NOT_FOUND);
        ta_type_registry_destroy(&invoker);
        ta_type_registry_destroy(&invoker);
        fixture_destroy(&f);
    }

    /* Malformed built-in sets. */
    static const ta_type_registry_builtin empty_name[] = { { .name = { .ptr = "", .len = 0 } } };
    static const ta_type_registry_builtin null_name[] = { { .name = { .ptr = NULL, .len = 3 } } };
    static const ta_type_registry_builtin twice[] = {
        { .name = { .ptr = "a", .len = 1 } },
        { .name = { .ptr = "a", .len = 1 } },
    };
    static const ta_type_registry_builtin alias_twice[] = {
        { .name = { .ptr = "a", .len = 1 } },
        { .name = { .ptr = "a", .len = 1 }, .canonical = 1 },
    };
    static const ta_type_registry_builtin forward[] = {
        { .name = { .ptr = "a", .len = 1 }, .canonical = 2 },
        { .name = { .ptr = "b", .len = 1 } },
    };
    static const ta_type_registry_builtin self[] = { { .name = { .ptr = "a", .len = 1 }, .canonical = 1 } };
    static const ta_type_registry_builtin of_alias[] = {
        { .name = { .ptr = "a", .len = 1 } },
        { .name = { .ptr = "b", .len = 1 }, .canonical = 1 },
        { .name = { .ptr = "c", .len = 1 }, .canonical = 2 },
    };
    static const ta_type_registry_builtin past_end[] = {
        { .name = { .ptr = "a", .len = 1 } },
        { .name = { .ptr = "b", .len = 1 }, .canonical = 7 },
    };
    const ta_type_registry_builtins malformed[] = {
        { .names = NULL, .count = 1 },
        { .names = empty_name, .count = 1 },
        { .names = null_name, .count = 1 },
        { .names = twice, .count = 2 },
        { .names = alias_twice, .count = 2 },
        { .names = forward, .count = 2 },
        { .names = self, .count = 1 },
        { .names = of_alias, .count = 3 },
        { .names = past_end, .count = 2 },
    };
    for (uint32_t at = 0; at < sizeof(malformed) / sizeof(malformed[0]); at += 1u) {
        fixture f;
        fixture_init(&f);
        uint32_t atoms = f.atoms.entries.count;
        ta_type_registry registry;
        CHECK(registry_init(&f, &registry, &malformed[at]) == TA_ERR_INVALID);
        CHECK(registry.entries.count == 0u);
        CHECK(registry.builtin_count == 0u);
        CHECK(f.atoms.entries.count == atoms);
        ta_type_registry_destroy(&registry);
        fixture_destroy(&f);
    }
}

/* Claim 10. */
static void test_side_by_side(void) {
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    ta_type_registry invoker;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    CHECK(registry_init(&f, &invoker, &TA_TYPE_REGISTRY_INVOKER_BUILTINS) == TA_OK);

    /* The same alias, a different canonical entry in each. */
    CHECK(canonical_of(&io, "scxml") == atom_of(&f.atoms, SCXML_IO));
    CHECK(canonical_of(&invoker, "scxml") == atom_of(&f.atoms, SCXML_INVOKE));
    CHECK(canonical_of(&io, "scxml") != canonical_of(&invoker, "scxml"));
    /* Each one's built-ins are only its own. */
    CHECK(canonical_of(&io, SCXML_INVOKE) == TA_ATOM_NONE);
    CHECK(canonical_of(&invoker, SCXML_IO) == TA_ATOM_NONE);

    /* A name built into one may be declared in the other. */
    CHECK(ta_type_registry_declare(&io, text(SCXML_INVOKE)) == TA_OK);
    CHECK(canonical_of(&io, SCXML_INVOKE) == atom_of(&f.atoms, SCXML_INVOKE));
    CHECK(ta_type_registry_declare(&invoker, text(SCXML_IO)) == TA_OK);
    CHECK(canonical_of(&invoker, SCXML_IO) == atom_of(&f.atoms, SCXML_IO));

    /* A declaration in one is not seen by the other. */
    CHECK(ta_type_registry_declare(&io, text("console")) == TA_OK);
    CHECK(canonical_of(&io, "console") == atom_of(&f.atoms, "console"));
    CHECK(canonical_of(&invoker, "console") == TA_ATOM_NONE);
    CHECK(ta_type_registry_declare(&invoker, text("http")) == TA_OK);
    CHECK(canonical_of(&invoker, "http") == atom_of(&f.atoms, "http"));
    CHECK(canonical_of(&io, "http") == TA_ATOM_NONE);
    /* And the same name declared in both is the same atom. */
    CHECK(ta_type_registry_declare(&invoker, text("console")) == TA_OK);
    CHECK(canonical_of(&invoker, "console") == canonical_of(&io, "console"));

    CHECK(io.entries.count == 4u);
    CHECK(invoker.entries.count == 6u);

    ta_type_registry_destroy(&invoker);
    ta_type_registry_destroy(&io);
    fixture_destroy(&f);
}

/* Claim 11. */
static void test_many(void) {
    enum { MANY = 1000 };
    fixture f;
    fixture_init(&f);
    ta_type_registry io;
    CHECK(registry_init(&f, &io, &TA_TYPE_REGISTRY_IO_BUILTINS) == TA_OK);
    char name[32];
    for (int at = 0; at < MANY; at += 1) {
        snprintf(name, sizeof name, "type-%d", at);
        CHECK(ta_type_registry_declare(&io, text(name)) == TA_OK);
    }
    CHECK(io.entries.count == 2u + MANY);
    CHECK(io.index.slot_count > TA_TABLE_MIN_SLOTS);
    for (int at = 0; at < MANY; at += 1) {
        snprintf(name, sizeof name, "type-%d", at);
        ta_atom atom = atom_of(&f.atoms, name);
        CHECK(atom != TA_ATOM_NONE);
        CHECK(canonical_of(&io, name) == atom);
    }
    CHECK(canonical_of(&io, "scxml") == atom_of(&f.atoms, SCXML_IO));
    CHECK(canonical_of(&io, "type-1000") == TA_ATOM_NONE);

    uint32_t atoms = f.atoms.entries.count;
    ta_type_registry_destroy(&io);
    ta_type_registry_destroy(&io);
    CHECK(io.entries.count == 0u);
    CHECK(f.atoms.entries.count == atoms);
    CHECK(atom_of(&f.atoms, "type-999") != TA_ATOM_NONE);
    fixture_destroy(&f);
}

int main(void) {
    test_builtins();
    test_not_declared();
    test_declare();
    test_idempotent();
    test_reserved();
    test_invalid_names();
    test_declare_refused();
    test_declared_never_allocates();
    test_init_failures();
    test_side_by_side();
    test_many();
    if (failures != 0) {
        fprintf(stderr, "type_registry_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("type_registry_test: ok\n");
    return 0;
}
