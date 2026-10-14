/* datamodel_ecmascript.c — SCXML ECMAScript datamodel on quickjs-ng (no I/O).
 *
 * Mapping (SCXML 1.0 §B.2):
 *   - one JSContext per session; <data> ids and system variables are
 *     properties of its global object;
 *   - _sessionid, _name, _ioprocessors are read-only; _event is read-only
 *     and only exists once the first event has been processed;
 *   - value expressions compile once per session to `function(){return(e)}`;
 *     locations compile to strict setters (assigning an undeclared location
 *     or a read-only system variable throws -> error.execution);
 *   - <script> compiles once per session to global code;
 *   - <content>/<data> text is JSON-parsed if possible, otherwise a string;
 *   - In('id') is a global function answered by the SCXML kernel;
 *   - payloads are JS values shared by all sessions of the runtime.
 */
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_ecmascript.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* Realms. An isolated session owns a JSContext (~54 KB of builtins). A shared
 * session is a scope object (~0.4 KB) in one locked-down context per
 * datamodel: its expressions are compiled once per chart as
 * function(__s){ with(__s){ ... } }, so identifiers resolve to the session's
 * <data>, system variables and _event before the (frozen) builtins. Charts
 * whose scripts declare top-level names (var/let/const/function/class) need a
 * real global scope per session and stay isolated. What a script declares is
 * asked of the engine (chart_analyze), not guessed from its text. */

/* A top-level function declaration of a chart's <script>: the function it
 * creates closes over the global scope only, so storing a session keeps its
 * name and re-runs the declaration on restore. */
typedef struct ecmascript_function_declaration {
    JSAtom name;
    uint32_t script;      /* expr id: compiled as "<script:N>" */
    int32_t line, column; /* of the function, as Function.prototype.lineNumber/columnNumber */
    int strict;           /* its script is strict code */
    char *source;         /* the declaration's source text */
} ecmascript_function_declaration;

typedef struct ecmascript_chart {
    const ta_image *image;
    uint64_t hash; /* the image's content hash */
    uint32_t chart, binding, expression_count;
    uint32_t segment; /* the host's segment of its attachment: a snapshot names it with chart */
    int shared;
    JSValue *getter, *setter; /* shared charts: compiled once, in the shared context */
    uint32_t refs;            /* systems attached (attach/detach) */
    uint32_t instance_count;  /* live sessions of this chart */
    uint64_t idle_since; /* refs == instance_count == 0: when (ecmascript->clock), for eviction */
    /* top-level declarations of the chart's <script>s (all scripts together) */
    JSAtom *variables;     /* var and function names: properties of the global object */
    JSAtom *lexical_names; /* let/const/class names: the global lexical scope */
    ecmascript_function_declaration *functions;
    uint32_t variable_count, lexical_name_count, function_count;
} ecmascript_chart;

typedef struct ecmascript_instance {
    JSContext *context; /* own context (isolated) or the shared context */
    ecmascript_chart *chart_context;
    JSValue scope;        /* shared: the session's variables (JS_UNDEFINED when isolated) */
    JSValue *getter;      /* isolated, per expr: compiled value function / script bytecode */
    JSValue *setter;      /* isolated, per expr: compiled strict assignment function */
    JSValue init;         /* invoke params (object) overriding <data>, or undefined */
    JSValue declarations; /* isolated: functions re-created on restore, name -> [fn, script, line,
                      column] (JS_UNDEFINED until one is) */
    ta_datamodel_environment *environment; /* valid during a datamodel call */
    char *string;                          /* last string returned to the core */
    int live;
    uint32_t next_free; /* slot free list */
} ecmascript_instance;

typedef struct ecmascript_iterator {
    JSContext *context;
    JSValue *items;
    uint32_t count;
} ecmascript_iterator;

struct ta_datamodel_ecmascript {
    JSRuntime *runtime;
    size_t heap;          /* bytes allocated by the engine (counted by es_alloc_*) */
    JSContext *auxiliary; /* context for host-side payload operations */
    uint32_t max_ops, operations;
    ecmascript_instance **instances;
    uint32_t instance_count, instance_capacity, free_slot; /* slots are reused (spawn churn) */
    JSContext *shared;            /* the shared realm, created on first use */
    ecmascript_instance *current; /* session being evaluated (In(), shared) */
    uint32_t realm;               /* TA_DATAMODEL_ECMASCRIPT_REALM_* */
    JSValue *payloads;
    uint32_t *payload_refs, *payload_next;
    uint32_t payload_count, payload_capacity, payload_free;
    ecmascript_iterator *iterations;
    uint32_t iteration_count, iteration_capacity, iteration_free;
    ecmascript_chart **charts;
    uint32_t chart_count, chart_capacity;
    uint32_t idle_count; /* charts nobody uses, kept for their compiled expressions */
    uint64_t clock;      /* idle order */
    JSValue base;        /* names of a fresh global object, in aux (JS_UNDEFINED until used) */
    JSValue explain; /* finds what in a value cannot be stored, in aux (JS_UNDEFINED until used) */
    char error[512]; /* why the last serialize/snapshot failed */
    ta_datamodel_environment
        *environment; /* of the vtable call in progress (NULL outside one): JS work is charged */
    const volatile int *abort_flag; /* the host's "stop now" (ta_datamodel_ecmascript_config) */
    size_t stack_size;              /* the engine's stack limit (bytes) */
    /* where the last to_wire, serialize or payload_serialize was written: the
     * view it gave is valid until the next call (datamodel.h); it keeps the
     * size of the largest */
    unsigned char *output;
    size_t output_capacity;
};

/* Fuel charged per interrupt check (quickjs checks every 10,000 operations):
 * one unit per 100 JavaScript operations, so a script costs what it does. */
#define FUEL_PER_CHECK 100u

/* Charts no system uses and no session needs are kept (their compiled
 * expressions are reused when the same image is attached again — a host
 * that builds a system per actor, like the PostgreSQL binding, attaches the
 * same chart over and over), up to this many; the oldest go first. */
#define ECMASCRIPT_IDLE_CHARTS 16

#define NONE 0xFFFFFFFFu

/* ---- helpers ------------------------------------------------------------------- */

static void *grow(void *p, uint32_t *capacity, uint32_t need, size_t elem) {
    if (need <= *capacity) return p;
    uint32_t nc = *capacity ? *capacity * 2 : 16;
    while (nc < need)
        nc *= 2;
    void *np = realloc(p, (size_t)nc * elem);
    if (!np) return NULL;
    *capacity = nc;
    return np;
}

static int interrupt(JSRuntime *rt, void *opaque) {
    (void)rt;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)opaque;
    if (ecmascript->environment)
        ta_datamodel_environment_charge(ecmascript->environment, FUEL_PER_CHECK);
    if (ecmascript->abort_flag && *ecmascript->abort_flag) return 1; /* the host wants it to stop */
    return ++ecmascript->operations > ecmascript->max_ops;
}

static void clear_exception(JSContext *context) {
    JS_FreeValue(context, JS_GetException(context));
}

static ecmascript_instance *I(ta_datamodel_ecmascript *ecmascript, uint64_t handle) {
    return handle && handle <= ecmascript->instance_count ? ecmascript->instances[handle - 1]
                                                          : NULL;
}

static ta_string_view expression_source(ecmascript_instance *instance, ta_expression expression,
                                        uint32_t *kind) {
    ta_expression_info info;
    ta_string_view none = {"", 0};
    if (ta_image_expression(instance->chart_context->image,
                            (ta_chart_item){instance->chart_context->chart, expression},
                            &info) != TA_OK)
        return none;
    if (kind) *kind = info.kind;
    return info.source;
}

/* Evaluate `pre` + src + `post` as global code named `file`. Chart code is
 * "<script:N>" (an isolated session's <script>) or "<expr:N>" (a wrapper),
 * both starting on the code's second line: expr N's line in the document is
 * then known for messages (function_line). */
static JSValue eval_file(JSContext *context, const char *pre, ta_string_view source,
                         const char *post, const char *file, int flags) {
    size_t lp = strlen(pre), lq = strlen(post), n = lp + source.length + lq;
    char *buffer = (char *)malloc(n + 1);
    if (!buffer) return JS_ThrowOutOfMemory(context);
    memcpy(buffer, pre, lp);
    if (source.length) memcpy(buffer + lp, source.pointer, source.length);
    memcpy(buffer + lp + source.length, post, lq);
    buffer[n] = 0;
    JSValue v = JS_Eval(context, buffer, n, file, flags);
    free(buffer);
    return v;
}

static JSValue eval_wrapped(JSContext *context, const char *pre, ta_string_view source,
                            const char *post, int flags) {
    return eval_file(context, pre, source, post, "<scxml>", flags);
}

static JSValue eval_expression(JSContext *context, const char *pre, ta_string_view source,
                               const char *post, ta_expression expression, int flags) {
    char file[32];
    snprintf(file, sizeof file, "<expr:%u>", (unsigned)expression);
    return eval_file(context, pre, source, post, file, flags);
}

static ta_string_view trim_expression(ta_string_view source) {
    /* tolerate a trailing ';' as in expr="new Foo();" */
    while (source.length &&
           (source.pointer[source.length - 1] == ';' || source.pointer[source.length - 1] == ' ' ||
            source.pointer[source.length - 1] == '\n' ||
            source.pointer[source.length - 1] == '\t' || source.pointer[source.length - 1] == '\r'))
        source.length--;
    return source;
}

static JSValue *getter_slot(ecmascript_instance *instance, ta_expression expression) {
    return instance->chart_context->shared ? &instance->chart_context->getter[expression]
                                           : &instance->getter[expression];
}

static JSValue get_getter(ecmascript_instance *instance, ta_expression expression) {
    if (expression >= instance->chart_context->expression_count)
        return JS_ThrowRangeError(instance->context, "bad expression id");
    JSValue *slot = getter_slot(instance, expression);
    if (JS_IsUndefined(*slot)) {
        uint32_t kind = 0;
        ta_string_view source = expression_source(instance, expression, &kind);
        JSValue f;
        if (instance->chart_context
                ->shared) { /* compiled once per chart; called with the session scope */
            f = kind == TA_EXPRESSION_SCRIPT
                    ? eval_expression(instance->context,
                                      "(function(__s){with(__s){(function(){'use strict';\n",
                                      source, "\n})();}})", expression, JS_EVAL_TYPE_GLOBAL)
                    : eval_expression(instance->context,
                                      "(function(__s){with(__s){return (function(){'use strict';"
                                      "return (\n",
                                      trim_expression(source), "\n);})();}})", expression,
                                      JS_EVAL_TYPE_GLOBAL);
        } else if (kind == TA_EXPRESSION_SCRIPT) {
            /* one line down, as in the scratch analysis (chart_analyze): the
             * positions of its function declarations are the same there */
            char file[32];
            snprintf(file, sizeof file, "<script:%u>", (unsigned)expression);
            f = eval_file(instance->context, "\n", source, "", file,
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        } else {
            f = eval_expression(instance->context, "(function(){return (\n",
                                trim_expression(source), "\n);})", expression, JS_EVAL_TYPE_GLOBAL);
        }
        if (JS_IsException(f)) return f;
        *slot = f;
    }
    return JS_DupValue(instance->context, *slot);
}

static JSValue get_setter(ecmascript_instance *instance, ta_expression expression) {
    if (expression >= instance->chart_context->expression_count)
        return JS_ThrowRangeError(instance->context, "bad expression id");
    JSValue *slot = instance->chart_context->shared ? &instance->chart_context->setter[expression]
                                                    : &instance->setter[expression];
    if (JS_IsUndefined(*slot)) {
        ta_string_view source = expression_source(instance, expression, NULL);
        JSValue f =
            instance->chart_context->shared
                ? eval_expression(instance->context, "(function(__s,__ta_v){with(__s){(\n", source,
                                  "\n) = __ta_v;}})", expression, JS_EVAL_TYPE_GLOBAL)
                : eval_expression(instance->context, "(function(__ta_v){'use strict';(\n", source,
                                  "\n) = __ta_v;})", expression, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(f)) return f;
        *slot = f;
    }
    return JS_DupValue(instance->context, *slot);
}

/* Minimal XML DOM for XML <content>/<data> (SCXML B.2: XML content becomes a
 * DOM object). Installed into a session's context on first use. */
static const char XML_PRELUDE[] =
    "(function(g){\n"
    "function N(t,n){this.nodeType=t;this.nodeName=n;this.childNodes=[];this.parentNode=null;}\n"
    "N.prototype.appendChild=function(c){c.parentNode=this;this.childNodes.push(c);return c;};\n"
    "Object.defineProperty(N.prototype,'firstChild',{get:function(){return "
    "this.childNodes[0]||null;}});\n"
    "Object.defineProperty(N.prototype,'textContent',{get:function(){\n"
    "  if(this.nodeType===3)return this.nodeValue;var s='';\n"
    "  for(var i=0;i<this.childNodes.length;i++)s+=this.childNodes[i].textContent;return s;}});\n"
    "N.prototype.getElementsByTagName=function(n){var r=[];(function w(e){\n"
    "  for(var i=0;i<e.childNodes.length;i++){var c=e.childNodes[i];\n"
    "    "
    "if(c.nodeType===1){if(n==='*'||c.nodeName===n||c.localName===n)r.push(c);w(c);}}})(this);"
    "return r;};\n"
    "N.prototype.getAttribute=function(n){return "
    "this.attributes&&Object.prototype.hasOwnProperty.call(this.attributes,n)?this.attributes[n]:"
    "null;};\n"
    "N.prototype.hasAttribute=function(n){return this.getAttribute(n)!==null;};\n"
    "function ent(s){return "
    "s.replace(/&(lt|gt|amp|quot|apos|#x[0-9a-fA-F]+|#[0-9]+);/g,function(m,e){\n"
    "  return e==='lt'?'<':e==='gt'?'>':e==='amp'?'&':e==='quot'?'\"':e==='apos'?\"'\":\n"
    "  "
    "e[1]==='x'?String.fromCharCode(parseInt(e.slice(2),16)):String.fromCharCode(parseInt(e.slice("
    "1),10));});}\n"
    "g.__ta_xml=function(src){\n"
    "  var doc=new "
    "N(9,'#document'),cur=doc,re=/"
    "<!--[\\s\\S]*?-->|<!\\[CDATA\\[([\\s\\S]*?)\\]\\]>|<\\?[\\s\\S]*?\\?>|<!DOCTYPE[^>]*>|<\\/"
    "([^\\s>]+)\\s*>|<([^\\s\\/>]+)([^>]*?)(\\/?)>|([^<]+)/g,m;\n"
    "  while((m=re.exec(src))){\n"
    "    if(m[1]!==undefined){var t=new "
    "N(4,'#cdata-section');t.nodeValue=m[1];cur.appendChild(t);}\n"
    "    else if(m[2]){if(cur.parentNode)cur=cur.parentNode;}\n"
    "    else if(m[3]){var e=new N(1,m[3]);e.tagName=m[3];var "
    "k=m[3].indexOf(':');e.localName=k<0?m[3]:m[3].slice(k+1);\n"
    "      e.attributes={};var ar=/([^\\s=]+)\\s*=\\s*(\"([^\"]*)\"|'([^']*)')/g,a;\n"
    "      while((a=ar.exec(m[4])))e.attributes[a[1]]=ent(a[3]!==undefined?a[3]:a[4]);\n"
    "      cur.appendChild(e);if(!m[5])cur=e;}\n"
    "    else if(m[6]!==undefined&&!/^\\s*$/.test(m[6])){var x=new "
    "N(3,'#text');x.nodeValue=ent(m[6]);cur.appendChild(x);}\n"
    "  }\n"
    "  for(var "
    "i=0;i<doc.childNodes.length;i++)if(doc.childNodes[i].nodeType===1)doc.documentElement=doc."
    "childNodes[i];\n"
    "  Object.defineProperty(doc,'toString',{value:function(){return src;}});\n"
    "  return doc;};\n"
    "})(globalThis);\n";

static JSValue xml_value(JSContext *context, ta_string_view source) {
    JSValue g = JS_GetGlobalObject(context);
    JSValue function = JS_GetPropertyStr(context, g, "__ta_xml");
    if (!JS_IsFunction(context, function)) {
        JS_FreeValue(context, function);
        JSValue r =
            JS_Eval(context, XML_PRELUDE, sizeof XML_PRELUDE - 1, "<xml>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            JS_FreeValue(context, g);
            return r;
        }
        JS_FreeValue(context, r);
        function = JS_GetPropertyStr(context, g, "__ta_xml");
    }
    JSValue arg = JS_NewStringLen(context, source.pointer, source.length);
    JSValue v = JS_Call(context, function, JS_UNDEFINED, 1, (JSValueConst *)&arg);
    JS_FreeValue(context, arg);
    JS_FreeValue(context, function);
    JS_FreeValue(context, g);
    return v;
}

static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* SCXML B.2: JSON if it parses, a DOM if it is XML, else a space-normalized string. */
static JSValue content_value(JSContext *context, ta_string_view source) {
    char *buffer = (char *)malloc(source.length + 1);
    if (!buffer) return JS_ThrowOutOfMemory(context);
    if (source.length) memcpy(buffer, source.pointer, source.length);
    buffer[source.length] = 0;
    JSValue v = JS_ParseJSON(context, buffer, source.length, "<content>");
    if (JS_IsException(v)) {
        clear_exception(context);
        uint32_t i = 0;
        while (i < source.length && is_whitespace(source.pointer[i]))
            i++;
        if (i < source.length && source.pointer[i] == '<') {
            v = xml_value(context, source);
            if (JS_IsException(v))
                clear_exception(context);
            else {
                free(buffer);
                return v;
            }
        }
        uint32_t n = 0; /* collapse whitespace runs, trim */
        for (i = 0; i < source.length; i++) {
            if (is_whitespace(source.pointer[i])) {
                if (n && buffer[n - 1] != ' ') buffer[n++] = ' ';
            } else {
                buffer[n++] = source.pointer[i];
            }
        }
        while (n && buffer[n - 1] == ' ')
            n--;
        v = JS_NewStringLen(context, buffer, n);
    }
    free(buffer);
    return v;
}

/* Evaluate expression `e` to a value (owned by the caller). */
static ta_status value_of(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                          ta_expression expression, JSValue *out) {
    uint32_t kind = 0;
    ta_string_view source = expression_source(instance, expression, &kind);
    ecmascript->operations = 0;
    if (kind == TA_EXPRESSION_CONTENT) {
        *out = content_value(instance->context, source);
        return JS_IsException(*out) ? (clear_exception(instance->context), TA_ERROR_EXECUTION)
                                    : TA_OK;
    }
    JSValue f = get_getter(instance, expression);
    if (JS_IsException(f)) {
        clear_exception(instance->context);
        return TA_ERROR_EXECUTION;
    }
    ecmascript->current = instance;
    JSValue v;
    if (instance->chart_context->shared) {
        v = JS_Call(instance->context, f, JS_UNDEFINED, 1, (JSValueConst *)&instance->scope);
        JS_FreeValue(instance->context, f);
    } else if (kind == TA_EXPRESSION_SCRIPT) {
        v = JS_EvalFunction(instance->context, f); /* consumes f */
    } else {
        v = JS_Call(instance->context, f, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(instance->context, f);
    }
    if (JS_IsException(v)) {
        clear_exception(instance->context);
        return TA_ERROR_EXECUTION;
    }
    *out = v;
    return TA_OK;
}

static int is_identifier(ta_string_view s);

/* Shared realm: a location must start with one of the session's own variables
 * (SCXML: assigning to an undeclared location is error.execution; system
 * variables are read-only) — otherwise the assignment would land in the
 * shared global object. */
static int shared_location_ok(ecmascript_instance *instance, ta_expression location) {
    ta_string_view s = expression_source(instance, location, NULL);
    while (s.length && (s.pointer[0] == ' ' || s.pointer[0] == '\t' || s.pointer[0] == '\n' ||
                        s.pointer[0] == '\r')) {
        s.pointer++;
        s.length--;
    }
    uint32_t n = 0;
    while (n < s.length &&
           ((s.pointer[n] >= 'a' && s.pointer[n] <= 'z') ||
            (s.pointer[n] >= 'A' && s.pointer[n] <= 'Z') || s.pointer[n] == '_' ||
            s.pointer[n] == '$' || (n && s.pointer[n] >= '0' && s.pointer[n] <= '9')))
        n++;
    char name[256];
    if (!n || n >= sizeof name) return 0;
    memcpy(name, s.pointer, n);
    name[n] = 0;
    if (!strcmp(name, "_event") || !strcmp(name, "_sessionid") || !strcmp(name, "_name") ||
        !strcmp(name, "_ioprocessors") || !strcmp(name, "In"))
        return 0;
    JSAtom a = JS_NewAtom(instance->context, name);
    int own = JS_GetOwnProperty(instance->context, NULL, instance->scope, a);
    JS_FreeAtom(instance->context, a);
    return own;
}

/* Assign `v` (consumed) to location `loc`. */
static ta_status assign_to(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                           ta_expression location, JSValue v) {
    if (instance->chart_context->shared && !shared_location_ok(instance, location)) {
        JS_FreeValue(instance->context, v);
        return TA_ERROR_EXECUTION;
    }
    JSValue f = get_setter(instance, location);
    if (JS_IsException(f)) {
        clear_exception(instance->context);
        JS_FreeValue(instance->context, v);
        return TA_ERROR_EXECUTION;
    }
    ecmascript->operations = 0;
    ecmascript->current = instance;
    JSValue r;
    if (instance->chart_context->shared) {
        JSValue args[2] = {instance->scope, v};
        r = JS_Call(instance->context, f, JS_UNDEFINED, 2, args);
    } else {
        r = JS_Call(instance->context, f, JS_UNDEFINED, 1, (JSValueConst *)&v);
    }
    JS_FreeValue(instance->context, f);
    JS_FreeValue(instance->context, v);
    if (JS_IsException(r)) {
        clear_exception(instance->context);
        return TA_ERROR_EXECUTION;
    }
    JS_FreeValue(instance->context, r);
    return TA_OK;
}

static int is_identifier(ta_string_view s) {
    if (!s.length) return 0;
    for (uint32_t i = 0; i < s.length; i++) {
        char c = s.pointer[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' ||
                 (i && c >= '0' && c <= '9');
        if (!ok) return 0;
    }
    return 1;
}

/* <foreach> may introduce new variables for item/index. */
static void declare_if_missing(ecmascript_instance *instance, ta_expression location) {
    ta_string_view s = expression_source(instance, location, NULL);
    while (s.length && (s.pointer[0] == ' ' || s.pointer[0] == '\t' || s.pointer[0] == '\n')) {
        s.pointer++;
        s.length--;
    }
    while (s.length && (s.pointer[s.length - 1] == ' ' || s.pointer[s.length - 1] == '\t' ||
                        s.pointer[s.length - 1] == '\n'))
        s.length--;
    if (!is_identifier(s)) return;
    char name[256];
    if (s.length >= sizeof name) return;
    memcpy(name, s.pointer, s.length);
    name[s.length] = 0;
    JSValue g = instance->chart_context->shared ? JS_DupValue(instance->context, instance->scope)
                                                : JS_GetGlobalObject(instance->context);
    JSAtom a = JS_NewAtom(instance->context, name);
    int has = instance->chart_context->shared ? JS_GetOwnProperty(instance->context, NULL, g, a)
                                              : JS_HasProperty(instance->context, g, a);
    if (has == 0)
        JS_DefinePropertyValue(instance->context, g, a, JS_UNDEFINED,
                               JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(instance->context, a);
    JS_FreeValue(instance->context, g);
}

static ta_status to_string_out(ecmascript_instance *instance, JSValue v, int json_objects,
                               ta_string_view *out) {
    JSValue s = v;
    int free_s = 0;
    if (json_objects && JS_IsObject(v) && !JS_IsFunction(instance->context, v)) {
        s = JS_JSONStringify(instance->context, v, JS_UNDEFINED, JS_UNDEFINED);
        free_s = 1;
        if (JS_IsException(s)) {
            clear_exception(instance->context);
            s = v;
            free_s = 0;
        }
    }
    size_t length;
    const char *c = JS_ToCStringLen(instance->context, &length, s);
    if (free_s) JS_FreeValue(instance->context, s);
    JS_FreeValue(instance->context, v);
    if (!c) {
        clear_exception(instance->context);
        return TA_ERROR_EXECUTION;
    }
    free(instance->string);
    instance->string = (char *)malloc(length + 1);
    if (instance->string) {
        memcpy(instance->string, c, length);
        instance->string[length] = 0;
    }
    JS_FreeCString(instance->context, c);
    out->pointer = instance->string ? instance->string : "";
    out->length = instance->string ? (uint32_t)length : 0;
    return TA_OK;
}

/* ---- payload table -------------------------------------------------------------- */

static ta_payload payload_new(ta_datamodel_ecmascript *ecmascript, JSValue v) {
    uint32_t i;
    if (ecmascript->payload_free != NONE) {
        i = ecmascript->payload_free;
        ecmascript->payload_free = ecmascript->payload_next[i];
    } else {
        uint32_t capacity = ecmascript->payload_capacity;
        JSValue *p = (JSValue *)grow(ecmascript->payloads, &capacity, ecmascript->payload_count + 1,
                                     sizeof(JSValue));
        if (!p) return 0;
        ecmascript->payloads = p;
        uint32_t c2 = ecmascript->payload_capacity;
        uint32_t *r = (uint32_t *)grow(ecmascript->payload_refs, &c2, ecmascript->payload_count + 1,
                                       sizeof(uint32_t));
        if (!r) return 0;
        ecmascript->payload_refs = r;
        uint32_t c3 = ecmascript->payload_capacity;
        uint32_t *n = (uint32_t *)grow(ecmascript->payload_next, &c3, ecmascript->payload_count + 1,
                                       sizeof(uint32_t));
        if (!n) return 0;
        ecmascript->payload_next = n;
        ecmascript->payload_capacity = capacity;
        i = ecmascript->payload_count++;
    }
    ecmascript->payloads[i] = v;
    ecmascript->payload_refs[i] = 1;
    return (ta_payload)i + 1;
}

static JSValue payload_get(ta_datamodel_ecmascript *ecmascript, ta_payload payload) {
    if (!payload || payload > ecmascript->payload_count || !ecmascript->payload_refs[payload - 1])
        return JS_UNDEFINED;
    return ecmascript->payloads[payload - 1];
}

static void ecmascript_retain(void *datamodel, ta_payload payload) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    if (payload && payload <= ecmascript->payload_count && ecmascript->payload_refs[payload - 1])
        ecmascript->payload_refs[payload - 1]++;
}

static void ecmascript_release(void *datamodel, ta_payload payload) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    if (!payload || payload > ecmascript->payload_count || !ecmascript->payload_refs[payload - 1])
        return;
    uint32_t i = (uint32_t)(payload - 1);
    if (--ecmascript->payload_refs[i]) return;
    JS_FreeValueRT(ecmascript->runtime, ecmascript->payloads[i]);
    ecmascript->payloads[i] = JS_UNDEFINED;
    ecmascript->payload_next[i] = ecmascript->payload_free;
    ecmascript->payload_free = i;
}

/* ---- In() -------------------------------------------------------------------------- */

static JSValue js_in(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    /* isolated: the context's session; shared realm: the evaluating session */
    ecmascript_instance *instance = (ecmascript_instance *)JS_GetContextOpaque(context);
    if (!instance)
        instance =
            ((ta_datamodel_ecmascript *)JS_GetRuntimeOpaque(JS_GetRuntime(context)))->current;
    if (argc < 1 || !instance || !instance->environment) return JS_FALSE;
    size_t length;
    const char *s = JS_ToCStringLen(context, &length, argv[0]);
    if (!s) return JS_EXCEPTION;
    ta_string_view id = {s, (uint32_t)length};
    uint32_t state =
        ta_image_state_by_name(instance->chart_context->image, instance->chart_context->chart, id);
    JS_FreeCString(context, s);
    return JS_NewBool(context,
                      state != TA_NIL && ta_datamodel_environment_in(instance->environment, state));
}

/* ---- what a chart's <script>s declare at the top level ---------------------------------
 * Shared-realm scripts run as a strict function body, so `var` and top-level
 * let/const/function/class would become locals instead of session globals
 * visible to later expressions: such charts keep isolated sessions. Storing an
 * isolated session needs those names too. Both are asked of the engine, in a
 * scratch context per script: the script is instantiated without running (a
 * `throw` ahead of its first statement: declarations are hoisted, nothing
 * executes); the scratch global object then holds its var and function names,
 * and its let/const/class names are those in their temporal dead zone. The
 * source text only proposes candidate names (every identifier-like run in it,
 * a superset); the engine decides. */

/* names of a fresh global object (the builtins), as an object in aux */
static int base_names(ta_datamodel_ecmascript *ecmascript) {
    if (!JS_IsUndefined(ecmascript->base)) return 1;
    JSContext *sc = JS_NewContext(ecmascript->runtime);
    if (!sc) return 0;
    JSValue g = JS_GetGlobalObject(sc), base = JS_NewObjectProto(ecmascript->auxiliary, JS_NULL);
    JSPropertyEnum *props;
    uint32_t n;
    if (!JS_IsException(base) &&
        JS_GetOwnPropertyNames(sc, &props, &n, g, JS_GPN_STRING_MASK) == 0) {
        for (uint32_t i = 0; i < n; i++)
            JS_DefinePropertyValue(ecmascript->auxiliary, base, props[i].atom, JS_TRUE,
                                   JS_PROP_ENUMERABLE);
        JS_FreePropertyEnum(sc, props, n);
        ecmascript->base = base;
    } else {
        clear_exception(sc);
        JS_FreeValue(ecmascript->auxiliary, base);
    }
    JS_FreeValue(sc, g);
    JS_FreeContext(sc);
    return !JS_IsUndefined(ecmascript->base);
}

static int base_has(ta_datamodel_ecmascript *ecmascript, JSAtom a) {
    return JS_GetOwnProperty(ecmascript->auxiliary, NULL, ecmascript->base, a) == 1;
}

static int atom_in(const JSAtom *v, uint32_t n, JSAtom a) {
    for (uint32_t i = 0; i < n; i++)
        if (v[i] == a) return 1;
    return 0;
}

static int atom_add(ta_datamodel_ecmascript *ecmascript, JSAtom **v, uint32_t *n, JSAtom a) {
    if (atom_in(*v, *n, a)) return 1;
    JSAtom *nv = (JSAtom *)realloc(*v, sizeof(JSAtom) * (*n + 1));
    if (!nv) return 0;
    *v = nv;
    nv[(*n)++] = JS_DupAtom(ecmascript->auxiliary, a);
    return 1;
}

/* length of a non-ASCII ECMAScript white space or line terminator at p (UTF-8), else 0 */
static size_t js_space(const unsigned char *p, const unsigned char *end) {
    size_t n = (size_t)(end - p);
    if (n >= 2 && p[0] == 0xC2 && p[1] == 0xA0) return 2; /* U+00A0 */
    if (n < 3) return 0;
    if (p[0] == 0xE1 && p[1] == 0x9A && p[2] == 0x80) return 3; /* U+1680 */
    if (p[0] == 0xE2 && p[1] == 0x80 &&
        ((p[2] >= 0x80 && p[2] <= 0x8A) || p[2] == 0xA8 || p[2] == 0xA9 || p[2] == 0xAF))
        return 3; /* U+2000-200A, U+2028, U+2029, U+202F */
    if (p[0] == 0xE2 && p[1] == 0x81 && p[2] == 0x9F) return 3; /* U+205F */
    if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) return 3; /* U+3000 */
    if (p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return 3; /* U+FEFF */
    return 0;
}

static int hexval(unsigned char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* a \uXXXX or \u{X...} escape at p: its UTF-8 in out (returns bytes consumed, 0 if none) */
static size_t uescape(const unsigned char *p, const unsigned char *end, char *out, size_t *olen) {
    if (end - p < 3 || p[0] != '\\' || p[1] != 'u') return 0;
    uint32_t cp = 0;
    size_t i = 2;
    if (p[2] == '{') {
        for (i = 3; p + i < end && p[i] != '}'; i++) {
            if (hexval(p[i]) < 0 || cp > 0x10FFFF) return 0;
            cp = cp * 16 + (uint32_t)hexval(p[i]);
        }
        if (p + i >= end || i == 3 || cp > 0x10FFFF) return 0;
        i++;
    } else {
        if (end - p < 6) return 0;
        for (; i < 6; i++) {
            if (hexval(p[i]) < 0) return 0;
            cp = cp * 16 + (uint32_t)hexval(p[i]);
        }
    }
    if (cp < 0x80) {
        out[0] = (char)cp;
        *olen = 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 63));
        *olen = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        *olen = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 63));
        out[2] = (char)(0x80 | ((cp >> 6) & 63));
        out[3] = (char)(0x80 | (cp & 63));
        *olen = 4;
    }
    return i;
}

/* Every identifier-like run of src (escapes decoded) onto array arr: a superset
 * of the names it declares. 0 if out of memory. */
static int candidates(JSContext *context, ta_string_view source, JSValue arr) {
    const unsigned char *p = (const unsigned char *)source.pointer, *end = p + source.length;
    char *buffer = NULL;
    size_t capacity = 0, length = 0;
    uint32_t k = 0;
    int ok = 1;
    for (;;) {
        char encoded[4];
        size_t cl = 0, adv = 1;
        if (p < end) {
            unsigned char c = *p;
            if (c >= 0x80) {
                adv = js_space(p, end);
                if (!adv) {
                    adv = 1;
                    encoded[0] = (char)c;
                    cl = 1;
                }
            } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '$') {
                encoded[0] = (char)c;
                cl = 1;
            } else if (c == '\\') {
                size_t u = uescape(p, end, encoded, &cl);
                if (u) adv = u;
            }
        }
        if (cl) {
            if (length + cl > capacity) {
                size_t nc = capacity ? capacity * 2 : 64;
                char *nb = (char *)realloc(buffer, nc);
                if (!nb) {
                    ok = 0;
                    break;
                }
                buffer = nb;
                capacity = nc;
            }
            memcpy(buffer + length, encoded, cl);
            length += cl;
        } else if (length) {
            if (JS_SetPropertyUint32(context, arr, k++, JS_NewStringLen(context, buffer, length)) <
                0) {
                ok = 0;
                break;
            }
            length = 0;
        }
        if (p >= end) break;
        p += adv;
    }
    free(buffer);
    return ok;
}

/* In a fresh scratch context, before any chart code is instantiated there (a
 * function declaration may replace any global): which of the names are in
 * their temporal dead zone, i.e. declared by let/const/class. */
static const char PROBE_JS[] =
    "(function(ev,RE,oc){return function(ns){var out=[],seen=oc(null);"
    "for(var i=0;i<ns.length;i++){var n=ns[i];if(seen[n])continue;seen[n]=1;"
    "try{ev('typeof '+n)}catch(e){if(e instanceof RE)out[out.length]=n}}return out}})"
    "(eval,ReferenceError,Object.create)";

static int function_in_file(JSContext *context, JSValueConst f, const char *file) {
    JSValue v = JS_GetPropertyStr(context, f, "fileName");
    const char *s = JS_IsString(v) ? JS_ToCString(context, v) : NULL;
    int r = s && !strcmp(s, file);
    if (s) JS_FreeCString(context, s);
    JS_FreeValue(context, v);
    clear_exception(context);
    return r;
}

static int32_t int_prop(JSContext *context, JSValueConst o, const char *name) {
    int32_t r = 0;
    JSValue v = JS_GetPropertyStr(context, o, name);
    if (JS_ToInt32(context, &r, v) < 0) clear_exception(context);
    JS_FreeValue(context, v);
    return r;
}

static int fdecl_add(ta_datamodel_ecmascript *ecmascript, ecmascript_chart *chart, JSContext *sc,
                     JSAtom a, JSValueConst f, uint32_t e, int strict) {
    size_t length = 0;
    const char *s = JS_ToCStringLen(sc, &length, f);
    ecmascript_function_declaration *nf =
        s ? (ecmascript_function_declaration *)realloc(chart->functions,
                                                       sizeof(ecmascript_function_declaration) *
                                                           (chart->function_count + 1))
          : NULL;
    char *source = nf ? (char *)malloc(length + 1) : NULL;
    if (nf) chart->functions = nf;
    if (!source) {
        if (s) JS_FreeCString(sc, s);
        clear_exception(sc);
        return 0;
    }
    memcpy(source, s, length);
    source[length] = 0;
    JS_FreeCString(sc, s);
    ecmascript_function_declaration *declaration = &chart->functions[chart->function_count++];
    declaration->name = JS_DupAtom(ecmascript->auxiliary, a);
    declaration->script = e;
    declaration->line = int_prop(sc, f, "lineNumber"); /* as in the session: one line down */
    declaration->column = int_prop(sc, f, "columnNumber");
    declaration->strict = strict;
    declaration->source = source;
    return 1;
}

typedef struct ecmascript_scratch {
    JSContext *context;
    JSValue probe; /* PROBE_JS, made before any chart code */
} ecmascript_scratch;

static int scratch_new(ta_datamodel_ecmascript *ecmascript, ecmascript_scratch *scratch) {
    scratch->context = JS_NewContext(ecmascript->runtime);
    if (!scratch->context) return 0;
    scratch->probe =
        JS_Eval(scratch->context, PROBE_JS, sizeof PROBE_JS - 1, "<probe>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(scratch->probe)) return 1;
    clear_exception(scratch->context);
    JS_FreeContext(scratch->context);
    scratch->context = NULL;
    return 0;
}

static void scratch_free(ecmascript_scratch *scratch) {
    if (!scratch->context) return;
    JS_FreeValue(scratch->context, scratch->probe);
    JS_FreeContext(scratch->context);
    scratch->context = NULL;
}

/* Script e of the chart, instantiated in scratch context `sc` (which may hold
 * the chart's earlier scripts): 1 if it declares top-level names (recorded in
 * cc), 0 if not, -1 if out of memory, 2 if it conflicts with an earlier
 * script there (ask again in a fresh one). */
static int analyze_script(ta_datamodel_ecmascript *ecmascript, ecmascript_chart *chart,
                          ecmascript_scratch *scratch, uint32_t e, ta_string_view source) {
    JSContext *sc = scratch->context;
    char file[32];
    snprintf(file, sizeof file, "<script:%u>", (unsigned)e);
    JSValue c =
        eval_file(sc, "", source, "", file, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(c)) { /* does not compile: never declares anything */
        clear_exception(sc);
        return 0;
    }
    JS_FreeValue(sc, c);
    /* strict code (a "use strict" directive) cannot have a with statement */
    c = eval_file(sc, "", source, "\n;with({}){}", file,
                  JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    int strict = JS_IsException(c);
    JS_FreeValue(sc, c);
    clear_exception(sc);
    JSValue r = eval_file(sc, strict ? "'use strict';throw 0;\n" : "throw 0;\n", source, "", file,
                          JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(sc, r);
    JSValue x = JS_GetException(sc);
    int hoisted = JS_VALUE_GET_TAG(x) == JS_TAG_INT && JS_VALUE_GET_INT(x) == 0;
    JS_FreeValue(sc, x);
    if (!hoisted) return 2; /* a conflict, or (in a fresh context) it never instantiates */
    int rc = 0;
    JSValue g = JS_GetGlobalObject(sc);
    JSPropertyEnum *props;
    uint32_t n;
    if (JS_GetOwnPropertyNames(sc, &props, &n, g, JS_GPN_STRING_MASK) < 0) {
        clear_exception(sc);
        JS_FreeValue(sc, g);
        return -1;
    }
    for (uint32_t i = 0; i < n && rc >= 0; i++) {
        JSValue v = JS_GetProperty(sc, g, props[i].atom);
        int function = JS_IsFunction(sc, v) && function_in_file(sc, v, file);
        if (function || !base_has(ecmascript, props[i].atom)) {
            rc = atom_add(ecmascript, &chart->variables, &chart->variable_count, props[i].atom)
                     ? 1
                     : -1;
            if (function && rc > 0 &&
                !fdecl_add(ecmascript, chart, sc, props[i].atom, v, e, strict))
                rc = -1;
        }
        JS_FreeValue(sc, v);
    }
    JS_FreePropertyEnum(sc, props, n);
    JS_FreeValue(sc, g);
    if (rc < 0) return rc;
    JSValue cands = JS_NewArray(sc);
    JSValue lex = JS_IsException(cands) || !candidates(sc, source, cands)
                      ? JS_EXCEPTION
                      : JS_Call(sc, scratch->probe, JS_UNDEFINED, 1, (JSValueConst *)&cands);
    JS_FreeValue(sc, cands);
    int64_t nl = 0;
    if (JS_IsException(lex) || JS_GetLength(sc, lex, &nl) < 0) {
        rc = -1;
    } else {
        for (uint32_t i = 0; i < (uint32_t)nl && rc >= 0; i++) {
            JSValue v = JS_GetPropertyUint32(sc, lex, i);
            JSAtom a = JS_ValueToAtom(sc, v);
            rc = a != JS_ATOM_NULL &&
                         atom_add(ecmascript, &chart->lexical_names, &chart->lexical_name_count, a)
                     ? 1
                     : -1;
            JS_FreeAtom(sc, a);
            JS_FreeValue(sc, v);
        }
    }
    JS_FreeValue(sc, lex);
    clear_exception(sc);
    return rc;
}

/* a declaration needs one of these keywords, literally (keywords have no escapes) */
static int may_declare(ta_string_view s) {
    static const char *const kw[] = {"var", "let", "const", "function", "class"};
    for (uint32_t k = 0; k < 5; k++) {
        size_t n = strlen(kw[k]);
        for (uint32_t i = 0; i + n <= s.length; i++)
            if (!memcmp(s.pointer + i, kw[k], n)) return 1;
    }
    return 0;
}

/* *declares: does any <script> of the chart declare top-level names? */
static ta_status chart_analyze(ta_datamodel_ecmascript *ecmascript, ecmascript_chart *chart,
                               int *declares) {
    *declares = 0;
    ecmascript_scratch shared = {NULL, JS_UNDEFINED};
    ta_status status = TA_OK;
    for (uint32_t e = 0; e < chart->expression_count && status == TA_OK; e++) {
        ta_expression_info info;
        if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, e}, &info) != TA_OK ||
            info.kind != TA_EXPRESSION_SCRIPT || !may_declare(info.source))
            continue;
        if (!base_names(ecmascript) || (!shared.context && !scratch_new(ecmascript, &shared))) {
            status = TA_ERROR_NO_MEMORY;
            break;
        }
        ecmascript->operations = 0;
        int r = analyze_script(ecmascript, chart, &shared, e, info.source);
        if (r == 2) { /* alone, then */
            ecmascript_scratch fresh;
            if (!scratch_new(ecmascript, &fresh)) {
                status = TA_ERROR_NO_MEMORY;
                break;
            }
            r = analyze_script(ecmascript, chart, &fresh, e, info.source);
            scratch_free(&fresh);
            if (r == 2) r = 0; /* it never instantiates */
        }
        if (r < 0) status = TA_ERROR_NO_MEMORY;
        if (r > 0) *declares = 1;
    }
    scratch_free(&shared);
    return status;
}

/* The document line of a function defined by chart code (0: unknown). */
static uint32_t function_line(JSContext *context, const ecmascript_chart *chart, JSValueConst f) {
    JSValue fv = JS_GetPropertyStr(context, f, "fileName");
    const char *s = JS_IsString(fv) ? JS_ToCString(context, fv) : NULL;
    int32_t ln = int_prop(context, f, "lineNumber");
    unsigned id = 0; /* the source starts on the code's second line */
    int ok = s && (sscanf(s, "<script:%u>", &id) == 1 || sscanf(s, "<expr:%u>", &id) == 1);
    if (s) JS_FreeCString(context, s);
    JS_FreeValue(context, fv);
    clear_exception(context);
    ta_expression_info info;
    if (!ok || ln < 2 ||
        ta_image_expression(chart->image, (ta_chart_item){chart->chart, id}, &info) != TA_OK ||
        !info.line)
        return 0;
    return info.line + (uint32_t)(ln - 2);
}

/* The shared realm: one context per datamodel with In() and the XML DOM
 * prelude installed, then the global object frozen so a session cannot
 * create or replace globals seen by other sessions. Builtin prototypes stay
 * mutable (freezing them breaks ordinary code through the "override
 * mistake"); charts that do not trust each other should use
 * TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED. */
static JSContext *shared_realm(ta_datamodel_ecmascript *ecmascript) {
    if (ecmascript->shared) return ecmascript->shared;
    JSContext *context = JS_NewContext(ecmascript->runtime);
    if (!context) return NULL;
    JSValue g = JS_GetGlobalObject(context);
    JS_DefinePropertyValueStr(context, g, "In", JS_NewCFunction(context, js_in, "In", 1), 0);
    JS_FreeValue(context, g);
    static const char lock[] = "Object.freeze(globalThis);";
    JSValue r = JS_Eval(context, XML_PRELUDE, sizeof XML_PRELUDE - 1, "<xml>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(r)) {
        JS_FreeValue(context, r);
        r = JS_Eval(context, lock, sizeof lock - 1, "<lock>", JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(r)) {
        clear_exception(context);
        JS_FreeContext(context);
        return NULL;
    }
    JS_FreeValue(context, r);
    ecmascript->shared = context;
    return context;
}

/* ---- vtable ---------------------------------------------------------------------------- */

static void chart_free(ta_datamodel_ecmascript *ecmascript, ecmascript_chart *chart) {
    for (uint32_t e = 0; chart->getter && e < chart->expression_count; e++) {
        JS_FreeValueRT(ecmascript->runtime, chart->getter[e]);
        JS_FreeValueRT(ecmascript->runtime, chart->setter[e]);
    }
    free(chart->getter);
    free(chart->setter);
    for (uint32_t i = 0; i < chart->variable_count; i++)
        JS_FreeAtomRT(ecmascript->runtime, chart->variables[i]);
    for (uint32_t i = 0; i < chart->lexical_name_count; i++)
        JS_FreeAtomRT(ecmascript->runtime, chart->lexical_names[i]);
    for (uint32_t i = 0; i < chart->function_count; i++) {
        JS_FreeAtomRT(ecmascript->runtime, chart->functions[i].name);
        free(chart->functions[i].source);
    }
    free(chart->variables);
    free(chart->lexical_names);
    free(chart->functions);
    free(chart);
}

/* a chart lost its last system or session: idle (kept), evicting the oldest */
static void chart_unused(ta_datamodel_ecmascript *ecmascript, ecmascript_chart *chart) {
    if (chart->refs || chart->instance_count) return;
    chart->idle_since = ++ecmascript->clock;
    ecmascript->idle_count++;
    while (ecmascript->idle_count > ECMASCRIPT_IDLE_CHARTS) {
        uint32_t old = NONE;
        for (uint32_t i = 0; i < ecmascript->chart_count; i++) {
            ecmascript_chart *c = ecmascript->charts[i];
            if (!c->refs && !c->instance_count &&
                (old == NONE || c->idle_since < ecmascript->charts[old]->idle_since))
                old = i;
        }
        if (old == NONE) break;
        chart_free(ecmascript, ecmascript->charts[old]);
        ecmascript->charts[old] = ecmascript->charts[--ecmascript->chart_count];
        ecmascript->idle_count--;
    }
}

static void chart_use(ta_datamodel_ecmascript *ecmascript, ecmascript_chart *chart, int session) {
    if (!chart->refs && !chart->instance_count) ecmascript->idle_count--;
    if (session)
        chart->instance_count++;
    else
        chart->refs++;
}

static ta_status ecmascript_attach(void *datamodel, const ta_datamodel_chart *chart,
                                   void **context) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(chart->image, &image_info);
    /* the same chart attached again: share it. A chart in use is shared only
     * through the same image (its image must stay valid while used); an idle
     * one is adopted by any image with the same content. */
    for (uint32_t i = 0; i < ecmascript->chart_count; i++) {
        ecmascript_chart *c = ecmascript->charts[i];
        if (c->hash != image_info.content_hash || c->chart != chart->chart ||
            c->binding != chart->binding)
            continue;
        int idle = !c->refs && !c->instance_count;
        if (!idle && c->image != chart->image) continue;
        c->image = chart->image;
        c->segment = chart->segment;
        chart_use(ecmascript, c, 0);
        *context = c;
        return TA_OK;
    }
    ecmascript_chart *chart_state = (ecmascript_chart *)calloc(1, sizeof *chart_state);
    ecmascript_chart **list =
        (ecmascript_chart **)grow(ecmascript->charts, &ecmascript->chart_capacity,
                                  ecmascript->chart_count + 1, sizeof(ecmascript_chart *));
    if (!chart_state || !list) {
        free(chart_state);
        return TA_ERROR_NO_MEMORY;
    }
    ecmascript->charts = list;
    ecmascript->charts[ecmascript->chart_count++] = chart_state;
    chart_state->image = chart->image;
    chart_state->hash = image_info.content_hash;
    chart_state->refs = 1;
    chart_state->chart = chart->chart;
    chart_state->segment = chart->segment;
    chart_state->binding = chart->binding;
    chart_state->expression_count = ta_image_expression_count(chart->image, chart->chart);
    int declares = 0;
    if (chart_analyze(ecmascript, chart_state, &declares) != TA_OK)
        return TA_ERROR_NO_MEMORY; /* freed with the datamodel */
    chart_state->shared = ecmascript->realm == TA_DATAMODEL_ECMASCRIPT_REALM_AUTO && !declares;
    if (chart_state->shared) {
        chart_state->getter =
            (JSValue *)malloc(sizeof(JSValue) * (chart_state->expression_count + 1));
        chart_state->setter =
            (JSValue *)malloc(sizeof(JSValue) * (chart_state->expression_count + 1));
        if (!chart_state->getter || !chart_state->setter)
            return TA_ERROR_NO_MEMORY; /* freed with the datamodel */
        for (uint32_t i = 0; i < chart_state->expression_count; i++)
            chart_state->getter[i] = chart_state->setter[i] = JS_UNDEFINED;
    }
    *context = chart_state;
    return TA_OK;
}

static void ecmascript_detach(void *datamodel, void *context) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ecmascript_chart *chart = (ecmascript_chart *)context;
    if (!chart || !chart->refs) return;
    chart->refs--;
    chart_unused(ecmascript,
                 chart); /* kept while sessions of it live (they may outlive the detach) */
}

static void def_ro(JSContext *context, JSValueConst obj, const char *name, JSValue v) {
    JS_DefinePropertyValueStr(context, obj, name, v, JS_PROP_ENUMERABLE);
}

/* The session's realm: a scope object in the shared context, or a context of
 * its own with In(). 0 if out of memory (nothing left allocated). */
static int instance_realm(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                          ecmascript_chart *chart) {
    instance->chart_context = chart;
    instance->scope = instance->init = instance->declarations = JS_UNDEFINED;
    if (chart->shared) {
        instance->context = shared_realm(ecmascript);
        if (!instance->context) return 0;
        /* null prototype: only the session's own names shadow the builtins */
        JSValue s = JS_NewObjectProto(instance->context, JS_NULL);
        if (JS_IsException(s)) return 0;
        instance->scope = s;
        return 1;
    }
    instance->context = JS_NewContext(ecmascript->runtime);
    instance->getter = (JSValue *)malloc(sizeof(JSValue) * (chart->expression_count + 1));
    instance->setter = (JSValue *)malloc(sizeof(JSValue) * (chart->expression_count + 1));
    if (!instance->context || !instance->getter || !instance->setter) {
        if (instance->context) JS_FreeContext(instance->context);
        free(instance->getter);
        free(instance->setter);
        instance->context = NULL;
        instance->getter = instance->setter = NULL;
        return 0;
    }
    for (uint32_t i = 0; i < chart->expression_count; i++)
        instance->getter[i] = instance->setter[i] = JS_UNDEFINED;
    JS_SetContextOpaque(instance->context, instance);
    JSValue g = JS_GetGlobalObject(instance->context);
    JS_DefinePropertyValueStr(instance->context, g, "In",
                              JS_NewCFunction(instance->context, js_in, "In", 1),
                              JS_PROP_CONFIGURABLE);
    JS_FreeValue(instance->context, g);
    return 1;
}

/* the object holding the session's variables (owned by the caller) */
static JSValue instance_variables(ecmascript_instance *instance) {
    return instance->chart_context->shared ? JS_DupValue(instance->context, instance->scope)
                                           : JS_GetGlobalObject(instance->context);
}

static ta_status ecmascript_create(void *datamodel, const ta_datamodel_start *start_,
                                   uint64_t *out) {
    void *chart_context = start_->chart_context;
    const ta_datamodel_system_variables *variables = start_->system_variables;
    ta_payload init = start_->init;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ecmascript_chart *chart = (ecmascript_chart *)chart_context;
    /* freed sessions stay in their slot as non-live tombstones, threaded
     * through next_free, and are reused (sessions churn under <invoke>) */
    uint32_t slot = ecmascript->free_slot;
    ecmascript_instance *instance;
    if (slot != NONE) {
        instance = ecmascript->instances[slot];
        ecmascript->free_slot = instance->next_free;
        memset(instance, 0, sizeof *instance);
    } else {
        ecmascript_instance **list = (ecmascript_instance **)grow(
            ecmascript->instances, &ecmascript->instance_capacity, ecmascript->instance_count + 1,
            sizeof(ecmascript_instance *));
        if (list) ecmascript->instances = list;
        instance = list ? (ecmascript_instance *)calloc(1, sizeof *instance) : NULL;
        if (!instance) return TA_ERROR_NO_MEMORY;
        slot = ecmascript->instance_count++;
        ecmascript->instances[slot] = instance;
    }
    chart_use(ecmascript, chart, 1);
    if (!instance_realm(ecmascript, instance, chart)) goto nomem;
    JSContext *context = instance->context;
    JSValue g = instance_variables(instance);
    def_ro(context, g, "_sessionid",
           JS_NewStringLen(context, variables->sessionid.pointer, variables->sessionid.length));
    def_ro(context, g, "_name",
           JS_NewStringLen(context, variables->name.pointer, variables->name.length));
    JSValue io = JS_NewObject(context);
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        JSValue e = JS_NewObject(context);
        JS_SetPropertyStr(context, e, "location",
                          JS_NewStringLen(context, variables->io_processors[i].location.pointer,
                                          variables->io_processors[i].location.length));
        char key[256];
        uint32_t kl = variables->io_processors[i].type.length < 255
                          ? variables->io_processors[i].type.length
                          : 255;
        memcpy(key, variables->io_processors[i].type.pointer, kl);
        key[kl] = 0;
        JS_SetPropertyStr(context, io, key, JS_DupValue(context, e));
        /* short names used by many documents and the W3C tests */
        const char *alias = NULL;
        if (kl > 20 && !strcmp(key + kl - 20, "#SCXMLEventProcessor")) alias = "scxml";
        if (kl > 24 && !strcmp(key + kl - 24, "#BasicHTTPEventProcessor")) alias = "basichttp";
        if (alias) JS_SetPropertyStr(context, io, alias, JS_DupValue(context, e));
        JS_FreeValue(context, e);
    }
    def_ro(context, g, "_ioprocessors", io);
    uint32_t nd = ta_image_data_count(chart->image, chart->chart);
    for (uint32_t i = 0; i < nd; i++) { /* declared from the start (late binding: undefined);
                                           configurable: a live migration may drop them */
        ta_data_info data_info;
        ta_image_data(chart->image, (ta_chart_item){chart->chart, i}, &data_info);
        JS_DefinePropertyValueStr(context, g, data_info.id.pointer, JS_UNDEFINED, JS_PROP_C_W_E);
    }
    JS_FreeValue(context, g);
    instance->init = JS_DupValue(context, payload_get(ecmascript, init));
    ecmascript_release(ecmascript, init); /* the init payload reference is passed to us */
    instance->live = 1;
    *out = (uint64_t)slot + 1;
    return TA_OK;
nomem:
    chart->instance_count--;
    chart_unused(ecmascript, chart);
    memset(instance, 0, sizeof *instance);
    instance->next_free = ecmascript->free_slot;
    ecmascript->free_slot = slot;
    ecmascript_release(ecmascript, init);
    return TA_ERROR_NO_MEMORY;
}

static void instance_free(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance) {
    if (!instance || !instance->live) return;
    JS_FreeValue(instance->context, instance->init);
    JS_FreeValue(instance->context, instance->declarations);
    free(instance->string);
    instance->string = NULL;
    if (instance->chart_context->shared) {
        JS_FreeValue(instance->context, instance->scope); /* the shared context stays */
    } else {
        for (uint32_t i = 0; i < instance->chart_context->expression_count; i++) {
            JS_FreeValue(instance->context, instance->getter[i]);
            JS_FreeValue(instance->context, instance->setter[i]);
        }
        free(instance->getter);
        free(instance->setter);
        JS_FreeContext(instance->context);
    }
    instance->getter = instance->setter = NULL;
    instance->scope = JS_UNDEFINED;
    instance->context = NULL;
    instance->live = 0;
    if (ecmascript->current == instance) ecmascript->current = NULL;
    instance->chart_context->instance_count--;
    chart_unused(ecmascript, instance->chart_context);
}

static void ecmascript_destroy(const ta_datamodel_call *call) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ecmascript_instance *instance = I(ecmascript, handle);
    if (!instance || !instance->live) return;
    instance_free(ecmascript, instance);
    instance->next_free = ecmascript->free_slot;
    ecmascript->free_slot = (uint32_t)(handle - 1);
}

#define ENTER(es, h, env_)                                                                         \
    ecmascript_instance *instance = I((es), (h));                                                  \
    if (!instance || !instance->live) return TA_ERROR_EXECUTION;                                   \
    instance->environment = (env_)

static ta_status ecmascript_init_data_(void *datamodel, uint64_t handle,
                                       ta_datamodel_environment *environment,
                                       uint32_t declaration) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    ta_data_info data_info;
    if (ta_image_data(instance->chart_context->image,
                      (ta_chart_item){instance->chart_context->chart, declaration},
                      &data_info) != TA_OK)
        return TA_ERROR_EXECUTION;
    JSValue v = JS_UNDEFINED;
    ta_status status = TA_OK;
    int from_init = 0;
    if (JS_IsObject(instance->init)) {
        JSAtom a = JS_NewAtom(instance->context, data_info.id.pointer);
        if (JS_HasProperty(instance->context, instance->init, a) == 1) {
            v = JS_GetProperty(instance->context, instance->init, a);
            from_init = 1;
        }
        JS_FreeAtom(instance->context, a);
    }
    if (!from_init && data_info.expression != TA_NIL) {
        status = value_of(ecmascript, instance, data_info.expression, &v);
        if (status != TA_OK) v = JS_UNDEFINED;
    }
    JSValue g = instance->chart_context->shared ? JS_DupValue(instance->context, instance->scope)
                                                : JS_GetGlobalObject(instance->context);
    JS_SetPropertyStr(instance->context, g, data_info.id.pointer, v);
    JS_FreeValue(instance->context, g);
    instance->environment = NULL;
    return status;
}

static JSValue str_or_undef(JSContext *context, ta_string_view s) {
    return s.length ? JS_NewStringLen(context, s.pointer, s.length) : JS_UNDEFINED;
}

static ta_status ecmascript_bind_event_(void *datamodel, uint64_t handle,
                                        ta_datamodel_environment *environment,
                                        const ta_event_view *view) {
    (void)environment;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ecmascript_instance *instance = I(ecmascript, handle);
    if (!instance || !instance->live) return TA_ERROR_EXECUTION;
    JSContext *context = instance->context;
    static const char *const types[] = {"platform", "internal", "external"};
    JSValue o = JS_NewObject(context);
    JS_SetPropertyStr(context, o, "name",
                      JS_NewStringLen(context, view->name.pointer, view->name.length));
    JS_SetPropertyStr(context, o, "type", JS_NewString(context, types[view->type % 3]));
    JS_SetPropertyStr(context, o, "sendid", str_or_undef(context, view->sendid));
    JS_SetPropertyStr(context, o, "origin", str_or_undef(context, view->origin));
    JS_SetPropertyStr(context, o, "origintype", str_or_undef(context, view->origintype));
    JS_SetPropertyStr(context, o, "invokeid", str_or_undef(context, view->invokeid));
    JS_SetPropertyStr(context, o, "data",
                      JS_DupValue(context, payload_get(ecmascript, view->data)));
    if (view->size >= sizeof *view && view->raw.length)
        JS_SetPropertyStr(context, o, "raw",
                          JS_NewStringLen(context, view->raw.pointer, view->raw.length));
    JSValue g = instance->chart_context->shared ? JS_DupValue(context, instance->scope)
                                                : JS_GetGlobalObject(context);
    JS_DefinePropertyValueStr(context, g, "_event", o, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(context, g);
    return TA_OK;
}

static ta_status ecmascript_condition_(void *datamodel, uint64_t handle,
                                       ta_datamodel_environment *environment,
                                       ta_expression expression, int *out) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    JSValue v;
    ta_status status = value_of(ecmascript, instance, expression, &v);
    if (status == TA_OK) {
        int b = JS_ToBool(instance->context, v);
        JS_FreeValue(instance->context, v);
        if (b < 0) {
            clear_exception(instance->context);
            status = TA_ERROR_EXECUTION;
        } else {
            *out = b;
        }
    }
    instance->environment = NULL;
    return status;
}

static ta_status ecmascript_string_(void *datamodel, uint64_t handle,
                                    ta_datamodel_environment *environment, ta_expression expression,
                                    ta_string_view *out) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    JSValue v;
    ta_status status = value_of(ecmascript, instance, expression, &v);
    if (status == TA_OK) status = to_string_out(instance, v, 0, out);
    instance->environment = NULL;
    return status;
}

static ta_status ecmascript_assign_(void *datamodel, uint64_t handle,
                                    ta_datamodel_environment *environment, ta_expression location,
                                    ta_expression value) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    JSValue v;
    ta_status status = value_of(ecmascript, instance, value, &v);
    if (status == TA_OK) status = assign_to(ecmascript, instance, location, v);
    instance->environment = NULL;
    return status;
}

static ta_status ecmascript_assign_string(const ta_datamodel_call *call, ta_expression location,
                                          ta_string_view v) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, NULL);
    return assign_to(ecmascript, instance, location,
                     JS_NewStringLen(instance->context, v.pointer, v.length));
}

static ta_status ecmascript_script_(void *datamodel, uint64_t handle,
                                    ta_datamodel_environment *environment,
                                    ta_expression expression) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    JSValue v;
    ta_status status = value_of(ecmascript, instance, expression, &v);
    if (status == TA_OK) JS_FreeValue(instance->context, v);
    instance->environment = NULL;
    return status;
}

static ta_status ecmascript_log_(void *datamodel, uint64_t handle,
                                 ta_datamodel_environment *environment, ta_expression expression,
                                 ta_string_view *out) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    JSValue v;
    ta_status status = value_of(ecmascript, instance, expression, &v);
    if (status == TA_OK) status = to_string_out(instance, v, 1, out);
    instance->environment = NULL;
    return status;
}

static ta_status ecmascript_foreach_begin_(void *datamodel, uint64_t handle,
                                           ta_datamodel_environment *environment, ta_expression arr,
                                           uint64_t *iter, uint32_t *n) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    JSValue a;
    ta_status status = value_of(ecmascript, instance, arr, &a);
    instance->environment = NULL;
    if (status != TA_OK) return status;
    int64_t length = 0;
    if (!JS_IsArray(a) || JS_GetLength(instance->context, a, &length) < 0 || length < 0 ||
        length > 0x7FFFFFFF) {
        clear_exception(instance->context);
        JS_FreeValue(instance->context, a);
        return TA_ERROR_EXECUTION;
    }
    uint32_t slot;
    if (ecmascript->iteration_free != NONE) {
        slot = ecmascript->iteration_free;
        ecmascript->iteration_free =
            ecmascript->iterations[slot].count; /* free list threaded through n */
    } else {
        ecmascript_iterator *iterator = (ecmascript_iterator *)grow(
            ecmascript->iterations, &ecmascript->iteration_capacity,
            ecmascript->iteration_count + 1, sizeof(ecmascript_iterator));
        if (!iterator) {
            JS_FreeValue(instance->context, a);
            return TA_ERROR_NO_MEMORY;
        }
        ecmascript->iterations = iterator;
        slot = ecmascript->iteration_count++;
    }
    ecmascript_iterator *iterator = &ecmascript->iterations[slot];
    iterator->context = instance->context;
    iterator->count = (uint32_t)length;
    iterator->items = (JSValue *)malloc(sizeof(JSValue) * (iterator->count + 1));
    for (uint32_t i = 0; i < iterator->count; i++) /* shallow copy */
        iterator->items[i] = JS_GetPropertyUint32(instance->context, a, i);
    JS_FreeValue(instance->context, a);
    *iter = (uint64_t)slot + 1;
    *n = iterator->count;
    return TA_OK;
}

static ta_status ecmascript_foreach_item(const ta_datamodel_call *call,
                                         const ta_datamodel_loop *loop) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    uint64_t iter = loop->iter;
    uint32_t i = loop->i;
    ta_expression item = loop->item;
    ta_expression index = loop->index;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, NULL);
    ecmascript_iterator *iterator = &ecmascript->iterations[iter - 1];
    declare_if_missing(instance, item);
    ta_status status =
        assign_to(ecmascript, instance, item, JS_DupValue(instance->context, iterator->items[i]));
    if (status == TA_OK && index != TA_NIL) {
        declare_if_missing(instance, index);
        status = assign_to(ecmascript, instance, index, JS_NewInt32(instance->context, (int32_t)i));
    }
    return status;
}

static void ecmascript_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    void *datamodel = call->datamodel;
    uint64_t iter = loop->iter;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    if (!iter || iter > ecmascript->iteration_count) return;
    ecmascript_iterator *iterator = &ecmascript->iterations[iter - 1];
    for (uint32_t i = 0; i < iterator->count; i++)
        JS_FreeValue(iterator->context, iterator->items[i]);
    free(iterator->items);
    iterator->items = NULL;
    iterator->count = ecmascript->iteration_free;
    ecmascript->iteration_free = (uint32_t)(iter - 1);
}

static ta_status ecmascript_capture_(void *datamodel, uint64_t handle,
                                     ta_datamodel_environment *environment,
                                     const ta_datamodel_capture *capture, ta_payload *out) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    ta_status status = TA_OK;
    JSValue v;
    if (capture->content != TA_NIL) {
        status = value_of(ecmascript, instance, capture->content, &v);
    } else {
        v = JS_NewObject(instance->context);
        for (uint32_t i = 0; i < capture->namelist_count && status == TA_OK; i++) {
            JSValue x;
            status = value_of(ecmascript, instance, capture->namelist[i], &x);
            if (status != TA_OK) break;
            ta_string_view name = expression_source(instance, capture->namelist[i], NULL);
            char key[256];
            uint32_t kl = name.length < 255 ? name.length : 255;
            memcpy(key, name.pointer, kl);
            key[kl] = 0;
            JS_SetPropertyStr(instance->context, v, key, x);
        }
        for (uint32_t i = 0; i < capture->param_count && status == TA_OK; i++) {
            JSValue x;
            status = value_of(ecmascript, instance, capture->params[2 * i + 1], &x);
            if (status != TA_OK) break;
            ta_string_view name =
                ta_image_string(instance->chart_context->image, capture->params[2 * i]);
            JS_SetPropertyStr(instance->context, v, name.pointer,
                              x); /* image strings are NUL-terminated */
        }
        if (status != TA_OK) JS_FreeValue(instance->context, v);
    }
    instance->environment = NULL;
    if (status != TA_OK) return status;
    *out = payload_new(ecmascript, v);
    if (!*out) {
        JS_FreeValue(instance->context, v);
        return TA_ERROR_NO_MEMORY;
    }
    return TA_OK;
}

/* ---- storing sessions: hibernation, export, snapshots ------------------------------------
 * A shared-realm session stores its scope (its variables and _event; system
 * variables come back from the system on wake) as one QuickJS object blob.
 * An isolated session stores its global state — [1, chart hash, state] with
 * state = { g: [[name, value, flags]] global object properties the chart made
 * (<data>, var, implicit globals, _event), f: [[name, script, line, column]]
 * function declarations, re-run on restore, l: [[name, kind, value]] global
 * let/const/class bindings (kind 0: uninitialized, 1: let, 2: const),
 * s: system variables (snapshots), i: invoke params }. JS_WriteObject keeps
 * the object graph (shared references, cycles, Date/Map/Set/typed arrays);
 * what it cannot write — a function other than an unchanged top-level
 * function declaration, an Error, a Promise... — is TA_ERROR_UNSUPPORTED, and
 * ta_datamodel_ecmascript_last_error names the variable (and the line). Changes to
 * builtins are not stored. */

static int is_sysvar(const char *n) {
    return !strcmp(n, "_sessionid") || !strcmp(n, "_name") || !strcmp(n, "_ioprocessors");
}

#define RESTORE_TEMPORARY "__ta_restore__"

/* globals of an isolated session that are the datamodel's, not the chart's */
static int is_internal(const char *n) {
    return is_sysvar(n) || !strcmp(n, "In") || !strcmp(n, "__ta_xml") ||
           !strcmp(n, RESTORE_TEMPORARY);
}

static void set_error(ta_datamodel_ecmascript *ecmascript, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(ecmascript->error, sizeof ecmascript->error, format, ap);
    va_end(ap);
}

static void hex_hash(uint64_t handle, char out[24]) {
    snprintf(out, 24, "%016llx", (unsigned long long)handle);
}

static uint32_t u32_at(JSContext *context, JSValueConst arr, uint32_t i) {
    uint32_t v = 0;
    JSValue x = JS_GetPropertyUint32(context, arr, i);
    if (JS_ToUint32(context, &v, x) < 0) clear_exception(context);
    JS_FreeValue(context, x);
    return v;
}

static int32_t i32_at(JSContext *context, JSValueConst arr, uint32_t i) {
    int32_t v = 0;
    JSValue x = JS_GetPropertyUint32(context, arr, i);
    if (JS_ToInt32(context, &v, x) < 0) clear_exception(context);
    JS_FreeValue(context, x);
    return v;
}

static void push(JSContext *context, JSValueConst arr, uint32_t *n, JSValue v) {
    JS_SetPropertyUint32(context, arr, (*n)++, v);
}

static JSValue tuple(JSContext *context, int n, JSValue *v) { /* consumes v */
    JSValue a = JS_NewArray(context);
    for (int i = 0; i < n; i++)
        JS_SetPropertyUint32(context, a, (uint32_t)i, v[i]);
    return a;
}

static void free_descriptor(JSContext *context, JSPropertyDescriptor *d) {
    JS_FreeValue(context, d->value);
    JS_FreeValue(context, d->getter);
    JS_FreeValue(context, d->setter);
}

static int is_data_id(const ecmascript_chart *chart, const char *name) {
    uint32_t nd = ta_image_data_count(chart->image, chart->chart);
    size_t l = strlen(name);
    for (uint32_t d = 0; d < nd; d++) {
        ta_data_info data_info;
        if (ta_image_data(chart->image, (ta_chart_item){chart->chart, d}, &data_info) == TA_OK &&
            data_info.id.length == l && !memcmp(data_info.id.pointer, name, l))
            return 1;
    }
    return 0;
}

/* Write with a quarter of the stack: reading back needs more stack per
 * nesting level than writing, so whatever is written can be read again (a
 * value too deep fails here, cleanly — not at the next load, which would brick
 * the session). NULL with the exception pending on failure. */
static uint8_t *write_obj(ta_datamodel_ecmascript *ecmascript, JSContext *context, JSValueConst v,
                          size_t *sz) {
    *sz = 0;
    JS_UpdateStackTop(ecmascript->runtime);
    JS_SetMaxStackSize(ecmascript->runtime, ecmascript->stack_size / 4);
    uint8_t *b = JS_WriteObject(context, sz, v, JS_WRITE_OBJ_REFERENCE);
    JS_SetMaxStackSize(ecmascript->runtime, ecmascript->stack_size);
    return b;
}

/* Finds what in a value JS_WriteObject cannot write: [path, kind, detail]
 * with kind 0 a function (detail: it), 1 a class (detail: it), 2 an object of
 * another class (detail: its name); null if nothing is found. */
static const char EXPLAIN_JS[] =
    "(function(ts,fts,keys,isA,S,J){"
    "var ok=/^\\[object (Object|Array|Date|RegExp|Map|Set|ArrayBuffer|Number|String|Boolean|"
    "BigInt|(Big)?(Int|Uint|Float)(8|16|32|64)(Clamped)?Array)\\]$/,id=/^[A-Za-z_$][\\w$]*$/;"
    "return function(v,p){var seen=new S(),hit=null;"
    "function walk(v,p,d){if(hit||d>1000)return;"
    "if(typeof v==='function'){var c=false;try{c=/^class\\b/.test(fts.call(v))}catch(e){}"
    "hit=[p,c?1:0,v];return}"
    "if(v===null||typeof v!=='object'||seen.has(v))return;seen.add(v);"
    "var t=ts.call(v);if(!ok.test(t)){hit=[p,2,t.slice(8,-1)];return}"
    "if(t==='[object Map]'){var i=0;v.forEach(function(x,k){walk(k,p+' (key '+i+')',d+1);"
    "walk(x,p+' (value '+(i++)+')',d+1)});return}"
    "if(t==='[object Set]'){var j=0;v.forEach(function(x){walk(x,p+' (item '+(j++)+')',d+1)});"
    "return}"
    "var ks=keys(v),a=isA(v);for(var i=0;i<ks.length&&!hit;i++){var k=ks[i];"
    "walk(v[k],a?p+'['+k+']':id.test(k)?p+'.'+k:p+'['+J(k)+']',d+1)}}"
    "try{walk(v,p,0)}catch(e){}return hit}})"
    "(Object.prototype.toString,Function.prototype.toString,Object.keys,Array.isArray,Set,"
    "JSON.stringify)";

/* Can variable `name` holding v be stored? If not: 1, and ecmascript->error says why. */
static int explain_var(ta_datamodel_ecmascript *ecmascript, JSContext *context,
                       const ecmascript_chart *chart, const char *name, JSValueConst v) {
    size_t sz;
    uint8_t *b = write_obj(ecmascript, context, v, &sz);
    if (b) {
        js_free(context, b);
        return 0;
    }
    char why[160] = "";
    JSValue ex = JS_GetException(context);
    const char *m = JS_ToCString(context, ex);
    if (m) {
        snprintf(why, sizeof why, "%s", m);
        JS_FreeCString(context, m);
    }
    JS_FreeValue(context, ex);
    clear_exception(context);
    if (JS_IsUndefined(ecmascript->explain)) {
        ecmascript->operations = 0;
        ecmascript->explain = JS_Eval(ecmascript->auxiliary, EXPLAIN_JS, sizeof EXPLAIN_JS - 1,
                                      "<explain>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(ecmascript->explain)) {
            clear_exception(ecmascript->auxiliary);
            ecmascript->explain = JS_UNDEFINED;
        }
    }
    JSValue hit = JS_UNDEFINED;
    if (!JS_IsUndefined(ecmascript->explain)) {
        ecmascript->operations = 0;
        JSValue args[2] = {JS_DupValue(ecmascript->auxiliary, v),
                           JS_NewString(ecmascript->auxiliary, name)};
        hit = JS_Call(ecmascript->auxiliary, ecmascript->explain, JS_UNDEFINED, 2, args);
        JS_FreeValue(ecmascript->auxiliary, args[0]);
        JS_FreeValue(ecmascript->auxiliary, args[1]);
        if (JS_IsException(hit)) {
            clear_exception(ecmascript->auxiliary);
            hit = JS_UNDEFINED;
        }
    }
    if (!JS_IsArray(hit)) {
        set_error(ecmascript, "variable '%s' cannot be stored: %s", name,
                  why[0] ? why : "unsupported value");
        JS_FreeValue(ecmascript->auxiliary, hit);
        return 1;
    }
    JSValue pv = JS_GetPropertyUint32(ecmascript->auxiliary, hit, 0),
            dv = JS_GetPropertyUint32(ecmascript->auxiliary, hit, 2);
    uint32_t kind = u32_at(ecmascript->auxiliary, hit, 1);
    const char *path = JS_ToCString(ecmascript->auxiliary, pv);
    char at[48] = "";
    uint32_t line = kind < 2 ? function_line(ecmascript->auxiliary, chart, dv) : 0;
    if (line) snprintf(at, sizeof at, " (line %u)", (unsigned)line);
    int self = path && !strcmp(path, name);
    if (kind == 1) {
        set_error(
            ecmascript,
            "variable '%s' cannot be stored: %s%s is a class%s; classes are not stored — keep "
            "the data in plain objects and the code in top-level function declarations",
            name,
            self   ? ""
            : path ? path
                   : "?",
            self ? "it" : "", at);
    } else if (kind == 0 && self) {
        set_error(ecmascript,
                  "variable '%s' cannot be stored: it holds a function%s; only top-level function "
                  "declarations are stored (re-created on restore): write `function %s(...) {...}`",
                  name, at, name);
    } else if (kind == 0) {
        set_error(ecmascript,
                  "variable '%s' cannot be stored: %s is a function%s; functions in data are not "
                  "stored",
                  name, path ? path : "?", at);
    } else {
        const char *class_name = JS_ToCString(ecmascript->auxiliary, dv);
        set_error(ecmascript, "variable '%s' cannot be stored: %s is %s %s object", name,
                  path ? path : "?", class_name && strchr("AEIOU", class_name[0]) ? "an" : "a",
                  class_name ? class_name : "unsupported");
        if (class_name) JS_FreeCString(ecmascript->auxiliary, class_name);
    }
    if (path) JS_FreeCString(ecmascript->auxiliary, path);
    JS_FreeValue(ecmascript->auxiliary, pv);
    JS_FreeValue(ecmascript->auxiliary, dv);
    JS_FreeValue(ecmascript->auxiliary, hit);
    clear_exception(ecmascript->auxiliary);
    return 1;
}

/* ecmascript->error for a state that failed to write as a whole: the first [name, value]
 * entry of the arrays (or, for an object, property) that cannot be stored */
static void explain_entries(ta_datamodel_ecmascript *ecmascript, JSContext *context,
                            const ecmascript_chart *chart, JSValueConst arr, uint32_t vi) {
    int64_t n = 0;
    if (ecmascript->error[0] || JS_GetLength(context, arr, &n) < 0) {
        clear_exception(context);
        return;
    }
    for (uint32_t i = 0; i < (uint32_t)n && !ecmascript->error[0]; i++) {
        JSValue e = JS_GetPropertyUint32(context, arr, i), nv = JS_GetPropertyUint32(context, e, 0),
                v = JS_GetPropertyUint32(context, e, vi);
        const char *name = JS_ToCString(context, nv);
        if (name) {
            explain_var(ecmascript, context, chart, name, v);
            JS_FreeCString(context, name);
        }
        JS_FreeValue(context, v);
        JS_FreeValue(context, nv);
        JS_FreeValue(context, e);
    }
    clear_exception(context);
}

static void explain_object(ta_datamodel_ecmascript *ecmascript, JSContext *context,
                           const ecmascript_chart *chart, JSValueConst o) {
    JSPropertyEnum *props;
    uint32_t n;
    if (ecmascript->error[0] ||
        JS_GetOwnPropertyNames(context, &props, &n, o, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        clear_exception(context);
        return;
    }
    for (uint32_t i = 0; i < n && !ecmascript->error[0]; i++) {
        const char *name = JS_AtomToCString(context, props[i].atom);
        JSValue v = JS_GetProperty(context, o, props[i].atom);
        if (name) explain_var(ecmascript, context, chart, name, v);
        JS_FreeValue(context, v);
        if (name) JS_FreeCString(context, name);
    }
    JS_FreePropertyEnum(context, props, n);
    clear_exception(context);
}

/* Global code built from a name (an identifier: probes and restores). */
static JSValue eval_name(JSContext *context, const char *pre, const char *name, const char *mid,
                         const char *post) {
    size_t n = strlen(pre) + 2 * strlen(name) + strlen(mid) + strlen(post) + 1;
    char *buffer = (char *)malloc(n);
    if (!buffer) return JS_ThrowOutOfMemory(context);
    int length = snprintf(buffer, n, "%s%s%s%s%s", pre, name, mid, mid[0] ? name : "", post);
    JSValue r = JS_Eval(context, buffer, (size_t)length, "<store>", JS_EVAL_TYPE_GLOBAL);
    free(buffer);
    return r;
}

/* State of the global lexical binding `name` of an isolated session: -1 none,
 * 0 uninitialized, 1 let/class, 2 const (*v: its value). Probed with global
 * code that changes nothing: `delete name` is false only for a lexical binding
 * (a configurable global property it deletes instead is put back). */
static int lex_probe(ta_datamodel_ecmascript *ecmascript, JSContext *context, JSValueConst g,
                     JSAtom a, const char *name, JSValue *v) {
    *v = JS_UNDEFINED;
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(context, &d, g, a);
    if (has < 0) {
        clear_exception(context);
        return -1;
    }
    if (has && !(d.flags & JS_PROP_CONFIGURABLE)) { /* then `let name` would have failed */
        free_descriptor(context, &d);
        return -1;
    }
    ecmascript->operations = 0;
    JSValue r = eval_name(context, "delete ", name, "", "");
    int gone = JS_IsException(r) ? 1 : JS_ToBool(context, r);
    JS_FreeValue(context, r);
    clear_exception(context);
    if (gone) {
        if (has && (d.flags & JS_PROP_GETSET)) {
            JS_DefinePropertyGetSet(context, g, a, d.getter, d.setter,
                                    d.flags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE));
            JS_FreeValue(context, d.value);
        } else if (has) {
            JS_DefinePropertyValue(context, g, a, d.value, d.flags & JS_PROP_C_W_E);
            JS_FreeValue(context, d.getter);
            JS_FreeValue(context, d.setter);
        }
        return -1;
    }
    if (has) free_descriptor(context, &d);
    r = eval_name(context, "typeof ", name, "", "");
    if (JS_IsException(r)) {
        clear_exception(context);
        return 0;
    }
    JS_FreeValue(context, r);
    *v = eval_name(context, "", name, "", "");
    if (JS_IsException(*v)) {
        clear_exception(context);
        *v = JS_UNDEFINED;
        return 0;
    }
    r = eval_name(context, "", name, "=", "");
    int is_const = JS_IsException(r);
    JS_FreeValue(context, r);
    clear_exception(context);
    return is_const ? 2 : 1;
}

/* The declaration that made function f, the value of global `a` — by running
 * its script (its position), or re-created on restore (in->declarations) — or NULL. */
static const ecmascript_function_declaration *function_declared(ecmascript_instance *instance,
                                                                JSAtom a, JSValueConst f) {
    JSContext *context = instance->context;
    uint32_t script = NONE;
    int32_t line = 0, col = 0;
    if (JS_IsObject(instance->declarations)) {
        JSValue e = JS_GetProperty(context, instance->declarations, a);
        if (JS_IsArray(e)) {
            JSValue x = JS_GetPropertyUint32(context, e, 0);
            if (JS_IsStrictEqual(context, x, f)) {
                script = u32_at(context, e, 1);
                line = i32_at(context, e, 2);
                col = i32_at(context, e, 3);
            }
            JS_FreeValue(context, x);
        }
        JS_FreeValue(context, e);
    }
    if (script == NONE) {
        JSValue fv = JS_GetPropertyStr(context, f, "fileName");
        const char *s = JS_IsString(fv) ? JS_ToCString(context, fv) : NULL;
        unsigned id;
        if (s && sscanf(s, "<script:%u>", &id) == 1) {
            script = id;
            line = int_prop(context, f, "lineNumber");
            col = int_prop(context, f, "columnNumber");
        }
        if (s) JS_FreeCString(context, s);
        JS_FreeValue(context, fv);
        clear_exception(context);
    }
    for (uint32_t i = 0; i < instance->chart_context->function_count; i++) {
        const ecmascript_function_declaration *declaration = &instance->chart_context->functions[i];
        if (declaration->name == a && declaration->script == script && declaration->line == line &&
            declaration->column == col)
            return declaration;
    }
    return NULL;
}

/* Is function f still as its declaration made it? If not: 1, what = the
 * property added (e.g. "f.prototype.m"). */
static int function_changed(JSContext *context, JSValueConst f, const char *name, char *what,
                            size_t capacity) {
    JSValue proto = JS_UNDEFINED, obj = JS_DupValue(context, f);
    int changed = 0;
    for (int level = 0; level < 2 && !changed && JS_IsObject(obj); level++) {
        JSPropertyEnum *p;
        uint32_t n;
        if (JS_GetOwnPropertyNames(context, &p, &n, obj, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) <
            0) {
            clear_exception(context);
            snprintf(what, capacity, "%s", name);
            changed = 1;
            break;
        }
        for (uint32_t i = 0; i < n && !changed; i++) {
            const char *k = JS_AtomToCString(context, p[i].atom);
            int ok;
            if (level == 0) {
                ok = k && (!strcmp(k, "length") || !strcmp(k, "name") || !strcmp(k, "prototype"));
            } else { /* the prototype object: only its constructor, f */
                JSValue c = JS_GetProperty(context, obj, p[i].atom);
                ok = k && !strcmp(k, "constructor") && JS_IsStrictEqual(context, c, f);
                JS_FreeValue(context, c);
            }
            if (!ok) {
                snprintf(what, capacity, "%s%s.%s", name, level ? ".prototype" : "", k ? k : "?");
                changed = 1;
            }
            if (k) JS_FreeCString(context, k);
        }
        JS_FreePropertyEnum(context, p, n);
        if (level == 0) proto = JS_GetPropertyStr(context, f, "prototype");
        JS_FreeValue(context, obj);
        obj = level == 0 ? proto : JS_UNDEFINED;
    }
    JS_FreeValue(context, obj);
    clear_exception(context);
    return changed;
}

/* An isolated session's state (see above) as an object in its context.
 * TA_ERROR_UNSUPPORTED (ecmascript->error says why) if part of it cannot be stored. */
static ta_status iso_capture(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                             int with_system, JSValue *out) {
    JSContext *context = instance->context;
    const ecmascript_chart *chart = instance->chart_context;
    *out = JS_UNDEFINED;
    if (!base_names(ecmascript)) return TA_ERROR_NO_MEMORY;
    JSValue g = JS_GetGlobalObject(context);
    JSValue gv = JS_NewArray(context), fv = JS_NewArray(context), lv = JS_NewArray(context);
    uint32_t ng = 0, nf = 0, nl = 0;
    ta_status status = TA_OK;
    JSPropertyEnum *props = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(context, &props, &n, g, JS_GPN_STRING_MASK) < 0) {
        clear_exception(context);
        status = TA_ERROR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < n && status == TA_OK; i++) {
        JSAtom a = props[i].atom;
        const char *name = JS_AtomToCString(context, a);
        if (!name) {
            status = TA_ERROR_NO_MEMORY;
            break;
        }
        JSPropertyDescriptor d;
        int has = 0;
        if (!is_internal(name) &&
            (!base_has(ecmascript, a) || atom_in(chart->variables, chart->variable_count, a) ||
             is_data_id(chart, name)))
            has = JS_GetOwnProperty(context, &d, g, a);
        if (has < 0) {
            clear_exception(context);
        } else if (has && (d.flags & JS_PROP_GETSET)) {
            set_error(ecmascript,
                      "variable '%s' cannot be stored: it is an accessor property (get/set)", name);
            status = TA_ERROR_UNSUPPORTED;
        } else if (has && JS_IsFunction(context, d.value)) {
            const ecmascript_function_declaration *declaration =
                function_declared(instance, a, d.value);
            char what[160];
            if (!declaration) {
                if (!explain_var(ecmascript, context, chart, name, d.value))
                    set_error(ecmascript, "variable '%s' cannot be stored: it holds a function",
                              name);
                status = TA_ERROR_UNSUPPORTED;
            } else if (function_changed(context, d.value, name, what, sizeof what)) {
                set_error(ecmascript,
                          "function '%s' cannot be stored: %s was added after its declaration "
                          "(only unchanged top-level function declarations are re-created)",
                          name, what);
                status = TA_ERROR_UNSUPPORTED;
            } else {
                JSValue t[4] = {JS_NewString(context, name),
                                JS_NewUint32(context, declaration->script),
                                JS_NewInt32(context, declaration->line),
                                JS_NewInt32(context, declaration->column)};
                push(context, fv, &nf, tuple(context, 4, t));
            }
        } else if (has) {
            JSValue t[3] = {JS_NewString(context, name), JS_DupValue(context, d.value),
                            JS_NewInt32(context, d.flags & JS_PROP_C_W_E)};
            push(context, gv, &ng, tuple(context, 3, t));
        }
        if (has > 0) free_descriptor(context, &d);
        JS_FreeCString(context, name);
    }
    JS_FreePropertyEnum(context, props, n);
    for (uint32_t i = 0; i < chart->lexical_name_count && status == TA_OK; i++) {
        const char *name = JS_AtomToCString(context, chart->lexical_names[i]);
        JSValue v = JS_UNDEFINED;
        int kind = name ? lex_probe(ecmascript, context, g, chart->lexical_names[i], name, &v) : -1;
        if (kind > 0 && JS_IsFunction(context, v)) {
            if (!explain_var(ecmascript, context, chart, name, v))
                set_error(ecmascript, "variable '%s' cannot be stored: it holds a function", name);
            status = TA_ERROR_UNSUPPORTED;
        } else if (kind >= 0) {
            JSValue t[3] = {JS_NewString(context, name), JS_NewInt32(context, kind), v};
            v = JS_UNDEFINED;
            push(context, lv, &nl, tuple(context, 3, t));
        }
        JS_FreeValue(context, v);
        if (name) JS_FreeCString(context, name);
    }
    if (status != TA_OK) {
        JS_FreeValue(context, gv);
        JS_FreeValue(context, fv);
        JS_FreeValue(context, lv);
        JS_FreeValue(context, g);
        return status;
    }
    JSValue state = JS_NewObject(context);
    JS_SetPropertyStr(context, state, "g", gv);
    JS_SetPropertyStr(context, state, "f", fv);
    JS_SetPropertyStr(context, state, "l", lv);
    if (with_system) {
        JSValue s = JS_NewObject(context);
        static const char *const system_variable_names[] = {"_sessionid", "_name", "_ioprocessors"};
        for (int k = 0; k < 3; k++)
            JS_SetPropertyStr(context, s, system_variable_names[k],
                              JS_GetPropertyStr(context, g, system_variable_names[k]));
        JS_SetPropertyStr(context, state, "s", s);
    }
    JS_SetPropertyStr(context, state, "i", JS_DupValue(context, instance->init));
    JS_FreeValue(context, g);
    *out = state;
    return TA_OK;
}

/* a name from a stored state, safe to put in global code: an identifier */
static int ident_ok(const char *s) {
    if (!*s || (*s >= '0' && *s <= '9')) return 0;
    const unsigned char *p = (const unsigned char *)s, *end = p + strlen(s);
    for (; p < end; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '$' || (*p >= 0x80 && !js_space(p, end))))
            return 0;
    return 1;
}

/* Re-run declaration d in an isolated session and remember the function. */
static int function_recreate(ecmascript_instance *instance,
                             const ecmascript_function_declaration *declaration) {
    JSContext *context = instance->context;
    char file[32];
    snprintf(file, sizeof file, "<script:%u>", (unsigned)declaration->script);
    ta_string_view source = {declaration->source, (uint32_t)strlen(declaration->source)};
    JSValue r = eval_file(context, declaration->strict ? "'use strict';" : "", source, "", file,
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        clear_exception(context);
        return 0;
    }
    JS_FreeValue(context, r);
    JSValue g = JS_GetGlobalObject(context), f = JS_GetProperty(context, g, declaration->name);
    JS_FreeValue(context, g);
    if (!JS_IsFunction(context, f)) {
        JS_FreeValue(context, f);
        clear_exception(context);
        return 0;
    }
    if (JS_IsUndefined(instance->declarations))
        instance->declarations = JS_NewObjectProto(context, JS_NULL);
    JSValue t[4] = {f, JS_NewUint32(context, declaration->script),
                    JS_NewInt32(context, declaration->line),
                    JS_NewInt32(context, declaration->column)};
    return JS_SetProperty(context, instance->declarations, declaration->name,
                          tuple(context, 4, t)) >= 0;
}

/* Put a stored state into a fresh session. iso: the isolated format (else an
 * object of variables, as shared sessions store); migrating: written for
 * another version of the chart (lexical bindings come back as properties the
 * migration can drop, functions as the new chart declares them); system: the
 * state carries the system variables (a snapshot). */
static ta_status apply_state(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                             JSValueConst state, int iso, int migrating, int system) {
    JSContext *context = instance->context;
    ecmascript_chart *chart = instance->chart_context;
    ecmascript->operations = 0;
    int isolated = !chart->shared;
    JSValue vars = instance_variables(instance);
    ta_status status = TA_OK;
    if (!iso) {
        JSPropertyEnum *props;
        uint32_t n;
        if (JS_GetOwnPropertyNames(context, &props, &n, state,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            clear_exception(context);
            JS_FreeValue(context, vars);
            return TA_ERROR_INVALID;
        }
        JSAtom event = JS_NewAtom(context, "_event");
        for (uint32_t i = 0; i < n; i++) {
            const char *name = JS_AtomToCString(context, props[i].atom);
            int sv = name && is_sysvar(name);
            if (name) JS_FreeCString(context, name);
            if (sv && !system) continue; /* the system supplied them */
            int ro = sv || props[i].atom == event;
            JS_DefinePropertyValue(context, vars, props[i].atom,
                                   JS_GetProperty(context, state, props[i].atom),
                                   ro ? JS_PROP_ENUMERABLE | (sv ? 0 : JS_PROP_CONFIGURABLE)
                                      : JS_PROP_C_W_E); /* as created by <data> */
        }
        JS_FreeAtom(context, event);
        JS_FreePropertyEnum(context, props, n);
        JS_FreeValue(context, vars);
        return TA_OK;
    }
    JSValue gv = JS_GetPropertyStr(context, state, "g"),
            fv = JS_GetPropertyStr(context, state, "f"),
            lv = JS_GetPropertyStr(context, state, "l");
    int64_t n = 0;
    /* global object properties */
    JS_GetLength(context, gv, &n);
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        JSValue e = JS_GetPropertyUint32(context, gv, i);
        JSValue nv = JS_GetPropertyUint32(context, e, 0);
        JSAtom a = JS_ValueToAtom(context, nv);
        int flags = i32_at(context, e, 2) & JS_PROP_C_W_E;
        const char *name = JS_ToCString(context, nv);
        if (name && !strcmp(name, "_event"))
            flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE; /* read-only */
        else if (!isolated || (migrating && !atom_in(chart->variables, chart->variable_count, a)))
            flags = JS_PROP_C_W_E;
        if (a != JS_ATOM_NULL && name && !is_internal(name))
            JS_DefinePropertyValue(context, vars, a, JS_GetPropertyUint32(context, e, 1), flags);
        if (name) JS_FreeCString(context, name);
        JS_FreeAtom(context, a);
        JS_FreeValue(context, nv);
        JS_FreeValue(context, e);
    }
    /* function declarations, re-run */
    n = 0;
    JS_GetLength(context, fv, &n);
    for (uint32_t i = 0; i < (uint32_t)n && isolated && status == TA_OK; i++) {
        JSValue e = JS_GetPropertyUint32(context, fv, i), nv = JS_GetPropertyUint32(context, e, 0);
        JSAtom a = JS_ValueToAtom(context, nv);
        uint32_t script = u32_at(context, e, 1);
        int32_t line = i32_at(context, e, 2), col = i32_at(context, e, 3);
        const ecmascript_function_declaration *declaration = NULL;
        for (uint32_t k = 0; k < chart->function_count && !declaration; k++)
            if (chart->functions[k].name == a && chart->functions[k].script == script &&
                chart->functions[k].line == line && chart->functions[k].column == col)
                declaration = &chart->functions[k];
        for (uint32_t k = 0; k < chart->function_count && !declaration && migrating; k++)
            if (chart->functions[k].name == a)
                declaration = &chart->functions[k]; /* as the new chart declares it */
        if (declaration ? !function_recreate(instance, declaration) : !migrating)
            status = TA_ERROR_INVALID;
        JS_FreeAtom(context, a);
        JS_FreeValue(context, nv);
        JS_FreeValue(context, e);
    }
    /* global lexical bindings: re-declared with their values */
    n = 0;
    JS_GetLength(context, lv, &n);
    char *tdz = NULL, *declaration_text = NULL;
    size_t tl = 0, dl = 0;
    uint32_t nv_ = 0;
    JSValue values = JS_NewArray(context);
    for (uint32_t i = 0; i < (uint32_t)n && status == TA_OK; i++) {
        JSValue e = JS_GetPropertyUint32(context, lv, i), nv = JS_GetPropertyUint32(context, e, 0);
        int kind = i32_at(context, e, 1);
        const char *name = JS_ToCString(context, nv);
        if (!name || !ident_ok(name)) {
            status = TA_ERROR_INVALID;
        } else if (!isolated || migrating) {
            JSAtom a = JS_NewAtom(context, name);
            if (kind > 0)
                JS_DefinePropertyValue(context, vars, a, JS_GetPropertyUint32(context, e, 2),
                                       JS_PROP_C_W_E);
            JS_FreeAtom(context, a);
        } else {
            char **buffer = kind > 0 ? &declaration_text : &tdz;
            size_t *length = kind > 0 ? &dl : &tl;
            size_t need = *length + strlen(name) + 64;
            char *nb = (char *)realloc(*buffer, need);
            if (!nb) {
                status = TA_ERROR_NO_MEMORY;
            } else {
                *buffer = nb;
                if (kind > 0) {
                    *length += (size_t)snprintf(nb + *length, need - *length,
                                                "%s %s=" RESTORE_TEMPORARY "[%u];",
                                                kind == 2 ? "const" : "let", name, (unsigned)nv_);
                    JS_SetPropertyUint32(context, values, nv_++,
                                         JS_GetPropertyUint32(context, e, 2));
                } else {
                    *length += (size_t)snprintf(nb + *length, need - *length, "let %s;", name);
                }
            }
        }
        if (name) JS_FreeCString(context, name);
        JS_FreeValue(context, nv);
        JS_FreeValue(context, e);
    }
    if (status == TA_OK && tdz) { /* uninitialized for good: declared, never initialized */
        ta_string_view s = {tdz, (uint32_t)tl};
        JSValue r = eval_file(context, "throw 0;\n", s, "", "<store>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(context, r);
        JSValue x = JS_GetException(context);
        if (!(JS_VALUE_GET_TAG(x) == JS_TAG_INT && JS_VALUE_GET_INT(x) == 0))
            status = TA_ERROR_INVALID;
        JS_FreeValue(context, x);
    }
    if (status == TA_OK && declaration_text) {
        ta_string_view s = {declaration_text, (uint32_t)dl};
        JS_DefinePropertyValueStr(context, vars, RESTORE_TEMPORARY, JS_DupValue(context, values),
                                  JS_PROP_C_W_E);
        JSValue r = eval_file(context, "", s, "", "<store>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) status = TA_ERROR_INVALID;
        JS_FreeValue(context, r);
        JSAtom a = JS_NewAtom(context, RESTORE_TEMPORARY);
        JS_DeleteProperty(context, vars, a, 0);
        JS_FreeAtom(context, a);
    }
    clear_exception(context);
    free(tdz);
    free(declaration_text);
    JS_FreeValue(context, values);
    /* system variables (snapshots), invoke params */
    JSValue s = JS_GetPropertyStr(context, state, "s");
    if (system && JS_IsObject(s)) {
        static const char *const names[] = {"_sessionid", "_name", "_ioprocessors"};
        for (int k = 0; k < 3; k++)
            def_ro(context, vars, names[k], JS_GetPropertyStr(context, s, names[k]));
    }
    JS_FreeValue(context, s);
    JSValue init = JS_GetPropertyStr(context, state, "i");
    if (!JS_IsUndefined(init)) {
        JS_FreeValue(context, instance->init);
        instance->init = init;
    }
    JS_FreeValue(context, gv);
    JS_FreeValue(context, fv);
    JS_FreeValue(context, lv);
    JS_FreeValue(context, vars);
    return status;
}

/* A shared session's variables and _event, as one object. */
static JSValue scope_capture(ecmascript_instance *instance) {
    JSContext *context = instance->context;
    JSPropertyEnum *props;
    uint32_t n;
    if (JS_GetOwnPropertyNames(context, &props, &n, instance->scope,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
        return JS_EXCEPTION;
    JSValue o = JS_NewObject(context);
    for (uint32_t i = 0; i < n; i++) {
        const char *name = JS_AtomToCString(context, props[i].atom);
        int skip = !name || is_sysvar(name);
        if (name) JS_FreeCString(context, name);
        if (!skip)
            JS_DefinePropertyValue(context, o, props[i].atom,
                                   JS_GetProperty(context, instance->scope, props[i].atom),
                                   JS_PROP_C_W_E);
    }
    JS_FreePropertyEnum(context, props, n);
    return o;
}

/* The session's state as one value in its context: shared, [2, the object of
 * its variables, its invoke params]; isolated, [1, chart hash, state]. The
 * invoke params are state: late binding applies them to <data> of states
 * entered later (docs/durability.md). */
static ta_status session_value(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                               int with_system, JSValue *out) {
    JSContext *context = instance->context;
    if (instance->chart_context->shared) {
        JSValue vars = scope_capture(instance);
        if (JS_IsException(vars)) {
            clear_exception(context);
            return TA_ERROR_NO_MEMORY;
        }
        JSValue t[3] = {JS_NewInt32(context, 2), vars, JS_DupValue(context, instance->init)};
        *out = tuple(context, 3, t);
        return TA_OK;
    }
    JSValue state;
    ta_status status = iso_capture(ecmascript, instance, with_system, &state);
    if (status != TA_OK) return status;
    char hex[24];
    hex_hash(instance->chart_context->hash, hex);
    JSValue t[3] = {JS_NewInt32(context, 1), JS_NewString(context, hex), state};
    *out = tuple(context, 3, t);
    return TA_OK;
}

/* ecmascript->error after a session value failed to write */
static void explain_session(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                            JSValueConst v) {
    JSContext *context = instance->context;
    if (instance->chart_context->shared) {
        JSValue vars = JS_GetPropertyUint32(context, v, 1);
        explain_object(ecmascript, context, instance->chart_context, vars);
        JS_FreeValue(context, vars);
    } else {
        JSValue state = JS_GetPropertyUint32(context, v, 2),
                gv = JS_GetPropertyStr(context, state, "g"),
                lv = JS_GetPropertyStr(context, state, "l");
        explain_entries(ecmascript, context, instance->chart_context, gv, 1);
        explain_entries(ecmascript, context, instance->chart_context, lv, 2);
        JS_FreeValue(context, lv);
        JS_FreeValue(context, gv);
        JS_FreeValue(context, state);
    }
}

/* Room for `length` bytes of output (datamodel.h: the view a call gives);
 * NULL if malloc refused. */
static unsigned char *output_room(ta_datamodel_ecmascript *ecmascript, size_t length) {
    if (length <= ecmascript->output_capacity) return ecmascript->output;
    size_t grown = ecmascript->output_capacity ? ecmascript->output_capacity : 256;
    while (grown < length)
        grown *= 2;
    unsigned char *room = (unsigned char *)malloc(grown);
    if (!room) return NULL;
    free(ecmascript->output);
    ecmascript->output = room;
    ecmascript->output_capacity = grown;
    return room;
}

/* `bytes` as the datamodel's output, in `out`. */
static ta_status output_keep(ta_datamodel_ecmascript *ecmascript, ta_bytes bytes, ta_bytes *out) {
    unsigned char *room = output_room(ecmascript, bytes.length);
    if (!room) return TA_ERROR_NO_MEMORY;
    if (bytes.length) memcpy(room, bytes.pointer, bytes.length);
    *out = (ta_bytes){room, bytes.length};
    return TA_OK;
}

static ta_status ecmascript_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ecmascript_instance *instance = I(ecmascript, handle);
    *out = (ta_bytes){0, 0};
    ecmascript->error[0] = 0;
    if (!instance || !instance->live) return TA_ERROR_INVALID;
    JSContext *context = instance->context;
    JSValue o;
    ta_status status = session_value(ecmascript, instance, 0, &o);
    if (status != TA_OK) return status;
    size_t sz = 0;
    uint8_t *bytes = write_obj(ecmascript, context, o, &sz);
    if (!bytes) {
        clear_exception(context);
        explain_session(ecmascript, instance, o);
        if (!ecmascript->error[0])
            set_error(ecmascript, "the session's variables cannot be stored together");
        JS_FreeValue(context, o);
        return TA_ERROR_UNSUPPORTED;
    }
    JS_FreeValue(context, o);
    status = output_keep(ecmascript, (ta_bytes){bytes, sz}, out);
    js_free(context, bytes);
    return status;
}

/* A session value read back in the session's context into it. */
static ta_status apply_value(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                             JSValueConst o, int system) {
    JSContext *context = instance->context;
    if (!JS_IsArray(o))
        return JS_IsObject(o) ? apply_state(ecmascript, instance, o, 0, 0, system)
                              : TA_ERROR_INVALID;
    if (u32_at(context, o, 0) == 2) { /* shared: [2, variables, invoke params] */
        JSValue vars = JS_GetPropertyUint32(context, o, 1),
                init = JS_GetPropertyUint32(context, o, 2);
        ta_status status = JS_IsObject(vars) ? apply_state(ecmascript, instance, vars, 0, 0, system)
                                             : TA_ERROR_INVALID;
        if (status == TA_OK) {
            JS_FreeValue(context, instance->init);
            instance->init = init;
        } else {
            JS_FreeValue(context, init);
        }
        JS_FreeValue(context, vars);
        clear_exception(context);
        return status;
    }
    JSValue hv = JS_GetPropertyUint32(context, o, 1), state = JS_GetPropertyUint32(context, o, 2);
    const char *hex = JS_ToCString(context, hv);
    char mine[24];
    hex_hash(instance->chart_context->hash, mine);
    ta_status status = TA_ERROR_INVALID;
    if (u32_at(context, o, 0) == 1 && hex && JS_IsObject(state))
        status = apply_state(ecmascript, instance, state, 1, strcmp(hex, mine) != 0, system);
    if (hex) JS_FreeCString(context, hex);
    JS_FreeValue(context, hv);
    JS_FreeValue(context, state);
    clear_exception(context);
    return status;
}

static ta_status ecmascript_deserialize(void *datamodel, const ta_datamodel_start *start,
                                        uint64_t *out) {
    const void *buffer = start->state.pointer;
    size_t length = start->state.length;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ta_datamodel_start fresh = {start->chart_context, start->system_variables, 0, {0, 0}};
    ta_status status = ecmascript_create(datamodel, &fresh, out);
    if (status != TA_OK) return status;
    ecmascript_instance *instance = I(ecmascript, *out);
    JSContext *context = instance->context;
    JS_UpdateStackTop(ecmascript->runtime);
    JSValue o = JS_ReadObject(context, (const uint8_t *)buffer, length, JS_READ_OBJ_REFERENCE);
    status = JS_IsException(o) ? TA_ERROR_INVALID : apply_value(ecmascript, instance, o, 0);
    clear_exception(context);
    JS_FreeValue(context, o);
    if (status != TA_OK) ecmascript_destroy(&(ta_datamodel_call){datamodel, *out, 0});
    return status;
}

/* Payloads in export blobs: the value itself, so another datamodel host
 * (another process, after a restart) can adopt the blob. */
static ta_status ecmascript_payload_serialize(void *datamodel, ta_payload payload, ta_bytes *out) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    *out = (ta_bytes){0, 0};
    if (!payload || payload > ecmascript->payload_count || !ecmascript->payload_refs[payload - 1])
        return TA_ERROR_INVALID;
    JSContext *context = shared_realm(ecmascript);
    if (!context) return TA_ERROR_NO_MEMORY;
    size_t sz = 0;
    uint8_t *bytes = write_obj(ecmascript, context, ecmascript->payloads[payload - 1], &sz);
    if (!bytes) {
        clear_exception(context);
        return TA_ERROR_UNSUPPORTED;
    }
    ta_status status = output_keep(ecmascript, (ta_bytes){bytes, sz}, out);
    js_free(context, bytes);
    return status;
}

static ta_status ecmascript_payload_deserialize(void *datamodel, ta_bytes blob, ta_payload *out) {
    const void *buffer = blob.pointer;
    size_t length = blob.length;
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    *out = 0;
    JSContext *context = shared_realm(ecmascript);
    if (!context) return TA_ERROR_NO_MEMORY;
    JS_UpdateStackTop(ecmascript->runtime);
    JSValue v = JS_ReadObject(context, (const uint8_t *)buffer, length, JS_READ_OBJ_REFERENCE);
    if (JS_IsException(v)) {
        clear_exception(context);
        return TA_ERROR_INVALID;
    }
    *out = payload_new(ecmascript, v);
    if (*out) return TA_OK;
    JS_FreeValue(context, v);
    return TA_ERROR_NO_MEMORY;
}

/* Live migration: the restored variables are the previous bindings (returned
 * as one object, the data of error.platform.upgrade); variables the new chart
 * does not declare are removed from the session (an isolated session keeps
 * what cannot be removed: var and function declarations). */
static ta_status ecmascript_migrate_(void *datamodel, uint64_t handle,
                                     ta_datamodel_environment *environment, ta_payload *bindings) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)datamodel;
    ENTER(ecmascript, handle, environment);
    instance->environment = NULL;
    *bindings = 0;
    JSContext *context = instance->context;
    if (!instance->chart_context->shared && !base_names(ecmascript)) return TA_ERROR_NO_MEMORY;
    JSValue vars = instance_variables(instance);
    JSPropertyEnum *props;
    uint32_t n;
    if (JS_GetOwnPropertyNames(context, &props, &n, vars,
                               JS_GPN_STRING_MASK |
                                   (instance->chart_context->shared ? JS_GPN_ENUM_ONLY : 0)) < 0) {
        clear_exception(context);
        JS_FreeValue(context, vars);
        return TA_ERROR_EXECUTION;
    }
    JSValue o = JS_NewObject(context);
    for (uint32_t i = 0; i < n; i++) {
        JSAtom a = props[i].atom;
        const char *name = JS_AtomToCString(context, a);
        if (!name) continue;
        int mine = !is_internal(name) && strcmp(name, "_event") != 0;
        if (mine && !instance->chart_context->shared) /* the chart's, not a builtin */
            mine = !base_has(ecmascript, a) || atom_in(instance->chart_context->variables,
                                                       instance->chart_context->variable_count, a);
        if (mine) {
            /* functions are top-level declarations (anything else could not have been
             * stored): code the new chart re-creates, not bindings — and a payload
             * holding one could not be stored (snapshot, the session's _event) */
            JSValue v = JS_GetProperty(context, vars, a);
            if (JS_IsFunction(context, v))
                JS_FreeValue(context, v);
            else
                JS_DefinePropertyValue(context, o, a, v, JS_PROP_C_W_E);
            /* what the new chart does not declare goes (its <data>, and for an isolated
             * session its scripts' var/function/let/const/class names, stay) */
            if (!is_data_id(instance->chart_context, name) &&
                (instance->chart_context->shared ||
                 (!atom_in(instance->chart_context->variables,
                           instance->chart_context->variable_count, a) &&
                  !atom_in(instance->chart_context->lexical_names,
                           instance->chart_context->lexical_name_count, a))))
                JS_DeleteProperty(context, vars, a, 0);
        }
        JS_FreeCString(context, name);
    }
    clear_exception(context);
    JS_FreePropertyEnum(context, props, n);
    JS_FreeValue(context, vars);
    *bindings = payload_new(ecmascript, o);
    if (*bindings) return TA_OK;
    JS_FreeValue(context, o);
    return TA_ERROR_NO_MEMORY;
}

/* ---- neutral values (docs/protocol.md, "Values") ----------------------------------------
 * CBOR, written from a value's tokens and read back from them. A value is
 * what JSON.stringify keeps (undefined, functions and symbols are dropped from
 * objects and are null in arrays and at the top; toJSON is called), with
 * numbers, bytes and BigInts exact: an integer within ±(2^53-1) is a CBOR
 * integer, any other number (-0, NaN, ±Infinity, fractions, larger
 * integers) a float; a BigInt within -2^64 … 2^64-1 an integer; an
 * ArrayBuffer, a typed array or a DataView its bytes. Keys starting with "$"
 * are not special. ECMAScript has no reference type: a reference (tag 32)
 * arrives as its address, a string. Read back, an integer within ±(2^53-1)
 * is a number and any other a BigInt; bytes are a Uint8Array. */

/* A value being written: the streaming writer (cbor.h) into the
 * datamodel's output, and the first failure of the JavaScript side. */
typedef struct wire_builder {
    ta_cbor_writer writer;
    ta_status status;
} wire_builder;

static void wire_fail(wire_builder *b, ta_status status) {
    if (b->status == TA_OK) b->status = status;
}

static int wire_failed(const wire_builder *b) {
    return b->status != TA_OK || b->writer.status != TA_OK;
}

/* The writer grows the datamodel's output through the C library's heap. */
static void *wire_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align; /* malloc's alignment is enough for bytes */
    return malloc(size);
}

static void wire_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static const ta_allocator WIRE_ALLOCATOR = {NULL, wire_alloc, wire_free};

/* A BigInt as a CBOR integer, if it is one (-2^64 … 2^64-1). */
static void wire_bigint(wire_builder *b, JSContext *context, JSValueConst v) {
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, v); /* decimal */
    if (!text) {
        clear_exception(context);
        wire_fail(b, TA_ERROR_NO_MEMORY);
        return;
    }
    int negative = text[0] == '-';
    const char *digits = text + negative;
    size_t count = length - (size_t)negative;
    uint64_t magnitude = 0;
    int fits = 1;
    for (size_t k = 0; k < count && fits; k++) {
        uint64_t digit = (uint64_t)(digits[k] - '0');
        if (magnitude > (UINT64_MAX - digit) / 10) fits = 0;
        else magnitude = magnitude * 10 + digit;
    }
    int lowest = negative && count == 20 && !memcmp(digits, "18446744073709551616", 20);
    JS_FreeCString(context, text);
    if (lowest) ta_cbor_write_negative(&b->writer, UINT64_MAX);
    else if (!fits) wire_fail(b, TA_ERROR_INVALID); /* not a neutral value */
    else if (!negative) ta_cbor_write_uint(&b->writer, magnitude);
    else ta_cbor_write_negative(&b->writer, magnitude - 1);
}

static void wire_number(wire_builder *b, double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    const double safe = 9007199254740991.0; /* 2^53 - 1 */
    if (d >= -safe && d <= safe && d == (double)(int64_t)d && bits != UINT64_C(1) << 63)
        ta_cbor_write_int(&b->writer, (int64_t)d);
    else
        ta_cbor_write_float(&b->writer, d);
}

/* Bytes of an ArrayBuffer: `length` of them from `offset`. */
typedef struct wire_range {
    size_t offset, length;
} wire_range;

static void wire_buffer_bytes(wire_builder *b, JSContext *context, JSValueConst buffer,
                              wire_range range) {
    size_t size = 0, offset = range.offset, length = range.length;
    uint8_t *bytes = JS_GetArrayBuffer(context, &size, buffer);
    if (!bytes || offset > size || length > size - offset) { /* detached */
        clear_exception(context);
        wire_fail(b, TA_ERROR_INVALID);
        return;
    }
    ta_cbor_write_bytes(&b->writer, (ta_bytes){bytes + offset, length});
}

/* v with its toJSON applied, as JSON.stringify applies it (again on what it
 * returns); a new reference, or JS_EXCEPTION with b's status set. Each call
 * counts as a level of nesting. */
static JSValue wire_resolve(wire_builder *b, JSContext *context, JSValueConst v, uint32_t *depth) {
    JSValue current = JS_DupValue(context, v);
    for (;;) {
        if (*depth > TA_CBOR_DEPTH_MAX) { /* a cycle, or too deep for a neutral value */
            JS_FreeValue(context, current);
            wire_fail(b, TA_ERROR_INVALID);
            return JS_EXCEPTION;
        }
        if (!JS_IsObject(current) || JS_IsFunction(context, current) || JS_IsArrayBuffer(current) ||
            JS_GetTypedArrayType(current) >= 0 || JS_IsDataView(current))
            return current;
        JSValue to_json = JS_GetPropertyStr(context, current, "toJSON");
        if (JS_IsException(to_json)) {
            clear_exception(context);
            JS_FreeValue(context, current);
            wire_fail(b, TA_ERROR_INVALID);
            return JS_EXCEPTION;
        }
        if (!JS_IsFunction(context, to_json)) {
            JS_FreeValue(context, to_json);
            return current;
        }
        JSValue r = JS_Call(context, to_json, current, 0, NULL);
        JS_FreeValue(context, to_json);
        JS_FreeValue(context, current);
        if (JS_IsException(r)) {
            clear_exception(context);
            wire_fail(b, TA_ERROR_INVALID);
            return JS_EXCEPTION;
        }
        current = r;
        (*depth)++;
    }
}

/* What JSON.stringify leaves out: undefined, a function, a symbol. */
static int wire_dropped(JSContext *context, JSValueConst v) {
    return JS_IsUndefined(v) || JS_IsSymbol(v) || JS_IsFunction(context, v);
}

static int wire_write(wire_builder *b, JSContext *context, JSValueConst v, uint32_t depth);
static void wire_value(wire_builder *b, JSContext *context, JSValueConst v, uint32_t depth);

/* An object's own enumerable members, a key only for a value that is kept:
 * the map's count is fixed when it closes. */
static void wire_members(wire_builder *b, JSContext *context, JSValueConst v, uint32_t depth) {
    JSPropertyEnum *properties;
    uint32_t count;
    if (JS_GetOwnPropertyNames(context, &properties, &count, v, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) <
        0) {
        clear_exception(context);
        wire_fail(b, TA_ERROR_INVALID);
        return;
    }
    ta_cbor_write_map(&b->writer, TA_CBOR_COUNT_LATER);
    for (uint32_t i = 0; i < count && !wire_failed(b); i++) {
        JSValue item = JS_GetProperty(context, v, properties[i].atom);
        if (JS_IsException(item)) {
            clear_exception(context);
            wire_fail(b, TA_ERROR_INVALID);
            break;
        }
        uint32_t item_depth = depth + 1;
        JSValue value = wire_resolve(b, context, item, &item_depth);
        JS_FreeValue(context, item);
        if (JS_IsException(value)) break;
        if (!wire_dropped(context, value)) { /* dropped: so is its key */
            size_t length = 0;
            const char *key = JS_AtomToCStringLen(context, &length, properties[i].atom);
            if (!key) {
                wire_fail(b, TA_ERROR_NO_MEMORY);
            } else {
                ta_cbor_write_text(&b->writer, (ta_string_view){key, (uint32_t)length});
                JS_FreeCString(context, key);
                wire_value(b, context, value, item_depth); /* resolved already */
            }
        }
        JS_FreeValue(context, value);
    }
    JS_FreePropertyEnum(context, properties, count);
    ta_cbor_write_end(&b->writer);
}

/* Write v; 1 when written, 0 when it is dropped (undefined, a function, a
 * symbol), -1 on failure (b's status). */
static int wire_write(wire_builder *b, JSContext *context, JSValueConst given, uint32_t depth) {
    if (wire_failed(b)) return -1;
    JSValue v = wire_resolve(b, context, given, &depth);
    if (JS_IsException(v)) return -1;
    int dropped = wire_dropped(context, v);
    if (!dropped) wire_value(b, context, v, depth);
    JS_FreeValue(context, v);
    return dropped ? 0 : wire_failed(b) ? -1 : 1;
}

/* Write v, whose toJSON was applied (wire_resolve) and which is kept. */
static void wire_value(wire_builder *b, JSContext *context, JSValueConst v, uint32_t depth) {
    if (JS_IsNull(v)) {
        ta_cbor_write_simple(&b->writer, TA_CBOR_NULL);
    } else if (JS_IsBool(v)) {
        ta_cbor_write_simple(&b->writer, JS_ToBool(context, v) ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    } else if (JS_IsNumber(v)) {
        double d = 0;
        JS_ToFloat64(context, &d, v);
        wire_number(b, d);
    } else if (JS_IsBigInt(v)) {
        wire_bigint(b, context, v);
    } else if (JS_IsString(v)) {
        size_t length = 0;
        const char *text = JS_ToCStringLen(context, &length, v);
        if (!text) {
            clear_exception(context);
            wire_fail(b, TA_ERROR_NO_MEMORY);
        } else if (length > UINT32_MAX) {
            JS_FreeCString(context, text);
            wire_fail(b, TA_ERROR_INVALID);
        } else {
            ta_cbor_write_text(&b->writer, (ta_string_view){text, (uint32_t)length});
            JS_FreeCString(context, text);
        }
    } else if (JS_IsArrayBuffer(v)) {
        size_t size = 0;
        JS_GetArrayBuffer(context, &size, v);
        wire_buffer_bytes(b, context, v, (wire_range){0, size});
    } else if (JS_GetTypedArrayType(v) >= 0) {
        size_t offset = 0, length = 0, element = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(context, v, &offset, &length, &element);
        if (JS_IsException(buffer)) {
            clear_exception(context);
            wire_fail(b, TA_ERROR_INVALID);
        } else {
            wire_buffer_bytes(b, context, buffer, (wire_range){offset, length});
            JS_FreeValue(context, buffer);
        }
    } else if (JS_IsDataView(v)) {
        JSValue buffer = JS_GetPropertyStr(context, v, "buffer");
        JSValue offset = JS_GetPropertyStr(context, v, "byteOffset");
        JSValue length = JS_GetPropertyStr(context, v, "byteLength");
        int64_t o = 0, l = 0;
        JS_ToInt64(context, &o, offset);
        JS_ToInt64(context, &l, length);
        wire_buffer_bytes(b, context, buffer, (wire_range){(size_t)o, (size_t)l});
        JS_FreeValue(context, buffer);
        JS_FreeValue(context, offset);
        JS_FreeValue(context, length);
    } else if (JS_IsArray(v)) {
        int64_t length = 0;
        if (JS_GetLength(context, v, &length) < 0 || length > UINT32_MAX) {
            clear_exception(context);
            wire_fail(b, TA_ERROR_INVALID);
        } else {
            ta_cbor_write_array(&b->writer, (uint32_t)length);
            for (int64_t i = 0; i < length && !wire_failed(b); i++) {
                JSValue item = JS_GetPropertyUint32(context, v, (uint32_t)i);
                if (JS_IsException(item)) {
                    clear_exception(context);
                    wire_fail(b, TA_ERROR_INVALID);
                    break;
                }
                if (!wire_write(b, context, item, depth + 1)) /* dropped: null in an array */
                    ta_cbor_write_simple(&b->writer, TA_CBOR_NULL);
                JS_FreeValue(context, item);
            }
        }
    } else {
        wire_members(b, context, v, depth);
    }
}

ta_status ta_datamodel_ecmascript_payload_to_cbor(ta_datamodel_ecmascript *ecmascript,
                                                  ta_payload payload, ta_bytes *out) {
    *out = (ta_bytes){0, 0};
    if (!payload || payload > ecmascript->payload_count || !ecmascript->payload_refs[payload - 1])
        return TA_ERROR_INVALID;
    ecmascript->operations = 0;
    JS_UpdateStackTop(ecmascript->runtime);
    wire_builder b;
    b.status = TA_OK;
    /* into the datamodel's output, kept until its next call (datamodel.h) */
    ta_cbor_writer_init(&b.writer, (ta_buffer){ecmascript->output, ecmascript->output_capacity, 0},
                        &WIRE_ALLOCATOR);
    if (!wire_write(&b, ecmascript->auxiliary, ecmascript->payloads[payload - 1], 0))
        ta_cbor_write_simple(&b.writer, TA_CBOR_NULL);
    if (b.writer.bytes != ecmascript->output) { /* it grew: the new block is the output now */
        free(ecmascript->output);
        ecmascript->output = b.writer.bytes;
        ecmascript->output_capacity = b.writer.capacity;
    }
    if (b.status != TA_OK) return b.status;
    return ta_cbor_writer_bytes(&b.writer, out);
}

/* The integer -1 - n, past what a number holds exactly: a BigInt. */
static JSValue wire_negative_bigint(JSContext *context, uint64_t n) {
    if (n <= (uint64_t)INT64_MAX) return JS_NewBigInt64(context, -1 - (int64_t)n);
    char text[24];
    int length = snprintf(text, sizeof text, "-%llu", (unsigned long long)n);
    for (int k = length - 1; k > 0; k--) { /* n + 1, in decimal */
        if (text[k] != '9') {
            text[k]++;
            break;
        }
        text[k] = '0';
        if (k == 1) { /* all nines: one digit more */
            memmove(text + 2, text + 1, (size_t)length);
            text[1] = '1';
        }
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context, global, "BigInt");
    JSValue argument = JS_NewString(context, text);
    JSValue r = JS_Call(context, constructor, JS_UNDEFINED, 1, &argument);
    JS_FreeValue(context, argument);
    JS_FreeValue(context, constructor);
    JS_FreeValue(context, global);
    return r;
}

/* The value at tokens[*at], moving *at past it; JS_EXCEPTION when it is not
 * a neutral value. */
static JSValue wire_read(JSContext *context, const ta_cbor_document *document, uint32_t *at) {
    const ta_cbor_token *token = &document->tokens[(*at)++];
    const uint64_t safe = UINT64_C(9007199254740991); /* 2^53 - 1 */
    switch (token->kind) {
    case TA_CBOR_UINT:
        return token->value <= safe ? JS_NewInt64(context, (int64_t)token->value)
                                    : JS_NewBigUint64(context, token->value);
    case TA_CBOR_NEGINT:
        return token->value < safe ? JS_NewInt64(context, -1 - (int64_t)token->value)
                                   : wire_negative_bigint(context, token->value);
    case TA_CBOR_BYTES: {
        ta_string_view bytes = ta_cbor_string(document, *at - 1);
        return JS_NewUint8ArrayCopy(context, (const uint8_t *)bytes.pointer, bytes.length);
    }
    case TA_CBOR_TEXT: {
        ta_string_view text = ta_cbor_string(document, *at - 1);
        return JS_NewStringLen(context, text.pointer, text.length);
    }
    case TA_CBOR_ARRAY: {
        JSValue array = JS_NewArray(context);
        for (uint32_t i = 0; i < token->count && !JS_IsException(array); i++) {
            JSValue item = wire_read(context, document, at);
            if (JS_IsException(item) ||
                JS_DefinePropertyValueUint32(context, array, i, item, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(context, array);
                array = JS_EXCEPTION;
            }
        }
        return array;
    }
    case TA_CBOR_MAP: {
        JSValue object = JS_NewObject(context);
        for (uint32_t i = 0; i < token->count && !JS_IsException(object); i++) {
            if (document->tokens[*at].kind != TA_CBOR_TEXT) { /* keys are text */
                JS_FreeValue(context, object);
                return JS_EXCEPTION;
            }
            ta_string_view key = ta_cbor_string(document, (*at)++);
            JSValue item = wire_read(context, document, at);
            JSAtom atom = JS_IsException(item) ? JS_ATOM_NULL
                                               : JS_NewAtomLen(context, key.pointer, key.length);
            if (atom == JS_ATOM_NULL ||
                JS_DefinePropertyValue(context, object, atom, item, JS_PROP_C_W_E) < 0) {
                if (atom == JS_ATOM_NULL) JS_FreeValue(context, item);
                JS_FreeValue(context, object);
                object = JS_EXCEPTION;
            }
            if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
        }
        return object;
    }
    case TA_CBOR_TAG: /* an actor reference: its address */
        if (token->value != TA_CBOR_TAG_URI || document->tokens[*at].kind != TA_CBOR_TEXT)
            return JS_EXCEPTION;
        return wire_read(context, document, at);
    case TA_CBOR_SIMPLE:
        switch (token->value) {
        case TA_CBOR_FALSE: return JS_FALSE;
        case TA_CBOR_TRUE: return JS_TRUE;
        case TA_CBOR_NULL: return JS_NULL;
        case TA_CBOR_UNDEFINED: return JS_UNDEFINED;
        default: return JS_EXCEPTION;
        }
    case TA_CBOR_FLOAT: {
        double d;
        memcpy(&d, &token->value, sizeof d);
        return JS_NewFloat64(context, d);
    }
    default: return JS_EXCEPTION;
    }
}

ta_status ta_datamodel_ecmascript_payload_from_cbor(ta_datamodel_ecmascript *ecmascript,
                                                    ta_bytes cbor, ta_payload *out) {
    *out = 0;
    ta_cbor_token small[64];
    ta_cbor_document document = {cbor, small, sizeof small / sizeof small[0], 0};
    ta_status status = ta_cbor_decode(&document);
    if (status == TA_ERROR_NO_MEMORY) {
        document.tokens = (ta_cbor_token *)malloc(document.count * sizeof(ta_cbor_token));
        if (!document.tokens) return TA_ERROR_NO_MEMORY;
        document.capacity = document.count;
        status = ta_cbor_decode(&document);
    }
    if (status != TA_OK) {
        if (document.tokens != small) free(document.tokens);
        return status == TA_ERROR_NO_MEMORY ? status : TA_ERROR_INVALID;
    }
    ecmascript->operations = 0;
    JS_UpdateStackTop(ecmascript->runtime);
    uint32_t at = 0;
    JSValue v = wire_read(ecmascript->auxiliary, &document, &at);
    if (document.tokens != small) free(document.tokens);
    if (JS_IsException(v)) {
        clear_exception(ecmascript->auxiliary);
        return TA_ERROR_INVALID;
    }
    *out = payload_new(ecmascript, v);
    if (*out) return TA_OK;
    JS_FreeValue(ecmascript->auxiliary, v);
    return TA_ERROR_NO_MEMORY;
}

static ta_status ecmascript_to_wire(void *datamodel, ta_payload payload, ta_bytes *out) {
    return ta_datamodel_ecmascript_payload_to_cbor((ta_datamodel_ecmascript *)datamodel, payload,
                                                   out);
}

static ta_status ecmascript_from_wire(void *datamodel, ta_bytes cbor, ta_payload *out) {
    return ta_datamodel_ecmascript_payload_from_cbor((ta_datamodel_ecmascript *)datamodel, cbor,
                                                     out);
}

/* An expression in a session: a variable of a shared-realm session is read
 * (a host storing <data> after every event; no compiling: the scope has a
 * null prototype, so an own name is what with(__s) would find, literals
 * excepted); anything else is evaluated in the session's scope. */
static JSValue eval_in_session(ta_datamodel_ecmascript *ecmascript, ecmascript_instance *instance,
                               ta_string_view expression) {
    ecmascript->current = instance;
    JSAtom a = JS_ATOM_NULL;
    char name[256];
    if (instance->chart_context->shared && is_identifier(expression) &&
        expression.length < sizeof name) {
        memcpy(name, expression.pointer, expression.length);
        name[expression.length] = 0;
        if (strcmp(name, "true") && strcmp(name, "false") && strcmp(name, "null") &&
            strcmp(name, "this"))
            a = JS_NewAtom(instance->context, name);
        if (a != JS_ATOM_NULL &&
            JS_GetOwnProperty(instance->context, NULL, instance->scope, a) != 1) {
            JS_FreeAtom(instance->context, a);
            a = JS_ATOM_NULL;
        }
    }
    if (a != JS_ATOM_NULL) {
        JSValue v = JS_GetProperty(instance->context, instance->scope, a);
        JS_FreeAtom(instance->context, a);
        return v;
    }
    if (!instance->chart_context->shared)
        return eval_wrapped(instance->context, "(\n", expression, "\n)", JS_EVAL_TYPE_GLOBAL);
    JSValue f = eval_wrapped(instance->context, "(function(__s){with(__s){return (\n", expression,
                             "\n);}})", JS_EVAL_TYPE_GLOBAL);
    JSValue v = JS_IsException(f)
                    ? f
                    : JS_Call(instance->context, f, JS_UNDEFINED, 1, (JSValueConst *)&instance->scope);
    JS_FreeValue(instance->context, f);
    return v;
}

ta_payload ta_datamodel_ecmascript_payload_eval(ta_datamodel_ecmascript *ecmascript,
                                                uint64_t instance_handle,
                                                ta_string_view expression) {
    ecmascript_instance *instance = NULL;
    if (instance_handle) {
        instance = I(ecmascript, instance_handle);
        if (!instance || !instance->live) return 0;
    }
    ecmascript->operations = 0;
    JSContext *context = instance ? instance->context : ecmascript->auxiliary;
    JSValue v = instance ? eval_in_session(ecmascript, instance, expression)
                         : eval_wrapped(context, "(\n", expression, "\n)", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        clear_exception(context);
        return 0;
    }
    ta_payload payload = payload_new(ecmascript, v);
    if (!payload) JS_FreeValue(context, v);
    return payload;
}

/* Vtable entries that evaluate JavaScript run with ecmascript->environment set, so the
 * interrupt handler charges their work to the actor's fuel. */
#define WITH_ENVIRONMENT(call)                                                                     \
    ta_datamodel_ecmascript *es_ = (ta_datamodel_ecmascript *)datamodel;                           \
    ta_datamodel_environment *prev_ = es_->environment;                                            \
    es_->environment = environment;                                                                \
    ta_status st_ = call;                                                                          \
    es_->environment = prev_;                                                                      \
    return st_
static ta_status ecmascript_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_init_data_(datamodel, handle, environment, declaration));
}
static ta_status ecmascript_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_bind_event_(datamodel, handle, environment, view));
}
static ta_status ecmascript_condition(const ta_datamodel_call *call, ta_expression expression,
                                      int *out) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_condition_(datamodel, handle, environment, expression, out));
}
static ta_status ecmascript_string(const ta_datamodel_call *call, ta_expression expression,
                                   ta_string_view *out) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_string_(datamodel, handle, environment, expression, out));
}
static ta_status ecmascript_assign(const ta_datamodel_call *call, ta_expression location,
                                   ta_expression value) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_assign_(datamodel, handle, environment, location, value));
}
static ta_status ecmascript_script(const ta_datamodel_call *call, ta_expression expression) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_script_(datamodel, handle, environment, expression));
}
static ta_status ecmascript_log(const ta_datamodel_call *call, ta_expression expression,
                                ta_string_view *out) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_log_(datamodel, handle, environment, expression, out));
}
static ta_status ecmascript_foreach_begin(const ta_datamodel_call *call, ta_expression arr,
                                          ta_datamodel_loop *loop) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    uint64_t *iter = &loop->iter;
    uint32_t *n = &loop->n;
    WITH_ENVIRONMENT(ecmascript_foreach_begin_(datamodel, handle, environment, arr, iter, n));
}
static ta_status ecmascript_capture(const ta_datamodel_call *call,
                                    const ta_datamodel_capture *capture, ta_payload *out) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_capture_(datamodel, handle, environment, capture, out));
}
static ta_status ecmascript_migrate(const ta_datamodel_call *call, ta_payload *bindings) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    ta_datamodel_environment *environment = call->environment;
    WITH_ENVIRONMENT(ecmascript_migrate_(datamodel, handle, environment, bindings));
}

static ta_status ecmascript_variable(const ta_datamodel_call *call, ta_string_view name,
                                     ta_payload *out) {
    void *datamodel = call->datamodel;
    uint64_t handle = call->instance;
    *out = ta_datamodel_ecmascript_payload_eval((ta_datamodel_ecmascript *)datamodel, handle, name);
    return *out ? TA_OK : TA_ERROR_INVALID;
}

static const ta_datamodel_vtable ecmascript_vtable = {sizeof(ta_datamodel_vtable),
                                                      0,
                                                      "ecmascript",
                                                      ecmascript_attach,
                                                      ecmascript_detach,
                                                      ecmascript_create,
                                                      ecmascript_destroy,
                                                      ecmascript_init_data,
                                                      ecmascript_bind_event,
                                                      NULL,
                                                      ecmascript_condition,
                                                      ecmascript_string,
                                                      ecmascript_assign,
                                                      ecmascript_assign_string,
                                                      ecmascript_script,
                                                      ecmascript_log,
                                                      ecmascript_foreach_begin,
                                                      ecmascript_foreach_item,
                                                      ecmascript_foreach_end,
                                                      ecmascript_capture,
                                                      ecmascript_retain,
                                                      ecmascript_release,
                                                      NULL,
                                                      ecmascript_serialize,
                                                      ecmascript_deserialize,
                                                      ecmascript_migrate,
                                                      ecmascript_to_wire,
                                                      ecmascript_from_wire,
                                                      ecmascript_payload_serialize,
                                                      ecmascript_payload_deserialize,
                                                      ecmascript_variable};

/* ---- counting allocator: O(1) heap size (JS_ComputeMemoryUsage walks every object) -- */

typedef struct ecmascript_header {
    size_t size;
    size_t pad; /* keeps the 16-byte alignment of malloc */
} ecmascript_header;

static void *ecmascript_alloc_malloc(void *opaque, size_t n) {
    ecmascript_header *header = (ecmascript_header *)malloc(sizeof *header + n);
    if (!header) return NULL;
    header->size = n;
    ((ta_datamodel_ecmascript *)opaque)->heap += n;
    return header + 1;
}

static void *ecmascript_alloc_calloc(void *opaque, size_t count, size_t size) {
    if (size && count > ((size_t)-1 - sizeof(ecmascript_header)) / size) return NULL;
    void *p = ecmascript_alloc_malloc(opaque, count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

static void ecmascript_alloc_free(void *opaque, void *ptr) {
    if (!ptr) return;
    ecmascript_header *header = (ecmascript_header *)ptr - 1;
    ((ta_datamodel_ecmascript *)opaque)->heap -= header->size;
    free(header);
}

static void *ecmascript_alloc_realloc(void *opaque, void *ptr, size_t n) {
    if (!ptr) return ecmascript_alloc_malloc(opaque, n);
    if (!n) {
        ecmascript_alloc_free(opaque, ptr);
        return NULL;
    }
    ecmascript_header *header = (ecmascript_header *)ptr - 1;
    size_t old = header->size;
    ecmascript_header *nh = (ecmascript_header *)realloc(header, sizeof *nh + n);
    if (!nh) return NULL;
    nh->size = n;
    ((ta_datamodel_ecmascript *)opaque)->heap += n - old;
    return nh + 1;
}

static size_t ecmascript_alloc_usable(const void *ptr) {
    return ptr ? ((const ecmascript_header *)ptr - 1)->size : 0;
}

size_t ta_datamodel_ecmascript_heap_bytes(const ta_datamodel_ecmascript *ecmascript) {
    return ecmascript ? ecmascript->heap : 0;
}

/* ---- public API --------------------------------------------------------------------------- */

const ta_datamodel_vtable *ta_datamodel_ecmascript_vtable(void) {
    return &ecmascript_vtable;
}

ta_datamodel_ecmascript *ta_datamodel_ecmascript_new(const ta_datamodel_ecmascript_config *config) {
    ta_datamodel_ecmascript *ecmascript = (ta_datamodel_ecmascript *)calloc(1, sizeof *ecmascript);
    if (!ecmascript) return NULL;
    static const JSMallocFunctions mf = {ecmascript_alloc_calloc, ecmascript_alloc_malloc,
                                         ecmascript_alloc_free, ecmascript_alloc_realloc,
                                         ecmascript_alloc_usable};
    ecmascript->runtime = JS_NewRuntime2(&mf, ecmascript);
    if (!ecmascript->runtime) {
        free(ecmascript);
        return NULL;
    }
    ecmascript->max_ops = config && config->max_ops ? config->max_ops : 1000000;
    if (config && config->memory_limit)
        JS_SetMemoryLimit(ecmascript->runtime, config->memory_limit);
    ecmascript->stack_size =
        config && config->stack_limit ? config->stack_limit : JS_DEFAULT_STACK_SIZE;
    JS_SetMaxStackSize(ecmascript->runtime, ecmascript->stack_size);
    JS_SetInterruptHandler(ecmascript->runtime, interrupt, ecmascript);
    if (config &&
        config->size >= offsetof(ta_datamodel_ecmascript_config, realm) + sizeof config->realm)
        ecmascript->realm = config->realm;
    if (config && config->size >= offsetof(ta_datamodel_ecmascript_config, abort_flag) +
                                      sizeof config->abort_flag)
        ecmascript->abort_flag = config->abort_flag;
    JS_SetRuntimeOpaque(ecmascript->runtime, ecmascript);
    ecmascript->auxiliary = JS_NewContext(ecmascript->runtime);
    ecmascript->base = ecmascript->explain = JS_UNDEFINED;
    ecmascript->payload_free = ecmascript->iteration_free = ecmascript->free_slot = NONE;
    return ecmascript;
}

void ta_datamodel_ecmascript_free(ta_datamodel_ecmascript *ecmascript) {
    if (!ecmascript) return;
    for (uint32_t i = 0; i < ecmascript->iteration_count; i++)
        if (ecmascript->iterations[i].items)
            ecmascript_foreach_end(&(ta_datamodel_call){ecmascript, 0, 0},
                                   &(ta_datamodel_loop){(uint64_t)i + 1, 0, 0, 0, 0});
    for (uint32_t i = 0; i < ecmascript->payload_count; i++)
        if (ecmascript->payload_refs[i])
            JS_FreeValueRT(ecmascript->runtime, ecmascript->payloads[i]);
    for (uint32_t i = 0; i < ecmascript->instance_count; i++)
        if (ecmascript->instances[i]) {
            instance_free(ecmascript, ecmascript->instances[i]);
            free(ecmascript->instances[i]);
        }
    for (uint32_t i = 0; i < ecmascript->chart_count; i++)
        chart_free(ecmascript, ecmascript->charts[i]);
    if (ecmascript->shared) JS_FreeContext(ecmascript->shared);
    JS_FreeValue(ecmascript->auxiliary, ecmascript->base);
    JS_FreeValue(ecmascript->auxiliary, ecmascript->explain);
    JS_FreeContext(ecmascript->auxiliary);
    JS_FreeRuntime(ecmascript->runtime);
    free(ecmascript->instances);
    free(ecmascript->payloads);
    free(ecmascript->payload_refs);
    free(ecmascript->payload_next);
    free(ecmascript->iterations);
    free(ecmascript->charts);
    free(ecmascript->output);
    free(ecmascript);
}

ta_datamodel_binding ta_datamodel_ecmascript_binding(ta_datamodel_ecmascript *ecmascript) {
    ta_datamodel_binding binding = {&ecmascript_vtable, ecmascript};
    return binding;
}

void ta_datamodel_ecmascript_set_memory_limit(ta_datamodel_ecmascript *ecmascript, size_t bytes) {
    JS_SetMemoryLimit(ecmascript->runtime, bytes);
}

void *ta_datamodel_ecmascript_js_runtime(ta_datamodel_ecmascript *ecmascript) {
    return ecmascript->runtime;
}

const char *ta_datamodel_ecmascript_last_error(const ta_datamodel_ecmascript *ecmascript) {
    return ecmascript ? ecmascript->error : "";
}

/* ---- snapshot / restore of the whole datamodel ------------------------------------------ */

/* { i: [[handle, hash, chart, scope, init]], p: [[handle, refs, value]], t: [[handle, items]] }
 * An isolated session's scope is an ArrayBuffer: its own blob, written in its
 * own context (its values belong to its realm), with its system variables and
 * invoke params inside. */
ta_status ta_datamodel_ecmascript_snapshot(ta_datamodel_ecmascript *ecmascript, ta_buffer *out) {
    out->length = 0;
    ecmascript->error[0] = 0;
    JSContext *context = shared_realm(ecmascript);
    if (!context) return TA_ERROR_NO_MEMORY;
    JSValue o = JS_NewObject(context), instances = JS_NewArray(context),
            pays = JS_NewArray(context), iters = JS_NewArray(context);
    uint32_t k = 0;
    ta_status status = TA_OK;
    for (uint32_t i = 0; i < ecmascript->instance_count && status == TA_OK; i++) {
        ecmascript_instance *instance = ecmascript->instances[i];
        if (!instance || !instance->live) continue;
        JSValue scope = JS_UNDEFINED, init = JS_UNDEFINED;
        if (instance->chart_context->shared) {
            scope = JS_DupValue(context, instance->scope);
            init = JS_DupValue(context, instance->init);
        } else {
            JSValue v;
            size_t sz = 0;
            uint8_t *bytes = NULL;
            status = session_value(ecmascript, instance, 1, &v);
            if (status != TA_OK) break;
            if (!(bytes = write_obj(ecmascript, instance->context, v, &sz))) {
                clear_exception(instance->context);
                explain_session(ecmascript, instance, v);
                if (!ecmascript->error[0])
                    set_error(ecmascript, "the session's variables cannot be stored together");
                status = TA_ERROR_UNSUPPORTED;
            }
            JS_FreeValue(instance->context, v);
            if (!bytes) break;
            scope = JS_NewArrayBufferCopy(context, bytes, sz);
            js_free(instance->context, bytes);
        }
        char hex[24];
        hex_hash(instance->chart_context->hash, hex);
        JSValue e = JS_NewArray(context);
        JS_SetPropertyUint32(context, e, 0, JS_NewUint32(context, i + 1));
        JS_SetPropertyUint32(context, e, 1, JS_NewString(context, hex));
        JS_SetPropertyUint32(context, e, 2, JS_NewUint32(context, instance->chart_context->chart));
        JS_SetPropertyUint32(context, e, 3, scope);
        JS_SetPropertyUint32(context, e, 4, init);
        JS_SetPropertyUint32(context, e, 5, JS_NewUint32(context, instance->chart_context->segment));
        JS_SetPropertyUint32(context, instances, k++, e);
    }
    k = 0;
    for (uint32_t i = 0; i < ecmascript->payload_count; i++) {
        if (!ecmascript->payload_refs[i]) continue;
        JSValue e = JS_NewArray(context);
        JS_SetPropertyUint32(context, e, 0, JS_NewUint32(context, i + 1));
        JS_SetPropertyUint32(context, e, 1, JS_NewUint32(context, ecmascript->payload_refs[i]));
        JS_SetPropertyUint32(context, e, 2, JS_DupValue(context, ecmascript->payloads[i]));
        JS_SetPropertyUint32(context, pays, k++, e);
    }
    k = 0;
    for (uint32_t i = 0; i < ecmascript->iteration_count; i++) {
        ecmascript_iterator *iterator = &ecmascript->iterations[i];
        if (!iterator->items) continue;
        JSValue items = JS_NewArray(context), e = JS_NewArray(context);
        for (uint32_t j = 0; j < iterator->count; j++)
            JS_SetPropertyUint32(context, items, j, JS_DupValue(context, iterator->items[j]));
        JS_SetPropertyUint32(context, e, 0, JS_NewUint32(context, i + 1));
        JS_SetPropertyUint32(context, e, 1, items);
        JS_SetPropertyUint32(context, iters, k++, e);
    }
    JS_SetPropertyStr(context, o, "i", instances);
    JS_SetPropertyStr(context, o, "p", pays);
    JS_SetPropertyStr(context, o, "t", iters);
    if (status == TA_OK) {
        size_t sz = 0;
        uint8_t *bytes = write_obj(ecmascript, context, o, &sz); /* what is written can be read */
        if (!bytes) {
            clear_exception(context);
            for (uint32_t i = 0; i < ecmascript->instance_count && !ecmascript->error[0]; i++) {
                ecmascript_instance *instance = ecmascript->instances[i];
                if (instance && instance->live && instance->chart_context->shared)
                    explain_object(ecmascript, context, instance->chart_context, instance->scope);
            }
            if (!ecmascript->error[0])
                set_error(ecmascript, "the datamodel's payloads cannot be stored");
            status = TA_ERROR_UNSUPPORTED; /* e.g. functions in the data */
        } else {
            out->length = sz;
            if (sz <= out->capacity)
                memcpy(out->pointer, bytes, sz);
            else
                status = TA_ERROR_NO_MEMORY;
            js_free(context, bytes);
        }
    }
    JS_FreeValue(context, o);
    return status;
}

/* The attached chart `chart` of the image whose content hash is `hex`. It
 * compares the hash stored at attach: a chart nobody has attached may keep
 * the image of a segment the host has freed since, so it is never read
 * here, and sessions are restored only onto attached charts. */
/* The attachment a snapshot's session names: its image's hash, the chart,
 * and the segment (two segments may run the same image; a restore keeps the
 * segments' indices). */
typedef struct chart_name {
    const char *hex;
    uint32_t chart, segment;
} chart_name;

static ecmascript_chart *chart_by_hash(ta_datamodel_ecmascript *ecmascript, chart_name name) {
    const char *hex = name.hex;
    for (uint32_t c = 0; c < ecmascript->chart_count; c++) {
        ecmascript_chart *chart_state = ecmascript->charts[c];
        if (chart_state->chart != name.chart || chart_state->segment != name.segment || !chart_state->refs) continue;
        char h[24];
        snprintf(h, sizeof h, "%016llx", (unsigned long long)chart_state->hash);
        if (!strcmp(h, hex)) return chart_state;
    }
    return NULL;
}

ta_status ta_datamodel_ecmascript_restore(ta_datamodel_ecmascript *ecmascript, ta_bytes snapshot) {
    const void *buffer = snapshot.pointer;
    size_t length = snapshot.length;
    for (uint32_t i = 0; i < ecmascript->instance_count; i++)
        if (ecmascript->instances[i] && ecmascript->instances[i]->live) return TA_ERROR_STATE;
    for (uint32_t i = 0; i < ecmascript->payload_count; i++)
        if (ecmascript->payload_refs[i]) return TA_ERROR_STATE;
    JSContext *context = shared_realm(ecmascript);
    if (!context) return TA_ERROR_NO_MEMORY;
    JS_UpdateStackTop(ecmascript->runtime);
    JSValue o = JS_ReadObject(context, (const uint8_t *)buffer, length, JS_READ_OBJ_REFERENCE);
    if (JS_IsException(o) || !JS_IsObject(o)) {
        clear_exception(context);
        JS_FreeValue(context, o);
        return TA_ERROR_INVALID;
    }
    ta_status status = TA_OK;
    JSValue instances = JS_GetPropertyStr(context, o, "i"),
            pays = JS_GetPropertyStr(context, o, "p"), iters = JS_GetPropertyStr(context, o, "t");
    int64_t n = 0;
    /* sessions, at their handles (gaps become free slots) */
    JS_GetLength(context, instances, &n);
    for (uint32_t k = 0; k < (uint32_t)n && status == TA_OK; k++) {
        JSValue e = JS_GetPropertyUint32(context, instances, k),
                hv = JS_GetPropertyUint32(context, e, 1);
        uint32_t handle = u32_at(context, e, 0), chart = u32_at(context, e, 2), segment = u32_at(context, e, 5);
        const char *hex = JS_ToCString(context, hv);
        ecmascript_chart *chart_state = hex ? chart_by_hash(ecmascript, (chart_name){hex, chart, segment}) : NULL;
        if (hex) JS_FreeCString(context, hex);
        JS_FreeValue(context, hv);
        if (!chart_state || !handle ||
            (handle <= ecmascript->instance_count && ecmascript->instances[handle - 1]->live)) {
            JS_FreeValue(context, e);
            status = TA_ERROR_INVALID;
            break;
        }
        while (ecmascript->instance_count < handle) { /* new slots: free tombstones */
            ecmascript_instance **list = (ecmascript_instance **)grow(
                ecmascript->instances, &ecmascript->instance_capacity,
                ecmascript->instance_count + 1, sizeof(ecmascript_instance *));
            ecmascript_instance *t = list ? (ecmascript_instance *)calloc(1, sizeof *t) : NULL;
            if (!t) {
                status = TA_ERROR_NO_MEMORY;
                break;
            }
            ecmascript->instances = list;
            ecmascript->instances[ecmascript->instance_count++] = t;
        }
        if (status != TA_OK) {
            JS_FreeValue(context, e);
            break;
        }
        ecmascript_instance *instance = ecmascript->instances[handle - 1];
        memset(instance, 0, sizeof *instance);
        chart_use(ecmascript, chart_state, 1);
        if (!instance_realm(ecmascript, instance, chart_state)) {
            chart_state->instance_count--;
            chart_unused(ecmascript, chart_state);
            memset(instance, 0, sizeof *instance);
            JS_FreeValue(context, e);
            status = TA_ERROR_NO_MEMORY;
            break;
        }
        instance->live = 1;
        JSValue scope = JS_GetPropertyUint32(context, e, 3);
        size_t blen = 0;
        uint8_t *blob = JS_GetArrayBuffer(context, &blen, scope);
        if (blob) { /* an isolated session's own blob, read in its own realm */
            JS_UpdateStackTop(ecmascript->runtime);
            JSValue v = JS_ReadObject(instance->context, blob, blen, JS_READ_OBJ_REFERENCE);
            status = JS_IsException(v) ? TA_ERROR_INVALID : apply_value(ecmascript, instance, v, 1);
            clear_exception(instance->context);
            JS_FreeValue(instance->context, v);
        } else {
            clear_exception(context);
            status = apply_state(ecmascript, instance, scope, 0, 0, 1);
            JS_FreeValue(instance->context, instance->init);
            instance->init = JS_GetPropertyUint32(context, e, 4);
        }
        JS_FreeValue(context, scope);
        JS_FreeValue(context, e);
    }
    ecmascript->free_slot = NONE; /* rebuild the free list from the tombstones */
    for (uint32_t i = ecmascript->instance_count; i-- > 0;)
        if (!ecmascript->instances[i]->live) {
            ecmascript->instances[i]->next_free = ecmascript->free_slot;
            ecmascript->free_slot = i;
        }
    /* payloads, at their handles */
    JS_GetLength(context, pays, &n);
    for (uint32_t k = 0; k < (uint32_t)n && status == TA_OK; k++) {
        JSValue e = JS_GetPropertyUint32(context, pays, k);
        uint32_t handle = u32_at(context, e, 0), refs = u32_at(context, e, 1);
        while (status == TA_OK && ecmascript->payload_count < handle) {
            uint32_t c1 = ecmascript->payload_capacity, c2 = ecmascript->payload_capacity,
                     c3 = ecmascript->payload_capacity;
            JSValue *pv = (JSValue *)grow(ecmascript->payloads, &c1, ecmascript->payload_count + 1,
                                          sizeof(JSValue));
            if (pv) ecmascript->payloads = pv;
            uint32_t *pr = pv ? (uint32_t *)grow(ecmascript->payload_refs, &c2,
                                                 ecmascript->payload_count + 1, 4)
                              : NULL;
            if (pr) ecmascript->payload_refs = pr;
            uint32_t *pn = pr ? (uint32_t *)grow(ecmascript->payload_next, &c3,
                                                 ecmascript->payload_count + 1, 4)
                              : NULL;
            if (!pn) {
                status = TA_ERROR_NO_MEMORY;
                break;
            }
            ecmascript->payload_next = pn;
            ecmascript->payload_capacity = c1;
            ecmascript->payloads[ecmascript->payload_count] = JS_UNDEFINED;
            ecmascript->payload_refs[ecmascript->payload_count] = 0;
            ecmascript->payload_count++;
        }
        if (status == TA_OK && handle && refs) {
            ecmascript->payloads[handle - 1] = JS_GetPropertyUint32(context, e, 2);
            ecmascript->payload_refs[handle - 1] = refs;
        }
        JS_FreeValue(context, e);
    }
    ecmascript->payload_free = NONE;
    for (uint32_t i = ecmascript->payload_count; i-- > 0;)
        if (!ecmascript->payload_refs[i]) {
            ecmascript->payload_next[i] = ecmascript->payload_free;
            ecmascript->payload_free = i;
        }
    /* <foreach> items in progress */
    JS_GetLength(context, iters, &n);
    for (uint32_t k = 0; k < (uint32_t)n && status == TA_OK; k++) {
        JSValue e = JS_GetPropertyUint32(context, iters, k),
                items = JS_GetPropertyUint32(context, e, 1);
        uint32_t handle = u32_at(context, e, 0);
        int64_t m = 0;
        JS_GetLength(context, items, &m);
        while (status == TA_OK && ecmascript->iteration_count < handle) {
            ecmascript_iterator *iterator = (ecmascript_iterator *)grow(
                ecmascript->iterations, &ecmascript->iteration_capacity,
                ecmascript->iteration_count + 1, sizeof(ecmascript_iterator));
            if (!iterator) {
                status = TA_ERROR_NO_MEMORY;
                break;
            }
            ecmascript->iterations = iterator;
            memset(&ecmascript->iterations[ecmascript->iteration_count++], 0,
                   sizeof(ecmascript_iterator));
        }
        if (status == TA_OK && handle) {
            ecmascript_iterator *iterator = &ecmascript->iterations[handle - 1];
            iterator->context = context;
            iterator->count = (uint32_t)m;
            iterator->items = (JSValue *)malloc(sizeof(JSValue) * (iterator->count + 1));
            for (uint32_t j = 0; iterator->items && j < iterator->count; j++)
                iterator->items[j] = JS_GetPropertyUint32(context, items, j);
        }
        JS_FreeValue(context, items);
        JS_FreeValue(context, e);
    }
    ecmascript->iteration_free = NONE;
    for (uint32_t i = ecmascript->iteration_count; i-- > 0;)
        if (!ecmascript->iterations[i].items) {
            ecmascript->iterations[i].count = ecmascript->iteration_free;
            ecmascript->iteration_free = i;
        }
    JS_FreeValue(context, instances);
    JS_FreeValue(context, pays);
    JS_FreeValue(context, iters);
    JS_FreeValue(context, o);
    return status;
}
