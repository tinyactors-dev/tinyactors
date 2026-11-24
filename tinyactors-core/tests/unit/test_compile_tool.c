/* ta-compile, the compiler's command line (tools/compile/compile.c), run as
 * a process: an image that opens, a C array, every diagnostic of a refused
 * chart, a manifest, and the exit statuses. */
#include <stdlib.h>
#include <sys/wait.h>
#include <tinyactors/image.h>
#include <tinyactors/runtime.h>
#include "../harness/harness.h"
#include "../support/file_util.h"

#ifndef TA_TEST_PROGRAMS
#define TA_TEST_PROGRAMS "."
#endif

#define NS "xmlns='http://www.w3.org/2005/07/scxml'"

/* Run ta-compile with `arguments` in the scratch directory, its stderr and
 * stdout into the files `errors` and `output`: its exit status. */
static int run(const char *arguments) {
    char command[2048];
    snprintf(command, sizeof command, "cd '%s' && '%s/ta-compile' %s > output 2> errors",
             TA_TEST_TEMPORARY_DIRECTORY, TA_TEST_PROGRAMS, arguments);
    int status = system(command);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The text of a scratch file, NUL-terminated (static: one at a time). */
static const char *text_of(const char *name) {
    static char text[65536];
    FILE *file = fopen(fixture_path(name), "rb");
    size_t length = file ? fread(text, 1, sizeof text - 1, file) : 0;
    if (file) fclose(file);
    text[length] = 0;
    return text;
}

static void test_charts_to_an_image(void) {
    CHECK(fixture_write("order.scxml", "<scxml " NS " name='order' initial='open'>"
                                       "<state id='open'><transition event='paid' target='charging'/></state>"
                                       "<state id='charging'><invoke src='payment.scxml'/></state></scxml>"));
    CHECK(fixture_write("payment.scxml", "<scxml " NS " name='payment'><final id='settled'/></scxml>"));
    CHECK_EQUAL(run("order.scxml -o order.tai --dump"), 0);
    const char *dump = text_of("output");
    CHECK(strstr(dump, "chart 0: order") != NULL);
    CHECK(strstr(dump, "state 1: open") != NULL);
    CHECK(strstr(dump, "chart 1: payment") != NULL); /* compiled in through its src */
    ta_bytes bytes;
    CHECK_EQUAL(ta_runtime_load_file(fixture_path("order.tai"), &bytes), TA_OK);
    ta_image image;
    CHECK_EQUAL(ta_image_open(bytes, &image), TA_OK);
    CHECK_EQUAL(ta_image_chart_by_name(&image, ta_string("payment")), 1u);
    ta_runtime_free_file(bytes);
}

static void test_a_c_array(void) {
    CHECK(fixture_write("tiny.scxml", "<scxml " NS " name='tiny'><final id='f'/></scxml>"));
    CHECK_EQUAL(run("--emit-c tiny_image tiny.scxml -o tiny.c"), 0);
    const char *c = text_of("tiny.c");
    CHECK(strstr(c, "const unsigned char tiny_image[] = {") != NULL);
    CHECK(strstr(c, "0x54, 0x41, 0x49, 0x31") != NULL); /* "TAI1" */
    CHECK(strstr(c, "const size_t tiny_image_length = ") != NULL);
}

static void test_diagnostics_and_statuses(void) {
    CHECK(fixture_write("bad.scxml", "<scxml " NS "><state id='a'>\n"
                                     "<transition target='nowhere'/></state></scxml>"));
    CHECK_EQUAL(run("bad.scxml -o bad.tai"), 1);
    CHECK(strstr(text_of("errors"), "bad.scxml:2:1: error: unknown state id 'nowhere'") != NULL);
    CHECK_EQUAL(run("missing.scxml -o missing.tai"), 2);
    CHECK(strstr(text_of("errors"), "cannot read missing.scxml") != NULL);
    CHECK_EQUAL(run("bad.scxml"), 2);       /* no output */
    CHECK_EQUAL(run("--frobnicate x"), 2); /* no such option */
    CHECK_EQUAL(run("--help"), 0);
    CHECK_EQUAL(run("--version"), 0);
    CHECK(strncmp(text_of("output"), "ta-compile ", 11) == 0);
}

static void test_a_manifest(void) {
    CHECK(fixture_write("ticket.scxml", "<scxml " NS " name='ticket'><state id='new'/></scxml>"));
    CHECK(fixture_write("system.xml", "<system>"
                                      "<chart name='ticket' src='ticket.scxml'/>"
                                      "<actor name='triage' chart='ticket'/></system>"));
    int status = run("--system system.xml -o system.tai --dump");
    if (status) fprintf(stderr, "  %s", text_of("errors"));
    CHECK_EQUAL(status, 0);
    CHECK(strstr(text_of("output"), "boot actor 0: triage of chart ticket") != NULL);
}

int main(void) {
    RUN(test_charts_to_an_image);
    RUN(test_a_c_array);
    RUN(test_diagnostics_and_statuses);
    RUN(test_a_manifest);
    TEST_MAIN_END();
}
