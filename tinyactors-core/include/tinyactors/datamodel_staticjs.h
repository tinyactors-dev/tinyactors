/* tinyactors/datamodel_staticjs.h — a "staticjs" datamodel over the staticjs
 * library (../staticjs): a spike for comparison with the json datamodel,
 * on a branch; not a decided API.
 *
 * A session's variables are staticjs values; every expression is a staticjs
 * expression compiled once per chart, whose globals are the chart's
 * variables, _event, _sessionid, _name, _ioprocessors, In and the functions
 * the application registered (staticjs native functions).
 */
#ifndef TINYACTORS_DATAMODEL_STATICJS_H
#define TINYACTORS_DATAMODEL_STATICJS_H

#include <staticjs.h>
#include <tinyactors/allocator.h>
#include <tinyactors/datamodel.h>
#include <tinyactors/datamodel_json.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_datamodel_staticjs ta_datamodel_staticjs;

typedef struct ta_staticjs_function_entry {
    uint32_t size;
    uint32_t pad;
    ta_string_view name;
    ta_string_view chart; /* the chart it serves; empty: every chart */
    sjs_native native;    /* called with staticjs values */
} ta_staticjs_function_entry;

typedef struct ta_datamodel_staticjs_config {
    uint32_t size;
    uint32_t pad;
    ta_allocator allocator;
} ta_datamodel_staticjs_config;

TA_API ta_status ta_datamodel_staticjs_new(const ta_datamodel_staticjs_config *config,
                                           ta_datamodel_staticjs **out);
TA_API ta_status ta_datamodel_staticjs_register(ta_datamodel_staticjs *datamodel,
                                                const ta_staticjs_function_entry *entry);
/* A function of the json datamodel's kind (its arguments one CBOR array, its
 * result one CBOR item), offered as a staticjs native: the values are written
 * as CBOR for the call and the result read back. A binding registers one
 * function with either datamodel this way. */
TA_API ta_status ta_datamodel_staticjs_register_json(ta_datamodel_staticjs *datamodel,
                                                     const ta_json_function_entry *entry);
TA_API ta_datamodel_binding ta_datamodel_staticjs_binding(ta_datamodel_staticjs *datamodel);
/* The context the datamodel's values live in, to make values and natives. */
TA_API sjs_context *ta_datamodel_staticjs_context(ta_datamodel_staticjs *datamodel);
TA_API ta_status ta_datamodel_staticjs_snapshot(ta_datamodel_staticjs *datamodel, ta_buffer *out);
TA_API ta_status ta_datamodel_staticjs_restore(ta_datamodel_staticjs *datamodel, ta_bytes state);
/* Bytes the datamodel and its staticjs context hold now. */
TA_API size_t ta_datamodel_staticjs_bytes(const ta_datamodel_staticjs *datamodel);
TA_API void ta_datamodel_staticjs_free(ta_datamodel_staticjs *datamodel);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_DATAMODEL_STATICJS_H */
