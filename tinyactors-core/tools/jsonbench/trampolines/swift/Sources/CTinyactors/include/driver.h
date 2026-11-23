/* The driver: calls a ta_json_function N times with a fixed ta_json_call,
 * the writer started again in the same room before each call, as the json
 * datamodel does (src/datamodel_json, its call of a function). */
#ifndef JSONBENCH_DRIVER_H
#define JSONBENCH_DRIVER_H

#include <stdint.h>
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_json.h>

typedef struct bench_spec {
    ta_json_function function;
    void *context;
    ta_bytes arguments;     /* one CBOR array */
    uint32_t argument_count;
    uint32_t pad;
    uint64_t iterations;
    ta_buffer room;         /* the writer's room, as the datamodel's */
} bench_spec;

typedef struct bench_result {
    uint64_t nanoseconds;   /* the whole loop */
    ta_status status;       /* the last call's, or the writer's */
    uint32_t pad;
    ta_bytes item;          /* the last result, a view of the room */
} bench_result;

void bench_run(const bench_spec *spec, bench_result *out);

/* The arguments of each function: 0 empty ([]), 1 hasItems, 2 addItem. */
ta_status bench_arguments(uint32_t which, ta_buffer *out);

/* Copy one encoded CBOR item into a writer, item by item (the writer takes
 * no raw bytes): what a binding pays when its encoder makes bytes. */
void bench_copy_item(ta_cbor_writer *writer, ta_bytes item);

/* Skip the next whole item; returns its end offset. */
size_t bench_skip(ta_cbor_reader *reader);

#endif
