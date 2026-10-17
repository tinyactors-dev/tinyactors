/*
 * The document's columns and the cursor read side, driven without a parser.
 *
 * The parser does not exist yet, so nothing else can reach the columns. This
 * test builds the sixteen-node document from docs/internal/document-layout.md
 * through the private append API — exactly the calls a parser will make — and
 * checks that the tables in that document are what the read side reports. It
 * also builds a document that is still open, to pin the TXML_PENDING rules.
 *
 * Unlike the rest of the suite it includes a private header; it goes away, or
 * shrinks to the append API's own contract, once spec_test can build columns
 * from XML.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/doc.h"

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

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

static txml_str S(const char *text) {
    return (txml_str){ text, (uint32_t)strlen(text) };
}

static bool str_is(txml_str text, const char *literal) {
    return txml_str_equal(text, S(literal));
}

static txml_doc *create(size_t chunk_size) {
    txml_parser_config config = {
        .allocator = { NULL, &heap_vtable },
        .limits = TXML_LIMITS_DEFAULT,
        .chunk_size = chunk_size,
    };
    txml_doc *doc = NULL;
    if (txml_doc_create(&doc, &config) != TXML_OK) {
        fprintf(stderr, "doc_test: txml_doc_create failed\n");
        exit(1);
    }
    return doc;
}

/* ---- builders: what a parser will do ---------------------------------- */

static uint32_t name(txml_doc *doc, uint32_t namespace_id, const char *local) {
    uint32_t id = 0u;
    txml_intern_params params = { namespace_id, S(local), S("") };
    CHECK(txml_doc_intern_name(doc, &params, &id) == TXML_OK);
    return id;
}

static uint32_t append(txml_doc *doc, txml_node node) {
    uint32_t index = 0u;
    const txml_node_append request = { node, { 0u, 1u, 1u }, { 0u, 1u, 1u } };
    CHECK(txml_doc_append_node(doc, &request, &index) == TXML_OK);
    return index;
}

static uint32_t text(txml_doc *doc, uint32_t parent, uint32_t depth, const char *run) {
    uint32_t offset = 0u;
    CHECK(txml_doc_pool_append(doc, S(run), &offset) == TXML_OK);
    const uint32_t index = doc->nodes.count;
    return append(doc, (txml_node){
        .kind = TXML_NODE_TEXT, .depth = (uint16_t)depth, .parent = parent, .end = index + 1u,
        .name = TXML_NAME_NONE, .as.text = { offset, (uint32_t)strlen(run) } });
}

static uint32_t element(txml_doc *doc, uint32_t parent, uint32_t depth, uint32_t name_id, uint32_t attrs,
                        uint32_t attr_count, bool empty) {
    const uint32_t index = doc->nodes.count;
    return append(doc, (txml_node){
        .kind = TXML_NODE_ELEMENT, .flags = empty ? TXML_NODE_EMPTY : TXML_NODE_OPEN,
        .depth = (uint16_t)depth, .parent = parent, .end = empty ? index + 1u : TXML_INDEX_NONE,
        .name = name_id, .as.element = { attrs, attr_count } });
}

static void attribute(txml_doc *doc, uint32_t name_id, const char *value, bool last) {
    uint32_t offset = 0u;
    uint32_t index = 0u;
    CHECK(txml_doc_pool_append(doc, S(value), &offset) == TXML_OK);
    const txml_attribute_append entry = {
        { last ? (name_id | TXML_ATTRIBUTE_LAST) : name_id, offset, (uint32_t)strlen(value) },
        { { 0u, 0u, 0u }, { 0u, 0u, 0u }, { 0u, 0u, 0u } } };
    CHECK(txml_doc_append_attribute(doc, &entry, &index) == TXML_OK);
}

/* The document of document-layout.md, node for node. */
static txml_doc *worked_example(uint32_t *ids) {
    txml_doc *doc = create(0u);
    uint32_t scxml_ns = 0u, xmlns_ns = 0u;
    CHECK(txml_doc_intern_namespace(doc, S("http://www.w3.org/2005/07/scxml"), &scxml_ns) == TXML_OK);
    CHECK(txml_doc_intern_namespace(doc, S("http://www.w3.org/2000/xmlns/"), &xmlns_ns) == TXML_OK);
    CHECK(scxml_ns == 1u && xmlns_ns == 2u);

    ids[0] = name(doc, scxml_ns, "scxml");
    ids[1] = name(doc, xmlns_ns, "xmlns");
    ids[2] = name(doc, 0u, "initial");
    element(doc, 0u, 1u, ids[0], 0u, 2u, false);                        /* 1 */
    attribute(doc, ids[1], "http://www.w3.org/2005/07/scxml", false);
    attribute(doc, ids[2], "closed", true);
    text(doc, 1u, 2u, "\n  ");                                          /* 2 */
    ids[3] = name(doc, scxml_ns, "state");
    ids[4] = name(doc, 0u, "id");
    element(doc, 1u, 2u, ids[3], 2u, 1u, false);                        /* 3 */
    attribute(doc, ids[4], "closed", true);
    text(doc, 3u, 3u, "\n    ");                                        /* 4 */
    ids[5] = name(doc, scxml_ns, "transition");
    ids[6] = name(doc, 0u, "event");
    ids[7] = name(doc, 0u, "target");
    element(doc, 3u, 3u, ids[5], 3u, 2u, true);                         /* 5 */
    attribute(doc, ids[6], "open", false);
    attribute(doc, ids[7], "opening", true);
    text(doc, 3u, 3u, "\n  ");                                          /* 6 */
    txml_doc_close_node(doc, 3u, &(txml_close){ 7u, { 0u, 1u, 1u } });
    text(doc, 1u, 2u, "\n  ");                                          /* 7 */
    element(doc, 1u, 2u, ids[3], 5u, 1u, false);                        /* 8 */
    attribute(doc, ids[4], "opening", true);
    text(doc, 8u, 3u, "\n    ");                                        /* 9 */
    element(doc, 8u, 3u, ids[5], 6u, 2u, true);                         /* 10 */
    attribute(doc, ids[6], "socket.open", false);
    attribute(doc, ids[7], "open", true);
    text(doc, 8u, 3u, "\n    ");                                        /* 11 */
    element(doc, 8u, 3u, ids[5], 8u, 2u, true);                         /* 12 */
    attribute(doc, ids[6], "error.communication", false);
    attribute(doc, ids[7], "closed", true);
    text(doc, 8u, 3u, "\n  ");                                          /* 13 */
    txml_doc_close_node(doc, 8u, &(txml_close){ 14u, { 0u, 1u, 1u } });
    text(doc, 1u, 2u, "\n");                                            /* 14 */
    txml_doc_close_node(doc, 1u, &(txml_close){ 15u, { 0u, 1u, 1u } });
    text(doc, 0u, 1u, "\n");                                            /* 15 */
    txml_doc_complete(doc, (txml_position){ 0u, 1u, 1u });
    return doc;
}

/* ---- checks ----------------------------------------------------------- */

static void test_layout(void) {
    _Static_assert(sizeof(txml_node) == 24u, "nodes are 24 bytes");
    _Static_assert(sizeof(txml_attribute_entry) == 12u, "attributes are 12 bytes");
    _Static_assert(sizeof(txml_name_entry) == 24u, "names are 24 bytes");
    _Static_assert(sizeof(txml_namespace_entry) == 8u, "namespaces are 8 bytes");
    _Static_assert(sizeof(txml_position) == 16u, "positions are 16 bytes");
}

static void test_worked_example(void) {
    uint32_t id[8];
    txml_doc *doc = worked_example(id);

    CHECK(txml_doc_node_count(doc) == 16u);
    CHECK(txml_doc_name_count(doc) == 8u);
    CHECK(doc->attributes.count == 10u);

    /* Ranges: the numbers document-layout.md reads off its table. */
    const txml_cursor root = txml_doc_root(doc);
    CHECK(txml_subtree_end(root) == 16u);
    CHECK(txml_subtree_end(txml_cursor_at(doc, 3u)) == 7u);
    CHECK(txml_subtree_end(txml_cursor_at(doc, 8u)) == 14u);
    CHECK(txml_subtree_end(txml_cursor_at(doc, 1u)) == 15u);
    CHECK(txml_subtree_end(txml_cursor_at(doc, 5u)) == 6u);
    CHECK((txml_flags(txml_cursor_at(doc, 5u)) & TXML_NODE_EMPTY) != 0u);
    CHECK(txml_contains(txml_cursor_at(doc, 3u), txml_cursor_at(doc, 6u)));
    CHECK(!txml_contains(txml_cursor_at(doc, 3u), txml_cursor_at(doc, 8u)));
    CHECK(txml_contains(txml_cursor_at(doc, 8u), txml_cursor_at(doc, 8u)));

    /* Walking the children of the document element skips nothing by itself. */
    txml_cursor c = txml_cursor_at(doc, 1u);
    CHECK(txml_first_child(&c) == TXML_MOVED && txml_index(c) == 2u);
    CHECK(txml_kind(c) == TXML_NODE_TEXT && str_is(txml_node_text(c), "\n  "));
    CHECK(txml_next_sibling(&c) == TXML_MOVED && txml_index(c) == 3u);
    CHECK(txml_next_sibling(&c) == TXML_MOVED && txml_index(c) == 7u);
    CHECK(txml_next_sibling(&c) == TXML_MOVED && txml_index(c) == 8u);
    CHECK(txml_next_sibling(&c) == TXML_MOVED && txml_index(c) == 14u);
    CHECK(txml_next_sibling(&c) == TXML_NONE && txml_index(c) == 14u);
    CHECK(txml_parent(&c) == TXML_MOVED && txml_index(c) == 1u);

    /* Node 13's end equals its parent's end: no next sibling. */
    c = txml_cursor_at(doc, 13u);
    CHECK(txml_next_sibling(&c) == TXML_NONE);

    /* next_element skips whitespace and never descends. */
    c = txml_cursor_at(doc, 3u);
    CHECK(txml_next_element(&c) == TXML_MOVED && txml_index(c) == 8u);
    CHECK(txml_next_element(&c) == TXML_NONE);

    /* The name scan: nodes 5, 10 and 12, and only them. */
    const txml_find query = { .name = id[5], .bound = txml_subtree_end(root) };
    static const uint32_t expected[] = { 5u, 10u, 12u };
    c = root;
    for (size_t i = 0; i < 3u; i++) {
        CHECK(txml_find_next(&c, &query) == TXML_MOVED && txml_index(c) == expected[i]);
    }
    CHECK(txml_find_next(&c, &query) == TXML_NONE);

    /* Names resolve by expanded name, not by prefix or position. */
    const txml_qname transition = { S("http://www.w3.org/2005/07/scxml"), S("transition") };
    const txml_qname wrong_ns = { S(""), S("transition") };
    CHECK(txml_doc_find_name(doc, &transition) == id[5]);
    CHECK(txml_doc_find_name(doc, &wrong_ns) == TXML_NAME_NONE);
    CHECK(txml_node_name_id(txml_cursor_at(doc, 10u)) == id[5]);
    CHECK(txml_node_name_id(txml_cursor_at(doc, 2u)) == TXML_NAME_NONE);
    txml_qname qname;
    CHECK(txml_doc_name_at(doc, id[7], &qname) == TXML_OK);
    CHECK(str_is(qname.local, "target") && qname.ns.length == 0u);

    /* Attributes: contiguous per element, iteration stops at the element's own. */
    c = txml_cursor_at(doc, 12u);
    CHECK(txml_attribute_count(c) == 2u);
    txml_attr_cursor a;
    txml_attribute_view view;
    CHECK(txml_first_attribute(c, &a) == TXML_MOVED);
    CHECK(txml_attribute_read(a, &view) == TXML_OK && view.name == id[6] && str_is(view.value, "error.communication"));
    CHECK(txml_next_attribute(&a) == TXML_MOVED);
    CHECK(txml_attribute_read(a, &view) == TXML_OK && view.name == id[7] && str_is(view.value, "closed"));
    CHECK(txml_next_attribute(&a) == TXML_NONE);

    /* A declaration is an attribute flagged as such, and reads by name. */
    txml_str value;
    CHECK(txml_first_attribute(txml_cursor_at(doc, 1u), &a) == TXML_MOVED);
    CHECK(txml_attribute_read(a, &view) == TXML_OK && (view.flags & TXML_ATTR_NS_DECL) != 0u);
    CHECK(txml_find_attribute(txml_cursor_at(doc, 1u), id[2], &value) && str_is(value, "closed"));
    CHECK(!txml_find_attribute(txml_cursor_at(doc, 3u), id[2], &value));
    CHECK(txml_attribute_count(txml_cursor_at(doc, 2u)) == 0u);

    /* Every node has a position; an absent cursor does not. */
    txml_position position;
    CHECK(txml_node_position(txml_cursor_at(doc, 15u), &position) == TXML_OK);
    CHECK(txml_node_position(txml_cursor_at(doc, 16u), &position) == TXML_ERR_INVALID);

    /* Absent cursors read as absent everywhere (cursor.md, "Node 0 and the edges"). */
    const txml_cursor absent = txml_cursor_at(doc, 999u);
    c = absent;
    CHECK(txml_kind(absent) == TXML_NODE_DOCUMENT);
    CHECK(txml_first_child(&c) == TXML_NONE && txml_next_sibling(&c) == TXML_NONE);
    CHECK(txml_parent(&c) == TXML_NONE && txml_next_element(&c) == TXML_NONE);
    CHECK(txml_subtree_end(absent) == TXML_INDEX_NONE && txml_node_text(absent).length == 0u);
    c = root;
    CHECK(txml_parent(&c) == TXML_NONE);

    /* Nothing is open once the parse is done. */
    for (uint32_t i = 0u; i < 16u; i++) {
        CHECK((txml_flags(txml_cursor_at(doc, i)) & TXML_NODE_OPEN) == 0u);
    }

    /* Pool and arena accounting are reported per arena. */
    CHECK(txml_doc_arena(doc, TXML_ARENA_VALUES)->used > 0u);
    CHECK(txml_doc_arena(doc, TXML_ARENA_TREE)->used > 0u);

    txml_doc_destroy(doc);
}

static void test_open_document(void) {
    txml_doc *doc = create(0u);
    const uint32_t item = name(doc, 0u, "item");
    const uint32_t a = element(doc, 0u, 1u, item, 0u, 0u, false);     /* 1, open */
    CHECK(a == 1u);

    txml_cursor c = txml_doc_root(doc);
    CHECK((txml_flags(c) & TXML_NODE_OPEN) != 0u);
    CHECK(txml_subtree_end(c) == TXML_INDEX_NONE);
    CHECK(txml_first_child(&c) == TXML_MOVED && txml_index(c) == 1u);
    CHECK(txml_subtree_end(c) == TXML_INDEX_NONE);
    CHECK(txml_next_sibling(&c) == TXML_PENDING && txml_index(c) == 1u);
    CHECK(txml_next_element(&c) == TXML_PENDING);
    CHECK(!txml_contains(c, c));

    /* An open element with no children yet: a child may still arrive. */
    CHECK(txml_first_child(&c) == TXML_PENDING);

    text(doc, 1u, 2u, "hi");
    CHECK(txml_first_child(&c) == TXML_MOVED && txml_index(c) == 2u);
    /* The text's next sibling is unknown: its parent is still open. */
    CHECK(txml_next_sibling(&c) == TXML_PENDING);

    const txml_find scan = { .name = item, .bound = TXML_INDEX_NONE };
    txml_cursor s = txml_doc_root(doc);
    CHECK(txml_find_next(&s, &scan) == TXML_MOVED && txml_index(s) == 1u);
    CHECK(txml_find_next(&s, &scan) == TXML_PENDING);

    txml_doc_close_node(doc, 1u, &(txml_close){ 3u, { 0u, 1u, 1u } });
    c = txml_cursor_at(doc, 2u);
    CHECK(txml_next_sibling(&c) == TXML_NONE);
    c = txml_cursor_at(doc, 1u);
    CHECK(txml_next_sibling(&c) == TXML_PENDING); /* the document is not done */

    txml_doc_complete(doc, (txml_position){ 0u, 1u, 1u });
    CHECK(txml_next_sibling(&c) == TXML_NONE);
    s = txml_cursor_at(doc, 1u);
    CHECK(txml_find_next(&s, &scan) == TXML_NONE);
    CHECK(txml_subtree_end(txml_doc_root(doc)) == 3u);
    txml_doc_destroy(doc);
}

/* Enough names, attributes and text to cross column pages, grow the name
 * index, and force pool runs past a page. */
static void test_growth(void) {
    txml_doc *doc = create(1024u);
    char buffer[32];
    uint32_t ids[1000];

    for (uint32_t i = 0u; i < 1000u; i++) {
        snprintf(buffer, sizeof buffer, "name%u", i);
        ids[i] = name(doc, 0u, buffer);
        CHECK(ids[i] == i);
    }
    for (uint32_t i = 0u; i < 1000u; i++) {
        snprintf(buffer, sizeof buffer, "name%u", i);
        CHECK(name(doc, 0u, buffer) == i); /* interning is idempotent */
        const txml_qname q = { S(""), S(buffer) };
        CHECK(txml_doc_find_name(doc, &q) == i);
    }

    static char big[5000];
    memset(big, 'x', sizeof big - 1u);
    uint32_t offsets[3];
    CHECK(txml_doc_pool_append(doc, S("abc"), &offsets[0]) == TXML_OK);
    CHECK(txml_doc_pool_append(doc, (txml_str){ big, sizeof big - 1u }, &offsets[1]) == TXML_OK);
    CHECK(txml_doc_pool_append(doc, S("def"), &offsets[2]) == TXML_OK);
    CHECK(str_is(txml_doc_pool_str(doc, offsets[0], 3u), "abc"));
    CHECK(txml_doc_pool_str(doc, offsets[1], sizeof big - 1u).ptr[sizeof big - 2u] == 'x');
    CHECK(str_is(txml_doc_pool_str(doc, offsets[2], 3u), "def"));

    /* Well past one 4096-entry page of nodes. */
    for (uint32_t i = 0u; i < 9000u; i++) {
        text(doc, 0u, 1u, "t");
    }
    CHECK(txml_doc_node_count(doc) == 9001u);
    CHECK(str_is(txml_node_text(txml_cursor_at(doc, 9000u)), "t"));
    txml_doc_destroy(doc);
}

/* Names are identified by (namespace, local), so two spellings share a name
 * ID; each node and attribute must still report the prefix it was written
 * with, including one that is not in scope where the first spelling was. */
static txml_doc *parse(const char *xml) {
    txml_parser_config config = { .allocator = { NULL, &heap_vtable }, .limits = TXML_LIMITS_DEFAULT };
    txml_parser *parser = NULL;
    CHECK(txml_parser_init(&parser, &config) == TXML_OK);
    const uint32_t length = (uint32_t)strlen(xml);
    CHECK(txml_parse_feed(parser, (txml_bytes){ (const uint8_t *)xml, length }, true) == length);
    txml_step step;
    do {
        step = txml_parse_step(parser);
    } while (step == TXML_STEP_PROGRESS);
    CHECK(step == TXML_STEP_DONE);
    txml_doc *doc = NULL;
    CHECK(txml_parser_finish(parser, &doc) == TXML_OK);
    return doc;
}

static void test_prefixes_are_per_node(void) {
    txml_doc *doc = parse("<a xmlns=\"u\"><p:b xmlns:p=\"u\"/><q:b xmlns:q=\"u\"/><b/><p:b xmlns:p=\"u\"/></a>");
    static const char *const expected[] = { "", "", "p", "q", "", "p" }; /* node 1 is <a> */
    const uint32_t b = txml_doc_find_name(doc, &(txml_qname){ S("u"), S("b") });

    CHECK(b != TXML_NAME_NONE);
    for (uint32_t i = 2u; i <= 5u; i++) {
        const txml_cursor c = txml_cursor_at(doc, i);
        CHECK(str_is(txml_node_prefix(c), expected[i]));
        CHECK(txml_node_name_id(c) == b); /* one name, four spellings */
    }
    /* The default is free: only the spellings that differ cost an entry. */
    CHECK(doc->node_prefixes.count == 2u);
    txml_doc_destroy(doc);

    doc = parse("<a xmlns:p=\"u\" xmlns:q=\"u\"><b p:k=\"1\"/><c q:k=\"2\"/><d k=\"3\"/></a>");
    /* attributes 0 and 1 are the declarations; then p:k, q:k, k */
    CHECK(doc->attributes.count == 5u);
    CHECK(str_is(txml_doc_prefix_of(doc, true, 2u), "p"));
    CHECK(str_is(txml_doc_prefix_of(doc, true, 3u), "q"));
    CHECK(str_is(txml_doc_prefix_of(doc, true, 4u), ""));
    CHECK(str_is(txml_doc_prefix_of(doc, true, 0u), "xmlns"));

    /* The public view reads the same records. */
    static const char *const attribute_prefixes[] = { "xmlns", "xmlns", "p", "q", "" };
    for (uint32_t i = 0u; i < 5u; i++) {
        txml_attribute_view view;
        CHECK(txml_attribute_read((txml_attr_cursor){ doc, i }, &view) == TXML_OK);
        CHECK(str_is(view.prefix, attribute_prefixes[i]));
    }
    txml_doc_destroy(doc);
}

/* The pool's directory grows with the document: strings on many more pages
 * than it starts with keep their addresses and bytes, and a page past what
 * max_document_bytes allows is still TXML_ERR_LIMIT. */
static void test_pool_directory_grows(void) {
    txml_doc *doc = create(1024u);
    static char page[1001];
    uint32_t offsets[200];
    const char *first = NULL;
    for (uint32_t i = 0u; i < 200u; i++) {
        memset(page, (int)('a' + i % 26u), sizeof page - 1u);
        CHECK(txml_doc_pool_append(doc, (txml_str){ page, sizeof page - 1u }, &offsets[i]) == TXML_OK);
        if (i == 0u) {
            first = txml_doc_pool_str(doc, offsets[0], sizeof page - 1u).ptr;
        }
    }
    CHECK(txml_doc_pool_str(doc, offsets[0], sizeof page - 1u).ptr == first); /* never moved */
    for (uint32_t i = 0u; i < 200u; i++) {
        const txml_str text = txml_doc_pool_str(doc, offsets[i], sizeof page - 1u);
        CHECK(text.length == sizeof page - 1u && text.ptr[0] == (char)('a' + i % 26u) &&
              text.ptr[sizeof page - 2u] == (char)('a' + i % 26u));
    }
    txml_doc_destroy(doc);

    txml_parser_config config = {
        .allocator = { NULL, &heap_vtable },
        .limits = TXML_LIMITS_DEFAULT,
        .chunk_size = 1024u,
    };
    config.limits.max_document_bytes = 8u * 1024u; /* 8 pages, and 2 to skip to */
    CHECK(txml_doc_create(&doc, &config) == TXML_OK);
    txml_result result = TXML_OK;
    uint32_t appended = 0u;
    while (appended < 20u && (result = txml_doc_pool_append(doc, (txml_str){ page, sizeof page - 1u }, &offsets[0])) == TXML_OK) {
        appended++;
    }
    CHECK(result == TXML_ERR_LIMIT);
    CHECK(appended == 10u);
    txml_doc_destroy(doc);
}

int main(void) {
    test_layout();
    test_worked_example();
    test_open_document();
    test_growth();
    test_pool_directory_grows();
    test_prefixes_are_per_node();

    if (failures != 0) {
        fprintf(stderr, "doc_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("doc_test: ok\n");
    return 0;
}
