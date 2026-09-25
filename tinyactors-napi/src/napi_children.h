#pragma once
/*
 * Children of the in-system SCXML invoker (ADR 0021).
 *
 * The invoker (adapters/invoker.c) asks which registered definition an
 * invoke starts. This module answers, on the Node-API side:
 * - a definition ID in `<content expr>` or `src` (builder charts) names it;
 * - a literal `<content>` is an SCXML document, imported here, inside the
 *   invoke, with the system's data models (ADR 0019);
 * - a `src` string is fetched with the parent definition's resolve(src)
 *   and imported the same way.
 * Imported children are cached by document bytes, and inherit their
 * parent's resolver. JavaScript runs only in resolve(src) and a registered
 * data model's compile; neither may call core (ADR 0013).
 */
#include <node_api.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef struct tna_napi_system tna_napi_system;

typedef struct {
    tna_vec resolvers; /* tna_child_resolver: define()'s resolve, per definition */
    tna_vec documents; /* tna_child_document: imported children, by bytes */
    /* The message of the last refused child document, for the invoke's fault. */
    char failure[1024];
} tna_children;

void tna_children_init(tna_napi_system *wrapper);
/* Keeps `resolve` (a function) with `definition`, for its children's src. */
ta_result tna_children_keep_resolver(tna_napi_system *wrapper, ta_definition_id definition, napi_value resolve);
/* Releases the resolvers (when the environment is usable) and the cache. */
void tna_children_close(tna_napi_system *wrapper, bool env_usable);

/*
 * A ta_chart_src_resolver over a JavaScript resolve(src) function (core ADR
 * 0053). What resolve throws stays pending for the caller.
 */
typedef struct {
    napi_env env;
    napi_value resolve;
} tna_src_resolver;
ta_chart_src_resolver tna_src_resolver_adapter(tna_src_resolver *resolver);
