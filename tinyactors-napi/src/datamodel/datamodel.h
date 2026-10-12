/* datamodel.h — the native "ecmascript" datamodel's parts: chart logic in
 * the application's engine, behind core's datamodel vtable
 * (tinyactors/datamodel.h; bindings.md, section 8). Its files call of core
 * only ta_datamodel_environment_*, ta_image_* and ta_cbor_* (the lint rule
 * datamodel-calls-environment-and-node-api), and Node-API.
 *
 *   handles.c      tables of rows: payloads (with reference counts),
 *                  sessions, charts, iterations, a host's images; the drops
 *                  deferred until the outer call into core returns
 *   expressions.c  a chart's expressions, compiled once at chart_attach as
 *                  strict functions inside `with (scope)`
 *   datamodel.c    the vtable: scopes, _event, In(), the entries
 *
 * Its state (ta_napi_datamodel) is a part of its Host (addon.h): a session,
 * a chart and a payload are rows, not allocations of their own (AGENTS.md,
 * "The binding's memory"). */
#ifndef TA_NAPI_DATAMODEL_H
#define TA_NAPI_DATAMODEL_H

#include "../addon.h"

/* A payload: its value, held in a one-item array, since Node-API version 8
 * references only objects, functions and symbols, and a payload may be a
 * number, a string or undefined. Its reference count is the row's. */
typedef struct ta_napi_payload_row {
    ta_napi_row row;
    napi_ref holder;
} ta_napi_payload_row;

/* A session: its scope, the spawn's or invoke's params, and its chart. */
typedef struct ta_napi_session_row {
    ta_napi_row row;
    napi_ref scope;
    napi_ref init;  /* an object, or NULL */
    uint64_t chart; /* a row of the charts table */
} ta_napi_session_row;

/* A chart attached: its compiled expressions, in two JavaScript arrays
 * indexed by expression (a function, or undefined where the source did
 * not compile: evaluating it is error.execution, as quickjs compiles lazily
 * and fails then). The row's references are the attachment and each
 * session of it (sessions may outlive the detach). */
typedef struct ta_napi_chart_row {
    ta_napi_row row;
    const ta_image *image;
    uint32_t chart;
    uint32_t count;
    napi_ref getters; /* (scope) => value; a script: (scope) => undefined */
    napi_ref setters; /* locations: (scope, value) => assigns */
} ta_napi_chart_row;

/* A <foreach> in progress: a shallow copy of its array. */
typedef struct ta_napi_iteration_row {
    ta_napi_row row;
    napi_ref items;
} ta_napi_iteration_row;

/* ---- expressions.c --------------------------------------------------------------- */

/* Compile chart `attach`'s expressions into a new row of the charts
 * table; its handle in *out. */
ta_status ta_napi_chart_compile(ta_napi_datamodel *datamodel, const ta_datamodel_chart *attach, uint64_t *out);
/* Drop one reference to chart `handle`; at the last, its functions go. */
void ta_napi_chart_unuse(ta_napi_datamodel *datamodel, uint64_t handle);
/* The source of expression `expression`, and its kind. */
typedef struct ta_napi_expression {
    ta_string_view source;
    uint32_t kind;
} ta_napi_expression;
ta_napi_expression ta_napi_chart_expression(const ta_napi_chart_row *chart, ta_expression expression);

#endif /* TA_NAPI_DATAMODEL_H */
