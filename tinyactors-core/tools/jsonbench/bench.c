/* json-bench: the json datamodel (src/datamodel_json, a spike) beside
 * quickjs, on one machine, in one process: bytes per actor at a
 * population, time per event, and the cost of an assignment as the
 * session's document grows. Branch work for
 * docs/proposals/json-datamodel.md.
 *
 *   json-bench [actors] [events]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/cbor.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/datamodel_json.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

/* ---- a counting allocator, for the host ----------------------------------------------- */

typedef struct counter {
    size_t live;
} counter;

static void *count_alloc(void *context, size_t size, size_t align) {
    (void)align;
    void *p = malloc(size);
    if (p) ((counter *)context)->live += size;
    return p;
}

static void count_free(void *context, void *p, size_t size) {
    if (!p) return;
    ((counter *)context)->live -= size;
    free(p);
}

/* ---- the json functions ------------------------------------------------------------------ */


/* The functions below write their result with a ta_cbor_writer; the
 * adapter runs one over the room the datamodel lends (call->result). */
typedef struct writer_call {
    ta_bytes arguments;
    ta_cbor_writer *result;
    ta_string_view message;
} writer_call;

typedef ta_status (*writer_function)(writer_call *call);

typedef struct writer_entry {
    const char *name;
    writer_function function;
} writer_entry;

static ta_status writer_adapter(ta_json_call *call) {
    const writer_entry *entry = (const writer_entry *)call->context;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){call->result->pointer, call->result->capacity, 0}, NULL);
    writer_call inner = {call->arguments, &writer, {NULL, 0}};
    ta_status status = entry->function(&inner);
    call->message = inner.message;
    if (status != TA_OK) return status;
    ta_bytes out;
    status = ta_cbor_writer_bytes(&writer, &out);
    if (status != TA_OK) return status;
    call->result->length = out.length;
    return TA_OK;
}

typedef struct span {
    const uint8_t *p;
    uint32_t n;
} span;

static span next_item(ta_cbor_reader *r) { /* the next whole item's bytes */
    size_t start = r->at;
    uint64_t pending = 1;
    ta_cbor_item it;
    while (pending && ta_cbor_read(r, &it) == 1) {
        pending--;
        if (it.kind == TA_CBOR_ARRAY) pending += it.count;
        else if (it.kind == TA_CBOR_MAP) pending += 2u * it.count;
        else if (it.kind == TA_CBOR_TAG) pending += 1;
    }
    return (span){(const uint8_t *)r->message.pointer + start, (uint32_t)(r->at - start)};
}

static void copy_item(ta_cbor_writer *w, span v) {
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, (ta_bytes){v.p, v.n});
    while (ta_cbor_read(&r, &it) == 1) {
        switch (it.kind) {
        case TA_CBOR_UINT: ta_cbor_write_uint(w, it.value); break;
        case TA_CBOR_NEGINT: ta_cbor_write_negative(w, it.value); break;
        case TA_CBOR_TEXT: ta_cbor_write_text(w, (ta_string_view){it.bytes.pointer, (uint32_t)it.bytes.length}); break;
        case TA_CBOR_BYTES: ta_cbor_write_bytes(w, it.bytes); break;
        case TA_CBOR_ARRAY: ta_cbor_write_array(w, it.count); break;
        case TA_CBOR_MAP: ta_cbor_write_map(w, it.count); break;
        case TA_CBOR_TAG: ta_cbor_write_tag(w, it.value); break;
        case TA_CBOR_SIMPLE: ta_cbor_write_simple(w, (uint32_t)it.value); break;
        case TA_CBOR_FLOAT: {
            double d;
            memcpy(&d, &it.value, 8);
            ta_cbor_write_float(w, d);
            break;
        }
        }
    }
}

/* add's sku and qty, read from its map */
static int read_add(span data, ta_string_view *sku, uint64_t *qty) {
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, (ta_bytes){data.p, data.n});
    if (ta_cbor_read(&r, &it) != 1 || it.kind != TA_CBOR_MAP) return 0;
    int got = 0;
    for (uint32_t i = 0; i < it.count; i++) {
        ta_cbor_item key, value;
        if (ta_cbor_read(&r, &key) != 1 || key.kind != TA_CBOR_TEXT) return 0;
        span v = next_item(&r);
        ta_cbor_reader vr;
        ta_cbor_reader_init(&vr, (ta_bytes){v.p, v.n});
        ta_cbor_read(&vr, &value);
        if (key.bytes.length == 3 && !memcmp(key.bytes.pointer, "sku", 3) && value.kind == TA_CBOR_TEXT)
            *sku = (ta_string_view){value.bytes.pointer, (uint32_t)value.bytes.length}, got |= 1;
        if (key.bytes.length == 3 && !memcmp(key.bytes.pointer, "qty", 3) && value.kind == TA_CBOR_UINT)
            *qty = value.value, got |= 2;
    }
    return got == 3;
}

static int64_t price(ta_string_view sku) {
    if (sku.length == 3 && !memcmp(sku.pointer, "tea", 3)) return 450;
    if (sku.length == 3 && !memcmp(sku.pointer, "mug", 3)) return 1200;
    return -1;
}

static int arguments(writer_call *call, span *out, uint32_t want) {
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, call->arguments);
    if (ta_cbor_read(&r, &it) != 1 || it.kind != TA_CBOR_ARRAY || it.count != want) return 0;
    for (uint32_t i = 0; i < want; i++) out[i] = next_item(&r);
    return 1;
}

static ta_status f_known_item(writer_call *call) {
    span a[1];
    ta_string_view sku;
    uint64_t qty;
    if (!arguments(call, a, 1)) return TA_ERROR_INVALID;
    int ok = read_add(a[0], &sku, &qty) && price(sku) >= 0 && qty >= 1 && qty <= 99;
    ta_cbor_write_simple(call->result, ok ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    return TA_OK;
}

static ta_status f_add_item(writer_call *call) {
    span a[2];
    ta_string_view sku;
    uint64_t qty;
    if (!arguments(call, a, 2) || !read_add(a[1], &sku, &qty)) return TA_ERROR_INVALID;
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, (ta_bytes){a[0].p, a[0].n});
    if (ta_cbor_read(&r, &it) != 1 || it.kind != TA_CBOR_ARRAY) return TA_ERROR_INVALID;
    ta_cbor_write_array(call->result, it.count + 1);
    for (uint32_t i = 0; i < it.count; i++) copy_item(call->result, next_item(&r));
    ta_cbor_write_map(call->result, 3);
    ta_cbor_write_text(call->result, ta_string("sku"));
    ta_cbor_write_text(call->result, sku);
    ta_cbor_write_text(call->result, ta_string("qty"));
    ta_cbor_write_uint(call->result, qty);
    ta_cbor_write_text(call->result, ta_string("cents"));
    ta_cbor_write_uint(call->result, (uint64_t)price(sku));
    return TA_OK;
}

/* newItem(add): the one item to append, its price from the catalog */
static ta_status f_new_item(writer_call *call) {
    span a[1];
    ta_string_view sku;
    uint64_t qty;
    if (!arguments(call, a, 1) || !read_add(a[0], &sku, &qty)) return TA_ERROR_INVALID;
    ta_cbor_write_map(call->result, 3);
    ta_cbor_write_text(call->result, ta_string("sku"));
    ta_cbor_write_text(call->result, sku);
    ta_cbor_write_text(call->result, ta_string("qty"));
    ta_cbor_write_uint(call->result, qty);
    ta_cbor_write_text(call->result, ta_string("cents"));
    ta_cbor_write_uint(call->result, (uint64_t)price(sku));
    return TA_OK;
}

static ta_status f_has_items(writer_call *call) {
    span a[1];
    ta_cbor_reader r;
    ta_cbor_item it;
    if (!arguments(call, a, 1)) return TA_ERROR_INVALID;
    ta_cbor_reader_init(&r, (ta_bytes){a[0].p, a[0].n});
    if (ta_cbor_read(&r, &it) != 1 || it.kind != TA_CBOR_ARRAY) return TA_ERROR_INVALID;
    ta_cbor_write_simple(call->result, it.count ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    return TA_OK;
}

static ta_status f_add(writer_call *call) {
    span a[2];
    ta_cbor_item x, y;
    ta_cbor_reader r;
    if (!arguments(call, a, 2)) return TA_ERROR_INVALID;
    ta_cbor_reader_init(&r, (ta_bytes){a[0].p, a[0].n});
    ta_cbor_read(&r, &x);
    ta_cbor_reader_init(&r, (ta_bytes){a[1].p, a[1].n});
    ta_cbor_read(&r, &y);
    if (x.kind != TA_CBOR_UINT || y.kind != TA_CBOR_UINT) return TA_ERROR_INVALID;
    ta_cbor_write_uint(call->result, x.value + y.value);
    return TA_OK;
}

static ta_status f_yes(writer_call *call) {
    ta_cbor_write_simple(call->result, TA_CBOR_TRUE);
    return TA_OK;
}

/* ---- charts ------------------------------------------------------------------------------ */

static const char JSON_ORDER[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='json' name='order' initial='open'>"
    "<datamodel><data id='items' expr='[]'/><data id='address' expr=\"{'country': ''}\"/>"
    "<data id='checks' expr='0'/></datamodel>"
    "<state id='open'>"
    "<transition event='add' cond='knownItem(_event.data)'><assign location='items' expr='addItem(items, _event.data)'/></transition>"
    "<transition event='address'><assign location='address.country' expr='_event.data.country'/></transition>"
    "<transition event='check' cond='hasItems(items)'><assign location='checks' expr='add(checks, 1)'/></transition>"
    "<transition event='guard' cond='yes()'/>"
    "</state></scxml>";

static const char JS_ORDER[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript' name='order' initial='open'>"
    "<datamodel><data id='items' expr='[]'/><data id='address' expr=\"({country: ''})\"/>"
    "<data id='checks' expr='0'/></datamodel>"
    "<state id='open'>"
    "<transition event='add' cond=\"(_event.data.sku === 'tea' || _event.data.sku === 'mug') &amp;&amp; _event.data.qty &gt;= 1 &amp;&amp; _event.data.qty &lt;= 99\">"
    "<assign location='items' expr=\"items.concat([{sku: _event.data.sku, qty: _event.data.qty, cents: _event.data.sku === 'tea' ? 450 : 1200}])\"/></transition>"
    "<transition event='address'><assign location='address.country' expr='_event.data.country'/></transition>"
    "<transition event='check' cond='items.length &gt; 0'><assign location='checks' expr='checks + 1'/></transition>"
    "<transition event='guard' cond='true'/>"
    "</state></scxml>";

/* Appending: `add` passes the whole array through a function and back,
 * `append` names only the new item and writes it at the array's length. */
static const char JSON_APPEND[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='json' name='cart' initial='open'>"
    "<datamodel><data id='items' expr='[]'/></datamodel><state id='open'>"
    "<transition event='add' cond='knownItem(_event.data)'><assign location='items' expr='addItem(items, _event.data)'/></transition>"
    "<transition event='append' cond='knownItem(_event.data)'><assign location='items[length(items)]' expr='newItem(_event.data)'/></transition>"
    "</state></scxml>";

static const char JS_APPEND[] =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='ecmascript' name='cart' initial='open'>"
    "<datamodel><data id='items' expr='[]'/></datamodel><state id='open'>"
    "<transition event='add' cond=\"(_event.data.sku === 'tea' || _event.data.sku === 'mug') &amp;&amp; _event.data.qty &gt;= 1 &amp;&amp; _event.data.qty &lt;= 99\">"
    "<assign location='items' expr=\"items.concat([{sku: _event.data.sku, qty: _event.data.qty, cents: _event.data.sku === 'tea' ? 450 : 1200}])\"/></transition>"
    "<transition event='append' cond=\"(_event.data.sku === 'tea' || _event.data.sku === 'mug') &amp;&amp; _event.data.qty &gt;= 1 &amp;&amp; _event.data.qty &lt;= 99\">"
    "<assign location='items[items.length]' expr=\"({sku: _event.data.sku, qty: _event.data.qty, cents: _event.data.sku === 'tea' ? 450 : 1200})\"/></transition>"
    "</state></scxml>";

/* A chart whose document carries `pad`, a string of `size` bytes. */
static char *blob_chart(int json, size_t size) {
    size_t n = size + 1024;
    char *s = malloc(n);
    char *pad = malloc(size + 1);
    memset(pad, 'x', size);
    pad[size] = 0;
    snprintf(s, n,
             "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='%s' name='blob' initial='s'>"
             "<datamodel><data id='n' expr='0'/><data id='pad' expr=\"'%s'\"/></datamodel>"
             "<state id='s'><transition event='touch'><assign location='n' expr='%s'/></transition></state></scxml>",
             json ? "json" : "ecmascript", pad, json ? "add(n, 1)" : "n + 1");
    free(pad);
    return s;
}

/* ---- a host ---------------------------------------------------------------------------- */

typedef struct bench {
    int json;
    counter host_bytes;
    ta_allocator allocator;
    ta_datamodel_json *json_datamodel;
    ta_datamodel_ecmascript *js_datamodel;
    ta_datamodel_binding binding;
    ta_host *host;
} bench;

static size_t datamodel_bytes(bench *b) {
    return b->json ? ta_datamodel_json_bytes(b->json_datamodel) : ta_datamodel_ecmascript_heap_bytes(b->js_datamodel);
}

static size_t total_bytes(bench *b) { return b->host_bytes.live + datamodel_bytes(b); }

static void drain(ta_host *host) {
    const ta_host_effect *effects;
    while (ta_host_effects(host, &effects) > 0) ta_host_effects_ack(host);
}

static void run_idle(ta_host *host) {
    for (;;) {
        drain(host);
        ta_host_advance_time(host, (ta_host_time){ta_runtime_now_ns(), 0});
        ta_run run = ta_host_run(host, 1000000);
        drain(host);
        if (run.status == TA_RUN_IDLE) return;
        if (run.status == TA_RUN_FAULT) {
            fprintf(stderr, "fault: %s\n", ta_status_name((ta_status)run.detail));
            exit(1);
        }
    }
}

static uint32_t trust_results; /* TA_DATAMODEL_JSON_TRUST_RESULTS, for a run without the check */

static void bench_open(bench *b, int json, const char *chart_text) {
    memset(b, 0, sizeof *b);
    b->json = json;
    b->allocator = (ta_allocator){&b->host_bytes, count_alloc, count_free};
    if (json) {
        ta_datamodel_json_config config;
        memset(&config, 0, sizeof config);
        config.size = sizeof config;
        config.allocator = ta_runtime_malloc();
        config.flags = trust_results;
        if (ta_datamodel_json_new(&config, &b->json_datamodel) != TA_OK) exit(1);
        static const writer_entry functions[] = {{"knownItem", f_known_item}, {"addItem", f_add_item},
                                                 {"newItem", f_new_item},     {"hasItems", f_has_items},
                                                 {"add", f_add},              {"yes", f_yes}};
        for (size_t i = 0; i < sizeof functions / sizeof functions[0]; i++) {
            ta_json_function_entry e;
            memset(&e, 0, sizeof e);
            e.size = sizeof e;
            e.name = (ta_string_view){functions[i].name, (uint32_t)strlen(functions[i].name)};
            e.function = writer_adapter;
            e.context = (void *)&functions[i];
            ta_datamodel_json_register(b->json_datamodel, &e);
        }
        b->binding = ta_datamodel_json_binding(b->json_datamodel);
    } else {
        ta_datamodel_ecmascript_config config;
        memset(&config, 0, sizeof config);
        config.size = sizeof config;
        b->js_datamodel = ta_datamodel_ecmascript_new(&config);
        b->binding = ta_datamodel_ecmascript_binding(b->js_datamodel);
    }
    unsigned char room[256];
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){room, sizeof room, 0}, 0);
    ta_cbor_write_map(&w, 1);
    ta_cbor_write_text(&w, ta_string("systemID"));
    ta_cbor_write_text(&w, ta_string("bench"));
    ta_bytes config;
    ta_cbor_writer_bytes(&w, &config);
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = 1;
    environment.datamodels = &b->binding;
    environment.allocator = b->allocator;
    if (ta_host_create(config, &environment, &b->host) != TA_OK) exit(1);
    ta_allocator malloc_allocator = ta_runtime_malloc();
    ta_compile_options options = {sizeof options, 0, malloc_allocator, NULL, NULL, NULL};
    ta_source source = {ta_string("chart.scxml"), {chart_text, (uint32_t)strlen(chart_text)}};
    ta_compile_result *compiled = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &compiled) != TA_OK) {
        fprintf(stderr, "compile failed\n");
        exit(1);
    }
    ta_string_view image = ta_compile_image(compiled);
    uint32_t segment;
    ta_host_add_image(b->host, (ta_bytes){image.pointer, image.length}, &segment);
    ta_compile_free(compiled);
}

static void bench_close(bench *b) {
    ta_host_free(b->host);
    if (b->json) ta_datamodel_json_free(b->json_datamodel);
    else ta_datamodel_ecmascript_free(b->js_datamodel);
}

static uint64_t *spawn_all(bench *b, const char *chart, uint32_t n) {
    ta_host_chart_ref ref;
    if (ta_host_chart(b->host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref) != TA_OK) exit(1);
    uint64_t *sessions = malloc(sizeof(uint64_t) * n);
    for (uint32_t i = 0; i < n; i++) {
        ta_spawn spawn = {ref.segment, ref.chart, {NULL, 0}, 0, {NULL, 0}};
        if (ta_host_spawn(b->host, &spawn, &sessions[i]) != TA_OK) exit(1);
        if (i % 1024 == 1023) run_idle(b->host);
    }
    run_idle(b->host);
    return sessions;
}

/* An event with data from CBOR bytes (0 length: none). */
static void send(bench *b, uint64_t session, const char *name, ta_bytes cbor) {
    ta_event_in in;
    memset(&in, 0, sizeof in);
    in.size = sizeof in;
    in.session = session;
    in.name = (ta_string_view){name, (uint32_t)strlen(name)};
    if (cbor.length && b->binding.vtable->from_wire(b->binding.datamodel, cbor, &in.data) != TA_OK) exit(1);
    if (ta_host_send(b->host, &in) != TA_OK) {
        fprintf(stderr, "send refused\n");
        exit(1);
    }
}

static ta_bytes add_data(unsigned char *room, size_t size, const char *sku, uint64_t qty) {
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){room, size, 0}, 0);
    ta_cbor_write_map(&w, 2);
    ta_cbor_write_text(&w, ta_string("sku"));
    ta_cbor_write_text(&w, (ta_string_view){sku, (uint32_t)strlen(sku)});
    ta_cbor_write_text(&w, ta_string("qty"));
    ta_cbor_write_uint(&w, qty);
    ta_bytes out;
    ta_cbor_writer_bytes(&w, &out);
    return out;
}

static ta_bytes address_data(unsigned char *room, size_t size) {
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){room, size, 0}, 0);
    ta_cbor_write_map(&w, 1);
    ta_cbor_write_text(&w, ta_string("country"));
    ta_cbor_write_text(&w, ta_string("DE"));
    ta_bytes out;
    ta_cbor_writer_bytes(&w, &out);
    return out;
}

/* ns per event of `name` (no data), events spread over the population */
static double time_events(bench *b, uint64_t *sessions, uint32_t actors, uint32_t events, const char *name) {
    uint64_t start = ta_runtime_now_ns();
    uint32_t sent = 0;
    while (sent < events) {
        uint32_t batch = events - sent < 4096 ? events - sent : 4096;
        for (uint32_t i = 0; i < batch; i++) send(b, sessions[(sent + i) % actors], name, (ta_bytes){0, 0});
        run_idle(b->host);
        sent += batch;
    }
    return (double)(ta_runtime_now_ns() - start) / events;
}

static void order(int json, uint32_t actors, uint32_t events) {
    bench b;
    bench_open(&b, json, json ? JSON_ORDER : JS_ORDER);
    run_idle(b.host);
    size_t before = total_bytes(&b), datamodel_before = datamodel_bytes(&b);
    uint64_t *sessions = spawn_all(&b, "order", actors);
    size_t idle = total_bytes(&b), datamodel_idle = datamodel_bytes(&b);
    unsigned char room[64];
    uint64_t start = ta_runtime_now_ns();
    for (uint32_t i = 0; i < actors; i++) {
        send(&b, sessions[i], "add", add_data(room, sizeof room, "tea", 2));
        send(&b, sessions[i], "add", add_data(room, sizeof room, "mug", 1));
        send(&b, sessions[i], "address", address_data(room, sizeof room));
        if (i % 1024 == 1023) run_idle(b.host);
    }
    run_idle(b.host);
    double per_order_event = (double)(ta_runtime_now_ns() - start) / (3.0 * actors);
    size_t after = total_bytes(&b), datamodel_after = datamodel_bytes(&b);
    double guard = time_events(&b, sessions, actors, events, "guard");
    double check = time_events(&b, sessions, actors, events, "check");
    printf("%-8s actors %u: bytes/actor idle %.0f (datamodel %.0f), after the order's 3 events %.0f (datamodel %.0f)\n",
           json ? "json" : "quickjs", actors, (double)(idle - before) / actors, (double)(datamodel_idle - datamodel_before) / actors,
           (double)(after - before) / actors, (double)(datamodel_after - datamodel_before) / actors);
    printf("%-8s ns/event: order's add/address %.0f, a guard alone %.0f, a guard and an assignment %.0f\n",
           json ? "json" : "quickjs", per_order_event, guard, check);
    free(sessions);
    bench_close(&b);
}

static void blob(int json, size_t size, uint32_t events) {
    char *chart = blob_chart(json, size);
    bench b;
    bench_open(&b, json, chart);
    uint32_t actors = 64;
    uint64_t *sessions = spawn_all(&b, "blob", actors);
    double ns = time_events(&b, sessions, actors, events, "touch");
    printf("%-8s document of %6zu bytes: an assignment %.0f ns/event\n", json ? "json" : "quickjs", size, ns);
    free(sessions);
    bench_close(&b);
    free(chart);
}

/* ns per event as each of 64 actors' arrays grows to `items`, by `event` */
static double grow(int json, const char *event, uint32_t items) {
    bench b;
    bench_open(&b, json, json ? JSON_APPEND : JS_APPEND);
    uint32_t actors = 64;
    uint64_t *sessions = spawn_all(&b, "cart", actors);
    unsigned char room[64];
    uint64_t start = ta_runtime_now_ns();
    for (uint32_t k = 0; k < items; k++) {
        for (uint32_t i = 0; i < actors; i++) send(&b, sessions[i], event, add_data(room, sizeof room, k % 2 ? "mug" : "tea", 1 + k % 9));
        run_idle(b.host);
    }
    double ns = (double)(ta_runtime_now_ns() - start) / ((double)items * actors);
    free(sessions);
    bench_close(&b);
    return ns;
}

int main(int argc, char **argv) {
    uint32_t actors = argc > 1 ? (uint32_t)atoi(argv[1]) : 10000;
    uint32_t events = argc > 2 ? (uint32_t)atoi(argv[2]) : 200000;
    if (argc > 3 && !strcmp(argv[3], "append")) { /* the append comparison only */
        uint32_t sizes[] = {10, 50, 200};
        for (size_t i = 0; i < 3; i++) {
            trust_results = 0;
            double add_checked = grow(1, "add", sizes[i]), append_checked = grow(1, "append", sizes[i]);
            trust_results = TA_DATAMODEL_JSON_TRUST_RESULTS;
            double add_trusted = grow(1, "add", sizes[i]), append_trusted = grow(1, "append", sizes[i]);
            double js_add = grow(0, "add", sizes[i]), js_append = grow(0, "append", sizes[i]);
            printf("to %3u items, ns/event: json addItem %.0f (trusted %.0f), json items[length(items)] %.0f (trusted %.0f); "
                   "quickjs concat %.0f, items[items.length] %.0f\n",
                   sizes[i], add_checked, add_trusted, append_checked, append_trusted, js_add, js_append);
        }
        return 0;
    }
    for (int json = 1; json >= 0; json--) order(json, actors, events);
    size_t sizes[] = {64, 1024, 16384, 60000};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        for (int json = 1; json >= 0; json--) blob(json, sizes[i], events / 4);
    return 0;
}
