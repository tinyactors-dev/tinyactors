/* Charts of two datamodels in one host: data that crosses from one to the
 * other (a send, an invoke's params, a done.invoke's donedata, a send to
 * another segment) is converted to the receiver's datamodel (to_wire, then
 * from_wire), never handed over as the sender's handle. The two datamodels
 * are two quickjs instances, the second under another name, so their payload
 * handles collide: a handle passed through unconverted would name another
 * value, or none. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/address.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include "../support/counting_allocator.h"
#include "../support/host_config.h"

#define CHECK_STR(a, b)                                                                            \
    do {                                                                                           \
        CHECK(strcmp((a), (b)) == 0);                                                              \
        if (strcmp((a), (b)) != 0) fprintf(stderr, "  got \"%s\", want \"%s\"\n", (a), (b));      \
    } while (0)

#define NS "xmlns='http://www.w3.org/2005/07/scxml' "

/* The parent, on "ecmascript", invokes the child, on "other", with params,
 * hears back from it, and logs what each event carried. */
#define PARENT                                                                                     \
    "<scxml " NS "datamodel='ecmascript' name='parent' initial='p'>"                               \
    "<state id='p'>"                                                                               \
    "<onentry><script>var noise = [{a: 'not'}, {b: 'yours'}]</script></onentry>"                   \
    "<invoke id='kid' src='child.scxml'><param name='list' expr='[1, 2, 3]'/></invoke>"            \
    "<transition event='back'><log expr=\"'back:' + JSON.stringify(_event.data)\"/></transition>"  \
    "<transition event='done.invoke.kid' target='end'>"                                            \
    "<log expr=\"'done:' + JSON.stringify(_event.data)\"/></transition>"                           \
    "</state><final id='end'/></scxml>"

#define CHILD                                                                                      \
    "<scxml " NS "datamodel='other' name='child' initial='c'>"                                     \
    "<datamodel><data id='list'/></datamodel>"                                                     \
    "<state id='c'><onentry><log expr=\"'child:' + JSON.stringify(list)\"/>"                       \
    "<send target='#_parent' event='back'><param name='more' expr='list.concat([4])'/></send>"     \
    "</onentry><transition target='end'/></state>"                                                 \
    "<final id='end'><donedata><param name='sum' expr='list[0] + list[1] + list[2]'/></donedata>"  \
    "</final></scxml>"

/* A listener on "other", alone in an image of its own (a segment of its own). */
#define LISTENER                                                                                   \
    "<scxml " NS "datamodel='other' name='listener' initial='l'>"                                  \
    "<state id='l'><transition event='hello'>"                                                     \
    "<log expr=\"'heard:' + JSON.stringify(_event.data)\"/></transition></state></scxml>"

/* A sender on "ecmascript" that greets the address it is given. */
#define SENDER                                                                                     \
    "<scxml " NS "datamodel='ecmascript' name='sender' initial='s'>"                               \
    "<datamodel><data id='noise' expr='[[9], [9], [9]]'/></datamodel>"                             \
    "<state id='s'><transition event='go'>"                                                        \
    "<send targetexpr='_event.data' event='hello'><param name='n' expr='[7, 8]'/></send>"          \
    "</transition></state></scxml>"

static counting_allocator allocator;

typedef struct world {
    ta_datamodel_ecmascript *first, *second;
    ta_datamodel_binding bindings[2];
    ta_datamodel_vtable other;
    ta_host *host;
    char log[2048];
} world;

static void world_open(world *w) {
    memset(w, 0, sizeof *w);
    w->first = ta_datamodel_ecmascript_new(NULL);
    w->second = ta_datamodel_ecmascript_new(NULL);
    w->bindings[0] = ta_datamodel_ecmascript_binding(w->first);
    w->bindings[1] = ta_datamodel_ecmascript_binding(w->second);
    w->other = *w->bindings[1].vtable;
    w->other.name = "other";
    w->bindings[1].vtable = &w->other;
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = w->bindings;
    config.datamodel_count = 2;
    config.allocator = allocator.api;
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->first);
    ta_datamodel_ecmascript_free(w->second);
}

static uint32_t add(world *w, const char *name, const char *text, const test_file *files) {
    size_t length = 0;
    void *image = compile_chart(name, text, files, &length, 0);
    CHECK(image != NULL);
    uint32_t segment = 0;
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){image, length}, &segment), TA_OK);
    free(image);
    return segment;
}

static uint64_t spawn(world *w, const char *chart) {
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w->host, ta_string(chart), &ref), TA_OK);
    uint64_t session = 0;
    ta_spawn spawn = {ref.segment, ref.chart, {0, 0}, 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &session), TA_OK);
    return session;
}

static void run(world *w) {
    for (int i = 0; i < 16; i++) {
        ta_run result = ta_host_run(w->host, 100000);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(w->host, &effects);
        for (uint32_t k = 0; k < n; k++)
            if (effects[k].kind == TA_EFFECT_LOG) {
                size_t l = strlen(w->log);
                snprintf(w->log + l, sizeof w->log - l, "%.*s;", (int)effects[k].event.length,
                         effects[k].event.pointer);
            }
        ta_host_effects_ack(w->host);
        if (result.status == TA_RUN_IDLE && !n) return;
    }
}

static void test_in_one_segment(void) {
    world w;
    world_open(&w);
    const test_file files[] = {{"child.scxml", CHILD}, {NULL, NULL}};
    add(&w, "parent.scxml", PARENT, files);
    spawn(&w, "parent");
    run(&w);
    CHECK_STR(w.log, "child:[1,2,3];back:{\"more\":[1,2,3,4]};done:{\"sum\":6};");
    world_close(&w);
}

static void test_across_segments(void) {
    world w;
    world_open(&w);
    add(&w, "listener.scxml", LISTENER, NULL);
    add(&w, "sender.scxml", SENDER, NULL);
    uint64_t listener = spawn(&w, "listener"), sender = spawn(&w, "sender");
    run(&w);
    char address[64];
    ta_buffer out = {address, sizeof address, 0};
    CHECK_EQUAL(ta_address_format(ta_string("h"), listener, &out), TA_OK);
    char cbor[80];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){cbor, sizeof cbor, 0}, NULL);
    ta_cbor_write_text(&writer, (ta_string_view){address, (uint32_t)out.length});
    ta_bytes data_bytes;
    CHECK_EQUAL(ta_cbor_writer_bytes(&writer, &data_bytes), TA_OK);
    ta_payload data = 0;
    CHECK_EQUAL(w.bindings[0].vtable->from_wire(w.bindings[0].datamodel, data_bytes, &data), TA_OK);
    ta_event_in go;
    memset(&go, 0, sizeof go);
    go.size = sizeof go;
    go.session = sender;
    go.name = ta_string("go");
    go.data = data;
    CHECK_EQUAL(ta_host_send(w.host, &go), TA_OK);
    run(&w);
    CHECK_STR(w.log, "heard:{\"n\":[7,8]};");
    world_close(&w);
}

int main(void) {
    counting_init(&allocator);
    RUN(test_in_one_segment);
    RUN(test_across_segments);
    CHECK_EQUAL(allocator.live_bytes, 0);
    TEST_MAIN_END();
}
