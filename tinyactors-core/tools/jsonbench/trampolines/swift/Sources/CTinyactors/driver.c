#include "driver.h"

#include <string.h>
#include <time.h>

static uint64_t bench_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void bench_run(const bench_spec *spec, bench_result *out) {
    ta_cbor_writer w;
    ta_status status = TA_OK;
    uint64_t start = bench_now();
    for (uint64_t i = 0; i < spec->iterations; i++) {
        ta_cbor_writer_init(&w, (ta_buffer){spec->room.pointer, spec->room.capacity, 0}, 0);
        ta_json_call call = {sizeof call, spec->argument_count, spec->context, spec->arguments, &w, {0, 0}};
        status = spec->function(&call);
        if (status != TA_OK) break;
    }
    out->nanoseconds = bench_now() - start;
    out->item = (ta_bytes){0, 0};
    if (status == TA_OK) status = ta_cbor_writer_bytes(&w, &out->item);
    out->status = status;
}

static void bench_item(ta_cbor_writer *w, const char *sku, uint64_t qty, uint64_t cents) {
    ta_cbor_write_map(w, cents ? 3 : 2);
    ta_cbor_write_text(w, ta_string("sku"));
    ta_cbor_write_text(w, ta_string(sku));
    ta_cbor_write_text(w, ta_string("qty"));
    ta_cbor_write_uint(w, qty);
    if (cents) {
        ta_cbor_write_text(w, ta_string("cents"));
        ta_cbor_write_uint(w, cents);
    }
}

ta_status bench_arguments(uint32_t which, ta_buffer *out) {
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){out->pointer, out->capacity, 0}, 0);
    ta_cbor_write_array(&w, which);
    if (which >= 1) {
        ta_cbor_write_array(&w, 2);
        bench_item(&w, "tea", 2, 450);
        bench_item(&w, "mug", 1, 1200);
    }
    if (which >= 2) bench_item(&w, "tea", 3, 0);
    ta_bytes bytes;
    ta_status status = ta_cbor_writer_bytes(&w, &bytes);
    out->length = bytes.length;
    return status;
}

size_t bench_skip(ta_cbor_reader *r) {
    uint64_t pending = 1;
    ta_cbor_item it;
    while (pending && ta_cbor_read(r, &it) == 1) {
        pending--;
        if (it.kind == TA_CBOR_ARRAY) pending += it.count;
        else if (it.kind == TA_CBOR_MAP) pending += 2u * it.count;
        else if (it.kind == TA_CBOR_TAG) pending += 1;
    }
    return r->at;
}

void bench_copy_item(ta_cbor_writer *w, ta_bytes item) {
    ta_cbor_reader r;
    ta_cbor_item it;
    ta_cbor_reader_init(&r, item);
    while (ta_cbor_read(&r, &it) == 1) {
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
        }
    }
}
