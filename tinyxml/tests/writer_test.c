/*
 * txml_writer: events in, bytes out.
 *
 * Every case runs twice, with a 4096-byte output buffer and with a 1-byte
 * one, and the two must agree byte for byte. The 1-byte run is what exercises
 * the TXML_NEED_SPACE / txml_writer_output / txml_writer_drain protocol at
 * every possible interruption point.
 *
 * These cases pin the writer's policies (escaping, empty tags, namespace
 * declarations, CDATA splitting); docs/internal/testing.md lists them.
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

typedef struct {
    txml_writer *writer;
    unsigned char buffer[4096];
    char sink[8192];
    size_t sink_length;
    int need_space;
    txml_result last;
} harness;

static void harness_init(harness *h, uint32_t capacity, uint32_t flags) {
    memset(h, 0, sizeof(*h));
    h->last = TXML_OK;

    txml_writer_config config = {
        .allocator = heap,
        .output = { .ptr = h->buffer, .capacity = capacity },
        .limits = TXML_LIMITS_DEFAULT,
        .flags = flags,
    };
    CHECK(txml_writer_init(&h->writer, &config) == TXML_OK);
}

static void harness_append(harness *h, txml_bytes bytes) {
    if (bytes.length == 0u) {
        return;
    }
    if (h->sink_length + bytes.length > sizeof(h->sink)) {
        fprintf(stderr, "writer_test: output overflowed the sink\n");
        failures += 1;
        return;
    }
    memcpy(h->sink + h->sink_length, bytes.ptr, bytes.length);
    h->sink_length += bytes.length;
}

/* Drive one emit call to completion, collecting everything it produces. */
static txml_result pump(harness *h, txml_result result) {
    while (result == TXML_NEED_SPACE) {
        h->need_space += 1;
        harness_append(h, txml_writer_output(h->writer));
        result = txml_writer_drain(h->writer);
    }
    if (result != TXML_OK) {
        h->last = result;
    }
    return result;
}

static txml_result harness_finish(harness *h) {
    txml_result result = pump(h, txml_writer_finish(h->writer));
    harness_append(h, txml_writer_output(h->writer));
    txml_writer_drain(h->writer);
    return result;
}

static void harness_destroy(harness *h) {
    txml_writer_destroy(h->writer);
    h->writer = NULL;
}

static txml_write_name name_of(const char *ns, const char *local, const char *prefix) {
    txml_write_name name = {
        .ns = { ns, (uint32_t)strlen(ns) },
        .local = { local, (uint32_t)strlen(local) },
        .prefix = { prefix, (uint32_t)strlen(prefix) },
    };
    return name;
}

static txml_str str_of(const char *text) {
    txml_str value = { text, (uint32_t)strlen(text) };
    return value;
}

static void start(harness *h, const char *ns, const char *local, const char *prefix) {
    const txml_write_name name = name_of(ns, local, prefix);
    CHECK(pump(h, txml_write_start(h->writer, &name)) == TXML_OK);
}

static void attribute(harness *h, const char *ns, const char *local, const char *prefix, const char *value) {
    const txml_write_attr attr = { .name = name_of(ns, local, prefix), .value = str_of(value) };
    CHECK(pump(h, txml_write_attribute(h->writer, &attr)) == TXML_OK);
}

static void text(harness *h, const char *value) {
    CHECK(pump(h, txml_write_text(h->writer, str_of(value))) == TXML_OK);
}

static void end(harness *h) {
    CHECK(pump(h, txml_write_end(h->writer)) == TXML_OK);
}

/* ------------------------------------------------------------------------
 * Scripts. Each one emits a document; the table below says what it must
 * produce.
 */

typedef void (*script_fn)(harness *h);

static void script_text(harness *h) {
    start(h, "", "a", "");
    text(h, "hi");
    end(h);
}

static void script_empty_element(harness *h) {
    start(h, "", "a", "");
    end(h);
}

static void script_attributes(harness *h) {
    start(h, "", "a", "");
    attribute(h, "", "z", "", "1");
    attribute(h, "", "m", "", "2");
    attribute(h, "", "a", "", "3");
    end(h);
}

static void script_text_escaping(harness *h) {
    start(h, "", "a", "");
    text(h, "a<b&c>d\"e'f");
    end(h);
}

static void script_attribute_escaping(harness *h) {
    start(h, "", "a", "");
    attribute(h, "", "k", "", "a<b&c>d\"e'f\tg\nh\ri");
    end(h);
}

static void script_explicit_prefix(harness *h) {
    start(h, "urn:x", "a", "p");
    attribute(h, XMLNS_NS, "p", "xmlns", "urn:x");
    start(h, "urn:x", "b", "p");
    end(h);
    end(h);
}

static void script_default_declaration(harness *h) {
    start(h, "urn:y", "a", "");
    end(h);
}

static void script_declared_prefix_is_generated(harness *h) {
    start(h, "", "root", "");
    start(h, "urn:z", "a", "q");
    end(h);
    end(h);
}

static void script_namespaced_attribute(harness *h) {
    start(h, "", "a", "");
    attribute(h, "urn:w", "k", "", "v");
    end(h);
}

static void script_inherited_namespace(harness *h) {
    start(h, "urn:x", "a", "");
    attribute(h, XMLNS_NS, "xmlns", "", "urn:x");
    start(h, "urn:x", "b", "");
    end(h);
    end(h);
}

static void script_comment_pi_cdata(harness *h) {
    start(h, "", "a", "");
    CHECK(pump(h, txml_write_comment(h->writer, str_of(" c "))) == TXML_OK);
    CHECK(pump(h, txml_write_pi(h->writer, str_of("p"), str_of("d"))) == TXML_OK);
    CHECK(pump(h, txml_write_cdata(h->writer, str_of("x<y"))) == TXML_OK);
    end(h);
}

static void script_cdata_split(harness *h) {
    start(h, "", "a", "");
    CHECK(pump(h, txml_write_cdata(h->writer, str_of("a]]>b"))) == TXML_OK);
    end(h);
}

static void script_nested(harness *h) {
    start(h, "", "a", "");
    start(h, "", "b", "");
    end(h);
    end(h);
}

static void script_mixed_content(harness *h) {
    start(h, "", "p", "");
    text(h, "one");
    start(h, "", "b", "");
    text(h, "two");
    end(h);
    text(h, "three");
    end(h);
}

/* A content fragment: text and several elements at the top level. */
static void script_fragment(harness *h) {
    text(h, "one ");
    start(h, "urn:x", "a", "");
    end(h);
    text(h, " two&");
    start(h, "", "b", "");
    text(h, "three");
    end(h);
}

typedef struct {
    const char *label;
    script_fn script;
    uint32_t flags;
    const char *expected;
} writer_case;

static const writer_case cases[] = {
    { "text", script_text, 0u, "<a>hi</a>" },
    { "empty element as a pair", script_empty_element, 0u, "<a></a>" },
    { "empty element self-closed", script_empty_element, TXML_WRITE_EMPTY_TAGS, "<a/>" },
    { "attribute order is preserved", script_attributes, TXML_WRITE_EMPTY_TAGS,
      "<a z=\"1\" m=\"2\" a=\"3\"/>" },
    { "text escaping", script_text_escaping, 0u, "<a>a&lt;b&amp;c&gt;d\"e'f</a>" },
    { "attribute escaping", script_attribute_escaping, TXML_WRITE_EMPTY_TAGS,
      "<a k=\"a&lt;b&amp;c&gt;d&quot;e'f&#9;g&#10;h&#13;i\"/>" },
    { "an explicit declaration is not repeated", script_explicit_prefix, TXML_WRITE_EMPTY_TAGS,
      "<p:a xmlns:p=\"urn:x\"><p:b/></p:a>" },
    { "an undeclared default namespace is declared", script_default_declaration,
      TXML_WRITE_EMPTY_TAGS, "<a xmlns=\"urn:y\"/>" },
    { "an undeclared prefix is declared", script_declared_prefix_is_generated,
      TXML_WRITE_EMPTY_TAGS, "<root><q:a xmlns:q=\"urn:z\"/></root>" },
    { "a namespaced attribute gets a generated prefix", script_namespaced_attribute,
      TXML_WRITE_EMPTY_TAGS, "<a xmlns:ns1=\"urn:w\" ns1:k=\"v\"/>" },
    { "an inherited default namespace is not redeclared", script_inherited_namespace,
      TXML_WRITE_EMPTY_TAGS, "<a xmlns=\"urn:x\"><b/></a>" },
    { "comment, pi, and cdata", script_comment_pi_cdata, 0u,
      "<a><!-- c --><?p d?><![CDATA[x<y]]></a>" },
    { "cdata containing the terminator is split", script_cdata_split, 0u,
      "<a><![CDATA[a]]]]><![CDATA[>b]]></a>" },
    { "nested elements", script_nested, TXML_WRITE_EMPTY_TAGS, "<a><b/></a>" },
    { "indented", script_nested, TXML_WRITE_EMPTY_TAGS | TXML_WRITE_INDENT, "<a>\n  <b/>\n</a>" },
    { "xml declaration", script_text, TXML_WRITE_DECLARATION,
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><a>hi</a>" },
    { "mixed content", script_mixed_content, 0u, "<p>one<b>two</b>three</p>" },
    { "a fragment has text and several elements at the top level", script_fragment,
      TXML_WRITE_EMPTY_TAGS | TXML_WRITE_FRAGMENT, "one <a xmlns=\"urn:x\"/> two&amp;<b>three</b>" },
};

static void report_mismatch(const char *label, uint32_t capacity, const harness *h, const char *expected) {
    fprintf(stderr, "writer_test: %s (buffer %u)\n  expected: %s\n  actual:   %.*s\n",
            label, capacity, expected, (int)h->sink_length, h->sink);
    failures += 1;
}

static void run_case(const writer_case *item) {
    const uint32_t capacities[] = { 4096u, 1u };
    char first[8192];
    size_t first_length = 0;

    for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
        harness h;
        harness_init(&h, capacities[i], item->flags);
        item->script(&h);
        CHECK(harness_finish(&h) == TXML_OK);

        const size_t expected_length = strlen(item->expected);
        if (h.sink_length != expected_length || memcmp(h.sink, item->expected, expected_length) != 0) {
            report_mismatch(item->label, capacities[i], &h, item->expected);
        }

        if (i == 0) {
            memcpy(first, h.sink, h.sink_length);
            first_length = h.sink_length;
        } else {
            /* Byte-identical however often the output was interrupted. */
            if (h.sink_length != first_length || memcmp(h.sink, first, first_length) != 0) {
                fprintf(stderr, "writer_test: %s differs between buffer sizes\n", item->label);
                failures += 1;
            }
            /* A one-byte buffer must have interrupted something. */
            if (expected_length > 1u) {
                CHECK(h.need_space > 0);
            }
        }

        harness_destroy(&h);
    }
}

/* ------------------------------------------------------------------------
 * Refusals: the writer rejects what it cannot write correctly.
 */

static void test_refusals(void) {
    harness h;

    /* An end tag with nothing open. */
    harness_init(&h, 4096u, 0u);
    CHECK(txml_write_end(h.writer) == TXML_ERR_INVALID);
    harness_destroy(&h);

    /* finish() with an element still open. */
    harness_init(&h, 4096u, 0u);
    start(&h, "", "a", "");
    CHECK(txml_writer_finish(h.writer) == TXML_ERR_INVALID);
    harness_destroy(&h);

    /* A comment may not contain "--" or end with "-". */
    harness_init(&h, 4096u, 0u);
    start(&h, "", "a", "");
    CHECK(txml_write_comment(h.writer, str_of("a--b")) == TXML_ERR_INVALID);
    CHECK(txml_write_comment(h.writer, str_of("trailing-")) == TXML_ERR_INVALID);
    harness_destroy(&h);

    /* PI data may not contain "?>", and a target is required. */
    harness_init(&h, 4096u, 0u);
    start(&h, "", "a", "");
    CHECK(txml_write_pi(h.writer, str_of("p"), str_of("a?>b")) == TXML_ERR_INVALID);
    CHECK(txml_write_pi(h.writer, str_of(""), str_of("d")) == TXML_ERR_INVALID);
    harness_destroy(&h);

    /* A prefixed name needs a local part. */
    harness_init(&h, 4096u, 0u);
    {
        const txml_write_name broken = name_of("urn:x", "", "p");
        CHECK(txml_write_start(h.writer, &broken) == TXML_ERR_INVALID);
    }
    harness_destroy(&h);

    /* A document has no text, and no second element, at the top level. */
    harness_init(&h, 4096u, 0u);
    CHECK(txml_write_text(h.writer, str_of("x")) == TXML_ERR_INVALID);
    start(&h, "", "a", "");
    end(&h);
    {
        const txml_write_name second = name_of("", "b", "");
        CHECK(txml_write_start(h.writer, &second) == TXML_ERR_INVALID);
    }
    harness_destroy(&h);

    /* A fragment has no XML declaration. */
    {
        unsigned char buffer[64];
        txml_writer *writer = NULL;
        const txml_writer_config config = {
            .allocator = heap,
            .output = { .ptr = buffer, .capacity = sizeof(buffer) },
            .limits = TXML_LIMITS_DEFAULT,
            .flags = TXML_WRITE_FRAGMENT | TXML_WRITE_DECLARATION,
        };
        CHECK(txml_writer_init(&writer, &config) == TXML_ERR_INVALID);
    }

    /* A prefix without a namespace is not a name the writer can honour. */
    harness_init(&h, 4096u, 0u);
    {
        const txml_write_name broken = name_of("", "a", "p");
        CHECK(txml_write_start(h.writer, &broken) == TXML_ERR_INVALID);
    }
    harness_destroy(&h);
}

/* ------------------------------------------------------------------------
 * The drain protocol itself: output is only ever handed over once.
 */

static void test_drain_protocol(void) {
    harness h;
    harness_init(&h, 1u, 0u);

    start(&h, "", "a", "");
    text(&h, "hello");
    end(&h);
    CHECK(harness_finish(&h) == TXML_OK);

    CHECK(h.need_space > 0);
    CHECK(h.sink_length == strlen("<a>hello</a>"));
    CHECK(memcmp(h.sink, "<a>hello</a>", h.sink_length) == 0);

    /* Everything has been drained. */
    CHECK(txml_writer_output(h.writer).length == 0u);

    harness_destroy(&h);
}

/* ------------------------------------------------------------------------
 * A fragment of a parsed document: every child of one element, written with
 * the namespace declarations it needs from outside itself. This is what an
 * SCXML <content> element's children must look like on their own (SCXML G.6).
 */

static void test_fragment_of_document(void) {
    static const char source[] =
        "<r xmlns=\"urn:d\" xmlns:c=\"urn:c\"><content>\n <m c:k=\"1\"><n/></m> <c:o/> t&lt;x</content></r>";
    txml_parser *parser = NULL;
    const txml_parser_config config = { .allocator = heap, .limits = TXML_LIMITS_DEFAULT };
    CHECK(txml_parser_init(&parser, &config) == TXML_OK);
    const txml_bytes input = { (const uint8_t *)source, (uint32_t)(sizeof(source) - 1u) };
    CHECK(txml_parse_feed(parser, input, true) == input.length);
    txml_step step;
    while ((step = txml_parse_step(parser)) == TXML_STEP_PROGRESS) {
    }
    CHECK(step == TXML_STEP_DONE);

    txml_cursor content = txml_doc_root(txml_parser_doc(parser));
    CHECK(txml_first_child(&content) == TXML_MOVED); /* <r> */
    CHECK(txml_first_child(&content) == TXML_MOVED); /* <content> */

    harness h;
    harness_init(&h, 7u, TXML_WRITE_EMPTY_TAGS | TXML_WRITE_FRAGMENT);
    txml_cursor child = content;
    for (txml_move move = txml_first_child(&child); move == TXML_MOVED; move = txml_next_sibling(&child)) {
        CHECK(pump(&h, txml_write_subtree(h.writer, child)) == TXML_OK);
    }
    CHECK(harness_finish(&h) == TXML_OK);
    static const char expected[] =
        "\n <m c:k=\"1\" xmlns=\"urn:d\" xmlns:c=\"urn:c\"><n/></m> <c:o xmlns:c=\"urn:c\"/> t&lt;x";
    if (h.sink_length != sizeof(expected) - 1u || memcmp(h.sink, expected, h.sink_length) != 0) {
        report_mismatch("a fragment of a parsed document", 7u, &h, expected);
    }
    harness_destroy(&h);
    txml_parser_destroy(parser);
}

int main(void) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_case(&cases[i]);
    }
    test_refusals();
    test_drain_protocol();
    test_fragment_of_document();

    if (failures != 0) {
        fprintf(stderr, "writer_test: %d check(s) failed\n", failures);
        return 1;
    }

    printf("writer_test: ok (%zu cases)\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
