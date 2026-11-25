/* The tinyactors command (tools/tinyactors, docs/cli.md), run as a process:
 * each command on charts, a manifest and an image, in each format, its exit
 * statuses, and what it says when the command line is wrong. */
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/image.h>
#include <tinyactors/runtime.h>
#include "../harness/harness.h"
#include "../support/file_util.h"
#include "../support/host_config.h"

#ifndef TA_TEST_PROGRAMS
#define TA_TEST_PROGRAMS "."
#endif
#ifndef TA_TEST_DIAGRAMS
#define TA_TEST_DIAGRAMS "tests/diagram"
#endif
#ifndef TA_TEST_REPOSITORY
#define TA_TEST_REPOSITORY ".."
#endif

#define NS "xmlns='http://www.w3.org/2005/07/scxml'"

/* Run `tinyactors <arguments>` in the scratch directory, its standard output
 * and error into the files `output` and `errors`: its exit status. */
static int run(const char *arguments) {
    char command[4096];
    snprintf(command, sizeof command, "cd '%s' && '%s/tinyactors' %s > output 2> errors",
             TA_TEST_TEMPORARY_DIRECTORY, TA_TEST_PROGRAMS, arguments);
    int status = system(command);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The text of a scratch file, NUL-terminated (static: one at a time). */
static const char *text_of(const char *name) {
    static char text[1 << 18];
    FILE *file = fopen(fixture_path(name), "rb");
    size_t length = file ? fread(text, 1, sizeof text - 1, file) : 0;
    if (file) fclose(file);
    text[length] = 0;
    return text;
}

static int has(const char *name, const char *needle) {
    int found = strstr(text_of(name), needle) != NULL;
    if (!found) fprintf(stderr, "  no \"%s\" in %s:\n%s\n", needle, name, text_of(name));
    return found;
}

static void write_charts(void) {
    CHECK(fixture_write("order.scxml",
                        "<scxml " NS " datamodel='ecmascript' name='order' initial='open'>\n"
                        "<datamodel><data id='total' expr='0'/></datamodel>\n"
                        "<state id='open'>\n"
                        "<transition event='item.added' cond='_event.data.price &gt; 0'>"
                        "<assign location='total' expr='total + _event.data.price'/></transition>\n"
                        "<transition event='checkout' target='paying'/></state>\n"
                        "<state id='paying'><invoke id='pay' src='payment.scxml'/>\n"
                        "<transition event='done.invoke.pay' target='paid'/></state>\n"
                        "<final id='paid'/></scxml>\n"));
    CHECK(fixture_write("payment.scxml", "<scxml " NS " name='payment'><final id='settled'/></scxml>"));
    CHECK(fixture_write("system.xml", "<!-- the shop -->\n<system><chart name='order' src='order.scxml'/>"
                                      "<actor name='first' chart='order'/></system>"));
}

static void test_compile_and_inspect(void) {
    write_charts();
    CHECK_EQUAL(run("compile order.scxml -o order.tai"), 0);
    CHECK(has("output", "- **Compiled**: yes"));
    ta_bytes bytes;
    CHECK_EQUAL(ta_runtime_load_file(fixture_path("order.tai"), &bytes), TA_OK);
    ta_image image;
    CHECK_EQUAL(ta_image_open(bytes, &image), TA_OK);
    CHECK_EQUAL(ta_image_chart_by_name(&image, ta_string("payment")), 1u); /* through its src */
    ta_runtime_free_file(bytes);
    /* the same answer from the image and from the charts it was compiled from */
    CHECK_EQUAL(run("inspect --format json order.tai"), 0);
    CHECK(has("output", "\"from\": \"image\""));
    CHECK(has("output", "\"name\": \"payment\""));
    CHECK_EQUAL(run("inspect -f json order.scxml"), 0);
    CHECK(has("output", "\"from\": \"charts\""));
    CHECK_EQUAL(run("-f xml inspect order.tai"), 0); /* a program option before the command */
    CHECK(has("output", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<inspect "));
    /* standard input */
    CHECK_EQUAL(run("inspect - < order.tai"), 0);
    CHECK(has("output", "| 1 | payment | payment.scxml |"));
}

static void test_a_system(void) {
    write_charts();
    CHECK_EQUAL(run("compile system.xml -o system.tai"), 0);
    CHECK_EQUAL(run("inspect system.tai"), 0);
    CHECK(has("output", "| first | order |"));
    CHECK_EQUAL(run("inspect -f json system.xml"), 0);
    CHECK(has("output", "\"from\": \"system manifest\""));
}

static void test_the_image_as_c_and_to_standard_output(void) {
    write_charts();
    CHECK_EQUAL(run("compile order.scxml --emit c --symbol order_image -o order.c"), 0);
    CHECK(has("order.c", "_Alignas(8) const unsigned char order_image[] = {"));
    CHECK(has("order.c", "0x54, 0x41, 0x49, 0x31")); /* "TAI1" */
    CHECK_EQUAL(run("compile order.scxml --emit c --symbol 9lives -o order.c"), 2);
    /* the image to standard output, the answer to standard error */
    CHECK_EQUAL(run("compile order.scxml -o -"), 0);
    CHECK(strncmp(text_of("output"), "TAI1", 4) == 0);
    CHECK(has("errors", "# Compile"));
}

static void test_the_charts_records(void) {
    write_charts();
    CHECK_EQUAL(run("states order.scxml --chart order"), 0);
    CHECK(has("output", "| 2 | paying | atomic | /scxml | 1 |  | 1 | 1 | 0 | no | no |  |"));
    CHECK(!strstr(text_of("output"), "settled")); /* --chart order: not payment's */
    CHECK_EQUAL(run("transitions -f json order.scxml"), 0);
    CHECK(has("output", "\"condition\": \"_event.data.price > 0\""));
    CHECK(has("output", "\"targets\": \"paid\""));
    CHECK_EQUAL(run("events order.scxml"), 0);
    CHECK(has("output", "| checkout | 1 | 0 | 0 |"));
    CHECK(!strstr(text_of("output"), "| item | 0 |"));/* a prefix of item.added: none uses it */
    CHECK(!strstr(text_of("output"), "done.state."));/* no transition handles a state's done event */
    CHECK_EQUAL(run("data order.scxml"), 0);
    CHECK(has("output", "| total | /scxml | 0 |"));
    CHECK_EQUAL(run("expressions --kind condition -f json order.scxml"), 0);
    CHECK(has("output", "\"kind\": \"condition\""));
    CHECK(!strstr(text_of("output"), "\"kind\": \"value\""));
    CHECK_EQUAL(run("states --chart shipment order.scxml"), 2);
    CHECK(has("errors", "no chart 'shipment' in the image; it has order, payment"));
}

static void test_refusals(void) {
    CHECK(fixture_write("broken.scxml", "<scxml " NS "><state id='a'>\n<transtion target='a'/></state></scxml>"));
    CHECK_EQUAL(run("check broken.scxml"), 1);
    CHECK(has("output", "- **Valid**: no"));
    CHECK(has("output", "| broken.scxml | 2 | 1 | error | `unknown element <transtion> in <state>` |"));
    CHECK_EQUAL(run("compile -f json broken.scxml -o broken.tai"), 1);
    CHECK(has("output", "\"compiled\": false"));
    struct stat status;
    CHECK(stat(fixture_path("broken.tai"), &status) != 0); /* nothing written */
    CHECK_EQUAL(run("states broken.scxml"), 1); /* every command reports why */
    CHECK(has("output", "`unknown element <transtion>"));
    /* an image that does not verify */
    CHECK(fixture_write("damaged.tai", "TAI1 and then not an image at all"));
    CHECK_EQUAL(run("check damaged.tai"), 1);
    CHECK(has("output", "the image does not verify"));
    CHECK(fixture_write("notes.txt", "a shopping list"));
    CHECK_EQUAL(run("inspect notes.txt"), 1);
    CHECK(has("errors", "notes.txt is neither an image (.tai), a snapshot, an SCXML chart nor a system manifest"));
    CHECK_EQUAL(run("inspect missing.tai"), 3);
    CHECK(has("errors", "tinyactors: cannot read missing.tai"));
    /* a file that cannot be written leaves nothing behind */
    write_charts();
    CHECK_EQUAL(run("compile order.scxml -o no-such-directory/order.tai"), 3);
    CHECK(has("errors", "cannot write no-such-directory/order.tai"));
}

static void test_the_command_line(void) {
    CHECK_EQUAL(run("inspcet order.tai"), 2);
    CHECK(has("errors", "tinyactors: 'inspcet' is not a tinyactors command; did you mean 'inspect'?"));
    CHECK(has("errors", "see 'tinyactors help'"));
    CHECK_EQUAL(run("inspect --frmat json order.tai"), 2);
    CHECK(has("errors", "unknown option '--frmat'; did you mean '--format'?"));
    CHECK(has("errors", "see 'tinyactors help inspect'"));
    CHECK_EQUAL(run("inspect -f yaml order.tai"), 2);
    CHECK(has("errors", "'yaml' is not a value of --format (one of: markdown, json, xml, svg)"));
    CHECK_EQUAL(run("compile order.scxml"), 2);
    CHECK(has("errors", "option --output is required"));
    CHECK_EQUAL(run("diff order.tai"), 2);
    CHECK(has("errors", "'diff' needs OLD NEW"));
    CHECK_EQUAL(run(""), 2); /* no command: what there is, and a usage error */
    CHECK(has("output", "| compile | compile charts or a system manifest into an image |"));
}

static void test_help_and_version(void) {
    CHECK_EQUAL(run("help compile"), 0);
    CHECK(has("output", "| -o, --output FILE | where the image goes (- for standard output) |"));
    CHECK(has("output", "tinyactors compile order.scxml payment.scxml -o order.tai"));
    CHECK_EQUAL(run("compile --help"), 0); /* --help answers: no -o needed */
    CHECK(has("output", "- **Usage**: tinyactors compile [options] INPUT..."));
    CHECK_EQUAL(run("help -f json"), 0);
    CHECK(has("output", "\"command\": \"disassemble\""));
    CHECK_EQUAL(run("help frobnicate"), 2);
    CHECK_EQUAL(run("version -f json"), 0);
    CHECK(has("output", "\"abi\": 1"));
    CHECK_EQUAL(run("--version"), 0);
    CHECK(has("output", "- **Program**: tinyactors"));
}

/* An order with its executable content: a foreach, a condition, sends. */
#define FULFILLMENT                                                                                \
    "<scxml " NS " datamodel='ecmascript' name='fulfillment' initial='packing'>\n"             \
    "<datamodel><data id='items' expr='[]'/><data id='count' expr='0'/></datamodel>\n"             \
    "<parallel id='packing'><state id='boxes'><onentry>"                                       \
    "<foreach array='items' item='item'><assign location='count' expr='count + 1'/></foreach>"     \
    "<if cond='count &gt; 10'><send event='parcel.heavy' delay='30s'/>"                        \
    "<else/><log label='parcel' expr='count'/></if></onentry>"                                 \
    "<transition event='packed' target='shipped'/></state>"                                    \
    "<state id='labels'><transition event='printed'/></state></parallel>\n"                   \
    "<final id='shipped'/></scxml>\n"

static void test_disassemble_and_graph(void) {
    CHECK(fixture_write("fulfillment.scxml", FULFILLMENT));
    CHECK_EQUAL(run("disassemble fulfillment.scxml"), 0);
    CHECK(has("output", "| onentry boxes | block |"));
    CHECK(has("output", "| foreach | `items` as `item` until "));
    CHECK(has("output", "| assign | `count` = `count + 1` |"));
    CHECK(has("output", "| jump-if-false | unless `count > 10` to "));
    CHECK(has("output", "| send | event parcel.heavy to itself after 30s |"));
    CHECK(has("output", "| log | label parcel `count` |"));
    CHECK_EQUAL(run("graph -f json fulfillment.scxml"), 0);
    CHECK(has("output", "\"source\": \"boxes\""));
    CHECK(has("output", "\"target\": \"shipped\""));
}

/* A support ticket: a compound state with a deep history, a parallel state of two regions, a
 * self loop, a transition with no target and one listed in its box instead of routed. */
static const char ticket[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' name='ticket' initial='open'>\n"
    "  <datamodel><data id='n' expr='0'/></datamodel>\n"
    "  <state id='open' initial='triage'><onentry><log expr='1'/></onentry><transition event='ping'/>\n"
    "    <state id='triage'>\n"
    "      <transition event='assign' target='working'/>\n"
    "      <transition event='note' cond='n &lt; 3'><assign location='n' expr='n + 1'/></transition>\n"
    "    </state>\n"
    "    <state id='working'><transition event='escalate' target='waiting'/></state>\n"
    "    <history id='hist' type='deep'><transition target='triage'/></history>\n"
    "    <transition event='close' target='closed'/>\n"
    "  </state>\n"
    "  <parallel id='escalated'>\n"
    "    <state id='review' initial='waiting'>\n"
    "      <state id='waiting'><transition event='reviewed' target='approved'/></state>\n"
    "      <final id='approved'/>\n"
    "    </state>\n"
    "    <state id='sla'><state id='ticking'><transition event='tick' target='ticking'/></state></state>\n"
    "    <transition event='back' target='hist'/>\n"
    "  </parallel>\n"
    "  <final id='closed'/>\n"
    "</scxml>\n";

/* graph's answers: the layout as data (every chart of the corpus, test_layout_corpus, checks
 * its boxes and routes) and as a picture. */
static void test_graph_layout(void) {
    CHECK(fixture_write("support-ticket.scxml", ticket));
    CHECK_EQUAL(run("graph -f json support-ticket.scxml"), 0);
    CHECK(has("output", "\"drawnAs\": \"region\""));
    CHECK(has("output", "\"route\": \"145,208 161,208 161,234 145,234\"")); /* the self loop */
    /* in their boxes: the targetless ones, and escalate, which crosses three borders */
    CHECK_EQUAL(run("graph support-ticket.scxml"), 0);
    CHECK(has("output", "| open | ping |  |  |"));
    CHECK(has("output", "| triage | note | `n < 3` |  |"));
    CHECK(has("output", "| working | escalate |  | waiting |"));

    /* the picture: well-formed, escaped, one <g> per chart, its routes as paths */
    CHECK_EQUAL(run("graph -f svg support-ticket.scxml"), 0);
    CHECK(has("output", "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"tinyactors-diagram\" width=\"1126.2\""));
    /* close leaves open's side level with closed's middle: one straight line, onto its pill */
    CHECK(has("output", "<path class=\"ta-edge\" d=\"M946.6,197 L1030.6,197\" marker-end=\"url(#tinyactors-arrow)\"/>"));
    CHECK(has("output", "<tspan class=\"ta-condition\">[n &lt; 3]</tspan>"));
    /* inline in a page, the drawing takes none of its styles and gives it none: every class and
     * id is prefixed, every rule under the root's class */
    const char *svg = text_of("output");
    int prefixed = 1;
    for (const char *at = strstr(svg, "class=\""); at; at = strstr(at + 1, "class=\"")) {
        const char *value = at + 7;
        while (*value && *value != '"') { /* each space-separated class */
            prefixed &= strncmp(value, "ta-", 3) == 0 || strncmp(value, "tinyactors-diagram", 18) == 0;
            while (*value && *value != ' ' && *value != '"')
                value++;
            while (*value == ' ')
                value++;
        }
    }
    for (const char *at = strstr(svg, " id=\""); at; at = strstr(at + 1, " id=\""))
        prefixed &= strncmp(at + 5, "tinyactors-", 11) == 0;
    CHECK(prefixed);
    const char *style_end = strstr(svg, "</style>");
    for (const char *line = strchr(strstr(svg, "<style>"), '\n'); line && line < style_end; line = strchr(line, '\n')) {
        line++;
        if (line < style_end && *line != ' ') CHECK(strncmp(line, ".tinyactors-diagram ", 20) == 0 || strncmp(line, "@media", 6) == 0);
    }
    CHECK(has("output", "H*"));
    CHECK(has("output", "@media (prefers-color-scheme: dark)"));
    CHECK(!strstr(text_of("output"), "mermaid"));
    /* a picture is graph's answer only */
    CHECK_EQUAL(run("states -f svg support-ticket.scxml"), 2);
    CHECK(has("errors", "--format svg is for graph"));
}

/* A whole file, NUL-terminated, in memory the caller frees; NULL when it cannot be read. */
static char *whole_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = length >= 0 ? malloc((size_t)length + 1) : NULL;
    size_t read = text ? fread(text, 1, (size_t)length, file) : 0;
    fclose(file);
    if (text) text[read] = 0;
    return text;
}

/* The layout's corpus (tests/diagram, README.md there): each chart's graph is the answer its
 * .md file holds, box for box and route for route. `mise run diagram:expect` writes the
 * answers again after a deliberate change to the layout. */
static void test_layout_corpus(void) {
    DIR *directory = opendir(TA_TEST_DIAGRAMS);
    CHECK(directory != NULL);
    if (!directory) return;
    int charts = 0;
    for (struct dirent *entry; (entry = readdir(directory));) {
        size_t length = strlen(entry->d_name);
        if (length < 6 || strcmp(entry->d_name + length - 6, ".scxml") != 0) continue;
        char command[2048], expected[2048];
        snprintf(command, sizeof command, "graph '%s/%s'", TA_TEST_DIAGRAMS, entry->d_name);
        snprintf(expected, sizeof expected, "%s/%.*s.md", TA_TEST_DIAGRAMS, (int)(length - 6), entry->d_name);
        CHECK_EQUAL(run(command), 0);
        char *answer = whole_file(fixture_path("output")), *wanted = whole_file(expected);
        int same = answer && wanted && strcmp(answer, wanted) == 0;
        CHECK(same);
        if (!same) fprintf(stderr, "  %s: the layout is not %s (mise run diagram:expect after a deliberate change)\n",
                           entry->d_name, expected);
        /* what may never be drawn, whatever the corpus records: an arrow off its box, a bent
         * segment, a box outside its parent or over a sibling (the quality problems, an edge off
         * a side's middle or a label over a line, are listed in the .md, to be worked down) */
        static const char *const never[] = {"| startDetached |", "| endDetached |", "| initialDetached |",
                                            "| notOrthogonal |", "| outsideParent |", "| boxesOverlap |"};
        for (size_t i = 0; answer && i < sizeof never / sizeof never[0]; i++) {
            int clean = strstr(answer, never[i]) == NULL;
            CHECK(clean);
            if (!clean) fprintf(stderr, "  %s: %s\n", entry->d_name, never[i]);
        }
        free(answer);
        free(wanted);
        charts++;
    }
    closedir(directory);
    CHECK(charts >= 10);
}

/* A version of order.scxml (write_charts) with `open`'s transitions, the
 * paying state and the data replaced as given. */
typedef struct order_version {
    const char *data, *open, *paying;
} order_version;

static void write_order(const char *name, order_version version) {
    char text[2048];
    snprintf(text, sizeof text,
             "<scxml " NS " datamodel='ecmascript' name='order' initial='open'>\n"
             "<datamodel>%s</datamodel>\n<state id='open'>%s</state>\n%s\n<final id='paid'/></scxml>\n",
             version.data, version.open, version.paying);
    CHECK(fixture_write(name, text));
}

#define ORDER_DATA "<data id='total' expr='0'/>"
#define ORDER_OPEN                                                                                 \
    "<transition event='item.added' cond='_event.data.price &gt; 0'>"                         \
    "<assign location='total' expr='total + _event.data.price'/></transition>\n"               \
    "<transition event='checkout' target='paying'/>"
#define ORDER_PAYING                                                                               \
    "<state id='paying'><invoke id='pay' src='payment.scxml'/>\n"                              \
    "<transition event='done.invoke.pay' target='paid'/></state>"

static void test_diff(void) {
    write_charts();
    CHECK_EQUAL(run("compile order.scxml -o order.tai"), 0);
    /* identical versions: nothing listed, every list there */
    CHECK_EQUAL(run("diff --exit-code order.tai order.scxml"), 0);
    CHECK(has("output", "- **Identical**: yes"));
    CHECK(has("output", "- **Differences**: 0"));
    CHECK(!strstr(text_of("output"), "**Migration**"));
    CHECK_EQUAL(run("diff -f json order.tai order.scxml"), 0);
    CHECK(has("output", "\"charts\": []"));
    CHECK(has("output", "\"eventsAdded\": []"));
    CHECK(has("output", "\"eventsRemoved\": []"));
    /* --all: the unchanged charts too, with empty lists */
    CHECK_EQUAL(run("diff -f json --all order.tai order.scxml"), 0);
    CHECK(has("output", "\"name\": \"payment\",\n      \"change\": \"unchanged\""));
    CHECK(has("output", "\"chartsUnchanged\": 2"));

    /* paying renamed settling: actors in paying lose their state */
    write_order("renamed.scxml", (order_version){ORDER_DATA, "<transition event='checkout' target='settling'/>",
                "<state id='settling'><invoke id='pay' src='payment.scxml'/>\n"
                "<transition event='done.invoke.pay' target='paid'/></state>"});
    CHECK_EQUAL(run("diff --breaking --exit-code order.tai renamed.scxml"), 1);
    CHECK(has("output", "| paying | removed | yes | atomic in /scxml |  |"));
    CHECK(!strstr(text_of("output"), "| settling | added |")); /* --breaking: not breaking */
    CHECK(has("output", "Actors in removed states are completed to a legal configuration"));
    CHECK(has("output", "no onentry runs: paying \xe2\x86\x92 open."));
    CHECK_EQUAL(run("diff order.tai renamed.scxml"), 0);
    CHECK(has("output", "| settling | added | no |  | atomic in /scxml |"));
    CHECK(has("output", "| open | checkout | targets changed | no | \xe2\x86\x92 paying (external) | "
                        "\xe2\x86\x92 settling (external) |"));

    /* open no longer handles checkout: actors waiting there ignore it */
    write_order("submit.scxml", (order_version){ORDER_DATA,
                "<transition event='item.added'><assign location='total' expr='total + _event.data.price'/>"
                "</transition>\n<transition event='order.submit' target='paying'/>",
                ORDER_PAYING});
    CHECK_EQUAL(run("diff --breaking --exit-code order.tai submit.scxml"), 1);
    CHECK(has("output", "| open | checkout | no longer handled |"));
    CHECK(has("output", "Actors in open no longer handle checkout"));
    CHECK_EQUAL(run("diff -f json order.tai submit.scxml"), 0);
    CHECK(has("output", "\"event\": \"order.submit\",\n          \"change\": \"newly handled\""));
    /* a name, not the prefix "order" its node hangs from */
    CHECK(has("output", "\"eventsAdded\": [\n    \"order.submit\"\n  ]"));
    CHECK(has("output", "\"eventsRemoved\": [\n    \"checkout\"\n  ]"));

    /* data added: not breaking; data removed: breaking */
    write_order("currency.scxml",
                (order_version){ORDER_DATA "<data id='currency' expr=\"'EUR'\"/>", ORDER_OPEN, ORDER_PAYING});
    CHECK_EQUAL(run("diff --breaking --exit-code order.tai currency.scxml"), 0);
    CHECK_EQUAL(run("diff --exit-code order.tai currency.scxml"), 1);
    CHECK(has("output", "| currency | added | no |  | `'EUR'` in /scxml |"));
    CHECK(!strstr(text_of("output"), "**Migration**"));
    CHECK_EQUAL(run("diff --breaking --exit-code currency.scxml order.tai"), 1);
    CHECK(has("output", "| currency | removed | yes | `'EUR'` in /scxml |  |"));
    CHECK(has("output", "Running actors lose currency"));

    /* a new condition and new actions: behaviour, never breaking */
    write_order("quantity.scxml", (order_version){ORDER_DATA,
                "<transition event='item.added' cond='_event.data.price &gt; 0 &amp;&amp; _event.data.quantity &gt; 0'>"
                "<assign location='total' expr='total + _event.data.price * _event.data.quantity'/>"
                "<log label='order' expr='total'/></transition>\n"
                "<transition event='checkout' target='paying'/>",
                ORDER_PAYING});
    CHECK_EQUAL(run("diff --breaking --exit-code order.tai quantity.scxml"), 0);
    CHECK(has("output", "- **Breaking**: 0"));
    CHECK_EQUAL(run("diff --exit-code order.tai quantity.scxml"), 1);
    CHECK(has("output", "| open | item.added | condition changed, actions changed | no | "
                        "cond `_event.data.price > 0`, no target | "
                        "cond `_event.data.price > 0 && _event.data.quantity > 0`, no target |"));
    CHECK(has("output", "| transition open on item.added | changed | assign `total` = `total + _event.data.price` | "
                        "assign `total` = `total + _event.data.price * _event.data.quantity`; log label order `total` |"));
    /* the same code elsewhere in the chart is the same code */
    write_order("moved-code.scxml", (order_version){ORDER_DATA,
                "<transition event='checkout' target='paying'/>\n"
                "<transition event='item.added' cond='_event.data.price &gt; 0'>"
                "<assign location='total' expr='total + _event.data.price'/></transition>",
                ORDER_PAYING});
    CHECK_EQUAL(run("diff --exit-code order.tai moved-code.scxml"), 0);

    /* a support ticket whose resolved state moves under a new closing state */
    CHECK(fixture_write("ticket.scxml",
                        "<scxml " NS " datamodel='ecmascript' name='ticket' initial='open'>\n"
                        "<state id='open'><transition event='resolve' target='resolved'/></state>\n"
                        "<state id='resolved'><transition event='reopen' target='open'/></state></scxml>\n"));
    CHECK(fixture_write("ticket-next.scxml",
                        "<scxml " NS " datamodel='ecmascript' name='ticket' initial='open'>\n"
                        "<state id='open'><transition event='resolve' target='resolved'/></state>\n"
                        "<state id='closing'><state id='resolved'><transition event='reopen' target='open'/>"
                        "</state></state></scxml>\n"));
    CHECK_EQUAL(run("diff --breaking --exit-code -f json ticket.scxml ticket-next.scxml"), 1);
    CHECK(has("output", "\"state\": \"resolved\",\n          \"change\": \"moved\",\n"
                        "          \"breaking\": true,\n          \"before\": \"atomic in /scxml\",\n"
                        "          \"after\": \"atomic in closing\""));
    /* the stable shape: every list present, empty when nothing is in it */
    CHECK(has("output", "\"transitions\": []"));
    CHECK(has("output", "\"actions\": []"));
    CHECK(has("output", "\"invokes\": []"));
    CHECK(has("output", "\"summary\": {"));
    CHECK(has("output", "\"migration\": \"Chart ticket: Actors in resolved (now in closing)"));

    CHECK_EQUAL(run("diff --chart shipment order.tai renamed.scxml"), 2);
    CHECK(has("errors", "no chart 'shipment' in either image"));
}

/* A refund: a descriptor shadowed by an earlier one, a rejected refund
 * nothing happens to any more, an escalation nothing enters, and a
 * parallel approval that pays and notifies. */
static const char refund[] =
    "<scxml " NS " datamodel='ecmascript' name='refund' initial='requested'>\n"
    "<datamodel><data id='amount' expr='0'/></datamodel>\n"
    "<state id='requested'>\n"
    "<transition event='review' target='reviewing'/>\n"
    "<transition event='review.urgent' target='approved'/></state>\n"
    "<state id='reviewing'>\n"
    "<transition event='approve' cond='amount &lt; 500' target='approved'/>\n"
    "<transition event='reject' target='rejected'/></state>\n"
    "<parallel id='approved'>\n"
    "<state id='paying'><state id='transferring'><transition event='transferred' target='transferred'/></state>"
    "<final id='transferred'/></state>\n"
    "<state id='notifying'><state id='emailing'><transition event='emailed' target='emailed'/></state>"
    "<final id='emailed'/></state>\n"
    "<transition event='done.state.approved' target='refunded'/></parallel>\n"
    "<state id='rejected'/>\n<state id='escalated'/>\n<final id='refunded'/></scxml>\n";

static void test_lint_and_path(void) {
    CHECK(fixture_write("refund.scxml", refund));
    CHECK_EQUAL(run("lint refund.scxml"), 0);
    CHECK(has("output", "# Lint"));
    CHECK(has("output", "- **Warnings**: 3"));
    CHECK(has("output", "| unreachable | warning | refund | escalated |  |  | 0 |"));
    CHECK(has("output", "| dead-end | warning | refund | rejected |  |  | 0 |"));
    CHECK(has("output", "| shadowed | warning | refund | requested | review.urgent \xe2\x86\x92 approved |  | 0 | "
                        "never taken: review \xe2\x86\x92 reviewing has no condition"));
    CHECK_EQUAL(run("lint -f json refund.scxml"), 0);
    CHECK(has("output", "\"rule\": \"dead-end\",\n      \"severity\": \"warning\",\n      \"chart\": \"refund\",\n"
                        "      \"state\": \"rejected\""));
    /* --deny fails on a rule's findings; --allow silences them */
    CHECK_EQUAL(run("lint --deny dead-end refund.scxml"), 1);
    CHECK(has("output", "- **Denied**: 1"));
    CHECK_EQUAL(run("lint --deny all --allow dead-end --allow shadowed --allow unreachable refund.scxml"), 0);
    CHECK(has("output", "- **Warnings**: 0"));
    CHECK_EQUAL(run("lint -d never-ends refund.scxml"), 0);
    CHECK_EQUAL(run("lint --deny door refund.scxml"), 2);
    CHECK(has("errors", "'door' is not a rule; the rules are all, unreachable, dead-end"));

    /* the shortest path: through the parallel approval, one region moving */
    CHECK_EQUAL(run("path refund.scxml --to emailed"), 0);
    CHECK(has("output", "# Path"));
    CHECK(has("output", "- **Result**: found"));
    CHECK(has("output", "- **Start**: requested"));
    CHECK(has("output", "| 2 | approve | no | `amount < 500` | reviewing | approved | transferring emailing |"));
    CHECK(has("output", "| 3 | emailed | no |  | emailing | emailed | transferring emailed |"));
    CHECK_EQUAL(run("path -f json refund.scxml --to emailed"), 0);
    CHECK(has("output", "\"transitions\": 3"));
    CHECK(has("output", "\"events\": \"review\",\n      \"eventless\": false"));
    /* nothing leaves a rejected refund */
    CHECK_EQUAL(run("path refund.scxml --from rejected --to refunded"), 1);
    CHECK(has("output", "- **Result**: unreachable"));
    CHECK(has("output", "_none_"));
    CHECK_EQUAL(run("path refund.scxml --from rejected --to refunded --unreachable"), 0);
    CHECK_EQUAL(run("path refund.scxml --to refunded --unreachable"), 1);
    CHECK_EQUAL(run("path refund.scxml --to settled"), 2);
    CHECK(has("errors", "chart 'refund' has no state 'settled'"));
    CHECK_EQUAL(run("path refund.scxml"), 2);
}

/* A refund request written by hand: attributes over two lines, elements on one line and
 * indented every which way, comments, an XML declaration, a script in CDATA, data whose
 * content is a value, references in attribute values. */
static const char refund_messy[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- a refund, from request to payout -->\n"
    "<scxml xmlns=\"http://www.w3.org/2005/07/scxml\" version=\"1.0\"\n"
    "       datamodel=\"ecmascript\" name=\"refund\" initial=\"requested\">\n"
    "<datamodel>   <data id=\"amount\" expr=\"0\"/>\n"
    "  <data id=\"reasons\">[ \"damaged\",\n     \"late\" ]</data>\n"
    "</datamodel>\n"
    "\n"
    "        <state id=\"requested\">  <!-- waiting for a decision -->\n"
    "  <transition event=\"approve\" cond=\"_event.data.amount &gt; 0 &amp;&amp; amount == 0\" target=\"paying\">"
    "<assign location=\"amount\" expr=\"_event.data.amount\"/>\n"
    "     <log label='refund' expr=\"'approved: ' + amount\"/></transition>\n"
    "<transition event=\"reject\" target=\"closed\"></transition>\n"
    "</state>\n"
    "<state id=\"paying\"><onentry><script><![CDATA[\n"
    "  if (amount > 500) { amount = 500; }\n"
    "]]></script></onentry>\n"
    "<transition event=\"paid\" target=\"closed\"/></state>\n"
    "<final id=\"closed\"/>\n"
    "</scxml>\n"
    "<!-- end of the refund -->\n";

static const char refund_canonical[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!-- a refund, from request to payout -->\n"
    "<scxml xmlns=\"http://www.w3.org/2005/07/scxml\" version=\"1.0\" datamodel=\"ecmascript\" name=\"refund\" "
    "initial=\"requested\">\n"
    "  <datamodel>\n"
    "    <data id=\"amount\" expr=\"0\"/>\n"
    "    <data id=\"reasons\">[ \"damaged\",\n     \"late\" ]</data>\n"
    "  </datamodel>\n"
    "  <state id=\"requested\">\n"
    "    <!-- waiting for a decision -->\n"
    "    <transition event=\"approve\" cond=\"_event.data.amount &gt; 0 &amp;&amp; amount == 0\" target=\"paying\">\n"
    "      <assign location=\"amount\" expr=\"_event.data.amount\"/>\n"
    "      <log label=\"refund\" expr=\"'approved: ' + amount\"/>\n"
    "    </transition>\n"
    "    <transition event=\"reject\" target=\"closed\"/>\n"
    "  </state>\n"
    "  <state id=\"paying\">\n"
    "    <onentry>\n"
    "      <script><![CDATA[\n"
    "  if (amount > 500) { amount = 500; }\n"
    "]]></script>\n"
    "    </onentry>\n"
    "    <transition event=\"paid\" target=\"closed\"/>\n"
    "  </state>\n"
    "  <final id=\"closed\"/>\n"
    "</scxml>\n"
    "<!-- end of the refund -->\n";

/* fmt: the canonical text of a messy chart, --check, -w, the image kept, refusals. */
static void test_canonical(void) {
    CHECK(fixture_write("refund.scxml", refund_messy));
    CHECK_EQUAL(run("fmt refund.scxml"), 0);
    CHECK(strcmp(text_of("output"), refund_canonical) == 0);
    if (strcmp(text_of("output"), refund_canonical) != 0) fprintf(stderr, "  got:\n%s\n", text_of("output"));
    /* the same image, lines and columns aside: the hash with the positions stripped */
    CHECK_EQUAL(run("fmt --check -f json refund.scxml"), 1);
    CHECK(has("output", "\"file\": \"refund.scxml\",\n      \"changed\": true"));
    CHECK(has("output", "\"image\": \"same\""));
    CHECK(has("output", "\"changed\": 1"));
    /* -w rewrites it whole; formatting it again changes nothing */
    CHECK_EQUAL(run("fmt -w refund.scxml"), 0);
    CHECK(strcmp(text_of("refund.scxml"), refund_canonical) == 0);
    CHECK(has("output", "| refund.scxml | yes | no | same |"));
    CHECK_EQUAL(run("fmt --check refund.scxml"), 0);
    CHECK(has("output", "| refund.scxml | no | no | same |"));
    CHECK_EQUAL(run("fmt -w refund.scxml"), 0);
    CHECK(strcmp(text_of("refund.scxml"), refund_canonical) == 0);
    /* the formatted chart behaves as the original: the same states, transitions, expressions */
    CHECK(fixture_write("refund-messy.scxml", refund_messy));
    CHECK_EQUAL(run("expressions -f json refund.scxml"), 0);
    CHECK(has("output", "\"source\": \"_event.data.amount > 0 && amount == 0\""));

    /* a manifest, and several files: --check names those not canonical, -w rewrites them */
    write_charts();
    CHECK_EQUAL(run("fmt --check system.xml order.scxml refund.scxml"), 1);
    CHECK(has("output", "| system.xml | yes | no | same |"));
    CHECK(has("output", "| order.scxml | yes | no | same |"));
    CHECK(has("output", "| refund.scxml | no | no | same |"));
    CHECK_EQUAL(run("fmt -w system.xml order.scxml"), 0);
    CHECK(strcmp(text_of("system.xml"), "<!-- the shop -->\n<system>\n  <chart name=\"order\" src=\"order.scxml\"/>\n"
                                        "  <actor name=\"first\" chart=\"order\"/>\n</system>\n") == 0);
    CHECK_EQUAL(run("fmt --check system.xml order.scxml refund.scxml"), 0);

    /* refusals: several files to standard output, -w with --check, what is no chart, what XML forbids */
    CHECK_EQUAL(run("fmt order.scxml refund.scxml"), 2);
    CHECK(has("errors", "fmt writes one file to standard output"));
    CHECK_EQUAL(run("fmt -w --check order.scxml"), 2);
    CHECK_EQUAL(run("compile order.scxml -o order.tai"), 0);
    CHECK_EQUAL(run("fmt --check order.tai"), 1);
    CHECK(has("output", "an image: fmt takes SCXML charts and system manifests"));
    CHECK(fixture_write("ticket.scxml", "<scxml " NS "><!-- escalate -- now --><final id='done'/></scxml>"));
    CHECK_EQUAL(run("fmt -w ticket.scxml"), 1);
    CHECK(has("output", "| ticket.scxml | no | yes |  |"));
    CHECK(has("output", "a comment holding '--'"));
    CHECK(has("ticket.scxml", "<!-- escalate -- now -->")); /* left as it was */
    CHECK(fixture_write("broken.scxml", "<scxml " NS "><state id='a'></scxml>"));
    CHECK_EQUAL(run("fmt broken.scxml"), 1);
    CHECK(has("output", "| broken.scxml | 1 | "));
    CHECK(has("output", "mismatched end tag"));
    CHECK_EQUAL(run("fmt missing.scxml"), 3);
    CHECK(has("errors", "cannot read missing.scxml"));
}

/* -I: a src beside the file naming it first, then in each directory given, in order. */
static void test_include_path(void) {
    char command[1024];
    snprintf(command, sizeof command,
             "cd '%s' && mkdir -p shop shared/payments legacy && "
             "printf '%%s' \"<scxml " NS " name='checkout'><state id='paying'><invoke id='pay' src='payment.scxml'/>"
             "</state></scxml>\" > shop/checkout.scxml && "
             "printf '%%s' \"<scxml " NS " name='payment'><final id='settled'/></scxml>\" > shared/payments/payment.scxml && "
             "printf '%%s' \"<scxml " NS " name='legacy-payment'><final id='settled'/></scxml>\" > legacy/payment.scxml",
             TA_TEST_TEMPORARY_DIRECTORY);
    CHECK_EQUAL(system(command) == 0 ? 0 : 1, 0);
    /* found nowhere: today's diagnostic, at the path beside the chart */
    CHECK_EQUAL(run("check shop/checkout.scxml"), 1);
    CHECK(has("output", "| shop/payment.scxml | 0 | 0 | error | cannot read the file |"));
    /* in a directory of the include path; the diagnostics and the image name the file found */
    CHECK_EQUAL(run("inspect -I shared/payments shop/checkout.scxml"), 0);
    CHECK(has("output", "| 1 | payment | shared/payments/payment.scxml |"));
    CHECK_EQUAL(run("compile --include shared/payments/ shop/checkout.scxml -o checkout.tai"), 0);
    /* in the order given */
    CHECK_EQUAL(run("inspect -I legacy -I shared/payments shop/checkout.scxml"), 0);
    CHECK(has("output", "| 1 | legacy-payment | legacy/payment.scxml |"));
    CHECK_EQUAL(run("inspect -I shared/payments -I legacy shop/checkout.scxml"), 0);
    CHECK(has("output", "| 1 | payment | shared/payments/payment.scxml |"));
    /* beside the chart first */
    CHECK(fixture_write("shop/payment.scxml", "<scxml " NS " name='shop-payment'><final id='settled'/></scxml>"));
    CHECK_EQUAL(run("inspect -I shared/payments shop/checkout.scxml"), 0);
    CHECK(has("output", "| 1 | shop-payment | shop/payment.scxml |"));
    remove(fixture_path("shop/payment.scxml"));
    /* found nowhere, with an include path: where it looked, in order */
    CHECK_EQUAL(run("check -I nowhere -I elsewhere shop/checkout.scxml"), 1);
    CHECK(has("output", "| shop/payment.scxml | 0 | 0 | error | cannot read the file; looked for it at "
                        "shop/payment.scxml, nowhere/payment.scxml, elsewhere/payment.scxml |"));
    /* fmt compiles through it too, to compare the images */
    CHECK_EQUAL(run("fmt --check -I shared/payments shop/checkout.scxml"), 1);
    CHECK(has("output", "| shop/checkout.scxml | yes | no | same |"));
    CHECK_EQUAL(run("help check"), 0);
    CHECK(has("output", "| -I, --include DIR |"));
}

/* Every chart of the repository: its canonical text compiles to the image the chart does
 * (fmt compares them, lines and columns aside), and formatting is idempotent. */
static void test_canonical_corpus(void) {
    char command[2048];
    snprintf(command, sizeof command,
             "cd '%s' && find . \\( -name .git -o -name node_modules -o -name build -o -name .claude "
             "-o -name dist \\) -prune -o -name '*.scxml' -type f -print | sort > '%s/corpus.list'",
             TA_TEST_REPOSITORY, TA_TEST_TEMPORARY_DIRECTORY);
    CHECK_EQUAL(system(command), 0);
    int charts = 0;
    for (const char *at = text_of("corpus.list"); (at = strchr(at, '\n')); at++)
        charts++;
    CHECK(charts >= 10);
    /* in place, writing nothing: no chart's canonical text means something else */
    snprintf(command, sizeof command,
             "cd '%s' && xargs '%s/tinyactors' fmt --check -f json < '%s/corpus.list' > '%s/output' 2> '%s/errors'",
             TA_TEST_REPOSITORY, TA_TEST_PROGRAMS, TA_TEST_TEMPORARY_DIRECTORY, TA_TEST_TEMPORARY_DIRECTORY,
             TA_TEST_TEMPORARY_DIRECTORY);
    system(command);
    CHECK(!strstr(text_of("output"), "\"image\": \"different\""));
    int same = 0;
    for (const char *at = text_of("output"); (at = strstr(at, "\"image\": \"same\"")); at++)
        same++;
    CHECK(same >= charts * 9 / 10); /* the rest do not compile here: a src elsewhere, a broken upload */
    fprintf(stderr, "  %d charts, %d compiled to the same image formatted\n", charts, same);
    /* a copy rewritten, then checked: nothing changes the second time */
    snprintf(command, sizeof command,
             "cd '%s' && rm -rf corpus && while read -r chart; do mkdir -p \"corpus/$(dirname \"$chart\")\" && "
             "cp \"%s/$chart\" \"corpus/$chart\"; done < corpus.list && cd corpus && "
             "xargs '%s/tinyactors' fmt -w < ../corpus.list > /dev/null 2>&1; "
             "xargs '%s/tinyactors' fmt --check -f json < ../corpus.list > ../output 2> ../errors",
             TA_TEST_TEMPORARY_DIRECTORY, TA_TEST_REPOSITORY, TA_TEST_PROGRAMS, TA_TEST_PROGRAMS);
    system(command);
    CHECK(!strstr(text_of("output"), "\"changed\": true"));
    CHECK(!strstr(text_of("output"), "\"image\": \"different\""));
}

/* ---- what the runtime writes: a snapshot and a blob ----------------------------------- */

/* Bytes into a scratch file. */
static int write_bytes(const char *name, const void *bytes, size_t length) {
    FILE *file = fopen(fixture_path(name), "wb");
    if (!file) return 0;
    size_t written = fwrite(bytes, 1, length, file);
    return fclose(file) == 0 && written == length;
}

static const char shop_order[] =
    "<scxml " NS " datamodel='ecmascript' name='order' initial='open'>\n"
    "<datamodel><data id='total' expr='0'/><data id='customer' expr=\"'ann'\"/></datamodel>\n"
    "<state id='open'>\n"
    "<transition event='item.added'><assign location='total' expr='total + 25'/></transition>\n"
    "<transition event='checkout' target='paying'/></state>\n"
    "<state id='paying'><onentry><send id='reminder' event='payment.reminder' delay='30s'/></onentry>\n"
    "<invoke id='pay' src='shop-payment.scxml'/>\n"
    "<transition event='done.invoke.pay' target='paid'/></state>\n"
    "<final id='paid'/></scxml>\n";
static const char shop_payment[] = "<scxml " NS " datamodel='ecmascript' name='payment' initial='authorizing'>"
                                   "<state id='authorizing'><transition event='authorized' target='settled'/>"
                                   "</state><final id='settled'/></scxml>";

/* The shop image's hash, as the command writes it. */
static char shop_hash[17];

/* `format` with the shop image's hash for its %s. */
static const char *with_hash(const char *format) {
    static char text[256];
    snprintf(text, sizeof text, format, shop_hash);
    return text;
}

static void shop_run(ta_host *host) {
    for (int i = 0; i < 16; i++) {
        ta_run result = ta_host_run(host, 1000000);
        const ta_host_effect *effects;
        uint32_t count = ta_host_effects(host, &effects);
        ta_host_effects_ack(host);
        if (result.status == TA_RUN_IDLE && !count) return;
    }
    CHECK(0); /* it never went idle */
}

static void shop_send(ta_host *host, uint64_t session, const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.session = session;
    CHECK_EQUAL(ta_host_send(host, &event), TA_OK);
}

static uint64_t shop_spawn(ta_host *host, uint32_t segment, const char *name) {
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = segment;
    spawn.chart = 0; /* order */
    spawn.name = (ta_string_view){name, (uint32_t)strlen(name)};
    uint64_t session = 0;
    CHECK_EQUAL(ta_host_spawn(host, &spawn, &session), TA_OK);
    return session;
}

/* A shop's host: ann's order paying (a reminder due in 30s, its payment an
 * invoked session) with an event queued, bob's order hibernated with one
 * queued. Its snapshot packed twice: with the ECMAScript datamodel's state
 * as it is (shop.snapshot), and wrapped in CBOR as a binding packs its own
 * (shop-cbor.snapshot); bob's blob (bob.blob); the image (shop.tai). */
static void write_shop(void) {
    CHECK(fixture_write("shop-order.scxml", shop_order));
    CHECK(fixture_write("shop-payment.scxml", shop_payment));
    CHECK_EQUAL(run("compile shop-order.scxml -o shop.tai"), 0);
    ta_bytes image;
    CHECK_EQUAL(ta_runtime_load_file(fixture_path("shop.tai"), &image), TA_OK);
    ta_image opened;
    ta_image_info info;
    info.size = sizeof info;
    CHECK_EQUAL(ta_image_open(image, &opened), TA_OK);
    ta_image_get_info(&opened, &info);
    snprintf(shop_hash, sizeof shop_hash, "%016llx", (unsigned long long)info.content_hash);
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"shop", 4};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    config.nonce = 0x5eed;
    ta_host *host;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(host, image, &segment), TA_OK);
    ta_runtime_free_file(image);
    uint64_t ann = shop_spawn(host, segment, "cart-ann");
    uint64_t bob = shop_spawn(host, segment, "cart-bob");
    shop_run(host);
    shop_send(host, ann, "item.added");
    shop_send(host, ann, "checkout");
    shop_send(host, bob, "item.added");
    shop_run(host);
    /* bob hibernated, its blob written before the ack ends it */
    ta_bytes blob;
    CHECK_EQUAL(ta_host_hibernate(host, bob, &blob), TA_OK);
    CHECK(write_bytes("bob.blob", blob.pointer, blob.length));
    ta_host_effects_ack(host);
    shop_send(host, bob, "checkout"); /* it waits in the mailbox; the WAKE is let go */
    shop_run(host);
    shop_send(host, ann, "item.added"); /* queued, not run */
    /* the datamodel's state, then the host packed with it */
    ta_buffer probe = {NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(ecmascript, &probe), TA_ERROR_NO_MEMORY);
    unsigned char *state = malloc(probe.length);
    ta_buffer state_out = {state, probe.length, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(ecmascript, &state_out), TA_OK);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &config.allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("datamodel"));
    ta_cbor_write_text(&writer, ta_string("ecmascript"));
    ta_cbor_write_text(&writer, ta_string("quickjs"));
    ta_cbor_write_bytes(&writer, (ta_bytes){state, state_out.length});
    ta_bytes wrapped;
    CHECK_EQUAL(ta_cbor_writer_bytes(&writer, &wrapped), TA_OK);
    const ta_datamodel_state states[2] = {{ta_string("ecmascript"), {state, state_out.length}},
                                          {ta_string("tinyactors-test"), wrapped}};
    const char *const names[2] = {"shop.snapshot", "shop-cbor.snapshot"};
    for (int i = 0; i < 2; i++) {
        probe = (ta_buffer){NULL, 0, 0};
        CHECK_EQUAL(ta_host_snapshot_pack(host, &states[i], &probe), TA_ERROR_NO_MEMORY);
        void *pack = aligned_alloc(8, (probe.length + 7) & ~(size_t)7);
        ta_buffer pack_out = {pack, probe.length, 0};
        CHECK_EQUAL(ta_host_snapshot_pack(host, &states[i], &pack_out), TA_OK);
        CHECK(write_bytes(names[i], pack, pack_out.length));
        free(pack);
    }
    /* the host's snapshot alone, without its images */
    probe = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot(host, &probe), TA_ERROR_NO_MEMORY);
    void *alone = aligned_alloc(8, (probe.length + 7) & ~(size_t)7);
    ta_buffer alone_out = {alone, probe.length, 0};
    CHECK_EQUAL(ta_host_snapshot(host, &alone_out), TA_OK);
    CHECK(write_bytes("shop.host", alone, alone_out.length));
    free(alone);
    /* ann's order and its payment exported, as a migration does inside a host */
    shop_run(host);
    ta_host_location where;
    CHECK_EQUAL(ta_host_locate(host, ann, &where), TA_OK);
    unsigned char exported[1 << 14];
    ta_buffer export_out = {exported, sizeof exported, 0};
    CHECK_EQUAL(ta_actor_export(ta_host_system(host, where.segment), where.actor, &export_out), TA_OK);
    CHECK(write_bytes("ann.export", exported, export_out.length));
    CHECK(write_bytes("notes.blob", "TAHB\x09\0\0\0", 8)); /* another format version */
    ta_cbor_writer_free(&writer);
    free(state);
    ta_host_free(host);
    ta_datamodel_ecmascript_free(ecmascript);
}

static void test_snapshots_and_blobs(void) {
    write_shop();
    /* a snapshot: its format, system, clock, segments and datamodels' state */
    CHECK_EQUAL(run("inspect shop.snapshot"), 0);
    CHECK(has("output", "- **From**: snapshot\n"));
    CHECK(has("output", "- **Format**: 3\n"));
    CHECK(has("output", "- **System ID**: shop\n"));
    CHECK(has("output", "- **Ticks per second**: 1000\n"));
    CHECK(has("output", "- **Restored**: yes\n"));
    CHECK(has("output", with_hash("| 0 | yes | %s |")));
    CHECK(has("output", "| order, payment | 3 | 1 |"));
    CHECK(has("output", "- **Label**: ecmascript\n"));
    CHECK(has("output", "- **Encoding**: opaque\n")); /* quickjs's own */
    CHECK_EQUAL(run("inspect - < shop.snapshot"), 0); /* standard input, read once */
    CHECK(has("output", "- **System ID**: shop\n"));
    /* a state a binding packs as CBOR is decoded: bytes as hexadecimal */
    CHECK_EQUAL(run("inspect -f json shop-cbor.snapshot"), 0);
    CHECK(has("output", "\"label\": \"tinyactors-test\""));
    CHECK(has("output", "\"encoding\": \"cbor\""));
    CHECK(has("output", "\"value\": {\"datamodel\": \"ecmascript\", \"quickjs\": {\"bytes\": \""));
    CHECK_EQUAL(run("inspect shop-cbor.snapshot"), 0);
    CHECK(has("output", "- **Value**: `{\"datamodel\":\"ecmascript\",\"quickjs\":h'"));

    /* every actor: ann paying, with a queued event, a reminder and its payment invoked;
     * bob hibernated with an event queued; the payment, ann's child */
    CHECK_EQUAL(run("actors shop.snapshot"), 0);
    CHECK(has("output", "### Actor 1: cart-ann\n\n- **Session**: 1\n- **Name**: cart-ann\n"
                        "- **Chart**: order\n- **Segment**: 0\n- **State**: runnable\n"
                        "- **Invocations**: 1\n- **Queued**: 1\n\n#### Configuration\n\n- paying\n"));
    CHECK(has("output", "| item.added |  |"));
    CHECK(has("output", "| payment.reminder | reminder | 30000 | 30s |"));
    CHECK(has("output", "### Actor 2: cart-bob\n\n- **Session**: 2\n- **Name**: cart-bob\n"
                        "- **Chart**: order\n- **Segment**: 0\n- **State**: hibernated\n"));
    CHECK(has("output", "| checkout |  |"));
    CHECK(has("output", "### Actor 3: pay\n\n- **Session**: 3\n- **Name**: pay\n- **Chart**: payment\n"
                        "- **Segment**: 0\n- **State**: idle\n- **Parent**: 1\n"));
    CHECK_EQUAL(run("actors --chart payment shop.snapshot"), 0);
    CHECK(has("output", "### Actor 1: pay\n"));
    CHECK(!strstr(text_of("output"), "cart-ann"));
    CHECK_EQUAL(run("actors --session 2 -f xml shop.snapshot"), 0);
    CHECK(has("output", "<actor session=\"2\" name=\"cart-bob\" chart=\"order\" segment=\"0\" "
                        "state=\"hibernated\""));
    CHECK(has("output", "<event event=\"checkout\" sendid=\"\"/>"));
    CHECK(!strstr(text_of("output"), "cart-ann"));
    CHECK_EQUAL(run("actors -f json --session 1 shop.snapshot"), 0);
    CHECK(has("output", "\"configuration\": [\n        \"paying\"\n      ]"));
    CHECK(has("output", "\"in\": \"30s\""));
    CHECK_EQUAL(run("actors --chart refund shop.snapshot"), 2);
    CHECK(has("errors", "no chart 'refund' in the snapshot's images"));
    CHECK_EQUAL(run("actors --segment 3 shop.snapshot"), 2);
    CHECK(has("errors", "no live segment 3 in the snapshot"));
    CHECK_EQUAL(run("actors --session ann shop.snapshot"), 2);
    CHECK_EQUAL(run("actors bob.blob"), 2);
    CHECK(has("errors", "bob.blob is not a snapshot"));

    /* the image commands read a snapshot's images */
    CHECK_EQUAL(run("states --chart payment shop.snapshot"), 0);
    CHECK(has("output", "| 1 | authorizing | atomic | /scxml |"));
    CHECK_EQUAL(run("states --segment 0 shop.snapshot"), 0);
    CHECK_EQUAL(run("states --segment 3 shop.snapshot"), 2);
    CHECK(has("errors", "shop.snapshot has no live segment 3"));
    CHECK_EQUAL(run("states --segment x shop.snapshot"), 2);
    CHECK_EQUAL(run("check shop.snapshot"), 0);
    CHECK(has("output", "- **Valid**: yes\n"));
    CHECK_EQUAL(run("compile shop.snapshot -o again.tai"), 2);
    CHECK(has("errors", "the input is a snapshot"));
    CHECK_EQUAL(run("lint --segment 0 shop.snapshot"), 0);
    CHECK(has("output", "# Lint"));
    CHECK_EQUAL(run("path --chart order shop.snapshot --to paying"), 0);
    CHECK(has("output", "- **Result**: found"));
    CHECK_EQUAL(run("path --segment 3 shop.snapshot --to paying"), 2);
    CHECK_EQUAL(run("fmt shop.snapshot"), 1);
    CHECK(has("output", "a snapshot, not XML"));
    CHECK_EQUAL(run("states bob.blob"), 2);
    CHECK(has("errors", "bob.blob is a hibernation or export blob: inspect reads it"));

    /* a hibernation blob: its session, image and identity; its chart and
     * configuration with the image it was written with, from a snapshot,
     * an image or the charts */
    CHECK_EQUAL(run("inspect bob.blob"), 0);
    CHECK(has("output", "- **From**: hibernation blob\n"));
    CHECK(has("output", "- **Format**: 4\n- **Readable**: yes\n- **Session**: 2\n"
                        "- **Image hash**: "));
    CHECK(has("output", with_hash("- **Image hash**: %s\n")));
    CHECK(has("output", "- **Wakes**: only in the host that hibernated it"));
    CHECK(!strstr(text_of("output"), "Configuration"));
    CHECK_EQUAL(run("inspect bob.blob shop.snapshot"), 0);
    CHECK(has("output", "- **Image matches**: yes\n"));
    CHECK(has("output", "- **Wakes in that snapshot**: yes\n"));
    CHECK(has("output", "- **Chart**: order\n- **Macrosteps**: 2\n\n#### Configuration\n\n- open\n"));
    CHECK_EQUAL(run("inspect bob.blob shop-order.scxml"), 0);
    CHECK(has("output", "- open\n"));
    CHECK_EQUAL(run("inspect bob.blob shop.tai"), 0);
    CHECK(has("output", "- **Image matches**: yes\n"));
    CHECK(fixture_write("voucher.scxml", "<scxml " NS " name='voucher'><final id='done'/></scxml>"));
    CHECK_EQUAL(run("inspect bob.blob voucher.scxml"), 0);
    CHECK(has("output", "- **Image matches**: no\n"));
    CHECK(!strstr(text_of("output"), "Configuration"));
    CHECK_EQUAL(run("inspect notes.blob"), 1);
    CHECK(has("output", "- **Format**: 9\n- **Readable**: no\n"));

    /* an export blob: the session tree, parents first, its delayed sends */
    CHECK_EQUAL(run("inspect ann.export shop.tai"), 0);
    CHECK(has("output", "- **From**: export blob\n"));
    CHECK(has("output", "- **Sessions**: 2\n- **Migrated**: no\n"));
    CHECK(has("output", "### Session 1: cart-ann\n"));
    CHECK(has("output", "| payment.reminder | reminder |  | 30000 | 0 | none |"));
    CHECK(has("output", "### Session 2: pay\n\n- **Session**: 3\n- **Name**: pay\n- **Parent**: 1\n"));
    CHECK(has("output", "- authorizing\n"));

    /* the host's snapshot alone: its segments, and that its actors need a pack */
    CHECK_EQUAL(run("inspect shop.host"), 0);
    CHECK(has("output", "- **From**: host snapshot\n"));
    CHECK(has("output", with_hash("| 0 | yes | %s |")));
    CHECK_EQUAL(run("inspect shop.snapshot shop.tai"), 2);
}

int main(void) {
    RUN(test_compile_and_inspect);
    RUN(test_a_system);
    RUN(test_the_image_as_c_and_to_standard_output);
    RUN(test_the_charts_records);
    RUN(test_refusals);
    RUN(test_the_command_line);
    RUN(test_disassemble_and_graph);
    RUN(test_graph_layout);
    RUN(test_layout_corpus);
    RUN(test_diff);
    RUN(test_lint_and_path);
    RUN(test_help_and_version);
    RUN(test_canonical);
    RUN(test_include_path);
    RUN(test_canonical_corpus);
    RUN(test_snapshots_and_blobs);
    TEST_MAIN_END();
}
