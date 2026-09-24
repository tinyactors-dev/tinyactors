#include "ecmascript_datamodel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"

/* ========================================================================
 * State, values, and memory
 */

enum { TAT_JS_HEADER = 16 };

typedef struct tat_js_value tat_js_value;

struct tat_js_state {
    ta_allocator allocator;
    JSRuntime *runtime;
    /* Reads values for encoding and value_text; never creates session values. */
    JSContext *utility;
    JSValue replacer;
    ta_arena values;
    tat_js_value *free_values;
    tat_js_value *live;
    uint32_t live_instances;
    uint64_t interrupt_budget;
    uint64_t interrupt_count;
    bool trace;
};

/*
 * A ta_value: one QuickJS reference plus its own refcount. A wire value
 * holds JSON text as a JS string and is parsed into each session that reads
 * it; a live value is the JS value itself.
 */
struct tat_js_value {
    tat_js_state *state;
    tat_js_value *prev;
    tat_js_value *next;
    size_t references;
    bool wire;
    JSValue value;
    const char *text;
    size_t text_len;
};

static void *tat_js_malloc(void *opaque, size_t size) {
    tat_js_state *state = opaque;
    if (size > SIZE_MAX - TAT_JS_HEADER) {
        return NULL;
    }
    unsigned char *block = state->allocator.v->alloc(state->allocator.self, size + TAT_JS_HEADER, TAT_JS_HEADER);
    if (block == NULL) {
        return NULL;
    }
    memcpy(block, &size, sizeof(size));
    return block + TAT_JS_HEADER;
}

static size_t tat_js_block_size(const void *ptr) {
    size_t size = 0;
    memcpy(&size, (const unsigned char *)ptr - TAT_JS_HEADER, sizeof(size));
    return size;
}

static void tat_js_free(void *opaque, void *ptr) {
    tat_js_state *state = opaque;
    if (ptr == NULL) {
        return;
    }
    size_t size = tat_js_block_size(ptr);
    state->allocator.v->free(state->allocator.self,
                             &(ta_allocator_free_params){
                                 .ptr = (unsigned char *)ptr - TAT_JS_HEADER,
                                 .size = size + TAT_JS_HEADER,
                                 .alignment = TAT_JS_HEADER,
                             });
}

static void *tat_js_calloc(void *opaque, size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) {
        return NULL;
    }
    void *ptr = tat_js_malloc(opaque, count * size);
    if (ptr != NULL) {
        memset(ptr, 0, count * size);
    }
    return ptr;
}

static void *tat_js_realloc(void *opaque, void *ptr, size_t size) {
    if (ptr == NULL) {
        return tat_js_malloc(opaque, size);
    }
    if (size == 0) {
        tat_js_free(opaque, ptr);
        return NULL;
    }
    size_t old = tat_js_block_size(ptr);
    void *grown = tat_js_malloc(opaque, size);
    if (grown == NULL) {
        return NULL;
    }
    memcpy(grown, ptr, old < size ? old : size);
    tat_js_free(opaque, ptr);
    return grown;
}

static size_t tat_js_usable_size(const void *ptr) {
    return ptr == NULL ? 0 : tat_js_block_size(ptr);
}

static const JSMallocFunctions TAT_JS_MALLOC = {
    .js_calloc = tat_js_calloc,
    .js_malloc = tat_js_malloc,
    .js_free = tat_js_free,
    .js_realloc = tat_js_realloc,
    .js_malloc_usable_size = tat_js_usable_size,
};

static int tat_js_interrupt(JSRuntime *runtime, void *opaque) {
    (void)runtime;
    tat_js_state *state = opaque;
    state->interrupt_count += 1;
    return state->interrupt_count > state->interrupt_budget;
}

/* Starts a unit of work: a fresh interrupt budget and the current stack top. */
static void tat_js_begin_work(tat_js_state *state) {
    state->interrupt_count = 0;
    JS_UpdateStackTop(state->runtime);
}

static void tat_js_value_retain(void *self);
static void tat_js_value_release(void *self);

/*
 * No clone: a value is a handle, and retain/release is the whole contract
 * (ADR 0050). The old clone shared anyway — a new wrapper around the same
 * JS value — so nothing here loses isolation it ever had.
 */
static const ta_value_vtable TAT_JS_VALUE_VTABLE = {
    .retain = tat_js_value_retain,
    .release = tat_js_value_release,
};

/* Wraps `value`, consuming it. */
static ta_result tat_js_wrap(tat_js_state *state, JSValue value, bool wire, ta_value *out) {
    tat_js_value *wrapped = state->free_values;
    if (wrapped != NULL) {
        state->free_values = wrapped->next;
    } else {
        wrapped = ta_arena_alloc(&state->values, sizeof(*wrapped), _Alignof(tat_js_value));
    }
    if (wrapped == NULL) {
        JS_FreeValueRT(state->runtime, value);
        return TA_ERR_NOMEM;
    }
    *wrapped = (tat_js_value){
        .state = state,
        .next = state->live,
        .references = 1,
        .wire = wire,
        .value = value,
    };
    if (state->live != NULL) {
        state->live->prev = wrapped;
    }
    state->live = wrapped;
    *out = (ta_value){.self = wrapped, .v = &TAT_JS_VALUE_VTABLE};
    return TA_OK;
}

static tat_js_value *tat_js_unwrap(const ta_value *value) {
    if (value == NULL || value->v != &TAT_JS_VALUE_VTABLE || value->self == NULL) {
        return NULL;
    }
    return value->self;
}

bool tat_js_value_is(const ta_value *value) {
    return tat_js_unwrap(value) != NULL;
}

static void tat_js_value_retain(void *self) {
    tat_js_value *value = self;
    value->references += 1;
}

static void tat_js_value_release(void *self) {
    tat_js_value *value = self;
    value->references -= 1;
    if (value->references != 0) {
        return;
    }
    tat_js_state *state = value->state;
    if (value->text != NULL) {
        JS_FreeCStringRT(state->runtime, value->text);
    }
    JS_FreeValueRT(state->runtime, value->value);
    if (value->prev != NULL) {
        value->prev->next = value->next;
    } else {
        state->live = value->next;
    }
    if (value->next != NULL) {
        value->next->prev = value->prev;
    }
    *value = (tat_js_value){.next = state->free_values};
    state->free_values = value;
}

/* ========================================================================
 * Context lifecycle
 */

static const char TAT_JS_REPLACER[] =
    "(function (key, value) {"
    " var type = typeof value;"
    " return type === 'undefined' || type === 'function' || type === 'symbol' ? null : value;"
    " })";

ta_result tat_js_datamodel_context_init(tat_js_datamodel_context *context,
                                        const tat_js_datamodel_config *config) {
    if (context == NULL || config == NULL || config->allocator.v == NULL) {
        return TA_ERR_INVALID;
    }
    *context = (tat_js_datamodel_context){0};
    ta_allocator allocator = config->allocator;
    tat_js_state *state = allocator.v->alloc(allocator.self, sizeof(*state), _Alignof(tat_js_state));
    if (state == NULL) {
        return TA_ERR_NOMEM;
    }
    *state = (tat_js_state){
        .allocator = allocator,
        .interrupt_budget = config->interrupt_budget != 0 ? config->interrupt_budget : 10000u,
        .replacer = JS_UNDEFINED,
        .trace = getenv("TAT_JS_TRACE") != NULL,
    };
    ta_result result = ta_arena_init(&state->values, allocator, 4096);
    if (result != TA_OK) {
        allocator.v->free(allocator.self, &(ta_allocator_free_params){.ptr = state, .size = sizeof(*state),
                                                                       .alignment = _Alignof(tat_js_state)});
        return result;
    }
    state->runtime = JS_NewRuntime2(&TAT_JS_MALLOC, state);
    if (state->runtime != NULL) {
        JS_SetMemoryLimit(state->runtime, config->memory_limit != 0 ? config->memory_limit : 256u * 1024u * 1024u);
        JS_SetMaxStackSize(state->runtime, config->stack_size != 0 ? config->stack_size : 1024u * 1024u);
        JS_SetInterruptHandler(state->runtime, tat_js_interrupt, state);
        state->utility = JS_NewContext(state->runtime);
    }
    if (state->utility != NULL) {
        state->replacer = JS_Eval(state->utility, TAT_JS_REPLACER, sizeof(TAT_JS_REPLACER) - 1, "<replacer>",
                                  JS_EVAL_TYPE_GLOBAL);
    }
    if (state->utility == NULL || JS_IsException(state->replacer)) {
        if (state->utility != NULL) {
            JS_FreeContext(state->utility);
        }
        if (state->runtime != NULL) {
            JS_FreeRuntime(state->runtime);
        }
        ta_arena_destroy(&state->values);
        allocator.v->free(allocator.self, &(ta_allocator_free_params){.ptr = state, .size = sizeof(*state),
                                                                       .alignment = _Alignof(tat_js_state)});
        return TA_ERR_NOMEM;
    }
    context->state = state;
    return TA_OK;
}

void tat_js_datamodel_context_destroy(tat_js_datamodel_context *context) {
    if (context == NULL || context->state == NULL) {
        return;
    }
    tat_js_state *state = context->state;
    ta_allocator allocator = state->allocator;
    if (state->live_instances == 0) {
        /* Values the RPC side still holds; nobody may release them later. */
        for (tat_js_value *value = state->live; value != NULL; value = value->next) {
            if (value->text != NULL) {
                JS_FreeCStringRT(state->runtime, value->text);
            }
            JS_FreeValueRT(state->runtime, value->value);
        }
        JS_FreeValue(state->utility, state->replacer);
        JS_FreeContext(state->utility);
        JS_RunGC(state->runtime);
        JS_FreeRuntime(state->runtime);
        ta_arena_destroy(&state->values);
        allocator.v->free(allocator.self, &(ta_allocator_free_params){.ptr = state, .size = sizeof(*state),
                                                                       .alignment = _Alignof(tat_js_state)});
    }
    /*
     * Otherwise some session was never destroyed (the RPC host does not
     * destroy systems at exit). JS_FreeRuntime would assert on its objects,
     * so the runtime is left to process exit.
     */
    context->state = NULL;
}

/* ========================================================================
 * Wire JSON
 */

static bool tat_js_json_marker(const char *kind, tat_js_json *out) {
    static const char undefined_marker[] = "{\"$value\":\"undefined\"}";
    static const char function_marker[] = "{\"$value\":\"function\"}";
    static const char symbol_marker[] = "{\"$value\":\"symbol\"}";
    static const char unserializable_marker[] = "{\"$value\":\"unserializable\"}";
    const char *text = unserializable_marker;
    if (strcmp(kind, "undefined") == 0) {
        text = undefined_marker;
    } else if (strcmp(kind, "function") == 0) {
        text = function_marker;
    } else if (strcmp(kind, "symbol") == 0) {
        text = symbol_marker;
    }
    *out = (tat_js_json){.ptr = text, .len = strlen(text), .owner = NULL};
    return true;
}

static void tat_js_clear_exception(JSContext *context) {
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
}

ta_result tat_js_value_to_json(const ta_value *value, tat_js_json *out) {
    tat_js_value *wrapped = tat_js_unwrap(value);
    if (wrapped == NULL || out == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    tat_js_state *state = wrapped->state;
    JSContext *utility = state->utility;
    tat_js_begin_work(state);
    JSValue text;
    if (wrapped->wire) {
        text = JS_DupValue(utility, wrapped->value);
    } else {
        JSValue v = wrapped->value;
        if (JS_IsUndefined(v)) {
            tat_js_json_marker("undefined", out);
            return TA_OK;
        }
        if (JS_IsFunction(utility, v)) {
            tat_js_json_marker("function", out);
            return TA_OK;
        }
        if (JS_IsSymbol(v)) {
            tat_js_json_marker("symbol", out);
            return TA_OK;
        }
        text = JS_JSONStringify(utility, v, state->replacer, JS_UNDEFINED);
        if (JS_IsException(text)) {
            tat_js_clear_exception(utility);
            tat_js_json_marker("unserializable", out);
            return TA_OK;
        }
        if (!JS_IsString(text)) {
            JS_FreeValue(utility, text);
            tat_js_json_marker("undefined", out);
            return TA_OK;
        }
    }
    size_t length = 0;
    const char *chars = JS_ToCStringLen(utility, &length, text);
    JS_FreeValue(utility, text);
    if (chars == NULL) {
        tat_js_clear_exception(utility);
        return TA_ERR_NOMEM;
    }
    *out = (tat_js_json){.ptr = chars, .len = length, .owner = state};
    return TA_OK;
}

void tat_js_json_release(tat_js_json *json) {
    if (json == NULL) {
        return;
    }
    if (json->owner != NULL && json->ptr != NULL) {
        JS_FreeCStringRT(json->owner->runtime, json->ptr);
    }
    *json = (tat_js_json){0};
}

ta_result tat_js_value_from_json(tat_js_datamodel_context *context, ta_str json, ta_value *out) {
    if (context == NULL || context->state == NULL || out == NULL || (json.ptr == NULL && json.len != 0)) {
        return TA_ERR_INVALID;
    }
    tat_js_state *state = context->state;
    JSValue text = JS_NewStringLen(state->utility, json.len == 0 ? "" : json.ptr, json.len);
    if (JS_IsException(text)) {
        tat_js_clear_exception(state->utility);
        return TA_ERR_NOMEM;
    }
    return tat_js_wrap(state, text, true, out);
}

/* ========================================================================
 * Definitions and instances
 */

typedef struct {
    const ta_expression *expressions;
    uint32_t expression_count;
    ta_allocator allocator;
} tat_js_shared;

typedef struct {
    tat_js_state *state;
    tat_js_shared *shared;
    JSContext *context;
    const ta_dm_host *host;
    JSValue declared;
    JSValue bound;
    JSValue event;
    bool has_event;
    ta_event event_key;
    JSValue session_id;
    JSValue session_name;
    JSValue ioprocessors;
} tat_js_instance;

enum {
    TAT_JS_EVENT = 0,
    TAT_JS_SESSION_ID,
    TAT_JS_SESSION_NAME,
    TAT_JS_IOPROCESSORS,
    TAT_JS_SYSTEM_COUNT
};

static const char *const TAT_JS_SYSTEM_NAMES[TAT_JS_SYSTEM_COUNT] = {
    "_event", "_sessionid", "_name", "_ioprocessors",
};

static void tat_js_fault(ta_fault *fault, int32_t code, const char *message) {
    if (fault == NULL) {
        return;
    }
    *fault = (ta_fault){.code = code, .message = {.ptr = message, .len = (uint32_t)strlen(message)}};
}

/* Clears the pending exception and reports `message` as the fault. */
static ta_op_status tat_js_error(tat_js_instance *instance, ta_fault *fault, const char *message) {
    JSContext *context = instance->context;
    JSValue exception = JS_GetException(context);
    if (instance->state->trace) {
        const char *text = JS_ToCString(context, exception);
        fprintf(stderr, "tinyactors-test ecmascript: %s: %s\n", message, text != NULL ? text : "?");
        if (text != NULL) {
            JS_FreeCString(context, text);
        } else {
            tat_js_clear_exception(context);
        }
    }
    JS_FreeValue(context, exception);
    tat_js_fault(fault, TA_ERR_INVALID, message);
    return TA_OP_ERROR;
}

static ta_op_status tat_js_nomem(ta_fault *fault) {
    tat_js_fault(fault, TA_ERR_NOMEM, "ecmascript: out of memory");
    return TA_OP_ERROR;
}

static void tat_js_begin(tat_js_instance *instance, const ta_dm_host *host) {
    instance->host = host;
    tat_js_begin_work(instance->state);
}

static ta_str tat_js_expression(const tat_js_instance *instance, ta_expr_id id) {
    const tat_js_shared *shared = instance->shared;
    if (shared == NULL || id >= shared->expression_count ||
        shared->expressions[id].kind != TA_EXPRESSION_SOURCE) {
        return (ta_str){0};
    }
    return shared->expressions[id].as.source;
}

/* Evaluates prefix + source + suffix as global code. The buffer is NUL-terminated for JS_Eval. */
typedef struct {
    const char *prefix;
    ta_str source;
    const char *suffix;
    int flags;
} tat_js_eval_params;

static JSValue tat_js_eval(tat_js_instance *instance, const tat_js_eval_params *params) {
    JSContext *context = instance->context;
    size_t prefix = strlen(params->prefix);
    size_t suffix = strlen(params->suffix);
    size_t length = prefix + params->source.len + suffix;
    char *buffer = js_malloc(context, length + 1);
    if (buffer == NULL) {
        return JS_EXCEPTION;
    }
    memcpy(buffer, params->prefix, prefix);
    if (params->source.len != 0) {
        memcpy(buffer + prefix, params->source.ptr, params->source.len);
    }
    memcpy(buffer + prefix + params->source.len, params->suffix, suffix);
    buffer[length] = '\0';
    JSValue result = JS_Eval(context, buffer, length, "<scxml>", JS_EVAL_TYPE_GLOBAL | params->flags);
    js_free(context, buffer);
    return result;
}

static bool tat_js_is_syntax_error(JSContext *context, JSValueConst exception) {
    if (!JS_IsError(exception)) {
        return false;
    }
    JSValue name = JS_GetPropertyStr(context, exception, "name");
    const char *text = JS_ToCString(context, name);
    JS_FreeValue(context, name);
    bool syntax = text != NULL && strcmp(text, "SyntaxError") == 0;
    if (text != NULL) {
        JS_FreeCString(context, text);
    } else {
        tat_js_clear_exception(context);
    }
    return syntax;
}

/*
 * A value expression: first as a parenthesized expression, so function and
 * object literals read as values; on a SyntaxError (e.g. a trailing `;`),
 * as a global script whose completion value is the result. A compile error
 * runs no code, so side effects never repeat.
 */
static JSValue tat_js_eval_expression(tat_js_instance *instance, ta_str source) {
    JSContext *context = instance->context;
    if (source.ptr == NULL) {
        return JS_ThrowSyntaxError(context, "unknown expression");
    }
    JSValue value = tat_js_eval(instance, &(tat_js_eval_params){.prefix = "(", .source = source, .suffix = "\n)"});
    if (!JS_IsException(value)) {
        return value;
    }
    JSValue exception = JS_GetException(context);
    if (!tat_js_is_syntax_error(context, exception)) {
        return JS_Throw(context, exception);
    }
    JS_FreeValue(context, exception);
    return tat_js_eval(instance, &(tat_js_eval_params){.prefix = "", .source = source, .suffix = ""});
}

/*
 * Imports a ta_value into this session: wire JSON is parsed in the
 * session's realm, live values are shared, absent values are undefined.
 * A foreign value throws.
 */
static JSValue tat_js_import(tat_js_instance *instance, const ta_value *value) {
    JSContext *context = instance->context;
    if (value == NULL || value->v == NULL) {
        return JS_UNDEFINED;
    }
    tat_js_value *wrapped = tat_js_unwrap(value);
    if (wrapped == NULL || wrapped->state != instance->state) {
        return JS_ThrowTypeError(context, "not an ECMAScript data-model value");
    }
    if (!wrapped->wire) {
        return JS_DupValue(context, wrapped->value);
    }
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, wrapped->value);
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    JSValue parsed = JS_ParseJSON(context, text, length, "<wire>");
    JS_FreeCString(context, text);
    return parsed;
}

static JSAtom tat_js_atom(JSContext *context, ta_str name) {
    return JS_NewAtomLen(context, name.len == 0 ? "" : name.ptr, name.len);
}

/* Records a name in `declared`, the set inspect() reports. */
static bool tat_js_note_declared(tat_js_instance *instance, JSAtom atom) {
    return JS_DefinePropertyValue(instance->context, instance->declared, atom, JS_TRUE, JS_PROP_C_W_E) >= 0;
}

/* Sets a global variable, creating it when missing. Consumes `value`. */
static bool tat_js_set_global(tat_js_instance *instance, JSAtom atom, JSValue value) {
    JSValue global = JS_GetGlobalObject(instance->context);
    int set = JS_SetProperty(instance->context, global, atom, value);
    JS_FreeValue(instance->context, global);
    return set >= 0;
}

/* ------------------------------------------------------------------------
 * System variables and In()
 */

/* One SCXML system-variable field as a JS string, or undefined when absent (ADR 0068: a view). */
static JSValue tat_js_host_io_text(tat_js_instance *instance, ta_dm_text_kind kind, uint32_t io_index) {
    ta_str text = {0};
    ta_result result =
        ta_dm_host_text(instance->host, &(ta_dm_text_params){.kind = kind, .io_index = io_index, .out = &text});
    if (result == TA_ERR_NOMEM) {
        return JS_ThrowOutOfMemory(instance->context);
    }
    return result == TA_OK ? JS_NewStringLen(instance->context, text.len == 0 ? "" : text.ptr, text.len)
                           : JS_UNDEFINED;
}

static JSValue tat_js_host_text(tat_js_instance *instance, ta_dm_text_kind kind) {
    return tat_js_host_io_text(instance, kind, 0);
}

static bool tat_js_same_event(const ta_event *a, const ta_event *b) {
    return a->name == b->name && a->type == b->type && a->send_id == b->send_id && a->invoke_id == b->invoke_id &&
           a->origin.kind == b->origin.kind && a->data.self == b->data.self && a->data.v == b->data.v;
}

static void tat_js_forget_event(tat_js_instance *instance) {
    if (instance->has_event) {
        JS_FreeValue(instance->context, instance->event);
        ta_value_release(&instance->event_key.data);
    }
    instance->event = JS_UNDEFINED;
    instance->has_event = false;
}

/* _event: undefined before the first event; one frozen object per event. */
static JSValue tat_js_event(tat_js_instance *instance) {
    JSContext *context = instance->context;
    const ta_event *event = instance->host == NULL ? NULL : ta_dm_host_event(instance->host);
    if (event == NULL) {
        return JS_UNDEFINED;
    }
    if (instance->has_event && tat_js_same_event(&instance->event_key, event)) {
        return JS_DupValue(context, instance->event);
    }
    static const struct {
        const char *name;
        ta_dm_text_kind kind;
    } fields[] = {
        {"name", TA_DM_TEXT_EVENT_NAME},         {"type", TA_DM_TEXT_EVENT_TYPE},
        {"sendid", TA_DM_TEXT_EVENT_SENDID},     {"origin", TA_DM_TEXT_EVENT_ORIGIN},
        {"origintype", TA_DM_TEXT_EVENT_ORIGINTYPE}, {"invokeid", TA_DM_TEXT_EVENT_INVOKEID},
    };
    JSValue object = JS_NewObject(context);
    if (JS_IsException(object)) {
        return object;
    }
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); index += 1) {
        JSValue field = tat_js_host_text(instance, fields[index].kind);
        if (JS_IsException(field) ||
            JS_DefinePropertyValueStr(context, object, fields[index].name, field, JS_PROP_ENUMERABLE) < 0) {
            JS_FreeValue(context, object);
            return JS_EXCEPTION;
        }
    }
    JSValue data = tat_js_import(instance, &event->data);
    if (JS_IsException(data) || JS_DefinePropertyValueStr(context, object, "data", data, JS_PROP_ENUMERABLE) < 0) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    JS_FreezeObject(context, object);
    tat_js_forget_event(instance);
    instance->event = JS_DupValue(context, object);
    instance->event_key = *event;
    ta_value_retain(&instance->event_key.data);
    instance->has_event = true;
    return object;
}

/*
 * _ioprocessors: one frozen {location} per processor core lists, keyed by its
 * type (SCXML 5.10, B.2, ADR 0068). Index 0, the SCXML processor, is also
 * reachable as "scxml". A processor without a location has an entry with no
 * location field.
 */
static JSValue tat_js_ioprocessors(tat_js_instance *instance) {
    JSContext *context = instance->context;
    JSValue processors = JS_NewObject(context);
    if (JS_IsException(processors)) {
        return processors;
    }
    for (uint32_t index = 0;; index += 1) {
        JSValue type = tat_js_host_io_text(instance, TA_DM_TEXT_IO_TYPE, index);
        if (JS_IsException(type)) {
            JS_FreeValue(context, processors);
            return type;
        }
        if (!JS_IsString(type)) {
            break;
        }
        JSValue location = tat_js_host_io_text(instance, TA_DM_TEXT_IO_LOCATION, index);
        JSValue processor = JS_NewObject(context);
        if (JS_IsException(location) || JS_IsException(processor)) {
            JS_FreeValue(context, type);
            JS_FreeValue(context, location);
            JS_FreeValue(context, processor);
            JS_FreeValue(context, processors);
            return JS_EXCEPTION;
        }
        if (JS_IsString(location)) {
            JS_DefinePropertyValueStr(context, processor, "location", location, JS_PROP_ENUMERABLE);
        } else {
            JS_FreeValue(context, location);
        }
        JS_FreezeObject(context, processor);
        if (index == 0) {
            JS_DefinePropertyValueStr(context, processors, "scxml", JS_DupValue(context, processor), JS_PROP_ENUMERABLE);
        }
        JSAtom atom = JS_ValueToAtom(context, type);
        JS_DefinePropertyValue(context, processors, atom, processor, JS_PROP_ENUMERABLE);
        JS_FreeAtom(context, atom);
        JS_FreeValue(context, type);
    }
    JS_FreezeObject(context, processors);
    return processors;
}

static JSValue tat_js_cached(tat_js_instance *instance, JSValue *slot, JSValue (*make)(tat_js_instance *)) {
    if (JS_IsUninitialized(*slot)) {
        JSValue made = make(instance);
        if (JS_IsException(made)) {
            return made;
        }
        *slot = made;
    }
    return JS_DupValue(instance->context, *slot);
}

static JSValue tat_js_make_session_id(tat_js_instance *instance) {
    return tat_js_host_text(instance, TA_DM_TEXT_SESSION_ID);
}

static JSValue tat_js_make_session_name(tat_js_instance *instance) {
    return tat_js_host_text(instance, TA_DM_TEXT_SESSION_NAME);
}

static JSValue tat_js_system_get(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv,
                                 int magic) {
    (void)this_value;
    (void)argc;
    (void)argv;
    tat_js_instance *instance = JS_GetContextOpaque(context);
    if (instance == NULL || instance->host == NULL) {
        return JS_UNDEFINED;
    }
    switch (magic) {
        case TAT_JS_EVENT:
            return tat_js_event(instance);
        case TAT_JS_SESSION_ID:
            return tat_js_cached(instance, &instance->session_id, tat_js_make_session_id);
        case TAT_JS_SESSION_NAME:
            return tat_js_cached(instance, &instance->session_name, tat_js_make_session_name);
        case TAT_JS_IOPROCESSORS:
            return tat_js_cached(instance, &instance->ioprocessors, tat_js_ioprocessors);
        default:
            return JS_UNDEFINED;
    }
}

static JSValue tat_js_system_set(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv,
                                 int magic) {
    (void)this_value;
    (void)argc;
    (void)argv;
    return JS_ThrowTypeError(context, "%s is read-only", TAT_JS_SYSTEM_NAMES[magic]);
}


static JSValue tat_js_in(JSContext *context, JSValueConst this_value, int argc, JSValueConst *argv) {
    (void)this_value;
    tat_js_instance *instance = JS_GetContextOpaque(context);
    if (instance == NULL) {
        return JS_ThrowReferenceError(context, "In() outlived its session");
    }
    if (argc < 1 || instance->host == NULL) {
        return JS_FALSE;
    }
    size_t length = 0;
    const char *id = JS_ToCStringLen(context, &length, argv[0]);
    if (id == NULL) {
        return JS_EXCEPTION;
    }
    /* Core owns the author ID -> state ID mapping (ADR 0049); a state the
     * chart does not have is TA_ERR_NOT_FOUND, and In() of it is false. */
    ta_state_id state = 0;
    bool active = ta_dm_host_find_state(instance->host, (ta_str){.ptr = id, .len = (uint32_t)length}, &state) == TA_OK &&
                  ta_dm_host_in_state(instance->host, state);
    JS_FreeCString(context, id);
    return JS_NewBool(context, active);
}

static bool tat_js_install_globals(tat_js_instance *instance) {
    JSContext *context = instance->context;
    JSValue global = JS_GetGlobalObject(context);
    bool ok = true;
    for (int index = 0; index < TAT_JS_SYSTEM_COUNT && ok; index += 1) {
        JSValue getter = JS_NewCFunctionMagic(context, tat_js_system_get, TAT_JS_SYSTEM_NAMES[index], 0,
                                              JS_CFUNC_generic_magic, index);
        JSValue setter = JS_NewCFunctionMagic(context, tat_js_system_set, TAT_JS_SYSTEM_NAMES[index], 1,
                                              JS_CFUNC_generic_magic, index);
        JSAtom atom = JS_NewAtom(context, TAT_JS_SYSTEM_NAMES[index]);
        ok = JS_DefinePropertyGetSet(context, global, atom, getter, setter, JS_PROP_ENUMERABLE) >= 0;
        JS_FreeAtom(context, atom);
    }
    if (ok) {
        JSValue platform = JS_NewObject(context);
        JS_FreezeObject(context, platform);
        ok = JS_DefinePropertyValueStr(context, global, "_x", platform, JS_PROP_ENUMERABLE) >= 0;
    }
    if (ok) {
        JSValue in = JS_NewCFunction(context, tat_js_in, "In", 1);
        ok = JS_DefinePropertyValueStr(context, global, "In", in, JS_PROP_ENUMERABLE) >= 0;
    }
    JS_FreeValue(context, global);
    return ok;
}

static bool tat_js_reserved_name(ta_str name) {
    static const char *const reserved[] = {"_event", "_sessionid", "_name", "_ioprocessors", "_x", "In"};
    for (size_t index = 0; index < sizeof(reserved) / sizeof(reserved[0]); index += 1) {
        size_t length = strlen(reserved[index]);
        if (name.len == length && memcmp(name.ptr, reserved[index], length) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Vtable
 */

static ta_result tat_js_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    if (params == NULL || params->shared == NULL || params->allocator.v == NULL ||
        (params->expressions == NULL && params->expression_count != 0)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < params->expression_count; index += 1) {
        ta_expression_kind kind = params->expressions[index].kind;
        if (kind != TA_EXPRESSION_NONE && kind != TA_EXPRESSION_SOURCE) {
            return TA_ERR_UNSUPPORTED;
        }
    }
    tat_js_shared *shared = params->allocator.v->alloc(params->allocator.self, sizeof(*shared),
                                                       _Alignof(tat_js_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (tat_js_shared){
        .expressions = params->expressions,
        .expression_count = params->expression_count,
        .allocator = params->allocator,
    };
    *params->shared = shared;
    return TA_OK;
}

static void tat_js_destroy_definition(void *self, void *shared_state, ta_allocator allocator) {
    (void)self;
    tat_js_shared *shared = shared_state;
    if (shared == NULL || allocator.v == NULL) {
        return;
    }
    allocator.v->free(allocator.self, &(ta_allocator_free_params){.ptr = shared, .size = sizeof(*shared),
                                                                   .alignment = _Alignof(tat_js_shared)});
}

static uint32_t tat_js_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)sizeof(tat_js_instance);
}

static uint32_t tat_js_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)_Alignof(tat_js_instance);
}

static void tat_js_teardown(tat_js_instance *instance) {
    JSContext *context = instance->context;
    tat_js_forget_event(instance);
    JS_FreeValue(context, instance->declared);
    JS_FreeValue(context, instance->bound);
    JS_FreeValue(context, instance->session_id);
    JS_FreeValue(context, instance->session_name);
    JS_FreeValue(context, instance->ioprocessors);
    JS_SetContextOpaque(context, NULL);
    JS_FreeContext(context);
    JS_RunGC(instance->state->runtime);
    instance->state->live_instances -= 1;
    *instance = (tat_js_instance){0};
}

/* Installs each own enumerable member of the binding object as a global. */
static ta_result tat_js_bind(tat_js_instance *instance, const ta_value *binding) {
    JSContext *context = instance->context;
    if (!tat_js_value_is(binding)) {
        return TA_ERR_UNSUPPORTED;
    }
    JSValue object = tat_js_import(instance, binding);
    if (JS_IsException(object)) {
        tat_js_clear_exception(context);
        return TA_ERR_INVALID;
    }
    if (!JS_IsObject(object) || JS_IsArray(object)) {
        JS_FreeValue(context, object);
        return TA_ERR_INVALID;
    }
    JSPropertyEnum *names = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &names, &count, object, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        tat_js_clear_exception(context);
        JS_FreeValue(context, object);
        return TA_ERR_NOMEM;
    }
    ta_result result = TA_OK;
    JSValue global = JS_GetGlobalObject(context);
    for (uint32_t index = 0; index < count && result == TA_OK; index += 1) {
        JSAtom atom = names[index].atom;
        JSValue value = JS_GetProperty(context, object, atom);
        if (JS_IsException(value) ||
            JS_DefinePropertyValue(context, global, atom, value, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE) < 0 ||
            JS_DefinePropertyValue(context, instance->bound, atom, JS_TRUE, JS_PROP_C_W_E) < 0 ||
            !tat_js_note_declared(instance, atom)) {
            tat_js_clear_exception(context);
            result = TA_ERR_INVALID;
        }
    }
    JS_FreeValue(context, global);
    JS_FreePropertyEnum(context, names, count);
    JS_FreeValue(context, object);
    return result;
}

static ta_result tat_js_init(void *self, const ta_datamodel_init_instance_params *params) {
    tat_js_datamodel_context *adapter = self;
    if (adapter == NULL || adapter->state == NULL || params == NULL || params->shared == NULL ||
        params->instance == NULL) {
        return TA_ERR_INVALID;
    }
    tat_js_state *state = adapter->state;
    tat_js_instance *instance = params->instance;
    *instance = (tat_js_instance){
        .state = state,
        .shared = params->shared,
        .host = params->host,
        .declared = JS_UNDEFINED,
        .bound = JS_UNDEFINED,
        .event = JS_UNDEFINED,
        .session_id = JS_UNINITIALIZED,
        .session_name = JS_UNINITIALIZED,
        .ioprocessors = JS_UNINITIALIZED,
    };
    tat_js_begin_work(state);
    instance->context = JS_NewContext(state->runtime);
    if (instance->context == NULL) {
        return TA_ERR_NOMEM;
    }
    JS_SetContextOpaque(instance->context, instance);
    state->live_instances += 1;
    instance->declared = JS_NewObjectProto(instance->context, JS_NULL);
    instance->bound = JS_NewObjectProto(instance->context, JS_NULL);
    ta_result result = TA_OK;
    if (JS_IsException(instance->declared) || JS_IsException(instance->bound) ||
        !tat_js_install_globals(instance)) {
        tat_js_clear_exception(instance->context);
        result = TA_ERR_NOMEM;
    }
    if (result == TA_OK && params->binding != NULL && params->binding->v != NULL) {
        result = tat_js_bind(instance, params->binding);
    }
    if (result != TA_OK) {
        if (JS_IsException(instance->declared)) {
            instance->declared = JS_UNDEFINED;
        }
        if (JS_IsException(instance->bound)) {
            instance->bound = JS_UNDEFINED;
        }
        tat_js_teardown(instance);
    }
    return result;
}

static void tat_js_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    tat_js_instance *js = instance;
    if (js != NULL && js->context != NULL) {
        tat_js_teardown(js);
    }
}

static ta_op_status tat_js_declare(void *self, const ta_datamodel_declare_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    if (params->name.ptr == NULL || tat_js_reserved_name(params->name)) {
        tat_js_fault(params->fault, TA_ERR_INVALID, "ecmascript: data ID names a system variable");
        return TA_OP_ERROR;
    }
    JSContext *context = instance->context;
    JSAtom atom = tat_js_atom(context, params->name);
    int bound = JS_HasProperty(context, instance->bound, atom);
    if (bound > 0) {
        if (params->bound != NULL) {
            *params->bound = true;
        }
        JS_FreeAtom(context, atom);
        return TA_OP_OK;
    }
    JSValue global = JS_GetGlobalObject(context);
    bool ok = bound == 0 &&
              JS_DefinePropertyValue(context, global, atom, JS_UNDEFINED,
                                     JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_THROW) >= 0 &&
              tat_js_note_declared(instance, atom);
    JS_FreeValue(context, global);
    JS_FreeAtom(context, atom);
    return ok ? TA_OP_OK : tat_js_error(instance, params->fault, "ecmascript: could not declare data");
}

static ta_op_status tat_js_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    JSValue value = tat_js_import(instance, params->value);
    if (JS_IsException(value)) {
        return tat_js_error(instance, params->fault, "ecmascript: data value is not an ECMAScript value");
    }
    JSAtom atom = tat_js_atom(context, params->name);
    bool ok = tat_js_set_global(instance, atom, value) && tat_js_note_declared(instance, atom);
    JS_FreeAtom(context, atom);
    return ok ? TA_OP_OK : tat_js_error(instance, params->fault, "ecmascript: could not set data");
}

static ta_op_status tat_js_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSValue value = tat_js_eval_expression(instance, tat_js_expression(instance, params->expr));
    if (JS_IsException(value)) {
        return tat_js_error(instance, params->fault, "ecmascript: expression threw");
    }
    return tat_js_wrap(instance->state, value, false, params->out) == TA_OK ? TA_OP_OK : tat_js_nomem(params->fault);
}

static ta_op_status tat_js_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSValue value = tat_js_eval_expression(instance, tat_js_expression(instance, params->expr));
    if (JS_IsException(value)) {
        return tat_js_error(instance, params->fault, "ecmascript: condition threw");
    }
    int truth = JS_ToBool(instance->context, value);
    JS_FreeValue(instance->context, value);
    if (truth < 0) {
        return tat_js_error(instance, params->fault, "ecmascript: condition threw");
    }
    *params->out = truth != 0;
    return TA_OP_OK;
}

static ta_op_status tat_js_read_location(void *self, const ta_datamodel_read_location_params *params) {
    return tat_js_eval_value(self, &(ta_datamodel_eval_value_params){
                                       .shared = params->shared,
                                       .instance = params->instance,
                                       .expr = params->location,
                                       .host = params->host,
                                       .out = params->out,
                                       .fault = params->fault,
                                   });
}

/* Any left-hand-side expression; strict mode makes an undeclared root an error. */
static ta_op_status tat_js_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    ta_str location = tat_js_expression(instance, params->location);
    if (location.ptr == NULL) {
        tat_js_fault(params->fault, TA_ERR_INVALID, "ecmascript: unknown location");
        return TA_OP_ERROR;
    }
    JSValue value = tat_js_import(instance, params->value);
    if (JS_IsException(value)) {
        return tat_js_error(instance, params->fault, "ecmascript: assigned value is not an ECMAScript value");
    }
    JSValue assign = tat_js_eval(instance, &(tat_js_eval_params){
                                               .prefix = "(function (__tat_assign_value) { \"use strict\"; ",
                                               .source = location,
                                               .suffix = "\n= __tat_assign_value; })",
                                           });
    if (JS_IsException(assign)) {
        JS_FreeValue(context, value);
        return tat_js_error(instance, params->fault, "ecmascript: invalid location");
    }
    JSValue result = JS_Call(context, assign, JS_UNDEFINED, 1, (JSValueConst *)&value);
    JS_FreeValue(context, assign);
    JS_FreeValue(context, value);
    if (JS_IsException(result)) {
        return tat_js_error(instance, params->fault, "ecmascript: assign threw");
    }
    JS_FreeValue(context, result);
    return TA_OP_OK;
}

static ta_op_status tat_js_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    ta_str source = tat_js_expression(instance, params->script);
    if (source.ptr == NULL) {
        tat_js_fault(params->fault, TA_ERR_INVALID, "ecmascript: unknown script");
        return TA_OP_ERROR;
    }
    JSValue result = tat_js_eval(instance, &(tat_js_eval_params){.prefix = "", .source = source, .suffix = ""});
    if (JS_IsException(result)) {
        return tat_js_error(instance, params->fault, "ecmascript: script threw");
    }
    JS_FreeValue(instance->context, result);
    return TA_OP_OK;
}


/* An object keyed by parameter name; a later duplicate replaces an earlier one. Values stay live. */
static ta_op_status tat_js_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    JSValue object = JS_NewObject(context);
    if (JS_IsException(object)) {
        return tat_js_error(instance, params->fault, "ecmascript: could not make params");
    }
    for (uint32_t index = 0; index < params->count; index += 1) {
        JSValue value = tat_js_import(instance, &params->values[index].value);
        if (JS_IsException(value)) {
            JS_FreeValue(context, object);
            return tat_js_error(instance, params->fault, "ecmascript: parameter is not an ECMAScript value");
        }
        JSAtom atom = tat_js_atom(context, params->values[index].name);
        int defined = JS_DefinePropertyValue(context, object, atom, value, JS_PROP_C_W_E);
        JS_FreeAtom(context, atom);
        if (defined < 0) {
            JS_FreeValue(context, object);
            return tat_js_error(instance, params->fault, "ecmascript: could not make params");
        }
    }
    return tat_js_wrap(instance->state, object, false, params->out) == TA_OK ? TA_OP_OK
                                                                             : tat_js_nomem(params->fault);
}

/* The declared data as an ECMAScript object; the RPC codec encodes it. */
static ta_result tat_js_inspect(void *self, const ta_datamodel_inspect_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    JSValue root = JS_NewObject(context);
    if (JS_IsException(root)) {
        tat_js_clear_exception(context);
        return TA_ERR_NOMEM;
    }
    JSPropertyEnum *names = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &names, &count, instance->declared, JS_GPN_STRING_MASK) < 0) {
        tat_js_clear_exception(context);
        JS_FreeValue(context, root);
        return TA_ERR_NOMEM;
    }
    JSValue global = JS_GetGlobalObject(context);
    ta_result result = TA_OK;
    for (uint32_t index = 0; index < count && result == TA_OK; index += 1) {
        JSValue value = JS_GetProperty(context, global, names[index].atom);
        if (JS_IsException(value)) {
            tat_js_clear_exception(context);
            value = JS_UNDEFINED;
        }
        if (JS_DefinePropertyValue(context, root, names[index].atom, value, JS_PROP_C_W_E) < 0) {
            tat_js_clear_exception(context);
            result = TA_ERR_NOMEM;
        }
    }
    JS_FreeValue(context, global);
    JS_FreePropertyEnum(context, names, count);
    if (result != TA_OK) {
        JS_FreeValue(context, root);
        return result;
    }
    return tat_js_wrap(instance->state, root, false, params->out);
}

/* Strings read as themselves; other values as ToString(value). */
static ta_op_status tat_js_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    tat_js_value *wrapped = tat_js_unwrap(params->value);
    if (wrapped == NULL) {
        tat_js_fault(params->fault, TA_ERR_UNSUPPORTED, "ecmascript: value is not an ECMAScript value");
        return TA_OP_ERROR;
    }
    tat_js_state *state = wrapped->state;
    if (wrapped->text == NULL) {
        JSContext *utility = state->utility;
        tat_js_begin_work(state);
        JSValue value = JS_DupValue(utility, wrapped->value);
        if (wrapped->wire) {
            size_t length = 0;
            const char *json = JS_ToCStringLen(utility, &length, value);
            JS_FreeValue(utility, value);
            value = json == NULL ? JS_EXCEPTION : JS_ParseJSON(utility, json, length, "<wire>");
            if (json != NULL) {
                JS_FreeCString(utility, json);
            }
        }
        JSValue text = JS_IsException(value) ? JS_EXCEPTION : JS_ToString(utility, value);
        JS_FreeValue(utility, value);
        const char *chars = JS_IsException(text) ? NULL : JS_ToCStringLen(utility, &wrapped->text_len, text);
        JS_FreeValue(utility, text);
        if (chars == NULL) {
            tat_js_clear_exception(utility);
            tat_js_fault(params->fault, TA_ERR_INVALID, "ecmascript: value has no text");
            return TA_OP_ERROR;
        }
        wrapped->text = chars;
    }
    *params->out = (ta_str){.ptr = wrapped->text, .len = (uint32_t)wrapped->text_len};
    return TA_OP_OK;
}

static ta_op_status tat_js_text_value(void *self, const ta_datamodel_text_value_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSValue text = JS_NewStringLen(instance->context, params->text.len == 0 ? "" : params->text.ptr,
                                   params->text.len);
    if (JS_IsException(text)) {
        return tat_js_error(instance, params->fault, "ecmascript: could not make text");
    }
    return tat_js_wrap(instance->state, text, false, params->out) == TA_OK ? TA_OP_OK : tat_js_nomem(params->fault);
}

/* JSON when the content parses as JSON; otherwise a whitespace-normalized string (B.2.1). */
static ta_op_status tat_js_import_content(void *self, const ta_datamodel_import_content_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    size_t length = params->content.len;
    const char *bytes = length == 0 ? "" : (const char *)params->content.ptr;
    char *buffer = js_malloc(context, length + 1);
    if (buffer == NULL) {
        tat_js_clear_exception(context);
        return tat_js_nomem(params->fault);
    }
    memcpy(buffer, bytes, length);
    buffer[length] = '\0';
    JSValue value = JS_ParseJSON(context, buffer, length, "<content>");
    if (JS_IsException(value)) {
        tat_js_clear_exception(context);
        size_t used = 0;
        bool space = false;
        for (size_t index = 0; index < length; index += 1) {
            char c = bytes[index];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                space = used != 0;
                continue;
            }
            if (space) {
                buffer[used++] = ' ';
                space = false;
            }
            buffer[used++] = c;
        }
        value = JS_NewStringLen(context, buffer, used);
    }
    js_free(context, buffer);
    if (JS_IsException(value)) {
        return tat_js_error(instance, params->fault, "ecmascript: could not import content");
    }
    return tat_js_wrap(instance->state, value, false, params->out) == TA_OK ? TA_OP_OK
                                                                            : tat_js_nomem(params->fault);
}

/* Only arrays iterate; the snapshot is a shallow copy in this session's realm. */
static ta_op_status tat_js_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    JSValue array = tat_js_eval_expression(instance, tat_js_expression(instance, params->array));
    if (JS_IsException(array)) {
        return tat_js_error(instance, params->fault, "ecmascript: foreach array threw");
    }
    if (!JS_IsArray(array)) {
        JS_FreeValue(context, array);
        tat_js_fault(params->fault, TA_ERR_INVALID, "ecmascript: foreach array is not an array");
        return TA_OP_ERROR;
    }
    int64_t length = 0;
    if (JS_GetLength(context, array, &length) < 0 || length < 0 || length > (int64_t)UINT32_MAX) {
        JS_FreeValue(context, array);
        return tat_js_error(instance, params->fault, "ecmascript: foreach array has no usable length");
    }
    JSValue copy = JS_NewArray(context);
    bool ok = !JS_IsException(copy);
    for (int64_t index = 0; index < length && ok; index += 1) {
        JSValue item = JS_GetPropertyUint32(context, array, (uint32_t)index);
        ok = !JS_IsException(item) &&
             JS_DefinePropertyValueUint32(context, copy, (uint32_t)index, item, JS_PROP_C_W_E) >= 0;
    }
    JS_FreeValue(context, array);
    if (!ok) {
        JS_FreeValue(context, copy);
        return tat_js_error(instance, params->fault, "ecmascript: could not copy foreach array");
    }
    *params->count = (uint32_t)length;
    return tat_js_wrap(instance->state, copy, false, params->out) == TA_OK ? TA_OP_OK
                                                                           : tat_js_nomem(params->fault);
}

/* A legal variable name: identifier characters only, then QuickJS compiles `var <name>;`. */
static bool tat_js_variable_name(tat_js_instance *instance, ta_str name) {
    if (name.ptr == NULL || name.len == 0) {
        return false;
    }
    for (uint32_t index = 0; index < name.len; index += 1) {
        unsigned char c = (unsigned char)name.ptr[index];
        bool legal = c >= 0x80 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '_' || c == '$';
        if (!legal) {
            return false;
        }
    }
    JSValue compiled = tat_js_eval(instance, &(tat_js_eval_params){
                                                 .prefix = "var ",
                                                 .source = name,
                                                 .suffix = ";",
                                                 .flags = JS_EVAL_FLAG_COMPILE_ONLY,
                                             });
    if (JS_IsException(compiled)) {
        tat_js_clear_exception(instance->context);
        return false;
    }
    JS_FreeValue(instance->context, compiled);
    return true;
}

static ta_op_status tat_js_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    tat_js_instance *instance = params->instance;
    tat_js_begin(instance, params->host);
    JSContext *context = instance->context;
    bool has_index = params->index_name.ptr != NULL;
    if (!tat_js_variable_name(instance, params->item) ||
        (has_index && !tat_js_variable_name(instance, params->index_name))) {
        tat_js_fault(params->fault, TA_ERR_INVALID, "ecmascript: foreach item or index is not a legal variable");
        return TA_OP_ERROR;
    }
    JSValue snapshot = tat_js_import(instance, params->snapshot);
    if (JS_IsException(snapshot)) {
        return tat_js_error(instance, params->fault, "ecmascript: foreach snapshot is not an ECMAScript value");
    }
    JSValue item = JS_GetPropertyUint32(context, snapshot, params->index);
    JS_FreeValue(context, snapshot);
    if (JS_IsException(item)) {
        return tat_js_error(instance, params->fault, "ecmascript: could not read foreach item");
    }
    JSAtom atom = tat_js_atom(context, params->item);
    bool ok = tat_js_set_global(instance, atom, item) && tat_js_note_declared(instance, atom);
    JS_FreeAtom(context, atom);
    if (ok && has_index) {
        atom = tat_js_atom(context, params->index_name);
        ok = tat_js_set_global(instance, atom, JS_NewUint32(context, params->index)) &&
             tat_js_note_declared(instance, atom);
        JS_FreeAtom(context, atom);
    }
    return ok ? TA_OP_OK : tat_js_error(instance, params->fault, "ecmascript: could not bind foreach variables");
}

static const ta_datamodel_vtable TAT_JS_DATAMODEL_VTABLE = {
    .name = {.ptr = "ecmascript", .len = 10},
    .prepare_definition = tat_js_prepare,
    .destroy_definition = tat_js_destroy_definition,
    .instance_size = tat_js_instance_size,
    .instance_alignment = tat_js_instance_alignment,
    .init_instance = tat_js_init,
    .destroy_instance = tat_js_destroy_instance,
    .declare = tat_js_declare,
    .set_named = tat_js_set_named,
    .eval_condition = tat_js_eval_condition,
    .eval_value = tat_js_eval_value,
    .read_location = tat_js_read_location,
    .assign = tat_js_assign,
    .execute_script = tat_js_execute_script,
    .make_params = tat_js_make_params,
    .inspect = tat_js_inspect,
    .value_text = tat_js_value_text,
    .text_value = tat_js_text_value,
    .import_content = tat_js_import_content,
    .foreach_snapshot = tat_js_foreach_snapshot,
    .foreach_bind = tat_js_foreach_bind,
};

ta_datamodel tat_js_datamodel(tat_js_datamodel_context *context) {
    return (ta_datamodel){.self = context, .v = &TAT_JS_DATAMODEL_VTABLE};
}
