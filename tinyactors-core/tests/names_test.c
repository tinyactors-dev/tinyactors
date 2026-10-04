/*
 * ta_names: the name resolver (src/names.h), which turns a taken
 * envelope's name bytes into atoms for the event matcher, and looks up
 * typeexpr and sendidexpr values, without interning anything.
 *
 * The test includes the amalgamation so it reaches the private unit and
 * the matcher it feeds (ta_pattern_match_run in src/definition.c); it uses
 * the C library, the core does not.
 *
 * Claims proven here, in order:
 *
 * 1. init binds the table and the scratch with nothing resolved (count 0)
 *    and writes nothing into the scratch;
 * 2. a one-segment name resolves to its atom, or to UNKNOWN when it is
 *    not an atom; only count entries of the scratch are written;
 * 3. the worked examples of run-time names resolve segment by segment to
 *    the atoms the definition registered, and to UNKNOWN for the rest;
 * 4. UNKNOWN equals no real atom (it is not valid in the table, and every
 *    atom interned differs from it), and the unchanged matcher, run over
 *    the resolved scratch, gives the worked examples' answers: UNKNOWN is
 *    taken only by a `*` segment;
 * 5. a name of exactly capacity segments resolves, at the minimum depth
 *    of 8 and the ceiling of 64, and one more segment answers
 *    TA_ERR_INVALID and changes nothing;
 * 6. an empty name, an empty segment (leading, trailing or doubled dot,
 *    a lone dot, only dots), and a NULL pointer with a nonzero length
 *    answer TA_ERR_INVALID and change nothing; a scratch of capacity 0
 *    refuses every name;
 * 7. segments are compared byte for byte: case, a prefix or extension of
 *    an atom and an embedded NUL give UNKNOWN, and a name inside a larger
 *    buffer is read only within its length;
 * 8. resolving never inserts into the atom table and never allocates:
 *    with an allocator that refuses everything, names, attackers' names
 *    included, still resolve, the table keeps its size, and destroying it
 *    leaks nothing;
 * 9. resolution is current: a segment that was UNKNOWN resolves to its
 *    atom once it is interned, and a later resolve replaces the earlier
 *    result, including a shorter one;
 * 10. lookup takes the whole text as one atom, dots and all: an atom
 *     gives itself, any other text gives UNKNOWN, empty text gives
 *     UNKNOWN although atom 0 is the empty text, and a NULL pointer with
 *     a nonzero length gives UNKNOWN; it leaves the scratch alone and
 *     never inserts.
 */
#include "../include/tinyactors.c"

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

/* ------------------------------------------------------------------------
 * An allocator that counts and can refuse.
 */

#define MAX_BLOCKS 256u

typedef struct {
    void *ptrs[MAX_BLOCKS];
    uint32_t live;
    uint32_t attempts;
    /* When true, every allocation is refused. */
    bool refuse;
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *heap = self;
    heap->attempts += 1u;
    if (heap->refuse || heap->live == MAX_BLOCKS) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = size == 0 ? alignment : (size + alignment - 1u) / alignment * alignment;
    void *ptr = aligned_alloc(alignment, size);
    if (ptr != NULL) {
        heap->ptrs[heap->live++] = ptr;
    }
    return ptr;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *heap = self;
    for (uint32_t index = 0; params->ptr != NULL && index < heap->live; index += 1u) {
        if (heap->ptrs[index] == params->ptr) {
            heap->ptrs[index] = heap->ptrs[--heap->live];
            free(params->ptr);
            return;
        }
    }
    if (params->ptr != NULL) {
        fprintf(stderr, "free of %p, which this allocator never handed out\n", params->ptr);
        failures += 1;
    }
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };

static ta_allocator allocator_of(counting_allocator *heap) {
    return (ta_allocator){ .self = heap, .v = &counting_vtable };
}

/* ------------------------------------------------------------------------
 * Fixtures: the worked examples' definition, and a scratch on the heap
 * of exactly its capacity, so AddressSanitizer catches a write past it.
 */

#define SENTINEL 0xA5A5A5A5u

static ta_str text(const char *cstr) {
    return (ta_str){ .ptr = cstr, .len = (uint32_t)strlen(cstr) };
}

static ta_atom intern(ta_atom_table *table, const char *cstr) {
    ta_atom atom = TA_ATOM_NONE;
    CHECK(ta_atom_intern(table, text(cstr), &atom) == TA_OK);
    return atom;
}

typedef struct {
    counting_allocator heap;
    ta_atom_table table;
    ta_atom *scratch;
    uint32_t capacity;
    ta_names names;
    /* The atoms the worked examples' definition registers. */
    ta_atom order, paid, error, communication, invoke, payment;
} fixture;

static void fixture_scratch_fill(fixture *f) {
    for (uint32_t index = 0; index < f->capacity; index += 1u) {
        f->scratch[index] = SENTINEL;
    }
}

/* The definition's descriptors are order, order.*.paid,
 * error.communication.invoke.payment and *; it registers the atoms order,
 * paid, error, communication, invoke and payment. */
static void fixture_init(fixture *f, uint32_t capacity) {
    memset(f, 0, sizeof *f);
    CHECK(ta_atom_table_init(&f->table, allocator_of(&f->heap)) == TA_OK);
    f->order = intern(&f->table, "order");
    f->paid = intern(&f->table, "paid");
    f->error = intern(&f->table, "error");
    f->communication = intern(&f->table, "communication");
    f->invoke = intern(&f->table, "invoke");
    f->payment = intern(&f->table, "payment");
    f->capacity = capacity;
    f->scratch = capacity == 0 ? NULL : malloc(capacity * sizeof(ta_atom));
    fixture_scratch_fill(f);
    ta_names_init(&f->names, &(ta_names_init_params){ .atoms = &f->table, .scratch = f->scratch, .capacity = capacity });
}

static void fixture_destroy(fixture *f) {
    free(f->scratch);
    ta_atom_table_destroy(&f->table);
    CHECK(f->heap.live == 0);
}

static bool scratch_is(const fixture *f, const ta_atom *expected, uint32_t count) {
    if (f->names.count != count) {
        return false;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        if (f->scratch[index] != expected[index]) {
            return false;
        }
    }
    /* Nothing past the segments is written. */
    for (uint32_t index = count; index < f->capacity; index += 1u) {
        if (f->scratch[index] != SENTINEL) {
            return false;
        }
    }
    return true;
}

/* Builds a descriptor's segments as registration does: `*` is a wildcard,
 * every other segment an atom interned now. */
typedef struct {
    ta_event_pattern_segment segments[8];
    uint32_t count;
} descriptor;

static descriptor descriptor_of(ta_atom_table *table, const char *dotted) {
    descriptor out = { .count = 0 };
    const char *start = dotted;
    for (const char *at = dotted;; at += 1) {
        if (*at != '.' && *at != '\0') {
            continue;
        }
        ta_str segment = { .ptr = start, .len = (uint32_t)(at - start) };
        ta_event_pattern_segment compiled = { .atom = TA_ATOM_NONE, .flags = 0 };
        if (segment.len == 1 && segment.ptr[0] == '*') {
            compiled.flags = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
        } else {
            CHECK(ta_atom_intern(table, segment, &compiled.atom) == TA_OK);
        }
        out.segments[out.count++] = compiled;
        if (*at == '\0') {
            break;
        }
        start = at + 1;
    }
    return out;
}

static bool matches(const fixture *f, const descriptor *d) {
    return ta_pattern_match_run(&(ta_pattern_match){
        .event = f->names.scratch,
        .event_count = f->names.count,
        .pattern = d->segments,
        .pattern_count = d->count,
    });
}

/* Builds `count` segments of "s" joined by dots into `buffer`. */
static ta_str segments_of(char *buffer, uint32_t count) {
    uint32_t len = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (index > 0) {
            buffer[len++] = '.';
        }
        buffer[len++] = 's';
    }
    return (ta_str){ .ptr = buffer, .len = len };
}

/* Resolves `name` expecting TA_ERR_INVALID and no change to the result
 * of the call before it. */
static void check_refused(fixture *f, ta_str name) {
    ta_atom before[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = f->names.count;
    if (f->capacity != 0) {
        memcpy(before, f->scratch, f->capacity * sizeof(ta_atom));
    }
    CHECK(ta_names_resolve(&f->names, name) == TA_ERR_INVALID);
    CHECK(f->names.count == count);
    CHECK(f->capacity == 0 || memcmp(before, f->scratch, f->capacity * sizeof(ta_atom)) == 0);
}

/* Claim 1. */
static void test_init(void) {
    fixture f;
    fixture_init(&f, 8);
    CHECK(f.names.atoms == &f.table);
    CHECK(f.names.scratch == f.scratch);
    CHECK(f.names.capacity == 8);
    CHECK(f.names.count == 0);
    CHECK(scratch_is(&f, NULL, 0));
    fixture_destroy(&f);
}

/* Claim 2. */
static void test_one_segment(void) {
    fixture f;
    fixture_init(&f, 8);
    CHECK(ta_names_resolve(&f.names, text("order")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order }, 1));
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("refund")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ TA_NAMES_UNKNOWN }, 1));
    fixture_destroy(&f);
}

/* Claim 3. */
static void test_worked_examples_resolve(void) {
    fixture f;
    fixture_init(&f, 8);
    const ta_atom U = TA_NAMES_UNKNOWN;

    CHECK(ta_names_resolve(&f.names, text("order.123.paid")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, U, f.paid }, 3));

    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("order.124")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, U }, 2));

    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("orders.1")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ U, U }, 2));

    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("error.communication.invoke.payment.timeout")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.error, f.communication, f.invoke, f.payment, U }, 5));

    /* A generated invocation ID, s3.7, is two segments no one authored. */
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("error.communication.invoke.s3.7.unavailable")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.error, f.communication, f.invoke, U, U, U }, 6));

    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("x9f2.qq.zz")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ U, U, U }, 3));
    fixture_destroy(&f);
}

/* Claim 4. */
static void test_worked_examples_match(void) {
    fixture f;
    fixture_init(&f, 8);
    CHECK(!ta_atom_valid(&f.table, TA_NAMES_UNKNOWN));
    CHECK(TA_NAMES_UNKNOWN == TA_ATOM_NONE);
    const ta_atom registered[] = { f.order, f.paid, f.error, f.communication, f.invoke, f.payment };
    for (uint32_t index = 0; index < sizeof registered / sizeof registered[0]; index += 1u) {
        CHECK(registered[index] != TA_NAMES_UNKNOWN);
        CHECK(registered[index] < TA_ATOM_NONE - 1u);
    }

    descriptor order = descriptor_of(&f.table, "order");
    descriptor order_any_paid = descriptor_of(&f.table, "order.*.paid");
    descriptor invoke_payment = descriptor_of(&f.table, "error.communication.invoke.payment");
    descriptor any = descriptor_of(&f.table, "*");
    /* Building the descriptors interned nothing new. */
    CHECK(f.table.entries.count == 7u);

    static const struct {
        const char *event;
        bool order, order_any_paid, invoke_payment, any;
    } rows[] = {
        { "order.123.paid", true, true, false, true },
        { "order.124", true, false, false, true },
        { "orders.1", false, false, false, true },
        { "error.communication.invoke.payment.timeout", false, false, true, true },
        { "error.communication.invoke.s3.7.unavailable", false, false, false, true },
        { "x9f2.qq.zz", false, false, false, true },
    };
    for (uint32_t row = 0; row < sizeof rows / sizeof rows[0]; row += 1u) {
        CHECK(ta_names_resolve(&f.names, text(rows[row].event)) == TA_OK);
        bool ok = matches(&f, &order) == rows[row].order &&
                  matches(&f, &order_any_paid) == rows[row].order_any_paid &&
                  matches(&f, &invoke_payment) == rows[row].invoke_payment && matches(&f, &any) == rows[row].any;
        if (!ok) {
            fprintf(stderr, "worked example %s matched wrongly\n", rows[row].event);
        }
        CHECK(ok);
    }

    /* UNKNOWN is never taken by a segment that is not `*`, even one whose
     * atom field holds the same value as a wildcard's. */
    CHECK(ta_names_resolve(&f.names, text("x")) == TA_OK);
    CHECK(f.scratch[0] == TA_NAMES_UNKNOWN);
    descriptor none = { .segments = { { .atom = f.order, .flags = 0 } }, .count = 1 };
    CHECK(!matches(&f, &none));
    CHECK(matches(&f, &any));
    fixture_destroy(&f);
}

/* Claim 5. */
static void test_depth(void) {
    static const uint32_t depths[] = { 8u, TA_EVENT_NAME_MAX_DEPTH };
    char buffer[2u * (TA_EVENT_NAME_MAX_DEPTH + 1u)];
    for (uint32_t which = 0; which < 2u; which += 1u) {
        fixture f;
        uint32_t depth = depths[which];
        fixture_init(&f, depth);
        ta_atom s = intern(&f.table, "s");
        ta_atom expected[TA_EVENT_NAME_MAX_DEPTH];
        for (uint32_t index = 0; index < depth; index += 1u) {
            expected[index] = s;
        }
        CHECK(ta_names_resolve(&f.names, segments_of(buffer, depth - 1u)) == TA_OK);
        CHECK(scratch_is(&f, expected, depth - 1u));
        CHECK(ta_names_resolve(&f.names, segments_of(buffer, depth)) == TA_OK);
        CHECK(scratch_is(&f, expected, depth));
        /* After a full resolve, a deeper one changes nothing. */
        check_refused(&f, segments_of(buffer, depth + 1u));
        CHECK(scratch_is(&f, expected, depth));
        /* After a short resolve too. */
        CHECK(ta_names_resolve(&f.names, text("order")) == TA_OK);
        check_refused(&f, segments_of(buffer, depth + 1u));
        CHECK(f.names.count == 1 && f.scratch[0] == f.order);
        fixture_destroy(&f);
    }
}

/* Claim 6. */
static void test_invalid_names(void) {
    fixture f;
    fixture_init(&f, 8);
    CHECK(ta_names_resolve(&f.names, text("order.paid")) == TA_OK);
    static const char *const refused[] = { "", ".", "..", ".order", "order.", "order..paid", ".order.paid.", "a.b..", "...a" };
    for (uint32_t index = 0; index < sizeof refused / sizeof refused[0]; index += 1u) {
        check_refused(&f, text(refused[index]));
    }
    check_refused(&f, (ta_str){ .ptr = NULL, .len = 0 });
    check_refused(&f, (ta_str){ .ptr = NULL, .len = 3 });
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, f.paid }, 2));
    /* Nothing is resolved yet: a refused first call leaves count at 0. */
    ta_names_init(&f.names, &(ta_names_init_params){ .atoms = &f.table, .scratch = f.scratch, .capacity = 8 });
    check_refused(&f, text("a..b"));
    CHECK(f.names.count == 0);
    fixture_destroy(&f);

    fixture empty;
    fixture_init(&empty, 0);
    check_refused(&empty, text("order"));
    check_refused(&empty, text(""));
    CHECK(empty.names.count == 0);
    fixture_destroy(&empty);
}

/* Claim 7. */
static void test_bytes(void) {
    fixture f;
    fixture_init(&f, 8);
    const ta_atom U = TA_NAMES_UNKNOWN;
    static const char *const unknown[] = { "Order", "ORDER", "orde", "order2", " order", "pai" };
    for (uint32_t index = 0; index < sizeof unknown / sizeof unknown[0]; index += 1u) {
        fixture_scratch_fill(&f);
        CHECK(ta_names_resolve(&f.names, text(unknown[index])) == TA_OK);
        CHECK(scratch_is(&f, (ta_atom[]){ U }, 1));
    }
    /* An embedded NUL is a byte like any other. */
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, (ta_str){ .ptr = "order.pa\0id", .len = 11 }) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, U }, 2));
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, (ta_str){ .ptr = "order\0.paid", .len = 11 }) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ U, f.paid }, 2));
    /* A name in a larger buffer, with no terminator, is read only within
     * its length; the buffer is exact so a read past it is caught. */
    char *buffer = malloc(14);
    memcpy(buffer, "xxorder.paidyy", 14);
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, (ta_str){ .ptr = buffer + 2, .len = 10 }) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, f.paid }, 2));
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, (ta_str){ .ptr = buffer + 2, .len = 9 }) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, U }, 2));
    char *exact = malloc(10);
    memcpy(exact, "order.paid", 10);
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, (ta_str){ .ptr = exact, .len = 10 }) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, f.paid }, 2));
    CHECK(ta_names_lookup(&f.names, (ta_str){ .ptr = exact, .len = 5 }) == f.order);
    free(exact);
    free(buffer);
    fixture_destroy(&f);
}

/* Claim 8. */
static void test_never_inserts_or_allocates(void) {
    fixture f;
    fixture_init(&f, 8);
    uint32_t atoms = f.table.entries.count;
    uint32_t live = f.heap.live;
    uint32_t attempts = f.heap.attempts;
    f.heap.refuse = true;
    static const char *const names[] = {
        "order.paid", "order.123.paid", "x9f2.qq.zz", "a.b.c.d.e.f.g.h", "error.communication.invoke.s3.7.unavailable",
    };
    char random_name[64];
    srand(7);
    for (uint32_t round = 0; round < 1000u; round += 1u) {
        ta_str name;
        if (round < sizeof names / sizeof names[0]) {
            name = text(names[round]);
        } else {
            /* An attacker posting random names of 1 to 8 segments. */
            uint32_t len = 0;
            uint32_t segments = 1u + (uint32_t)rand() % 8u;
            for (uint32_t segment = 0; segment < segments; segment += 1u) {
                if (segment > 0) {
                    random_name[len++] = '.';
                }
                uint32_t size = 1u + (uint32_t)rand() % 6u;
                for (uint32_t at = 0; at < size; at += 1u) {
                    random_name[len++] = (char)('a' + rand() % 26);
                }
            }
            name = (ta_str){ .ptr = random_name, .len = len };
        }
        CHECK(ta_names_resolve(&f.names, name) == TA_OK);
        (void)ta_names_lookup(&f.names, name);
    }
    check_refused(&f, text("a..b"));
    check_refused(&f, text("a.b.c.d.e.f.g.h.i"));
    CHECK(f.heap.attempts == attempts);
    CHECK(f.heap.live == live);
    CHECK(f.table.entries.count == atoms);
    f.heap.refuse = false;
    fixture_destroy(&f);
}

/* Claim 9. */
static void test_current(void) {
    fixture f;
    fixture_init(&f, 8);
    const ta_atom U = TA_NAMES_UNKNOWN;
    CHECK(ta_names_resolve(&f.names, text("orders.1.shipped")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ U, U, U }, 3));
    /* A definition registered while the envelope waited adds atoms. */
    ta_atom orders = intern(&f.table, "orders");
    ta_atom shipped = intern(&f.table, "shipped");
    fixture_scratch_fill(&f);
    CHECK(ta_names_resolve(&f.names, text("orders.1.shipped")) == TA_OK);
    CHECK(scratch_is(&f, (ta_atom[]){ orders, U, shipped }, 3));
    descriptor orders_shipped = descriptor_of(&f.table, "orders.*.shipped");
    CHECK(matches(&f, &orders_shipped));
    /* A shorter name replaces the result; the entries past it keep what
     * was there, but count says where it ends. */
    CHECK(ta_names_resolve(&f.names, text("paid")) == TA_OK);
    CHECK(f.names.count == 1 && f.scratch[0] == f.paid);
    fixture_destroy(&f);
}

/* Claim 10. */
static void test_lookup(void) {
    fixture f;
    fixture_init(&f, 8);
    ta_atom processor = intern(&f.table, "http://www.w3.org/TR/scxml/#SCXMLEventProcessor");
    ta_atom literal = intern(&f.table, "t.1");
    uint32_t atoms = f.table.entries.count;
    CHECK(ta_names_resolve(&f.names, text("order.paid")) == TA_OK);

    CHECK(ta_names_lookup(&f.names, text("http://www.w3.org/TR/scxml/#SCXMLEventProcessor")) == processor);
    CHECK(ta_names_lookup(&f.names, text("t.1")) == literal);
    CHECK(ta_names_lookup(&f.names, text("order")) == f.order);
    CHECK(ta_names_lookup(&f.names, text("order.paid")) == TA_NAMES_UNKNOWN);
    CHECK(ta_names_lookup(&f.names, text("#_send.7")) == TA_NAMES_UNKNOWN);
    CHECK(ta_names_lookup(&f.names, text("http://example.com/unregistered")) == TA_NAMES_UNKNOWN);
    /* Atom 0 is the empty text, but an empty name resolves to nothing. */
    ta_atom empty = TA_ATOM_NONE;
    CHECK(ta_atom_find(&f.table, text(""), &empty) == TA_OK && empty == 0);
    CHECK(ta_names_lookup(&f.names, text("")) == TA_NAMES_UNKNOWN);
    CHECK(ta_names_lookup(&f.names, (ta_str){ .ptr = NULL, .len = 0 }) == TA_NAMES_UNKNOWN);
    CHECK(ta_names_lookup(&f.names, (ta_str){ .ptr = NULL, .len = 3 }) == TA_NAMES_UNKNOWN);

    /* The scratch keeps the last resolved name, and the table its size. */
    CHECK(scratch_is(&f, (ta_atom[]){ f.order, f.paid }, 2));
    CHECK(f.table.entries.count == atoms);
    fixture_destroy(&f);
}

int main(void) {
    test_init();
    test_one_segment();
    test_worked_examples_resolve();
    test_worked_examples_match();
    test_depth();
    test_invalid_names();
    test_bytes();
    test_never_inserts_or_allocates();
    test_current();
    test_lookup();
    if (failures != 0) {
        fprintf(stderr, "names_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("names_test: ok\n");
    return 0;
}
