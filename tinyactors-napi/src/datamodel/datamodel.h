/* datamodel.h — the native "ecmascript" datamodel's parts: chart logic in
 * the application's engine, behind core's datamodel vtable
 * (tinyactors/datamodel.h; bindings.md, section 8). Its files call of core
 * only ta_datamodel_environment_*, ta_image_* and ta_cbor_* (the lint rule
 * datamodel-calls-environment-and-node-api), and Node-API.
 *
 *   handles.c      the two slot tables, payloads and sessions: a slot and a
 *                  generation per handle, never a pointer; a payload's
 *                  reference count; the drops deferred until the outer call
 *                  into core returns
 *   expressions.c  a chart's expressions, compiled once at chart_attach as
 *                  strict functions inside `with (scope)`
 *   datamodel.c    the vtable: scopes, _event, In(), the entries */
#ifndef TA_NAPI_DATAMODEL_H
#define TA_NAPI_DATAMODEL_H

#include "../addon.h"

/* ---- handles.c ----------------------------------------------------------------- */

/* A slot: a JavaScript value (a payload's, or a session's scope) and what
 * the table keeps beside it. */
typedef struct ta_napi_slot {
    napi_ref value;
    void *data;          /* a session's state; NULL for a payload */
    uint32_t references; /* a payload's reference count; 0: the slot is free */
    uint32_t generation; /* bumped when the slot is freed: an older handle is stale */
    uint32_t next_free;
} ta_napi_slot;

typedef struct ta_napi_slots {
    ta_napi_slot *items;
    uint32_t count, capacity;
    uint32_t free; /* first free slot, or TA_NIL */
} ta_napi_slots;

/* What a new slot holds. */
typedef struct ta_napi_slot_init {
    napi_value value;
    void *data;
} ta_napi_slot_init;

/* A handle of a new slot holding `init` (one reference); 0 when out of memory. */
uint64_t ta_napi_slots_add(ta_napi_datamodel *datamodel, ta_napi_slots *slots, const ta_napi_slot_init *init);
/* The live slot `handle` names, or NULL (stale, freed, or never one). */
ta_napi_slot *ta_napi_slots_get(ta_napi_slots *slots, uint64_t handle);
/* Free slot `handle`: its value's reference goes to the deferred drops. */
void ta_napi_slots_remove(ta_napi_datamodel *datamodel, ta_napi_slots *slots, uint64_t handle);
/* A reference to drop once the outer call into core returns: dropping the
 * last one can let JavaScript run (a finalizer), and core calls release
 * outside a run too (bindings.md, section 8). */
void ta_napi_defer_drop(ta_napi_datamodel *datamodel, napi_ref reference);
/* The value of a live slot. */
napi_status ta_napi_slot_value(ta_napi_datamodel *datamodel, const ta_napi_slot *slot, napi_value *out);

/* ---- expressions.c --------------------------------------------------------------- */

/* A chart's compiled expressions: per expression its kind and a function
 * (NULL when the source did not compile: evaluating it is
 * error.execution, as quickjs compiles lazily and fails then). */
typedef struct ta_napi_chart {
    const ta_image *image;
    uint32_t chart;
    uint32_t count;
    uint32_t *kinds;
    napi_ref *getters; /* (scope) => value; a script: (scope) => undefined */
    napi_ref *setters; /* locations: (scope, value) => assigns */
} ta_napi_chart;

ta_status ta_napi_chart_compile(ta_napi_datamodel *datamodel, const ta_datamodel_chart *chart, ta_napi_chart **out);
void ta_napi_chart_free(ta_napi_datamodel *datamodel, ta_napi_chart *chart);
/* The source of expression `expression`. */
ta_string_view ta_napi_chart_source(const ta_napi_chart *chart, ta_expression expression);

/* ---- datamodel.c: the datamodel's state -------------------------------------------- */

struct ta_napi_datamodel {
    napi_env env;
    ta_napi_slots payloads, sessions;
    napi_ref *drops; /* the deferred drops (handles.c) */
    uint32_t drop_count, drop_capacity;
    napi_ref helpers;  /* the helper functions of datamodel.c */
    napi_ref in;       /* In(), shared by every scope */
    napi_ref *iterations; /* <foreach> arrays in progress, by iter - 1 */
    uint32_t iteration_count, iteration_capacity;
    /* the entry running, for In() */
    ta_datamodel_environment *environment;
    const ta_napi_chart *chart;
};

#endif /* TA_NAPI_DATAMODEL_H */
