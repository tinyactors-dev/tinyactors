#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

/*
 * The ECMAScript data model (SCXML B.2), backed by quickjs-ng. One adapter
 * context owns one JSRuntime; every data-model instance (session) gets its
 * own JSContext in it. See docs/ecmascript-datamodel.md.
 *
 * Threading: QuickJS is single-threaded. Every system that uses one context,
 * and every retain/release/clone of its values, must be externally
 * synchronized on one lock (or run on one thread).
 */

typedef struct tat_js_state tat_js_state;

typedef struct {
    ta_allocator allocator;
    /*
     * Zero selects the default: 256 MiB, 1 MiB, and 10,000 interrupt checks
     * per operation. QuickJS checks about every 10,000 bytecode operations,
     * so the default stops a script after roughly 10^8 operations.
     */
    size_t memory_limit;
    size_t stack_size;
    uint64_t interrupt_budget;
} tat_js_datamodel_config;

typedef struct {
    tat_js_state *state;
} tat_js_datamodel_context;

ta_result tat_js_datamodel_context_init(tat_js_datamodel_context *context,
                                        const tat_js_datamodel_config *config);
/* Every value and instance must be released first; see the design doc. */
void tat_js_datamodel_context_destroy(tat_js_datamodel_context *context);

ta_datamodel tat_js_datamodel(tat_js_datamodel_context *context);

/* True for values this data model created. */
bool tat_js_value_is(const ta_value *value);

/*
 * Wire JSON text of an ECMAScript value. A top-level value JSON cannot
 * represent becomes a marker such as {"$value":"undefined"}; nested ones
 * become null. Release with tat_js_json_release().
 */
typedef struct {
    const char *ptr;
    size_t len;
    tat_js_state *owner;
} tat_js_json;

ta_result tat_js_value_to_json(const ta_value *value, tat_js_json *out);
void tat_js_json_release(tat_js_json *json);

/*
 * A value holding wire JSON text. It is parsed in each session that reads
 * it, so it belongs to that session's realm. `json` must be valid JSON.
 */
ta_result tat_js_value_from_json(tat_js_datamodel_context *context,
                                 ta_str json,
                                 ta_value *out);
