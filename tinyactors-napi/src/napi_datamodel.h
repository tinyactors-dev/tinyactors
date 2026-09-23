#pragma once
/*
 * The JavaScript data model (ADR 0011, docs/design.md), named "javascript".
 *
 * Expressions are JavaScript, not source text:
 * - a function is an expression: a condition, a value or a script. It is
 *   called with `this` bound to the actor's data root and one argument, the
 *   evaluation context;
 * - literal(value) is a constant (docs/design.md: a function in a value
 *   position is an expression, literal() stores one as data);
 * - an object with `get` and `set` functions is a location;
 * - source text is accepted in two forms only, so plain and exported specs
 *   stay readable: `In(state)` as a condition, and a data name (an
 *   identifier) as a value or a location. Anything else is refused when the
 *   chart is defined, naming the expression (core ADR 0058).
 *
 * Each JavaScript expression is a host expression (TA_EXPRESSION_HOST) whose
 * value is an expression record: one strong napi_ref and a count, retained
 * by the compiled definition and released with it.
 *
 * An actor's data is one JavaScript object, its root: the spawn binding when
 * one is given, otherwise a new object. <data> declarations and <foreach>
 * variables are its properties; assigning to a name the root does not have
 * is `error.execution`.
 *
 * The evaluation context is one object per system, built once. Its getters
 * read the core call in progress (data, event, sessionId, name,
 * ioProcessors, inState), and throw once that call has returned, so a
 * closure that keeps the context cannot read stale state.
 *
 * Every callback opens its own handle scope (ADR 0007), and an exception
 * becomes a fault whose data is the thrown value, which core turns into
 * error.execution. Callbacks run inside a core call, so JavaScript they
 * call cannot call core (the reentrancy guard, ADR 0013 point 4).
 */
#include <node_api.h>
#include <stdbool.h>

#include "tinyactors.h"

typedef struct tna_napi_system tna_napi_system;

typedef enum {
    TNA_EXPR_FUNCTION = 0, /* a condition, value or script */
    TNA_EXPR_LITERAL,      /* literal(value): a constant */
    TNA_EXPR_LOCATION      /* { get, set } */
} tna_expr_kind;

typedef struct tna_js_datamodel {
    tna_napi_system *wrapper;
    napi_env env;
    napi_ref context;     /* the evaluation context object */
    napi_ref json_parse;  /* JSON.parse, for literal content */
    napi_ref array_from;  /* Array.from, for <foreach> snapshots */
    /* The call in progress: set around each callback, NULL between them. */
    const ta_dm_host *host;
    void *instance;
    napi_ref event;       /* the event object of the call, built on first use */
    /* False while close runs from a finalizer, where no JavaScript may run. */
    bool js_usable;
    /*
     * The next compile uses this data model instead of core's null data
     * model; the compile resets it. The generated layer alone compiles
     * against the null data model.
     */
    bool select_javascript;
} tna_js_datamodel;

/* Builds the context object; false when it cannot be made. */
bool tna_js_datamodel_init(tna_js_datamodel *dm, tna_napi_system *wrapper, napi_env env);
void tna_js_datamodel_close(tna_js_datamodel *dm, bool refs_usable);
ta_datamodel tna_js_datamodel_adapter(tna_js_datamodel *dm);

/*
 * A host expression for `value` of `kind`, with one reference the caller
 * owns: release it with tna_expression_release().
 */
bool tna_expression_make(tna_js_datamodel *dm, napi_value value, tna_expr_kind kind, ta_expression *out);
void tna_expression_release(ta_expression *expression);
/*
 * The JavaScript value of a host expression this module made (the function,
 * the literal() object or the location object), or NULL.
 */
napi_value tna_expression_value(napi_env env, const ta_expression *expression);
/* True for a literal() object: it carries the brand lib/statechart.js registers. */
bool tna_expression_is_literal(napi_env env, napi_value value);
