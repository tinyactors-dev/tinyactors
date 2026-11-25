/* The report document: one tree written as Markdown, JSON and XML. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "cli/report.h"

typedef struct test_heap {
    int refuse; /* 1: every allocation is refused */
} test_heap;

static void *test_alloc(void *context, size_t size, size_t align) {
    (void)align;
    test_heap *heap = (test_heap *)context;
    return heap->refuse ? NULL : malloc(size);
}

static void test_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

/* An arena and an output that grow through the heap. */
typedef struct test_document {
    test_heap heap;
    ta_allocator allocator;
    ta_growable_arena arena;
    ta_report report;
    char room[16];
    ta_output out;
} test_document;

static void document_begin(test_document *document, const char *title, int refuse) {
    memset(document, 0, sizeof *document);
    document->heap.refuse = refuse;
    document->allocator.context = &document->heap;
    document->allocator.alloc = test_alloc;
    document->allocator.free = test_free;
    ta_growable_arena_config config = {sizeof config, 0, 256, &document->allocator};
    ta_growable_arena_init(&document->arena, &config);
    ta_report_init(&document->report, &document->arena, ta_string(title));
}

static ta_status document_render(test_document *document, uint32_t format) {
    ta_output_free(&document->out);
    ta_buffer room = {document->room, sizeof document->room, 0};
    document->out = ta_output_over(room, &document->allocator);
    return ta_report_render(&document->report, format, &document->out);
}

/* A literal as a view, NUL bytes inside it included. */
#define EXPECT(literal) ((ta_string_view){(literal), (uint32_t)(sizeof(literal) - 1)})

static int document_is(test_document *document, uint32_t format, ta_string_view expected) {
    if (document_render(document, format) != TA_OK) return 0;
    int same = ta_output_fits(&document->out) && document->out.length == expected.length &&
               memcmp(document->out.bytes, expected.pointer, expected.length) == 0;
    if (!same)
        fprintf(stderr, "format %u gave:\n%.*s\n--- expected:\n%.*s\n", format,
                (int)document->out.length, (const char *)document->out.bytes, (int)expected.length,
                expected.pointer);
    return same;
}

static void document_end(test_document *document) {
    ta_output_free(&document->out);
    ta_growable_arena_free(&document->arena);
}

static ta_report_list_names list_names(const char *name, const char *item) {
    ta_report_list_names names = {ta_string(name), ta_string(item)};
    return names;
}

#define S(text) ta_string(text)

/* Every kind of node, in all three formats. */
static void test_every_kind(void) {
    test_document document;
    ta_report *report = &document.report;
    document_begin(&document, "orderReport", 0);
    ta_report_text(report, S("orderChart"), S("checkout"));
    ta_report_number(report, S("stateCount"), 3);
    ta_report_boolean(report, S("valid"), 1);
    ta_report_object(report, S("image"));
    ta_report_text(report, S("imageHash"), S("ab12"));
    ta_report_number(report, S("size"), 512);
    ta_report_end(report);
    ta_report_list(report, list_names("states", "state"));
    ta_report_object(report, S("ignored"));
    ta_report_text(report, S("name"), S("idle"));
    ta_report_boolean(report, S("initial"), 1);
    ta_report_end(report);
    ta_report_object(report, S("ignored"));
    ta_report_text(report, S("name"), S("paid"));
    ta_report_boolean(report, S("final"), 0);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_list(report, list_names("tags", "tag"));
    ta_report_text(report, S("ignored"), S("a"));
    ta_report_text(report, S("ignored"), S("b"));
    ta_report_end(report);
    ta_report_list(report, list_names("transitions", "transition"));
    ta_report_object(report, S(""));
    ta_report_text(report, S("event"), S("pay"));
    ta_report_object(report, S("target"));
    ta_report_text(report, S("state"), S("paid"));
    ta_report_end(report);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_list(report, list_names("errors", "error"));
    ta_report_end(report);
    ta_report_code code = {S("mermaid"), S("graph\n```x```\n")};
    ta_report_block(report, S("diagram"), code);

    CHECK(document_is(&document, TA_REPORT_MARKDOWN,
                      EXPECT("# Order report\n"
                             "\n"
                             "- **Order chart**: checkout\n"
                             "- **State count**: 3\n"
                             "- **Valid**: yes\n"
                             "\n"
                             "## Image\n"
                             "\n"
                             "- **Image hash**: ab12\n"
                             "- **Size**: 512\n"
                             "\n"
                             "## States\n"
                             "\n"
                             "| Name | Initial | Final |\n"
                             "| --- | --- | --- |\n"
                             "| idle | yes |  |\n"
                             "| paid |  | no |\n"
                             "\n"
                             "## Tags\n"
                             "\n"
                             "- a\n"
                             "- b\n"
                             "\n"
                             "## Transitions\n"
                             "\n"
                             "### Transition 1: pay\n"
                             "\n"
                             "- **Event**: pay\n"
                             "\n"
                             "#### Target\n"
                             "\n"
                             "- **State**: paid\n"
                             "\n"
                             "## Errors\n"
                             "\n"
                             "_none_\n"
                             "\n"
                             "## Diagram\n"
                             "\n"
                             "````mermaid\n"
                             "graph\n"
                             "```x```\n"
                             "````\n")));
    CHECK(document_is(&document, TA_REPORT_JSON,
                      EXPECT("{\n"
                             "  \"orderChart\": \"checkout\",\n"
                             "  \"stateCount\": 3,\n"
                             "  \"valid\": true,\n"
                             "  \"image\": {\n"
                             "    \"imageHash\": \"ab12\",\n"
                             "    \"size\": 512\n"
                             "  },\n"
                             "  \"states\": [\n"
                             "    {\n"
                             "      \"name\": \"idle\",\n"
                             "      \"initial\": true\n"
                             "    },\n"
                             "    {\n"
                             "      \"name\": \"paid\",\n"
                             "      \"final\": false\n"
                             "    }\n"
                             "  ],\n"
                             "  \"tags\": [\n"
                             "    \"a\",\n"
                             "    \"b\"\n"
                             "  ],\n"
                             "  \"transitions\": [\n"
                             "    {\n"
                             "      \"event\": \"pay\",\n"
                             "      \"target\": {\n"
                             "        \"state\": \"paid\"\n"
                             "      }\n"
                             "    }\n"
                             "  ],\n"
                             "  \"errors\": [],\n"
                             "  \"diagram\": \"graph\\n```x```\\n\"\n"
                             "}\n")));
    CHECK(
        document_is(&document, TA_REPORT_XML,
                    EXPECT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<orderReport orderChart=\"checkout\" stateCount=\"3\" valid=\"true\">\n"
                           "  <image imageHash=\"ab12\" size=\"512\"/>\n"
                           "  <states>\n"
                           "    <state name=\"idle\" initial=\"true\"/>\n"
                           "    <state name=\"paid\" final=\"false\"/>\n"
                           "  </states>\n"
                           "  <tags>\n"
                           "    <tag>a</tag>\n"
                           "    <tag>b</tag>\n"
                           "  </tags>\n"
                           "  <transitions>\n"
                           "    <transition event=\"pay\">\n"
                           "      <target state=\"paid\"/>\n"
                           "    </transition>\n"
                           "  </transitions>\n"
                           "  <errors/>\n"
                           "  <diagram>graph\n"
                           "```x```\n"
                           "</diagram>\n"
                           "</orderReport>\n")));
    document_end(&document);
}

/* What each format escapes. */
static void test_escaping(void) {
    test_document document;
    ta_report *report = &document.report;
    document_begin(&document, "payment", 0);
    ta_string_view note = {"a\"b\\c<d>&e\n\t\r\x01\x1f\0z \xc3\xa9", 20};
    ta_report_text(report, S("note"), note);
    ta_report_list(report, list_names("lines", "line"));
    ta_report_object(report, S(""));
    ta_report_text(report, S("memo"), S("a|b\nc"));
    ta_report_end(report);
    ta_report_end(report);
    ta_report_list(report, list_names("notes", "note"));
    ta_report_text(report, S(""), note);

    CHECK(document_is(
        &document, TA_REPORT_JSON,
        EXPECT("{\n"
               "  \"note\": \"a\\\"b\\\\c<d>&e\\n\\t\\r\\u0001\\u001F\\u0000z \xc3\xa9\",\n"
               "  \"lines\": [\n"
               "    {\n"
               "      \"memo\": \"a|b\\nc\"\n"
               "    }\n"
               "  ],\n"
               "  \"notes\": [\n"
               "    \"a\\\"b\\\\c<d>&e\\n\\t\\r\\u0001\\u001F\\u0000z \xc3\xa9\"\n"
               "  ]\n"
               "}\n")));
    CHECK(document_is(
        &document, TA_REPORT_XML,
        EXPECT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<payment note=\"a&quot;b\\c&lt;d&gt;&amp;e&#xA;&#x9;&#xD;&#x1;&#x1F;z "
               "\xc3\xa9\">\n"
               "  <lines>\n"
               "    <line memo=\"a|b&#xA;c\"/>\n"
               "  </lines>\n"
               "  <notes>\n"
               "    <note>a&quot;b\\c&lt;d&gt;&amp;e\n\t\r&#x1;&#x1F;z \xc3\xa9</note>\n"
               "  </notes>\n"
               "</payment>\n")));
    CHECK(document_is(&document, TA_REPORT_MARKDOWN,
                      EXPECT("# Payment\n"
                             "\n"
                             "- **Note**: `a\"b\\c<d>&e\n\t\r\x01\x1f\0z \xc3\xa9`\n"
                             "\n"
                             "## Lines\n"
                             "\n"
                             "| Memo |\n"
                             "| --- |\n"
                             "| a\\|b c |\n"
                             "\n"
                             "## Notes\n"
                             "\n"
                             "- `a\"b\\c<d>&e\n\t\r\x01\x1f\0z \xc3\xa9`\n")));
    document_end(&document);
}

/* Nested objects take headings one level down, at most six; a list of
 * objects that are not flat gives each item a heading; a scalar in such a
 * list is a paragraph under its own. */
static void test_headings(void) {
    test_document document;
    ta_report *report = &document.report;
    document_begin(&document, "subscription", 0);
    ta_report_object(report, S("plan"));
    ta_report_object(report, S("billingCycle"));
    ta_report_object(report, S("trial"));
    ta_report_object(report, S("grace"));
    ta_report_object(report, S("retryPolicy"));
    ta_report_object(report, S("attempt2Delay"));
    ta_report_number(report, S("seconds"), 60);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_end(report);
    ta_report_list(report, list_names("invoices", "invoice"));
    ta_report_object(report, S(""));
    ta_report_number(report, S("total"), 10);
    ta_report_list(report, list_names("lines", "line"));
    ta_report_text(report, S(""), S("seat"));
    ta_report_end(report);
    ta_report_end(report);
    ta_report_number(report, S(""), 7);
    /* left open: the render closes it */

    CHECK(document_is(&document, TA_REPORT_MARKDOWN,
                      EXPECT("# Subscription\n"
                             "\n"
                             "## Plan\n"
                             "\n"
                             "### Billing cycle\n"
                             "\n"
                             "#### Trial\n"
                             "\n"
                             "##### Grace\n"
                             "\n"
                             "###### Retry policy\n"
                             "\n"
                             "###### Attempt2 delay\n"
                             "\n"
                             "- **Seconds**: 60\n"
                             "\n"
                             "## Invoices\n"
                             "\n"
                             "### Invoice 1\n"
                             "\n"
                             "- **Total**: 10\n"
                             "\n"
                             "#### Lines\n"
                             "\n"
                             "- seat\n"
                             "\n"
                             "### Invoice 2\n"
                             "\n"
                             "7\n")));
    CHECK(document_is(&document, TA_REPORT_XML,
                      EXPECT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                             "<subscription>\n"
                             "  <plan>\n"
                             "    <billingCycle>\n"
                             "      <trial>\n"
                             "        <grace>\n"
                             "          <retryPolicy>\n"
                             "            <attempt2Delay seconds=\"60\"/>\n"
                             "          </retryPolicy>\n"
                             "        </grace>\n"
                             "      </trial>\n"
                             "    </billingCycle>\n"
                             "  </plan>\n"
                             "  <invoices>\n"
                             "    <invoice total=\"10\">\n"
                             "      <lines>\n"
                             "        <line>seat</line>\n"
                             "      </lines>\n"
                             "    </invoice>\n"
                             "    <invoice>7</invoice>\n"
                             "  </invoices>\n"
                             "</subscription>\n")));
    document_end(&document);
}

/* An empty document, and an empty list at the root's level. */
static void test_empty(void) {
    test_document document;
    document_begin(&document, "ticket", 0);
    CHECK(document_is(&document, TA_REPORT_MARKDOWN, EXPECT("# Ticket\n")));
    CHECK(document_is(&document, TA_REPORT_JSON, EXPECT("{}\n")));
    CHECK(document_is(&document, TA_REPORT_XML,
                      EXPECT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ticket/>\n")));
    ta_report_list(&document.report, list_names("replies", "reply"));
    CHECK(document_is(&document, TA_REPORT_MARKDOWN, EXPECT("# Ticket\n\n## Replies\n\n_none_\n")));
    CHECK(document_is(&document, TA_REPORT_JSON, EXPECT("{\n  \"replies\": []\n}\n")));
    document_end(&document);
}

/* A block: three backticks need a fence of four; no language, no fence word;
 * a text without a last newline gets one. */
static void test_blocks(void) {
    test_document document;
    document_begin(&document, "checkout", 0);
    ta_report_code plain = {S(""), S("total = 3")};
    ta_report_block(&document.report, S("listing"), plain);
    ta_report_code ticks = {S("text"), S("a ```` b")};
    ta_report_block(&document.report, S("quoted"), ticks);
    CHECK(document_is(&document, TA_REPORT_MARKDOWN,
                      EXPECT("# Checkout\n"
                             "\n"
                             "## Listing\n"
                             "\n"
                             "```\n"
                             "total = 3\n"
                             "```\n"
                             "\n"
                             "## Quoted\n"
                             "\n"
                             "`````text\n"
                             "a ```` b\n"
                             "`````\n")));
    document_end(&document);
}

/* A copy lives in the arena, independent of the caller's text. */
static void test_copy(void) {
    test_document document;
    document_begin(&document, "order", 0);
    char text[] = "pending";
    ta_string_view copy = ta_report_copy(&document.report, ta_string(text));
    text[0] = 'X';
    ta_report_text(&document.report, S("status"), copy);
    CHECK(document_is(&document, TA_REPORT_JSON, EXPECT("{\n  \"status\": \"pending\"\n}\n")));
    CHECK_EQUAL(ta_report_copy(&document.report, S("")).length, 0);
    document_end(&document);
}

/* Misuse: too deep, or an end with only the root open. Nothing is written,
 * and later calls do nothing. */
static void test_state(void) {
    test_document document;
    document_begin(&document, "chat", 0);
    for (int i = 0; i < TA_REPORT_MAX_DEPTH - 1; i++)
        ta_report_object(&document.report, S("level"));
    CHECK_EQUAL(document.report.status, TA_OK);
    CHECK_EQUAL(document.report.depth, TA_REPORT_MAX_DEPTH);
    ta_report_list(&document.report, list_names("deeper", "item"));
    CHECK_EQUAL(document.report.status, TA_ERROR_STATE);
    ta_report_end(&document.report);
    CHECK_EQUAL(document.report.depth, TA_REPORT_MAX_DEPTH);
    CHECK_EQUAL(document_render(&document, TA_REPORT_JSON), TA_ERROR_STATE);
    CHECK_EQUAL(document.out.length, 0);
    document_end(&document);

    document_begin(&document, "chat", 0);
    ta_report_object(&document.report, S("session"));
    ta_report_end(&document.report);
    CHECK_EQUAL(document.report.status, TA_OK);
    ta_report_end(&document.report);
    CHECK_EQUAL(document.report.status, TA_ERROR_STATE);
    ta_report_text(&document.report, S("after"), S("nothing"));
    ta_report_object(&document.report, S("after"));
    CHECK_EQUAL(document.report.depth, 1);
    CHECK_EQUAL(document.report.status, TA_ERROR_STATE);
    CHECK_EQUAL(document_render(&document, TA_REPORT_MARKDOWN), TA_ERROR_STATE);
    CHECK_EQUAL(document.out.length, 0);
    document_end(&document);
}

/* An arena that refuses: TA_ERROR_NO_MEMORY, nothing written. */
static void test_no_memory(void) {
    test_document document;
    document_begin(&document, "refund", 1);
    CHECK_EQUAL(document.report.status, TA_ERROR_NO_MEMORY);
    ta_report_text(&document.report, S("reason"), S("late"));
    ta_report_object(&document.report, S("amount"));
    ta_report_end(&document.report);
    CHECK_EQUAL(ta_report_copy(&document.report, S("text")).length, 0);
    for (uint32_t format = 0; format <= TA_REPORT_XML; format++) {
        CHECK_EQUAL(document_render(&document, format), TA_ERROR_NO_MEMORY);
        CHECK_EQUAL(document.out.length, 0);
    }
    document_end(&document);

    /* refused after the root: the first node that needs a chunk */
    document_begin(&document, "refund", 0);
    document.heap.refuse = 1;
    for (int i = 0; i < 64 && document.report.status == TA_OK; i++)
        ta_report_text(&document.report, S("reason"), S("late"));
    CHECK_EQUAL(document.report.status, TA_ERROR_NO_MEMORY);
    document.heap.refuse = 0;
    CHECK_EQUAL(document_render(&document, TA_REPORT_XML), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(document.out.length, 0);
    document_end(&document);
}

/* In Markdown, a value with '<' outside a code span is written as code:
 * whole, or each such word when the value has code spans of its own. */
static void test_angle_brackets(void) {
    test_document document;
    document_begin(&document, "order", 0);
    ta_report_text(&document.report, S("state"), S("<scxml>"));
    ta_report_text(&document.report, S("condition"), S("cond `total < 10` in <scxml>"));
    ta_report_text(&document.report, S("guard"), S("attempts < 3"));
    ta_report_text(&document.report, S("plain"), S("total > 0"));
    CHECK(document_is(&document, TA_REPORT_MARKDOWN,
                      EXPECT("# Order\n\n- **State**: `<scxml>`\n- **Condition**: cond `total < 10` in `<scxml>`\n"
                             "- **Guard**: `attempts < 3`\n- **Plain**: total > 0\n")));
    document_end(&document);
}

/* Names as words in Markdown: an acronym stays in capitals. */
static void test_acronyms_in_names(void) {
    test_document document;
    document_begin(&document, "orderSummary", 0);
    ta_report_number(&document.report, S("sessionID"), 1);
    ta_report_number(&document.report, S("bootActors"), 2);
    ta_report_number(&document.report, S("paymentURLPath"), 3);
    ta_report_number(&document.report, S("abi"), 4);
    ta_report_number(&document.report, S("idPage"), 5);
    ta_report_number(&document.report, S("identity"), 6);
    CHECK(document_is(&document, TA_REPORT_MARKDOWN,
                      EXPECT("# Order summary\n\n- **Session ID**: 1\n- **Boot actors**: 2\n"
                             "- **Payment URL path**: 3\n- **ABI**: 4\n- **ID page**: 5\n- **Identity**: 6\n")));
    document_end(&document);
}

static void test_unknown_format(void) {
    test_document document;
    document_begin(&document, "order", 0);
    CHECK_EQUAL(document_render(&document, TA_REPORT_XML + 1), TA_ERROR_INVALID);
    CHECK_EQUAL(document.out.length, 0);
    document_end(&document);
}

int main(void) {
    RUN(test_every_kind);
    RUN(test_escaping);
    RUN(test_headings);
    RUN(test_empty);
    RUN(test_blocks);
    RUN(test_copy);
    RUN(test_state);
    RUN(test_no_memory);
    RUN(test_angle_brackets);
    RUN(test_acronyms_in_names);
    RUN(test_unknown_format);
    TEST_MAIN_END();
}
