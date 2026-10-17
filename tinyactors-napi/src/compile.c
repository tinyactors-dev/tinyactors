/* The compiler (AGENTS.md, "Layers and the resolver"; docs/api-map.md;
 * translated from tinyactors-cpython/src/compile.c): compileCharts is
 * ta_compile_charts, compileSystem is ta_compile_system (a <system>
 * manifest), a compile result is the object both return, and a resolver,
 * any callable (base, source) => string, is ta_compile_options.resolve:
 * the only place a resolver is given, so it lives at compile time only.
 *
 * Who holds what:
 *   - a compile result's object wraps the ta_compile_result itself
 *     (napi_wrap, type-tagged): close() frees it (ta_compile_free) and
 *     removes the wrap, the finalizer frees it when close() was not called;
 *   - `image` is a Buffer copied from the result on its first read
 *     (napi_create_buffer_copy; decision d-image in docs/plan.xml: never a
 *     view of the result's memory), kept on the object under the
 *     instance's `image` symbol, the application's from then on;
 *   - the compiler allocates through ta_napi_allocator, as a host does, so
 *     the engine sees the result's memory; the call's sources are views in
 *     the module's arena (a mark per call), a resolver's texts and
 *     messages are copied into the compilation's scratch arena
 *     (request->scratch), which lives until the compile call returns. */
#include "addon.h"
#include "tinyactors_runtime.h"

#include <string.h>

/* napi_type_tag_object's tag of a compile result. */
static const napi_type_tag ta_napi_compiled_tag = {0x7461636f6d70696cULL, 0x6564417069303031ULL};

/* CompileError and ResolveError, defined by a script as TinyactorsError
 * is (module.c): subclasses of it, so `instanceof TinyactorsError` holds
 * and `status` is set. */
static const char ta_napi_compile_errors_source[] =
    "(function (TinyactorsError) {\n"
    "  class CompileError extends TinyactorsError {\n"
    "    constructor(message, options) {\n"
    "      const given = options ?? {};\n"
    "      super(message, { status: given.status ?? 'TA_ERROR_INVALID' });\n"
    "      Object.defineProperty(this, 'diagnostics', {\n"
    "        value: Object.freeze([...(given.diagnostics ?? [])]), enumerable: true });\n"
    "    }\n"
    "    get name() { return 'CompileError'; }\n"
    "  }\n"
    "  class ResolveError extends TinyactorsError {\n"
    "    constructor(message, options) {\n"
    "      super(message, { status: 'TA_ERROR_INVALID' });\n"
    "      const given = options ?? {};\n"
    "      Object.defineProperties(this, {\n"
    "        line: { value: given.line, enumerable: true },\n"
    "        column: { value: given.column, enumerable: true },\n"
    "      });\n"
    "    }\n"
    "    get name() { return 'ResolveError'; }\n"
    "  }\n"
    "  return [CompileError, ResolveError];\n"
    "})";

static bool ta_napi_pending(napi_env env) {
    bool pending = false;
    napi_is_exception_pending(env, &pending);
    return pending;
}

/* A Node-API call failed: its exception, or one saying so. */
static napi_value ta_napi_compile_failed(napi_env env) {
    if (!ta_napi_pending(env)) napi_throw_error(env, NULL, "a Node-API call failed while compiling");
    return NULL;
}

/* ---- diagnostics --------------------------------------------------------------- */

static const char *const ta_napi_severities[] = {"note", "warning", "error"};

/* One diagnostic as a frozen object of severity, code, file, line,
 * column and message. */
static napi_value ta_napi_diagnostic(napi_env env, const ta_diagnostic *diagnostic) {
    napi_value object, value;
    const char *severity = ta_napi_severities[diagnostic->severity < 3 ? diagnostic->severity : 2];
    if (napi_create_object(env, &object) != napi_ok ||
        napi_create_string_utf8(env, severity, NAPI_AUTO_LENGTH, &value) != napi_ok ||
        napi_set_named_property(env, object, "severity", value) != napi_ok ||
        napi_create_uint32(env, diagnostic->code, &value) != napi_ok ||
        napi_set_named_property(env, object, "code", value) != napi_ok ||
        napi_create_string_utf8(env, diagnostic->file.pointer ? diagnostic->file.pointer : "", diagnostic->file.length,
                                &value) != napi_ok ||
        napi_set_named_property(env, object, "file", value) != napi_ok ||
        napi_create_uint32(env, diagnostic->line, &value) != napi_ok ||
        napi_set_named_property(env, object, "line", value) != napi_ok ||
        napi_create_uint32(env, diagnostic->column, &value) != napi_ok ||
        napi_set_named_property(env, object, "column", value) != napi_ok ||
        napi_create_string_utf8(env, diagnostic->message.pointer ? diagnostic->message.pointer : "",
                                diagnostic->message.length, &value) != napi_ok ||
        napi_set_named_property(env, object, "message", value) != napi_ok || napi_object_freeze(env, object) != napi_ok)
        return NULL;
    return object;
}

/* "file:line:column: error: message" of an error, as a compiler prints it
 * (and as Python's CompileError reads), in `arena`; NULL when the arena
 * refused. */
static napi_value ta_napi_located(napi_env env, const ta_diagnostic *first, ta_growable_arena *arena) {
    static const char severity[] = " error: ";
    size_t length = first->file.length + first->message.length + 2 * 10 + 2 + sizeof severity;
    char *text = (char *)ta_growable_arena_alloc(arena, length, 1);
    if (text == NULL) return NULL;
    size_t at = 0;
    if (first->file.length) memcpy(text, first->file.pointer, first->file.length);
    at += first->file.length;
    uint32_t positions[2] = {first->line, first->column};
    for (int p = 0; p < 2; p++) {
        char digits[10];
        size_t count = 0;
        uint32_t n = positions[p];
        do digits[count++] = (char)('0' + n % 10); while ((n /= 10) != 0);
        text[at++] = ':';
        while (count > 0) text[at++] = digits[--count];
    }
    text[at++] = ':';
    memcpy(text + at, severity, sizeof severity - 1);
    at += sizeof severity - 1;
    if (first->message.length) memcpy(text + at, first->message.pointer, first->message.length);
    at += first->message.length;
    napi_value message;
    if (napi_create_string_utf8(env, text, at, &message) != napi_ok) return NULL;
    return message;
}

/* Every diagnostic of a result, converted once, as a frozen array, and
 * the first error as a compiler prints it, the message of a CompileError:
 * both made before the result is freed. */
typedef struct ta_napi_diagnostics {
    ta_growable_arena *arena; /* the message's scratch */
    napi_value list;          /* out */
    napi_value first;         /* out: the first error's message, or NULL */
} ta_napi_diagnostics;

static bool ta_napi_diagnostics_of(napi_env env, const ta_compile_result *result, ta_napi_diagnostics *out) {
    const ta_diagnostic *diagnostics = NULL;
    uint32_t count = result != NULL ? ta_compile_diagnostics(result, &diagnostics) : 0;
    out->first = NULL;
    if (napi_create_array_with_length(env, count, &out->list) != napi_ok) return false;
    for (uint32_t i = 0; i < count; i++) {
        napi_value item = ta_napi_diagnostic(env, &diagnostics[i]);
        if (item == NULL || napi_set_element(env, out->list, i, item) != napi_ok) return false;
        if (diagnostics[i].severity == TA_DIAGNOSTIC_ERROR && out->first == NULL) {
            out->first = ta_napi_located(env, &diagnostics[i], out->arena);
            if (out->first == NULL) return false;
        }
    }
    return napi_object_freeze(env, out->list) == napi_ok;
}

/* ---- the resolver -------------------------------------------------------------- */

/* One compile call's resolver: the function, ResolveError, and the first
 * exception that is not a refusal, thrown from the compile call once the
 * result is freed. */
typedef struct ta_napi_resolving {
    napi_env env;
    napi_value resolver;
    napi_value resolve_error;
    napi_ref raised;
} ta_napi_resolving;

/* A number option of a ResolveError (line, column): an integer from 1,
 * else 0 (unspecified). */
static uint32_t ta_napi_position(napi_env env, napi_value error, const char *name) {
    napi_value value;
    napi_valuetype type = napi_undefined;
    double number = 0;
    if (napi_get_named_property(env, error, name, &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_number || napi_get_value_double(env, value, &number) != napi_ok || !(number >= 1) ||
        number > 4294967295.0)
        return 0;
    return (uint32_t)number;
}

/* `value` as text in the compilation's scratch arena; an empty view when it
 * cannot be had (the compiler then gives its generic message). */
static ta_string_view ta_napi_scratch_text(napi_env env, napi_value value, const ta_resolve_request *request) {
    napi_value text;
    ta_napi_text out = {request->scratch, "a message", {"", 0}};
    if (value == NULL || napi_coerce_to_string(env, value, &text) != napi_ok || !ta_napi_text_of(env, text, &out)) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
        return (ta_string_view){"", 0};
    }
    return out.view;
}

/* The exception pending now as the compiler's refusal: a ResolveError is
 * a diagnostic at its line and column; anything else (any value: in a
 * one-element array) is kept, to be thrown from the compile call, and its
 * text is the diagnostic. */
static ta_status ta_napi_resolve_refused(ta_napi_resolving *resolving, const ta_resolve_request *request, ta_resolve_result *out) {
    napi_env env = resolving->env;
    napi_value error = NULL, message = NULL;
    bool refusal = false;
    if (napi_get_and_clear_last_exception(env, &error) != napi_ok) return TA_ERROR_INVALID;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, error, &type) == napi_ok && (type == napi_object || type == napi_function) &&
        napi_instanceof(env, error, resolving->resolve_error, &refusal) != napi_ok) {
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
        refusal = false;
    }
    if (refusal) {
        if (napi_get_named_property(env, error, "message", &message) != napi_ok) message = NULL;
        out->error.message = ta_napi_scratch_text(env, message, request);
        out->error.line = ta_napi_position(env, error, "line");
        out->error.column = ta_napi_position(env, error, "column");
    } else {
        out->error.message = ta_napi_scratch_text(env, error, request);
        napi_value box; /* any value may be thrown, and Node-API 8 references objects only */
        if (resolving->raised == NULL && napi_create_array_with_length(env, 1, &box) == napi_ok &&
            napi_set_element(env, box, 0, error) == napi_ok)
            napi_create_reference(env, box, 1, &resolving->raised);
    }
    return TA_ERROR_INVALID;
}

/* ta_compile_options.resolve: resolver(base, source), its string's UTF-8
 * copied into the scratch arena. A handle scope per call: a compilation
 * resolves many files in one native call. */
static ta_status ta_napi_resolve(void *context, const ta_resolve_request *request, ta_resolve_result *out) {
    ta_napi_resolving *resolving = (ta_napi_resolving *)context;
    napi_env env = resolving->env;
    napi_handle_scope scope;
    if (napi_open_handle_scope(env, &scope) != napi_ok) return TA_ERROR_FATAL;
    napi_value argv[2], undefined, text = NULL;
    ta_status status = TA_OK;
    bool called = napi_get_undefined(env, &undefined) == napi_ok &&
                  napi_create_string_utf8(env, request->base.pointer ? request->base.pointer : "", request->base.length,
                                          &argv[0]) == napi_ok &&
                  napi_create_string_utf8(env, request->source.pointer ? request->source.pointer : "",
                                          request->source.length, &argv[1]) == napi_ok &&
                  napi_call_function(env, undefined, resolving->resolver, 2, argv, &text) == napi_ok;
    ta_napi_text copy = {request->scratch, "a resolver (base, source) => string returns a string", {NULL, 0}};
    if (!called || !ta_napi_text_of(env, text, &copy)) {
        if (!ta_napi_pending(env)) napi_throw_error(env, NULL, "the resolver could not be called");
        status = ta_napi_resolve_refused(resolving, request, out);
    } else {
        out->source.name = request->source;
        out->source.text = copy.view;
    }
    napi_close_handle_scope(env, scope);
    return status;
}

/* ---- compileCharts, compileSystem ------------------------------------------------- */

/* What a compile call is: the sources an array of {name, text}, or the
 * manifest one {name, text}. */
typedef struct ta_napi_compile_call {
    bool manifest;
    const char *usage;
} ta_napi_compile_call;

/* A source, {name, text} of strings, into `source` (views in `arena`). */
typedef struct ta_napi_source_of {
    ta_growable_arena *arena;
    const char *usage;
    ta_source *source; /* out */
} ta_napi_source_of;

static bool ta_napi_source(napi_env env, napi_value value, const ta_napi_source_of *of) {
    napi_valuetype type = napi_undefined;
    napi_value name, text;
    if (napi_typeof(env, value, &type) != napi_ok) return false;
    if (type != napi_object) {
        napi_throw_type_error(env, NULL, of->usage);
        return false;
    }
    ta_napi_text out = {of->arena, of->usage, {NULL, 0}};
    if (napi_get_named_property(env, value, "name", &name) != napi_ok || !ta_napi_text_of(env, name, &out)) return false;
    of->source->name = out.view;
    if (napi_get_named_property(env, value, "text", &text) != napi_ok || !ta_napi_text_of(env, text, &out)) return false;
    of->source->text = out.view;
    return true;
}

/* The sources of the call into an array in the module's arena; their
 * count, or -1 with an exception pending. */
typedef struct ta_napi_sources {
    ta_growable_arena *arena;
    const ta_napi_compile_call *how;
    ta_source *list; /* out */
} ta_napi_sources;

static int64_t ta_napi_sources_of(napi_env env, napi_value given, ta_napi_sources *out) {
    const ta_napi_compile_call *how = out->how;
    bool array = false;
    uint32_t count = 1;
    if (!how->manifest) {
        if (napi_is_array(env, given, &array) != napi_ok) return -1;
        if (!array) {
            napi_throw_type_error(env, NULL, how->usage);
            return -1;
        }
        if (napi_get_array_length(env, given, &count) != napi_ok) return -1;
    }
    out->list = (ta_source *)ta_growable_arena_alloc(out->arena, sizeof(ta_source) * (count ? count : 1), 8);
    if (out->list == NULL) {
        ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "no memory for the sources"});
        return -1;
    }
    for (uint32_t i = 0; i < count; i++) {
        napi_value item = given;
        if (!how->manifest && napi_get_element(env, given, i, &item) != napi_ok) return -1;
        if (!ta_napi_source(env, item, &(ta_napi_source_of){out->arena, how->usage, &out->list[i]})) return -1;
    }
    return count;
}

/* Throws what a failed compilation gives: TA_ERROR_NO_MEMORY, or a
 * CompileError with every diagnostic, its message the first error's. */
typedef struct ta_napi_failure {
    ta_status status;
    ta_napi_diagnostics diagnostics;
} ta_napi_failure;

static napi_value ta_napi_compile_throw(napi_env env, ta_napi_instance *instance, const ta_napi_failure *failure) {
    if (failure->status == TA_ERROR_NO_MEMORY)
        return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_NO_MEMORY, "the compiler's allocator refused memory"});
    napi_value class, options, value, argv[2] = {NULL, NULL}, error;
    const char *status = ta_status_name(failure->status == TA_OK ? TA_ERROR_INVALID : failure->status);
    if (napi_get_reference_value(env, instance->compile_error, &class) != napi_ok || napi_create_object(env, &options) != napi_ok ||
        napi_set_named_property(env, options, "diagnostics", failure->diagnostics.list) != napi_ok ||
        napi_create_string_utf8(env, status, NAPI_AUTO_LENGTH, &value) != napi_ok ||
        napi_set_named_property(env, options, "status", value) != napi_ok)
        return ta_napi_compile_failed(env);
    argv[0] = failure->diagnostics.first;
    if (argv[0] == NULL && napi_create_string_utf8(env, status, NAPI_AUTO_LENGTH, &argv[0]) != napi_ok)
        return ta_napi_compile_failed(env);
    argv[1] = options;
    if (napi_new_instance(env, class, 2, argv, &error) != napi_ok) return ta_napi_compile_failed(env);
    napi_throw(env, error);
    return NULL;
}

static napi_value ta_napi_compile_with(napi_env env, napi_callback_info info, const ta_napi_compile_call *how) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return NULL;
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL}, result = NULL, class, resolve_error;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    if (argc < 1 && napi_get_undefined(env, &argv[0]) != napi_ok) return NULL;
    if (argc < 2 && napi_get_undefined(env, &argv[1]) != napi_ok) return NULL;
    napi_valuetype resolver = napi_undefined;
    if (napi_typeof(env, argv[1], &resolver) != napi_ok) return NULL;
    if (resolver != napi_undefined && resolver != napi_null && resolver != napi_function) {
        napi_throw_type_error(env, NULL, "a resolver is a function (base, source) => string");
        return NULL;
    }
    if (napi_get_reference_value(env, instance->compiled, &class) != napi_ok ||
        napi_get_reference_value(env, instance->resolve_error, &resolve_error) != napi_ok)
        return NULL;

    ta_growable_arena_mark mark = ta_growable_arena_position(&instance->arena);
    ta_napi_sources sources = {&instance->arena, how, NULL};
    int64_t count = ta_napi_sources_of(env, argv[0], &sources);
    if (count >= 0) {
        ta_napi_resolving resolving = {env, argv[1], resolve_error, NULL};
        ta_compile_options options;
        memset(&options, 0, sizeof options);
        options.size = sizeof options;
        options.alloc = ta_napi_allocator(env);
        if (resolver == napi_function) {
            options.resolve = ta_napi_resolve;
            options.resolve_context = &resolving;
        }
        ta_compile_result *compiled = NULL;
        ta_status status = how->manifest ? ta_compile_system(&options, &sources.list[0], &compiled)
                                         : ta_compile_charts(&options, (ta_source_list){sources.list, (uint32_t)count},
                                                             &compiled);
        ta_napi_failure failure = {status, {&instance->arena, NULL, NULL}};
        bool converted = ta_napi_diagnostics_of(env, compiled, &failure.diagnostics);
        napi_value raised = NULL, box = NULL;
        if (resolving.raised != NULL) {
            if (napi_get_reference_value(env, resolving.raised, &box) != napi_ok || box == NULL ||
                napi_get_element(env, box, 0, &raised) != napi_ok)
                raised = NULL;
            napi_delete_reference(env, resolving.raised);
        }
        if (!converted || raised != NULL || status != TA_OK || failure.diagnostics.first != NULL) {
            if (compiled != NULL) ta_compile_free(compiled); /* freed before it throws */
            if (!converted) ta_napi_compile_failed(env);
            else if (raised != NULL) napi_throw(env, raised);
            else ta_napi_compile_throw(env, instance, &failure);
        } else {
            napi_value args[2] = {NULL, failure.diagnostics.list};
            if (napi_create_external(env, compiled, NULL, NULL, &args[0]) != napi_ok ||
                napi_new_instance(env, class, 2, args, &result) != napi_ok) {
                ta_compile_free(compiled); /* the constructor wraps it last: not wrapped */
                result = NULL;
                ta_napi_compile_failed(env);
            }
        }
    }
    ta_napi_arena_release(&instance->arena, mark, TA_NAPI_INSTANCE_ARENA_CAP);
    return result;
}

static napi_value ta_napi_compile_charts(napi_env env, napi_callback_info info) {
    static const ta_napi_compile_call how = {false, "compileCharts(sources, resolver?): sources an array of {name, text} of strings"};
    return ta_napi_compile_with(env, info, &how);
}

static napi_value ta_napi_compile_system(napi_env env, napi_callback_info info) {
    static const ta_napi_compile_call how = {true, "compileSystem(manifest, resolver?): the manifest a {name, text} of strings"};
    return ta_napi_compile_with(env, info, &how);
}

/* ---- the compile result ------------------------------------------------------------ */

static void ta_napi_compiled_finalize(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    ta_compile_free((ta_compile_result *)data); /* the collector's fallback: basic-env calls only */
}

/* The constructor, called by the compile calls only, with the result as an
 * external and the diagnostics: type-tagged, the diagnostics a read-only
 * property, and the result wrapped last (from there the finalizer frees
 * it; a constructor that fails has not wrapped it). */
static napi_value ta_napi_compiled_new(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2] = {NULL, NULL}, self;
    napi_valuetype type = napi_undefined;
    void *result = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, &self, NULL) != napi_ok) return NULL;
    if (argc < 2 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_external ||
        napi_get_value_external(env, argv[0], &result) != napi_ok || result == NULL) {
        napi_throw_type_error(env, NULL, "a compile result is made by compileCharts and compileSystem");
        return NULL;
    }
    napi_property_descriptor property = {"diagnostics", NULL, NULL, NULL, NULL, argv[1], napi_enumerable, NULL};
    if (napi_type_tag_object(env, self, &ta_napi_compiled_tag) != napi_ok ||
        napi_define_properties(env, self, 1, &property) != napi_ok ||
        napi_wrap(env, self, result, ta_napi_compiled_finalize, NULL, NULL) != napi_ok) /* last: from here the finalizer frees it */
        return NULL;
    return self;
}

/* `self` checked as a compile result: its result, or NULL once closed
 * (`*open` false); false with a TypeError for another object. */
typedef struct ta_napi_compiled_check {
    napi_value self;
    ta_compile_result *result; /* out: NULL once closed */
} ta_napi_compiled_check;

static bool ta_napi_compiled_of(napi_env env, napi_callback_info info, ta_napi_compiled_check *check) {
    size_t argc = 0;
    bool tagged = false;
    napi_valuetype type = napi_undefined;
    check->result = NULL;
    if (napi_get_cb_info(env, info, &argc, NULL, &check->self, NULL) != napi_ok) return false;
    if (napi_typeof(env, check->self, &type) != napi_ok || type != napi_object ||
        napi_check_object_type_tag(env, check->self, &ta_napi_compiled_tag, &tagged) != napi_ok || !tagged) {
        napi_throw_type_error(env, NULL, "not a compile result");
        return false;
    }
    void *data = NULL;
    if (napi_unwrap(env, check->self, &data) != napi_ok) { /* closed: the wrap was removed */
        napi_value ignored;
        napi_get_and_clear_last_exception(env, &ignored);
        data = NULL;
    }
    check->result = (ta_compile_result *)data;
    return true;
}

/* The instance's `image` symbol. */
static napi_value ta_napi_image_key(napi_env env) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    napi_value keys, key;
    if (instance == NULL || napi_get_reference_value(env, instance->keys, &keys) != napi_ok ||
        napi_get_named_property(env, keys, "image", &key) != napi_ok)
        return NULL;
    return key;
}

/* compiled.image: the Buffer copied on the first read, the same one after;
 * a first read after close() throws TA_ERROR_STALE. */
static napi_value ta_napi_compiled_image(napi_env env, napi_callback_info info) {
    ta_napi_compiled_check check;
    if (!ta_napi_compiled_of(env, info, &check)) return NULL;
    napi_value key = ta_napi_image_key(env), image;
    napi_valuetype type = napi_undefined;
    if (key == NULL || napi_get_property(env, check.self, key, &image) != napi_ok || napi_typeof(env, image, &type) != napi_ok)
        return NULL;
    if (type != napi_undefined) return image;
    if (check.result == NULL)
        return ta_napi_throw(env, &(ta_napi_refusal){TA_ERROR_STALE, "the compile result was closed before its image was read"});
    ta_string_view bytes = ta_compile_image(check.result);
    void *data = NULL;
    if (napi_create_buffer_copy(env, bytes.length, bytes.pointer, &data, &image) != napi_ok) return NULL;
    napi_property_descriptor property = {NULL, key, NULL, NULL, NULL, image, napi_default, NULL};
    if (napi_define_properties(env, check.self, 1, &property) != napi_ok) return NULL;
    return image;
}

/* close(): the result freed (ta_compile_free) and the wrap removed, so the
 * finalizer frees nothing; again, nothing. */
static napi_value ta_napi_compiled_close(napi_env env, napi_callback_info info) {
    ta_napi_compiled_check check;
    napi_value undefined;
    if (!ta_napi_compiled_of(env, info, &check) || napi_get_undefined(env, &undefined) != napi_ok) return NULL;
    if (check.result != NULL) {
        void *data = NULL;
        if (napi_remove_wrap(env, check.self, &data) != napi_ok) return NULL;
        ta_compile_free((ta_compile_result *)data);
    }
    return undefined;
}

/* Symbol.<name> of this realm, or NULL when it has none. */
static napi_value ta_napi_symbol(napi_env env, const char *name) {
    napi_value global, symbol, value;
    napi_valuetype type = napi_undefined;
    if (napi_get_global(env, &global) != napi_ok || napi_get_named_property(env, global, "Symbol", &symbol) != napi_ok ||
        napi_get_named_property(env, symbol, name, &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_symbol)
        return NULL;
    return value;
}

napi_status ta_napi_compile_define(napi_env env, napi_value exports) {
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance == NULL) return napi_pending_exception;
    napi_value source, factory, base, classes, compile_error = NULL, resolve_error = NULL, keys, description, symbol, class,
        undefined;
    napi_status status = napi_create_string_utf8(env, ta_napi_compile_errors_source, sizeof ta_napi_compile_errors_source - 1, &source);
    if (status == napi_ok) status = napi_run_script(env, source, &factory);
    if (status == napi_ok) status = napi_get_reference_value(env, instance->error, &base);
    if (status == napi_ok) status = napi_get_undefined(env, &undefined);
    if (status == napi_ok) status = napi_call_function(env, undefined, factory, 1, &base, &classes);
    if (status == napi_ok) status = napi_get_element(env, classes, 0, &compile_error);
    if (status == napi_ok) status = napi_get_element(env, classes, 1, &resolve_error);
    if (status == napi_ok) status = napi_create_reference(env, compile_error, 1, &instance->compile_error);
    if (status == napi_ok) status = napi_create_reference(env, resolve_error, 1, &instance->resolve_error);
    if (status == napi_ok) status = napi_get_reference_value(env, instance->keys, &keys);
    if (status == napi_ok) status = napi_create_string_utf8(env, "tinyactors.image", NAPI_AUTO_LENGTH, &description);
    if (status == napi_ok) status = napi_create_symbol(env, description, &symbol);
    if (status == napi_ok) status = napi_set_named_property(env, keys, "image", symbol);
    napi_value dispose = ta_napi_symbol(env, "dispose");
    napi_property_descriptor properties[] = {
        {"image", NULL, NULL, ta_napi_compiled_image, NULL, NULL, napi_enumerable, NULL},
        {"close", NULL, ta_napi_compiled_close, NULL, NULL, NULL, napi_default_method, NULL},
        {NULL, dispose, ta_napi_compiled_close, NULL, NULL, NULL, napi_default_method, NULL},
    };
    size_t count = sizeof properties / sizeof properties[0] - (dispose == NULL ? 1 : 0);
    if (status == napi_ok)
        status = napi_define_class(env, "Compiled", NAPI_AUTO_LENGTH, ta_napi_compiled_new, NULL, count, properties, &class);
    if (status == napi_ok) status = napi_create_reference(env, class, 1, &instance->compiled);
    napi_property_descriptor exported[] = {
        {"compileCharts", NULL, ta_napi_compile_charts, NULL, NULL, NULL, napi_enumerable, NULL},
        {"compileSystem", NULL, ta_napi_compile_system, NULL, NULL, NULL, napi_enumerable, NULL},
        {"CompileError", NULL, NULL, NULL, NULL, compile_error, napi_enumerable, NULL},
        {"ResolveError", NULL, NULL, NULL, NULL, resolve_error, napi_enumerable, NULL},
    };
    if (status == napi_ok) status = napi_define_properties(env, exports, sizeof exported / sizeof exported[0], exported);
    return status;
}
