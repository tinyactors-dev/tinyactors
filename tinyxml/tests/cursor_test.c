/*
 * Cursors: navigation, ranges, containment, and the name-scan query.
 *
 * This is the half of the document API that the spec corpus cannot reach.
 * tests/spec_test.c renders a document by walking node indices directly, so
 * every case in tests/spec exercises the columns and none of them exercise a
 * single navigation function. The properties pinned here are the ones ADR 0004
 * and ADR 0005 are about: a subtree is the index range [index, end), so
 * containment is two comparisons, and a name scan is a walk over that range.
 *
 * Everything below reads one fixture document. It is written without
 * whitespace between tags on purpose: no blank text nodes means the node table
 * is the same under every flag combination, so a failure here is a cursor bug
 * and never a disagreement about what the parser should have produced.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/txml.h"

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

#define XMLNS_NS "http://www.w3.org/2000/xmlns/"

/* ------------------------------------------------------------------------
 * Fixtures.
 */

static void *heap_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    size_t actual = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    size_t rounded = (size + actual - 1u) / actual * actual;
    return aligned_alloc(actual, rounded == 0u ? actual : rounded);
}

static void heap_free(void *self, const txml_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const txml_allocator_vtable heap_vtable = { .alloc = heap_alloc, .free = heap_free };
static const txml_allocator heap = { .self = NULL, .v = &heap_vtable };

static bool str_is(txml_str text, const char *literal) {
    const size_t length = strlen(literal);
    if (text.length != length) {
        return false;
    }
    return length == 0u || memcmp(text.ptr, literal, length) == 0;
}

/* Parse a whole document, or return NULL having reported why not. */
static txml_doc *parse(const char *xml, uint32_t flags) {
    txml_parser_config config = {
        .allocator = heap,
        .limits = TXML_LIMITS_DEFAULT,
        .flags = flags,
        .staging_length = 0u,
        .chunk_size = 0u,
        .step_budget = 0u,
    };

    txml_parser *parser = NULL;
    if (txml_parser_init(&parser, &config) != TXML_OK) {
        fprintf(stderr, "cursor_test: parser init failed\n");
        failures += 1;
        return NULL;
    }

    const uint32_t total = (uint32_t)strlen(xml);
    uint32_t fed = 0u;
    bool final_sent = false;

    for (unsigned long iterations = 0u;; iterations++) {
        if (iterations > 1000000u) {
            fprintf(stderr, "cursor_test: the parser made no progress\n");
            failures += 1;
            txml_parser_destroy(parser);
            return NULL;
        }

        const txml_step step = txml_parse_step(parser);
        if (step == TXML_STEP_PROGRESS) {
            continue;
        }
        if (step == TXML_STEP_NEED_INPUT) {
            const uint32_t want = total - fed;
            const txml_bytes piece = { (const uint8_t *)xml + fed, want };
            const uint32_t taken = txml_parse_feed(parser, piece, true);
            fed += taken;
            final_sent = taken == want;
            if (!final_sent && taken == 0u) {
                fprintf(stderr, "cursor_test: the parser accepted no input\n");
                failures += 1;
                txml_parser_destroy(parser);
                return NULL;
            }
            continue;
        }
        if (step == TXML_STEP_DONE) {
            break;
        }

        fprintf(stderr, "cursor_test: the parse did not finish (step %d)\n", (int)step);
        failures += 1;
        txml_parser_destroy(parser);
        return NULL;
    }

    txml_doc *doc = NULL;
    if (txml_parser_finish(parser, &doc) != TXML_OK || doc == NULL) {
        fprintf(stderr, "cursor_test: finishing the parse failed\n");
        failures += 1;
        txml_parser_destroy(parser);
        return NULL;
    }
    return doc;
}

/*
 * The fixture, and its node table. Two records so that "the next one" and
 * "not this one" are both expressible, a comment and a PI between them so that
 * txml_next_element() has something to skip, and an empty element so that a
 * childless node is covered.
 */
#define CATALOG                                                  \
    "<catalog xmlns=\"urn:cat\" xmlns:m=\"urn:meta\">"           \
    "<item id=\"a\" m:rank=\"1\"><name>First</name></item>"      \
    "<!-- between items -->"                                     \
    "<item id=\"b\"><name>Second</name><note/></item>"           \
    "<?render fast?>"                                            \
    "</catalog>"

enum {
    N_DOCUMENT = 0, /* document  end=11 depth=0 */
    N_CATALOG,      /* element   end=11 depth=1 attributes=2 (both NS_DECL) */
    N_ITEM_A,       /* element   end=5  depth=2 attributes=2 */
    N_NAME_A,       /* element   end=5  depth=3 */
    N_TEXT_A,       /* text      end=5  depth=4 "First" */
    N_COMMENT,      /* comment   end=6  depth=2 */
    N_ITEM_B,       /* element   end=10 depth=2 attributes=1 */
    N_NAME_B,       /* element   end=9  depth=3 */
    N_TEXT_B,       /* text      end=9  depth=4 "Second" */
    N_NOTE,         /* element   end=10 depth=3 EMPTY */
    N_PI,           /* pi        end=11 depth=2 */
    N_COUNT
};

/* index -> {subtree end, depth}, the two facts every other range claim rests
 * on. Written out rather than computed, so a wrong end is a failure here and
 * not a matching pair of wrong answers. */
static const struct {
    uint32_t node;
    uint32_t end;
    uint32_t depth;
} ranges[] = {
    { N_DOCUMENT, N_COUNT, 0u },  { N_CATALOG, N_COUNT, 1u },
    { N_ITEM_A, N_COMMENT, 2u },  { N_NAME_A, N_COMMENT, 3u },
    { N_TEXT_A, N_COMMENT, 4u },  { N_COMMENT, N_ITEM_B, 2u },
    { N_ITEM_B, N_PI, 2u },       { N_NAME_B, N_NOTE, 3u },
    { N_TEXT_B, N_NOTE, 4u },     { N_NOTE, N_PI, 3u },
    { N_PI, N_COUNT, 2u },
};

/* ------------------------------------------------------------------------
 * The shape of the fixture, so a later failure is read as a cursor bug rather
 * than as the document being something else.
 */

static void test_fixture_shape(const txml_doc *doc) {
    CHECK(txml_doc_node_count(doc) == (uint32_t)N_COUNT);

    static const txml_node_kind kinds[N_COUNT] = {
        TXML_NODE_DOCUMENT, TXML_NODE_ELEMENT, TXML_NODE_ELEMENT, TXML_NODE_ELEMENT,
        TXML_NODE_TEXT,     TXML_NODE_COMMENT, TXML_NODE_ELEMENT, TXML_NODE_ELEMENT,
        TXML_NODE_TEXT,     TXML_NODE_ELEMENT, TXML_NODE_PI,
    };

    for (uint32_t i = 0; i < (uint32_t)N_COUNT; i++) {
        const txml_cursor c = txml_cursor_at(doc, i);
        CHECK(txml_index(c) == i);
        CHECK(txml_kind(c) == kinds[i]);
        /* Nothing is open in a finished document. */
        CHECK((txml_flags(c) & TXML_NODE_OPEN) == 0u);
    }

    CHECK((txml_flags(txml_cursor_at(doc, N_NOTE)) & TXML_NODE_EMPTY) != 0u);
    CHECK((txml_flags(txml_cursor_at(doc, N_ITEM_A)) & TXML_NODE_EMPTY) == 0u);
    CHECK(str_is(txml_node_text(txml_cursor_at(doc, N_TEXT_A)), "First"));
    CHECK(str_is(txml_node_text(txml_cursor_at(doc, N_TEXT_B)), "Second"));
}

/* ------------------------------------------------------------------------
 * Navigation. Every function moves the cursor only on TXML_MOVED — the half of
 * the contract that a test which only checks the happy path never sees.
 */

static void test_navigation(const txml_doc *doc) {
    txml_cursor c = txml_doc_root(doc);
    CHECK(txml_index(c) == (uint32_t)N_DOCUMENT);
    CHECK(txml_kind(c) == TXML_NODE_DOCUMENT);
    CHECK(txml_depth(c) == 0u);

    /* Down to the document element, then to the first record. */
    CHECK(txml_first_child(&c) == TXML_MOVED);
    CHECK(txml_index(c) == (uint32_t)N_CATALOG);
    CHECK(txml_first_child(&c) == TXML_MOVED);
    CHECK(txml_index(c) == (uint32_t)N_ITEM_A);

    /* A childless element and a text node have no first child, and the
     * refused move leaves the cursor exactly where it was. */
    txml_cursor leaf = txml_cursor_at(doc, N_TEXT_A);
    CHECK(txml_first_child(&leaf) == TXML_NONE);
    CHECK(txml_index(leaf) == (uint32_t)N_TEXT_A);

    txml_cursor empty = txml_cursor_at(doc, N_NOTE);
    CHECK(txml_first_child(&empty) == TXML_NONE);
    CHECK(txml_index(empty) == (uint32_t)N_NOTE);

    /* Siblings: every child of the document element, whatever its kind. */
    txml_cursor s = txml_cursor_at(doc, N_ITEM_A);
    CHECK(txml_next_sibling(&s) == TXML_MOVED);
    CHECK(txml_index(s) == (uint32_t)N_COMMENT);
    CHECK(txml_next_sibling(&s) == TXML_MOVED);
    CHECK(txml_index(s) == (uint32_t)N_ITEM_B);
    CHECK(txml_next_sibling(&s) == TXML_MOVED);
    CHECK(txml_index(s) == (uint32_t)N_PI);
    CHECK(txml_next_sibling(&s) == TXML_NONE);
    CHECK(txml_index(s) == (uint32_t)N_PI);

    /* next_element skips the comment, and stops rather than landing on the
     * PI: it moves to elements only. */
    txml_cursor e = txml_cursor_at(doc, N_ITEM_A);
    CHECK(txml_next_element(&e) == TXML_MOVED);
    CHECK(txml_index(e) == (uint32_t)N_ITEM_B);
    CHECK(txml_next_element(&e) == TXML_NONE);
    CHECK(txml_index(e) == (uint32_t)N_ITEM_B);

    /* Up from the deepest node to the document node, which has no parent. */
    txml_cursor p = txml_cursor_at(doc, N_TEXT_A);
    CHECK(txml_parent(&p) == TXML_MOVED);
    CHECK(txml_index(p) == (uint32_t)N_NAME_A);
    CHECK(txml_parent(&p) == TXML_MOVED);
    CHECK(txml_index(p) == (uint32_t)N_ITEM_A);
    CHECK(txml_parent(&p) == TXML_MOVED);
    CHECK(txml_index(p) == (uint32_t)N_CATALOG);
    CHECK(txml_parent(&p) == TXML_MOVED);
    CHECK(txml_index(p) == (uint32_t)N_DOCUMENT);
    CHECK(txml_parent(&p) == TXML_NONE);
    CHECK(txml_index(p) == (uint32_t)N_DOCUMENT);

    /* first_child then parent returns to where it started, for every node
     * that has a child at all. */
    for (uint32_t i = 0; i < (uint32_t)N_COUNT; i++) {
        txml_cursor down = txml_cursor_at(doc, i);
        if (txml_first_child(&down) != TXML_MOVED) {
            continue;
        }
        CHECK(txml_index(down) == i + 1u); /* a first child is the next node */
        CHECK(txml_parent(&down) == TXML_MOVED);
        CHECK(txml_index(down) == i);
    }

    /* A cursor is a value: copying one and moving the copy leaves the
     * original alone. */
    txml_cursor original = txml_cursor_at(doc, N_ITEM_A);
    txml_cursor copy = original;
    CHECK(txml_next_sibling(&copy) == TXML_MOVED);
    CHECK(txml_index(copy) == (uint32_t)N_COMMENT);
    CHECK(txml_index(original) == (uint32_t)N_ITEM_A);
}

/* ------------------------------------------------------------------------
 * Ranges and containment (ADR 0005).
 */

static void test_ranges(const txml_doc *doc) {
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        const txml_cursor c = txml_cursor_at(doc, ranges[i].node);
        CHECK(txml_subtree_end(c) == ranges[i].end);
        CHECK(txml_depth(c) == ranges[i].depth);
        /* A subtree is non-empty and never reaches past the document. */
        CHECK(txml_subtree_end(c) > ranges[i].node);
        CHECK(txml_subtree_end(c) <= (uint32_t)N_COUNT);
    }

    /* Containment is exactly the range test, for every pair of nodes. This is
     * the claim ADR 0005 makes: an ancestor test is two comparisons. */
    for (uint32_t a = 0; a < (uint32_t)N_COUNT; a++) {
        const txml_cursor ancestor = txml_cursor_at(doc, a);
        const uint32_t end = txml_subtree_end(ancestor);
        for (uint32_t n = 0; n < (uint32_t)N_COUNT; n++) {
            const txml_cursor node = txml_cursor_at(doc, n);
            const bool expected = n >= a && n < end;
            if (txml_contains(ancestor, node) != expected) {
                fprintf(stderr, "%s:%d: contains(%u, %u) should be %s\n", __FILE__, __LINE__, a, n,
                        expected ? "true" : "false");
                failures += 1;
            }
        }
    }

    /* Spelled out, so the property above is anchored to something readable. */
    const txml_cursor document = txml_cursor_at(doc, N_DOCUMENT);
    const txml_cursor catalog = txml_cursor_at(doc, N_CATALOG);
    const txml_cursor item_a = txml_cursor_at(doc, N_ITEM_A);
    const txml_cursor item_b = txml_cursor_at(doc, N_ITEM_B);
    const txml_cursor text_a = txml_cursor_at(doc, N_TEXT_A);

    CHECK(txml_contains(document, item_b));
    CHECK(txml_contains(catalog, text_a));
    CHECK(txml_contains(item_a, text_a));
    CHECK(txml_contains(item_a, item_a)); /* a node contains itself */
    CHECK(!txml_contains(item_a, item_b));
    CHECK(!txml_contains(item_b, text_a));
    CHECK(!txml_contains(item_a, catalog)); /* containment has a direction */
    CHECK(!txml_contains(catalog, document));
}

/* ------------------------------------------------------------------------
 * Cursor calls are total: an out-of-range cursor reads as absent rather than
 * returning an error or trapping.
 */

static void test_absent_cursor(const txml_doc *doc) {
    txml_cursor absent = txml_cursor_at(doc, (uint32_t)N_COUNT);
    txml_cursor far_absent = txml_cursor_at(doc, TXML_INDEX_NONE);

    txml_cursor probes[2];
    probes[0] = absent;
    probes[1] = far_absent;

    for (size_t i = 0; i < 2u; i++) {
        txml_cursor c = probes[i];

        /* txml_kind has no absent value, so the header names the one it
         * reports rather than leaving it to the implementation. */
        CHECK(txml_kind(c) == TXML_NODE_DOCUMENT);
        CHECK(txml_subtree_end(c) == TXML_INDEX_NONE);
        CHECK(txml_node_name_id(c) == TXML_NAME_NONE);
        CHECK(txml_node_text(c).length == 0u);
        CHECK(txml_node_prefix(c).length == 0u);
        CHECK(txml_attribute_count(c) == 0u);
        CHECK(!txml_contains(c, txml_doc_root(doc)));
        CHECK(!txml_contains(txml_doc_root(doc), c));

        txml_attr_cursor attribute;
        CHECK(txml_first_attribute(c, &attribute) == TXML_NONE);

        txml_str value;
        CHECK(!txml_find_attribute(c, 0u, &value));

        /* No move succeeds, and none of them moves the cursor. */
        const uint32_t before = txml_index(c);
        CHECK(txml_first_child(&c) == TXML_NONE);
        CHECK(txml_next_sibling(&c) == TXML_NONE);
        CHECK(txml_parent(&c) == TXML_NONE);
        CHECK(txml_next_element(&c) == TXML_NONE);
        CHECK(txml_index(c) == before);
    }
}

/* ------------------------------------------------------------------------
 * Names are interned per document (ADR 0006), and a name ID is what every
 * query takes.
 */

static void test_names(const txml_doc *doc) {
    const txml_qname item_q = { TXML_STR("urn:cat"), TXML_STR("item") };
    const uint32_t item = txml_doc_find_name(doc, &item_q);
    CHECK(item != TXML_NAME_NONE);

    txml_qname back;
    CHECK(txml_doc_name_at(doc, item, &back) == TXML_OK);
    CHECK(txml_qname_equal(&back, &item_q));

    /* find_name and name_at are inverses over the whole table. */
    const uint32_t count = txml_doc_name_count(doc);
    CHECK(count > 0u);
    for (uint32_t id = 0; id < count; id++) {
        txml_qname name;
        CHECK(txml_doc_name_at(doc, id, &name) == TXML_OK);
        CHECK(txml_doc_find_name(doc, &name) == id);
    }

    /* A name the document does not contain resolves to TXML_NAME_NONE, which
     * is what lets a query be answered without scanning anything. */
    const txml_qname missing = { TXML_STR("urn:cat"), TXML_STR("missing") };
    CHECK(txml_doc_find_name(doc, &missing) == TXML_NAME_NONE);

    /* The namespace is part of a name's identity, and the prefix is not. */
    const txml_qname wrong_ns = { TXML_STR("urn:meta"), TXML_STR("item") };
    const txml_qname no_ns = { TXML_STR(""), TXML_STR("item") };
    CHECK(txml_doc_find_name(doc, &wrong_ns) == TXML_NAME_NONE);
    CHECK(txml_doc_find_name(doc, &no_ns) == TXML_NAME_NONE);

    /* An element's name ID is the one find_name resolves for it. */
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_ITEM_A)) == item);
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_ITEM_B)) == item);

    const txml_qname catalog_q = { TXML_STR("urn:cat"), TXML_STR("catalog") };
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_CATALOG)) == txml_doc_find_name(doc, &catalog_q));

    /* A PI target is a name; an unqualified one. */
    const txml_qname render_q = { TXML_STR(""), TXML_STR("render") };
    CHECK(txml_doc_find_name(doc, &render_q) != TXML_NAME_NONE);
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_PI)) == txml_doc_find_name(doc, &render_q));

    /* The kinds that have no name say so. */
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_DOCUMENT)) == TXML_NAME_NONE);
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_TEXT_A)) == TXML_NAME_NONE);
    CHECK(txml_node_name_id(txml_cursor_at(doc, N_COMMENT)) == TXML_NAME_NONE);

    /* The prefix as written is reported, and is not part of identity: both
     * items carry the default namespace with no prefix. */
    CHECK(txml_node_prefix(txml_cursor_at(doc, N_ITEM_A)).length == 0u);
}

/* ------------------------------------------------------------------------
 * txml_find_next: the name scan, the fastest query the document supports.
 */

static void test_find_next(const txml_doc *doc) {
    const txml_qname item_q = { TXML_STR("urn:cat"), TXML_STR("item") };
    const txml_qname name_q = { TXML_STR("urn:cat"), TXML_STR("name") };
    const uint32_t item = txml_doc_find_name(doc, &item_q);
    const uint32_t name = txml_doc_find_name(doc, &name_q);

    /* The README's loop: every <item> in document order. */
    txml_find query = { .name = item, .bound = TXML_INDEX_NONE };
    txml_cursor c = txml_doc_root(doc);
    uint32_t found[4];
    uint32_t n = 0u;
    while (n < 4u && txml_find_next(&c, &query) == TXML_MOVED) {
        found[n++] = txml_index(c);
    }
    CHECK(n == 2u);
    CHECK(found[0] == (uint32_t)N_ITEM_A);
    CHECK(found[1] == (uint32_t)N_ITEM_B);

    /* Exhausted: no move, and the cursor stays on the last match. */
    CHECK(txml_find_next(&c, &query) == TXML_NONE);
    CHECK(txml_index(c) == (uint32_t)N_ITEM_B);

    /* The scan starts after the cursor, so a match never finds itself. */
    txml_cursor from_a = txml_cursor_at(doc, N_ITEM_A);
    CHECK(txml_find_next(&from_a, &query) == TXML_MOVED);
    CHECK(txml_index(from_a) == (uint32_t)N_ITEM_B);

    /* `bound` stops the scan before an index. */
    txml_find bounded = { .name = item, .bound = (uint32_t)N_COMMENT };
    txml_cursor b = txml_doc_root(doc);
    CHECK(txml_find_next(&b, &bounded) == TXML_MOVED);
    CHECK(txml_index(b) == (uint32_t)N_ITEM_A);
    CHECK(txml_find_next(&b, &bounded) == TXML_NONE);
    CHECK(txml_index(b) == (uint32_t)N_ITEM_A);

    /* The idiom that makes a subtree query one range: bound the scan with the
     * subtree's end, so it cannot escape into the next record. */
    txml_cursor scope = txml_cursor_at(doc, N_ITEM_B);
    txml_find inside = { .name = name, .bound = txml_subtree_end(scope) };
    CHECK(txml_find_next(&scope, &inside) == TXML_MOVED);
    CHECK(txml_index(scope) == (uint32_t)N_NAME_B);
    CHECK(txml_find_next(&scope, &inside) == TXML_NONE);

    /* Unbounded from the same start, the scan does leave the subtree — so the
     * bound above is doing the work, not the document running out. */
    txml_cursor unscoped = txml_cursor_at(doc, N_ITEM_A);
    txml_find any_name = { .name = name, .bound = TXML_INDEX_NONE };
    CHECK(txml_find_next(&unscoped, &any_name) == TXML_MOVED);
    CHECK(txml_index(unscoped) == (uint32_t)N_NAME_A);
    CHECK(txml_find_next(&unscoped, &any_name) == TXML_MOVED);
    CHECK(txml_index(unscoped) == (uint32_t)N_NAME_B);

    /* A name the document does not contain matches nothing. */
    txml_find nothing = { .name = TXML_NAME_NONE, .bound = TXML_INDEX_NONE };
    txml_cursor z = txml_doc_root(doc);
    CHECK(txml_find_next(&z, &nothing) == TXML_NONE);
    CHECK(txml_index(z) == (uint32_t)N_DOCUMENT);

    /* A bound at or before the cursor matches nothing either. */
    txml_find behind = { .name = item, .bound = (uint32_t)N_DOCUMENT };
    txml_cursor y = txml_doc_root(doc);
    CHECK(txml_find_next(&y, &behind) == TXML_NONE);

    /* The scan only ever reports elements. */
    const txml_qname render_q = { TXML_STR(""), TXML_STR("render") };
    txml_find pi_name = { .name = txml_doc_find_name(doc, &render_q), .bound = TXML_INDEX_NONE };
    txml_cursor x = txml_doc_root(doc);
    CHECK(txml_find_next(&x, &pi_name) == TXML_NONE);
}

/* ------------------------------------------------------------------------
 * Attribute lookup by name ID, and its agreement with iteration.
 */

static void test_find_attribute(const txml_doc *doc) {
    const txml_qname id_q = { TXML_STR(""), TXML_STR("id") };
    const txml_qname rank_q = { TXML_STR("urn:meta"), TXML_STR("rank") };
    const uint32_t id = txml_doc_find_name(doc, &id_q);
    const uint32_t rank = txml_doc_find_name(doc, &rank_q);
    CHECK(id != TXML_NAME_NONE);
    CHECK(rank != TXML_NAME_NONE);

    /* An unprefixed attribute has no namespace: the default xmlns in scope
     * does not reach it, so this expanded name does not exist. */
    const txml_qname id_in_default = { TXML_STR("urn:cat"), TXML_STR("id") };
    CHECK(txml_doc_find_name(doc, &id_in_default) == TXML_NAME_NONE);

    txml_str value;
    const txml_cursor item_a = txml_cursor_at(doc, N_ITEM_A);
    const txml_cursor item_b = txml_cursor_at(doc, N_ITEM_B);

    CHECK(txml_find_attribute(item_a, id, &value));
    CHECK(str_is(value, "a"));
    CHECK(txml_find_attribute(item_a, rank, &value));
    CHECK(str_is(value, "1"));

    CHECK(txml_find_attribute(item_b, id, &value));
    CHECK(str_is(value, "b"));
    /* Present on one element is not present on the other. */
    CHECK(!txml_find_attribute(item_b, rank, &value));
    CHECK(!txml_find_attribute(item_b, TXML_NAME_NONE, &value));
    /* A kind that has no attributes at all. */
    CHECK(!txml_find_attribute(txml_cursor_at(doc, N_TEXT_A), id, &value));
    CHECK(!txml_find_attribute(txml_cursor_at(doc, N_DOCUMENT), id, &value));

    /* A namespace declaration is an attribute, reachable by its expanded
     * name like any other. */
    const txml_cursor catalog = txml_cursor_at(doc, N_CATALOG);
    const txml_qname default_decl = { TXML_STR(XMLNS_NS), TXML_STR("xmlns") };
    const txml_qname m_decl = { TXML_STR(XMLNS_NS), TXML_STR("m") };
    CHECK(txml_find_attribute(catalog, txml_doc_find_name(doc, &default_decl), &value));
    CHECK(str_is(value, "urn:cat"));
    CHECK(txml_find_attribute(catalog, txml_doc_find_name(doc, &m_decl), &value));
    CHECK(str_is(value, "urn:meta"));

    /* Lookup and iteration are two views of one list: every attribute the
     * iteration yields is found by name, with the same value. */
    static const uint32_t elements[] = { N_CATALOG, N_ITEM_A, N_ITEM_B, N_NAME_A, N_NOTE };
    static const uint32_t counts[] = { 2u, 2u, 1u, 0u, 0u };

    for (size_t i = 0; i < sizeof(elements) / sizeof(elements[0]); i++) {
        const txml_cursor element = txml_cursor_at(doc, elements[i]);
        CHECK(txml_attribute_count(element) == counts[i]);

        txml_attr_cursor attribute;
        uint32_t seen = 0u;
        txml_move move = txml_first_attribute(element, &attribute);
        while (move == TXML_MOVED) {
            txml_attribute_view view;
            CHECK(txml_attribute_read(attribute, &view) == TXML_OK);

            txml_str looked_up;
            CHECK(txml_find_attribute(element, view.name, &looked_up));
            CHECK(txml_str_equal(looked_up, view.value));

            seen += 1u;
            move = txml_next_attribute(&attribute);
        }
        CHECK(move == TXML_NONE);
        CHECK(seen == counts[i]);
    }
}

/* ------------------------------------------------------------------------ */

int main(void) {
    txml_doc *doc = parse(CATALOG, 0u);
    if (doc == NULL) {
        fprintf(stderr, "cursor_test: the fixture document did not parse\n");
        return 1;
    }

    test_fixture_shape(doc);
    test_navigation(doc);
    test_ranges(doc);
    test_absent_cursor(doc);
    test_names(doc);
    test_find_next(doc);
    test_find_attribute(doc);

    txml_doc_destroy(doc);

    if (failures != 0) {
        fprintf(stderr, "cursor_test: %d check(s) failed\n", failures);
        return 1;
    }

    printf("cursor_test: ok\n");
    return 0;
}
