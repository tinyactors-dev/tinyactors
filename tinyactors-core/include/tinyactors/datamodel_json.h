/* tinyactors/datamodel_json.h — the "json" datamodel (a spike of
 * docs/proposals/json-datamodel.md, on a branch; not a decided API).
 *
 * Each session's variables are one JSON object stored as canonical CBOR.
 * Expressions are data paths, JSON literals, calls of functions the
 * application registers by name, and In(). A function takes its arguments
 * as one CBOR array and writes one CBOR item as its result.
 */
#ifndef TINYACTORS_DATAMODEL_JSON_H
#define TINYACTORS_DATAMODEL_JSON_H

#include <tinyactors/allocator.h>
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_datamodel_json ta_datamodel_json;

typedef struct ta_json_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;          /* the function's own, as registered */
    ta_bytes arguments;     /* one CBOR array of the arguments, valid during the call */
    ta_cbor_writer *result; /* write exactly one item */
    ta_string_view message; /* a failing function sets it; the datamodel copies it */
} ta_json_call;

typedef ta_status (*ta_json_function)(ta_json_call *call);

typedef struct ta_json_function_entry {
    uint32_t size;
    uint32_t pad;
    ta_string_view name;
    ta_string_view chart; /* the chart it serves; empty: every chart */
    ta_json_function function;
    void *context;
} ta_json_function_entry;

typedef struct ta_datamodel_json_config {
    uint32_t size;
    uint32_t max_document_bytes; /* 0: the default */
    ta_allocator allocator;
} ta_datamodel_json_config;

#define TA_DATAMODEL_JSON_MAX_DOCUMENT_BYTES_DEFAULT (64u * 1024u)

TA_API void ta_datamodel_json_config_normalize(ta_datamodel_json_config *config);
TA_API ta_status ta_datamodel_json_new(const ta_datamodel_json_config *config, ta_datamodel_json **out);
TA_API ta_status ta_datamodel_json_register(ta_datamodel_json *datamodel,
                                            const ta_json_function_entry *entry);
TA_API ta_datamodel_binding ta_datamodel_json_binding(ta_datamodel_json *datamodel);
/* Every row and the tables' handle namespaces, into out
 * (TA_ERROR_NO_MEMORY: out->length is the room it needs); and back, after
 * ta_host_restore attached the charts again. */
TA_API ta_status ta_datamodel_json_snapshot(ta_datamodel_json *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_json_restore(ta_datamodel_json *datamodel, ta_bytes state);
/* Bytes the datamodel holds now (blocks, rows, tables), for measurement. */
TA_API size_t ta_datamodel_json_bytes(const ta_datamodel_json *datamodel);
TA_API void ta_datamodel_json_free(ta_datamodel_json *datamodel);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_DATAMODEL_JSON_H */
