#include "driver.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JB_ROOM (64u * 1024u) /* the datamodel's max_document_bytes default */

static uint64_t jb_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

double jb_time(const jb_case *c, uint64_t n) {
    unsigned char *room = malloc(JB_ROOM);
    if (!room) return TA_ERROR_NO_MEMORY;
    ta_cbor_writer w;
    ta_json_call call = {sizeof call, 0, c->context, c->arguments, &w, {0, 0}};
    ta_status failed = TA_OK;
    uint64_t start = jb_now();
    for (uint64_t i = 0; i < n; i++) {
        ta_cbor_writer_init(&w, (ta_buffer){room, JB_ROOM, 0}, 0);
        call.message = (ta_string_view){0, 0};
        ta_status s = c->function(&call);
        ta_bytes result;
        if (s != TA_OK || (!c->writes_nothing && (ta_cbor_writer_bytes(&w, &result) != TA_OK || !result.length))) failed = TA_ERROR_INVALID;
    }
    uint64_t elapsed = jb_now() - start;
    free(room);
    if (failed != TA_OK) return failed;
    return (double)elapsed / (double)n;
}

ta_status jb_once(const jb_case *c, ta_buffer *out) {
    unsigned char room[4096];
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){room, sizeof room, 0}, 0);
    ta_json_call call = {sizeof call, 0, c->context, c->arguments, &w, {0, 0}};
    ta_status s = c->function(&call);
    if (s != TA_OK) return s;
    ta_bytes result;
    s = ta_cbor_writer_bytes(&w, &result);
    if (s != TA_OK) return s;
    if (result.length > out->capacity) return TA_ERROR_NO_MEMORY;
    memcpy(out->pointer, result.pointer, result.length);
    out->length = result.length;
    return TA_OK;
}

void *jb_handle_context(uintptr_t handle) { return (void *)handle; }

/* ---- reading -------------------------------------------------------------------------- */

typedef struct jb_span {
    const uint8_t *p;
    uint32_t n;
} jb_span;

static jb_span jb_next_item(ta_cbor_reader *r) { /* the next whole item's bytes */
    size_t start = r->at;
    uint64_t pending = 1;
    ta_cbor_item it;
    while (pending && ta_cbor_read(r, &it) == 1) {
        pending--;
        if (it.kind == TA_CBOR_ARRAY) pending += it.count;
        else if (it.kind == TA_CBOR_MAP) pending += 2u * it.count;
        else if (it.kind == TA_CBOR_TAG) pending += 1;
    }
    return (jb_span){(const uint8_t *)r->message.pointer + start, (uint32_t)(r->at - start)};
}

static int jb_key(const ta_cbor_item *k, const char *name) {
    size_t n = strlen(name);
    return k->kind == TA_CBOR_TEXT && k->bytes.length == n && !memcmp(k->bytes.pointer, name, n);
}

typedef struct jb_item {
    ta_string_view sku;
    uint64_t qty, cents;
} jb_item;

/* One item map {sku, qty, cents?} read from the reader, every field typed
 * and checked, as the Go struct decode does. */
static int jb_read_item(ta_cbor_reader *r, jb_item *out) {
    ta_cbor_item m, k, v;
    if (ta_cbor_read(r, &m) != 1 || m.kind != TA_CBOR_MAP) return 0;
    *out = (jb_item){{0, 0}, 0, 0};
    for (uint32_t i = 0; i < m.count; i++) {
        if (ta_cbor_read(r, &k) != 1 || k.kind != TA_CBOR_TEXT || ta_cbor_read(r, &v) != 1) return 0;
        if (jb_key(&k, "sku") && v.kind == TA_CBOR_TEXT)
            out->sku = (ta_string_view){v.bytes.pointer, (uint32_t)v.bytes.length};
        else if (jb_key(&k, "qty") && v.kind == TA_CBOR_UINT) out->qty = v.value;
        else if (jb_key(&k, "cents") && v.kind == TA_CBOR_UINT) out->cents = v.value;
        else return 0;
    }
    return 1;
}

static int jb_open(ta_cbor_reader *r, uint32_t kind, uint32_t *count) {
    ta_cbor_item it;
    if (ta_cbor_read(r, &it) != 1 || it.kind != kind) return 0;
    *count = it.count;
    return 1;
}

static uint64_t jb_price(ta_string_view sku) {
    if (sku.length == 3 && !memcmp(sku.pointer, "tea", 3)) return 450;
    if (sku.length == 3 && !memcmp(sku.pointer, "mug", 3)) return 1200;
    return 0;
}

static void jb_write_item_map(ta_cbor_writer *w, const jb_item *it) {
    ta_cbor_write_map(w, 3);
    ta_cbor_write_text(w, (ta_string_view){"sku", 3});
    ta_cbor_write_text(w, it->sku);
    ta_cbor_write_text(w, (ta_string_view){"qty", 3});
    ta_cbor_write_uint(w, it->qty);
    ta_cbor_write_text(w, (ta_string_view){"cents", 5});
    ta_cbor_write_uint(w, it->cents);
}

/* ---- the C baselines ------------------------------------------------------------------ */

ta_status jb_c_empty(ta_json_call *call) {
    ta_cbor_write_simple(call->result, TA_CBOR_TRUE);
    return TA_OK;
}

ta_status jb_c_has_items(ta_json_call *call) {
    ta_cbor_reader r;
    uint32_t args, items;
    jb_item it;
    ta_cbor_reader_init(&r, call->arguments);
    if (!jb_open(&r, TA_CBOR_ARRAY, &args) || args != 1 || !jb_open(&r, TA_CBOR_ARRAY, &items))
        return TA_ERROR_INVALID;
    for (uint32_t i = 0; i < items; i++)
        if (!jb_read_item(&r, &it)) return TA_ERROR_INVALID;
    ta_cbor_write_simple(call->result, items > 0 ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    return TA_OK;
}

ta_status jb_c_add_item(ta_json_call *call) {
    ta_cbor_reader r;
    uint32_t args, items;
    jb_item list[16], add;
    ta_cbor_reader_init(&r, call->arguments);
    if (!jb_open(&r, TA_CBOR_ARRAY, &args) || args != 2 || !jb_open(&r, TA_CBOR_ARRAY, &items) || items >= 16)
        return TA_ERROR_INVALID;
    for (uint32_t i = 0; i < items; i++)
        if (!jb_read_item(&r, &list[i])) return TA_ERROR_INVALID;
    if (!jb_read_item(&r, &add)) return TA_ERROR_INVALID;
    add.cents = jb_price(add.sku);
    ta_cbor_write_array(call->result, items + 1);
    for (uint32_t i = 0; i < items; i++) jb_write_item_map(call->result, &list[i]);
    jb_write_item_map(call->result, &add);
    return TA_OK;
}

ta_status jb_c_reemit(ta_json_call *call) {
    return jb_write_item(call->result, *(const ta_bytes *)call->context);
}

/* ---- re-emitting an encoded item ------------------------------------------------------ */

ta_status jb_write_item(ta_cbor_writer *w, ta_bytes item) {
    ta_cbor_reader r;
    ta_cbor_item it;
    int32_t got;
    ta_cbor_reader_init(&r, item);
    while ((got = ta_cbor_read(&r, &it)) == 1) {
        switch (it.kind) {
        case TA_CBOR_UINT: ta_cbor_write_uint(w, it.value); break;
        case TA_CBOR_NEGINT: ta_cbor_write_negative(w, it.value); break;
        case TA_CBOR_TEXT:
            ta_cbor_write_text(w, (ta_string_view){it.bytes.pointer, (uint32_t)it.bytes.length});
            break;
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
        default: return TA_ERROR_INVALID;
        }
    }
    return got == 0 ? w->status : (ta_status)got;
}
