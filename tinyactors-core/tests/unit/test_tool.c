/* The tinyactors command (tools/tinyactors, docs/cli.md), run as a process:
 * each command on charts, a manifest and an image, in each format, its exit
 * statuses, and what it says when the command line is wrong. */
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tinyactors/image.h>
#include <tinyactors/runtime.h>
#include "../harness/harness.h"
#include "../support/file_util.h"

#ifndef TA_TEST_PROGRAMS
#define TA_TEST_PROGRAMS "."
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
    CHECK(has("output", "| 2 | paying | atomic | <scxml> | 1 |  | 1 | 1 | 0 | no | no |  |"));
    CHECK(!strstr(text_of("output"), "settled")); /* --chart order: not payment's */
    CHECK_EQUAL(run("transitions -f json order.scxml"), 0);
    CHECK(has("output", "\"condition\": \"_event.data.price > 0\""));
    CHECK(has("output", "\"targets\": \"paid\""));
    CHECK_EQUAL(run("events order.scxml"), 0);
    CHECK(has("output", "| checkout | 1 | 0 | 0 |"));
    CHECK_EQUAL(run("data order.scxml"), 0);
    CHECK(has("output", "| total | <scxml> | 0 |"));
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
    CHECK(has("output", "| broken.scxml | 2 | 1 | error | unknown element <transtion> in <state> |"));
    CHECK_EQUAL(run("compile -f json broken.scxml -o broken.tai"), 1);
    CHECK(has("output", "\"compiled\": false"));
    struct stat status;
    CHECK(stat(fixture_path("broken.tai"), &status) != 0); /* nothing written */
    CHECK_EQUAL(run("states broken.scxml"), 1); /* every command reports why */
    CHECK(has("output", "unknown element <transtion>"));
    /* an image that does not verify */
    CHECK(fixture_write("damaged.tai", "TAI1 and then not an image at all"));
    CHECK_EQUAL(run("check damaged.tai"), 1);
    CHECK(has("output", "the image does not verify"));
    CHECK(fixture_write("notes.txt", "a shopping list"));
    CHECK_EQUAL(run("inspect notes.txt"), 1);
    CHECK(has("errors", "notes.txt is neither an image (.tai) nor an SCXML chart nor a system manifest"));
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
    CHECK(has("errors", "'yaml' is not a value of --format (one of: markdown, json, xml)"));
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
    "<scxml " NS " datamodel='ecmascript' name='fulfillment' initial='packing'>\n"                 \
    "<datamodel><data id='items' expr='[]'/><data id='count' expr='0'/></datamodel>\n"             \
    "<parallel id='packing'><state id='boxes'><onentry>"                                           \
    "<foreach array='items' item='item'><assign location='count' expr='count + 1'/></foreach>"     \
    "<if cond='count &gt; 10'><send event='parcel.heavy' delay='30s'/>"                            \
    "<else/><log label='parcel' expr='count'/></if></onentry>"                                     \
    "<transition event='packed' target='shipped'/></state>"                                        \
    "<state id='labels'><transition event='printed'/></state></parallel>\n"                       \
    "<final id='shipped'/></scxml>\n"

static void test_disassemble_and_graph(void) {
    CHECK(fixture_write("fulfillment.scxml", FULFILLMENT));
    CHECK_EQUAL(run("disassemble fulfillment.scxml"), 0);
    CHECK(has("output", "| onentry boxes | block |"));
    CHECK(has("output", "| foreach | `items` as `item` until "));
    CHECK(has("output", "| assign | `count` = `count + 1` |"));
    CHECK(has("output", "| jump-if-false | unless `count > 10` to "));
    CHECK(has("output", "| send | event parcel.heavy to itself after 30000000000ns |"));
    CHECK(has("output", "| log | label parcel `count` |"));
    CHECK_EQUAL(run("graph fulfillment.scxml"), 0);
    CHECK(has("output", "```mermaid\nstateDiagram-v2\n"));
    CHECK(has("output", "state \"boxes\" as s2"));
    CHECK(has("output", "    --\n")); /* the parallel state's regions */
    CHECK(has("output", "s2 --> s4 : packed"));
    CHECK_EQUAL(run("graph -f json fulfillment.scxml"), 0);
    CHECK(has("output", "\"source\": \"boxes\""));
    CHECK(has("output", "\"target\": \"shipped\""));
}

static void test_diff(void) {
    write_charts();
    CHECK_EQUAL(run("compile order.scxml -o order.tai"), 0);
    /* the next version: paying is now settling, and the order keeps a currency */
    CHECK(fixture_write("order-next.scxml",
                        "<scxml " NS " datamodel='ecmascript' name='order' initial='open'>\n"
                        "<datamodel><data id='total' expr='0'/><data id='currency' expr=\"'EUR'\"/></datamodel>\n"
                        "<state id='open'><transition event='checkout' target='settling'/></state>\n"
                        "<state id='settling'><invoke id='pay' src='payment.scxml'/>\n"
                        "<transition event='done.invoke.pay' target='paid'/></state>\n"
                        "<final id='paid'/></scxml>\n"));
    CHECK_EQUAL(run("diff order.tai order.scxml"), 0);
    CHECK(has("output", "- **Identical**: yes"));
    CHECK_EQUAL(run("diff --exit-code order.tai order.scxml"), 0);
    CHECK_EQUAL(run("diff -f json --exit-code order.tai order-next.scxml"), 1);
    CHECK(has("output", "\"statesAdded\": \"settling\""));
    CHECK(has("output", "\"statesRemoved\": \"paying\""));
    CHECK(has("output", "\"dataAdded\": \"currency\""));
    CHECK(has("output", "one in a state the new version removed is completed to a legal configuration"));
    CHECK_EQUAL(run("diff --chart shipment order.tai order-next.scxml"), 2);
    CHECK(has("errors", "no chart 'shipment' in either image"));
}

int main(void) {
    RUN(test_compile_and_inspect);
    RUN(test_a_system);
    RUN(test_the_image_as_c_and_to_standard_output);
    RUN(test_the_charts_records);
    RUN(test_refusals);
    RUN(test_the_command_line);
    RUN(test_disassemble_and_graph);
    RUN(test_diff);
    RUN(test_help_and_version);
    TEST_MAIN_END();
}
