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
#include <tinyactors/datamodel_staticjs.h>
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

/* ---- the staticjs natives: the same functions over staticjs values ------------------- */

enum { KIND_QUICKJS, KIND_JSON, KIND_STATICJS, KIND_STATICJS_NATIVES, KIND_COUNT };
static const char *const kind_names[KIND_COUNT] = {"quickjs", "json", "staticjs", "staticjs+natives"};

static int sjs_text_is(sjs_value v, const char *text) {
    sjs_str t = sjs_string_text(v);
    return v.kind == SJS_STRING && t.length == strlen(text) && !memcmp(t.ptr, text, t.length);
}

/* add's sku and qty, read from its object */
static int sjs_read_add(sjs_value data, sjs_value *sku, double *qty) {
    sjs_value q;
    if (data.kind != SJS_OBJECT || sjs_object_get(data, SJS_STR("sku"), sku) != SJS_OK || sku->kind != SJS_STRING ||
        sjs_object_get(data, SJS_STR("qty"), &q) != SJS_OK || q.kind != SJS_NUMBER)
        return 0;
    *qty = q.as.number;
    return 1;
}

static double sjs_price(sjs_value sku) { return sjs_text_is(sku, "tea") ? 450 : sjs_text_is(sku, "mug") ? 1200 : -1; }

static sjs_result sjs_refuse(sjs_native_call *call) {
    call->fault = SJS_STR("bad arguments");
    return SJS_ERR_PROGRAM;
}

static int sjs_item(sjs_native_call *call, sjs_value data, sjs_value *out) {
    sjs_value sku;
    double qty;
    if (!sjs_read_add(data, &sku, &qty) || sjs_object_new(call->context, 3, out) != SJS_OK) return 0;
    sjs_retain(sku);
    sjs_property p[3] = {{SJS_STR("sku"), sku}, {SJS_STR("qty"), sjs_number(qty)}, {SJS_STR("cents"), sjs_number(sjs_price(sku))}};
    for (int i = 0; i < 3; i++) sjs_object_put(call->context, out, &p[i]);
    return 1;
}

static sjs_result native_known_item(void *self, sjs_native_call *call) {
    (void)self;
    sjs_value sku;
    double qty;
    if (call->count != 1) return sjs_refuse(call);
    call->result = sjs_boolean(sjs_read_add(call->arguments[0], &sku, &qty) && sjs_price(sku) >= 0 && qty >= 1 && qty <= 99);
    return SJS_OK;
}

static sjs_result native_new_item(void *self, sjs_native_call *call) {
    (void)self;
    if (call->count != 1 || !sjs_item(call, call->arguments[0], &call->result)) return sjs_refuse(call);
    return SJS_OK;
}

static sjs_result native_add_item(void *self, sjs_native_call *call) {
    (void)self;
    if (call->count != 2 || call->arguments[0].kind != SJS_ARRAY) return sjs_refuse(call);
    sjs_value items = call->arguments[0], array, item;
    uint32_t n = sjs_length(items);
    if (!sjs_item(call, call->arguments[1], &item)) return sjs_refuse(call);
    if (sjs_array_new(call->context, n + 1, &array) != SJS_OK) return sjs_release(call->context, item), sjs_refuse(call);
    for (uint32_t i = 0; i < n; i++) {
        sjs_value e = sjs_array_at(items, i);
        sjs_retain(e);
        sjs_array_append(call->context, &array, e);
    }
    sjs_array_append(call->context, &array, item);
    call->result = array;
    return SJS_OK;
}

static sjs_result native_has_items(void *self, sjs_native_call *call) {
    (void)self;
    if (call->count != 1 || call->arguments[0].kind != SJS_ARRAY) return sjs_refuse(call);
    call->result = sjs_boolean(sjs_length(call->arguments[0]) > 0);
    return SJS_OK;
}

static sjs_result native_add(void *self, sjs_native_call *call) {
    (void)self;
    if (call->count != 2 || call->arguments[0].kind != SJS_NUMBER || call->arguments[1].kind != SJS_NUMBER) return sjs_refuse(call);
    call->result = sjs_number(call->arguments[0].as.number + call->arguments[1].as.number);
    return SJS_OK;
}

static sjs_result native_yes(void *self, sjs_native_call *call) {
    (void)self;
    call->result = sjs_boolean(1);
    return SJS_OK;
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

/* text with every `from` replaced by `to`, made new */
static char *replace(const char *text, const char *from, const char *to) {
    size_t n = strlen(text), f = strlen(from), t = strlen(to), count = 0;
    for (const char *p = text; (p = strstr(p, from)); p += f) count++;
    char *out = malloc(n + count * (t > f ? t - f : 0) + 1), *o = out;
    for (const char *p = text;;) {
        const char *q = strstr(p, from);
        if (!q) {
            strcpy(o, p);
            break;
        }
        memcpy(o, p, (size_t)(q - p));
        o += q - p;
        memcpy(o, to, t);
        o += t;
        p = q + f;
    }
    return out;
}

/* the chart for a datamodel: staticjs runs the ECMAScript chart as it is, and
 * with natives the json chart (whose grammar is JavaScript but for length()) */
static char *chart_for(int kind, const char *json_text, const char *js_text) {
    switch (kind) {
    case KIND_JSON: return replace(json_text, "@", "@");
    case KIND_STATICJS: return replace(js_text, "datamodel='ecmascript'", "datamodel='staticjs'");
    case KIND_STATICJS_NATIVES: {
        char *a = replace(json_text, "datamodel='json'", "datamodel='staticjs'");
        char *b = replace(a, "items[length(items)]", "items[items.length]");
        free(a);
        return b;
    }
    default: return replace(js_text, "@", "@");
    }
}

/* A chart whose document carries `pad`, a string of `size` bytes. */
static char *blob_chart(int kind, size_t size) {
    size_t n = size + 1024;
    char *s = malloc(n);
    char *pad = malloc(size + 1);
    memset(pad, 'x', size);
    pad[size] = 0;
    snprintf(s, n,
             "<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='%s' name='blob' initial='s'>"
             "<datamodel><data id='n' expr='0'/><data id='pad' expr=\"'%s'\"/></datamodel>"
             "<state id='s'><transition event='touch'><assign location='n' expr='%s'/></transition></state></scxml>",
             kind == KIND_JSON ? "json" : kind == KIND_QUICKJS ? "ecmascript" : "staticjs", pad,
             kind == KIND_JSON || kind == KIND_STATICJS_NATIVES ? "add(n, 1)" : "n + 1");
    free(pad);
    return s;
}

/* ---- a host ---------------------------------------------------------------------------- */

typedef struct bench {
    int kind;
    ta_datamodel_staticjs *staticjs_datamodel;
    counter host_bytes;
    ta_allocator allocator;
    ta_datamodel_json *json_datamodel;
    ta_datamodel_ecmascript *js_datamodel;
    ta_datamodel_binding binding;
    ta_host *host;
} bench;

static size_t datamodel_bytes(bench *b) {
    if (b->kind == KIND_JSON) return ta_datamodel_json_bytes(b->json_datamodel);
    if (b->kind == KIND_QUICKJS) return ta_datamodel_ecmascript_heap_bytes(b->js_datamodel);
    return ta_datamodel_staticjs_bytes(b->staticjs_datamodel);
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

static void bench_open(bench *b, int kind, const char *chart_text) {
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->allocator = (ta_allocator){&b->host_bytes, count_alloc, count_free};
    if (kind == KIND_STATICJS || kind == KIND_STATICJS_NATIVES) {
        ta_datamodel_staticjs_config config;
        memset(&config, 0, sizeof config);
        config.size = sizeof config;
        config.allocator = ta_runtime_malloc();
        if (ta_datamodel_staticjs_new(&config, &b->staticjs_datamodel) != TA_OK) exit(1);
        static const sjs_native_vtable known = {native_known_item, 0}, add_item = {native_add_item, 0}, new_item = {native_new_item, 0},
                                       has_items = {native_has_items, 0}, add = {native_add, 0}, yes = {native_yes, 0};
        static const struct {
            const char *name;
            const sjs_native_vtable *vtable;
        } natives[] = {{"knownItem", &known}, {"addItem", &add_item}, {"newItem", &new_item},
                       {"hasItems", &has_items}, {"add", &add}, {"yes", &yes}};
        for (size_t i = 0; kind == KIND_STATICJS_NATIVES && i < sizeof natives / sizeof natives[0]; i++) {
            ta_staticjs_function_entry e;
            memset(&e, 0, sizeof e);
            e.size = sizeof e;
            e.name = (ta_string_view){natives[i].name, (uint32_t)strlen(natives[i].name)};
            e.native = (sjs_native){0, natives[i].vtable};
            ta_datamodel_staticjs_register(b->staticjs_datamodel, &e);
        }
        b->binding = ta_datamodel_staticjs_binding(b->staticjs_datamodel);
    } else if (kind == KIND_JSON) {
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
    if (b->kind == KIND_JSON) ta_datamodel_json_free(b->json_datamodel);
    else if (b->kind == KIND_QUICKJS) ta_datamodel_ecmascript_free(b->js_datamodel);
    else ta_datamodel_staticjs_free(b->staticjs_datamodel);
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

static void order(int kind, uint32_t actors, uint32_t events) {
    bench b;
    char *chart = chart_for(kind, JSON_ORDER, JS_ORDER);
    bench_open(&b, kind, chart);
    free(chart);
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
    printf("%-16s actors %u: bytes/actor idle %.0f (datamodel %.0f), after the order's 3 events %.0f (datamodel %.0f)\n",
           kind_names[kind], actors, (double)(idle - before) / actors, (double)(datamodel_idle - datamodel_before) / actors,
           (double)(after - before) / actors, (double)(datamodel_after - datamodel_before) / actors);
    printf("%-16s ns/event: order's add/address %.0f, a guard alone %.0f, a guard and an assignment %.0f\n",
           kind_names[kind], per_order_event, guard, check);
    free(sessions);
    bench_close(&b);
}

static void blob(int kind, size_t size, uint32_t events) {
    char *chart = blob_chart(kind, size);
    bench b;
    bench_open(&b, kind, chart);
    uint32_t actors = 64;
    uint64_t *sessions = spawn_all(&b, "blob", actors);
    double ns = time_events(&b, sessions, actors, events, "touch");
    printf("%-16s document of %6zu bytes: an assignment %.0f ns/event\n", kind_names[kind], size, ns);
    free(sessions);
    bench_close(&b);
    free(chart);
}

/* ns per event as each of 64 actors' arrays grows to `items`, by `event` */
static double grow(int kind, const char *event, uint32_t items) {
    bench b;
    char *chart = chart_for(kind, JSON_APPEND, JS_APPEND);
    bench_open(&b, kind, chart);
    free(chart);
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

/* What a binding pays with staticjs natives in another language: the
 * arguments written as CBOR and the result read back, per call (addItem's). */
static void sjs_allocator_free(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    (void)params->size;
    free(params->ptr);
}
static void *sjs_allocator_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    return malloc(size);
}

static void convert(uint32_t calls) {
    static const sjs_allocator_vtable heap = {sjs_allocator_alloc, sjs_allocator_free};
    sjs_context *context;
    sjs_context_config config = {{0, &heap}, SJS_LIMITS_DEFAULT};
    if (sjs_context_init(&context, &config) != SJS_OK) exit(1);
    /* [[{sku: "tea", qty: 2, cents: 450}, {sku: "mug", qty: 1, cents: 1200}], {sku: "tea", qty: 3}] */
    sjs_value items, data, arguments;
    sjs_array_new(context, 2, &items);
    const char *skus[] = {"tea", "mug"};
    double qty[] = {2, 1}, cents[] = {450, 1200};
    for (int i = 0; i < 2; i++) {
        sjs_value item, sku;
        sjs_object_new(context, 3, &item);
        sjs_string_new(context, (sjs_str){skus[i], 3}, &sku);
        sjs_property p[3] = {{SJS_STR("sku"), sku}, {SJS_STR("qty"), sjs_number(qty[i])}, {SJS_STR("cents"), sjs_number(cents[i])}};
        for (int k = 0; k < 3; k++) sjs_object_put(context, &item, &p[k]);
        sjs_array_append(context, &items, item);
    }
    sjs_value tea;
    sjs_object_new(context, 2, &data);
    sjs_string_new(context, SJS_STR("tea"), &tea);
    sjs_property d[2] = {{SJS_STR("sku"), tea}, {SJS_STR("qty"), sjs_number(3)}};
    for (int k = 0; k < 2; k++) sjs_object_put(context, &data, &d[k]);
    sjs_array_new(context, 2, &arguments);
    sjs_retain(items);
    sjs_retain(data);
    sjs_array_append(context, &arguments, items);
    sjs_array_append(context, &arguments, data);
    /* the result, three items, as CBOR */
    static const uint8_t result[] = {0x83, 0xa3, 0x63, 's', 'k', 'u', 0x63, 't', 'e', 'a', 0x63, 'q', 't', 'y', 0x02, 0x65, 'c', 'e', 'n', 't', 's', 0x19, 0x01, 0xc2,
                                     0xa3, 0x63, 's', 'k', 'u', 0x63, 'm', 'u', 'g', 0x63, 'q', 't', 'y', 0x01, 0x65, 'c', 'e', 'n', 't', 's', 0x19, 0x04, 0xb0,
                                     0xa3, 0x63, 's', 'k', 'u', 0x63, 't', 'e', 'a', 0x63, 'q', 't', 'y', 0x03, 0x65, 'c', 'e', 'n', 't', 's', 0x19, 0x01, 0xc2};
    uint8_t room[512];
    uint64_t start = ta_runtime_now_ns();
    for (uint32_t i = 0; i < calls; i++) {
        sjs_writer *writer;
        sjs_writer_config wc = {context, arguments, SJS_STR(""), SJS_FORMAT_CBOR, 0};
        sjs_writer_init(&writer, &wc);
        sjs_buffer buffer = {room, sizeof room, 0};
        if (sjs_writer_write(writer, &buffer) != SJS_OK) exit(1);
        sjs_writer_destroy(writer);
        sjs_value v;
        sjs_read_params rp = {SJS_FORMAT_CBOR, {result, sizeof result}, 0, 0};
        if (sjs_value_read(context, &rp, &v) != SJS_OK) exit(1);
        sjs_release(context, v);
    }
    double ns = (double)(ta_runtime_now_ns() - start) / calls;
    uint64_t write_start = ta_runtime_now_ns();
    for (uint32_t i = 0; i < calls; i++) {
        sjs_writer *writer;
        sjs_writer_config wc = {context, arguments, SJS_STR(""), SJS_FORMAT_CBOR, 0};
        sjs_writer_init(&writer, &wc);
        sjs_buffer buffer = {room, sizeof room, 0};
        sjs_writer_write(writer, &buffer);
        sjs_writer_destroy(writer);
    }
    double write_ns = (double)(ta_runtime_now_ns() - write_start) / calls;
    printf("staticjs natives in another language, per addItem call: arguments to CBOR and the result back %.0f ns "
           "(writing the arguments %.0f, reading the result %.0f)\n", ns, write_ns, ns - write_ns);
    sjs_release(context, arguments);
    sjs_release(context, items);
    sjs_release(context, data);
    sjs_context_destroy(context);
}

int main(int argc, char **argv) {
    uint32_t actors = argc > 1 ? (uint32_t)atoi(argv[1]) : 10000;
    uint32_t events = argc > 2 ? (uint32_t)atoi(argv[2]) : 200000;
    if (argc > 3 && !strcmp(argv[3], "convert")) {
        convert(events);
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "append")) { /* the append comparison only */
        uint32_t sizes[] = {10, 50, 200};
        for (size_t i = 0; i < 3; i++) {
            trust_results = 0;
            double add_checked = grow(KIND_JSON, "add", sizes[i]), append_checked = grow(KIND_JSON, "append", sizes[i]);
            trust_results = TA_DATAMODEL_JSON_TRUST_RESULTS;
            double add_trusted = grow(KIND_JSON, "add", sizes[i]), append_trusted = grow(KIND_JSON, "append", sizes[i]);
            trust_results = 0;
            double js_add = grow(KIND_QUICKJS, "add", sizes[i]), js_append = grow(KIND_QUICKJS, "append", sizes[i]);
            double sjs_add = grow(KIND_STATICJS, "add", sizes[i]), sjs_append = grow(KIND_STATICJS, "append", sizes[i]);
            double sjsn_add = grow(KIND_STATICJS_NATIVES, "add", sizes[i]), sjsn_append = grow(KIND_STATICJS_NATIVES, "append", sizes[i]);
            printf("to %3u items, ns/event: json addItem %.0f (trusted %.0f), json items[length(items)] %.0f (trusted %.0f); "
                   "quickjs concat %.0f, items[items.length] %.0f; staticjs concat %.0f, items[items.length] %.0f; "
                   "staticjs+natives addItem %.0f, newItem %.0f\n",
                   sizes[i], add_checked, add_trusted, append_checked, append_trusted, js_add, js_append, sjs_add, sjs_append,
                   sjsn_add, sjsn_append);
        }
        return 0;
    }
    static const int kinds[] = {KIND_JSON, KIND_STATICJS, KIND_STATICJS_NATIVES, KIND_QUICKJS};
    for (size_t k = 0; k < 4; k++) order(kinds[k], actors, events);
    size_t sizes[] = {64, 1024, 16384, 60000};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
        for (size_t k = 0; k < 4; k++) blob(kinds[k], sizes[i], events / 4);
    return 0;
}
