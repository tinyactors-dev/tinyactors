/* host.c — the Host class: one function per method, each a ta_host_* call
 * or a request (docs/api-map.md), and compile(), which makes the images a
 * host runs. What the loop does after an input is loop.c's. */
#include "addon.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

const napi_type_tag ta_napi_host_tag = {0x7461686f73744e41u, 0x5049686f73743031u};

/* ---- the host's allocator (AGENTS.md, "The host's allocator") ------------------ */

/* malloc, reported to the engine so its garbage collector sees the host's
 * memory. Core asks for at most 8 (allocator.h), which malloc gives. */
static void *ta_napi_host_alloc(void *context, size_t size, size_t align) {
    assert(align <= 8);
    (void)align;
    void *block = malloc(size);
    int64_t adjusted;
    if (block) napi_adjust_external_memory((node_api_basic_env)context, (int64_t)size, &adjusted);
    return block;
}

static void ta_napi_host_free_block(void *context, void *ptr, size_t size) {
    int64_t adjusted;
    if (!ptr) return;
    free(ptr);
    napi_adjust_external_memory((node_api_basic_env)context, -(int64_t)size, &adjusted);
}

/* ---- arguments --------------------------------------------------------------------- */

int ta_napi_call_read(napi_env env, napi_callback_info info, ta_napi_call *call) {
    napi_value undefined;
    napi_get_undefined(env, &undefined);
    for (size_t i = 0; i < sizeof call->argv / sizeof call->argv[0]; i++)
        call->argv[i] = undefined;
    call->argc = sizeof call->argv / sizeof call->argv[0];
    return napi_get_cb_info(env, info, &call->argc, call->argv, &call->self, NULL) == napi_ok;
}

/* The Host `this` is, open; NULL (and an exception thrown) otherwise. */
static ta_napi_host *ta_napi_host_this(napi_env env, napi_callback_info info, ta_napi_call *call) {
    if (!ta_napi_call_read(env, info, call)) return NULL;
    bool tagged = false;
    void *host = NULL;
    if (napi_check_object_type_tag(env, call->self, &ta_napi_host_tag, &tagged) != napi_ok || !tagged ||
        napi_unwrap(env, call->self, &host) != napi_ok) {
        ta_napi_throw_status(env, "Host", TA_ERROR_INVALID);
        return NULL;
    }
    if (((ta_napi_host *)host)->closed || ((ta_napi_host *)host)->closing) {
        ta_napi_throw_status(env, "Host", TA_ERROR_STATE);
        return NULL;
    }
    /* what a refused call before this one released (its payload) */
    if (!((ta_napi_host *)host)->running) ta_napi_datamodel_drain(((ta_napi_host *)host)->native);
    return (ta_napi_host *)host;
}

/* A member of an options object; undefined when `object` is not one or has
 * none. */
static napi_value ta_napi_member(napi_env env, napi_value object, const char *name) {
    napi_value value;
    napi_valuetype type = napi_undefined;
    napi_get_undefined(env, &value);
    if (napi_typeof(env, object, &type) == napi_ok && type == napi_object)
        napi_get_named_property(env, object, name, &value);
    return value;
}

static int ta_napi_is_undefined(napi_env env, napi_value value) {
    napi_valuetype type = napi_undefined;
    napi_typeof(env, value, &type);
    return type == napi_undefined;
}

/* An id the API takes: a BigInt within 64 bits, as the API gives them
 * (AGENTS.md: ids are always BigInt), or a number that is a safe integer,
 * as a request's reply gives a session by the value table
 * (docs/decisions.md, "The Host's API"). Never napi_get_value_int64, which
 * saturates silently. */
static int ta_napi_id(napi_env env, napi_value value, uint64_t *out) {
    bool lossless = false;
    double number = 0;
    if (napi_get_value_bigint_uint64(env, value, out, &lossless) == napi_ok) return lossless;
    if (napi_get_value_double(env, value, &number) != napi_ok || !(number >= 0 && number <= 9007199254740991.0) ||
        number != (double)(uint64_t)number)
        return 0;
    *out = (uint64_t)number;
    return 1;
}

static napi_value ta_napi_bigint(napi_env env, uint64_t id) {
    napi_value value = NULL;
    napi_create_bigint_uint64(env, id, &value);
    return value;
}

/* A refused call: the status thrown; NULL. */
static napi_value ta_napi_refused(napi_env env, const char *function, ta_status status) {
    ta_napi_throw_status(env, function, status);
    return NULL;
}

static napi_value ta_napi_undefined(napi_env env) {
    napi_value value = NULL;
    napi_get_undefined(env, &value);
    return value;
}

/* ---- new Host(options) -------------------------------------------------------- */

void ta_napi_host_release(ta_napi_host *host) {
    if (host->closed) return;
    host->closed = 1;
    if (host->clock) ta_napi_clock_forget(host->clock, host);
    host->clock = NULL;
    if (host->host) ta_host_free(host->host);
    host->host = NULL;
    if (host->quickjs) ta_datamodel_ecmascript_free(host->quickjs);
    host->quickjs = NULL;
    ta_napi_datamodel_free(host->native);
    host->native = NULL;
    for (uint32_t i = 0; i < host->image_count; i++) {
        free(host->images[i]->bytes);
        free(host->images[i]);
    }
    free(host->images);
    host->images = NULL;
    host->image_count = host->image_capacity = 0;
}

/* Deletes the references a host holds; a finalizer may (napi_delete_reference
 * takes a node_api_basic_env: Node.js 22's include/node/js_native_api.h:339). */
static void ta_napi_host_unreference(ta_napi_host *host) {
    napi_ref *references[] = {&host->handlers, &host->clock_object, &host->wake, &host->timer, &host->immediate,
                              &host->error};
    for (size_t i = 0; i < sizeof references / sizeof references[0]; i++) {
        if (*references[i]) napi_delete_reference(host->env, *references[i]);
        *references[i] = NULL;
    }
}

/* The garbage collector took a Host its application never closed: a
 * fallback, since handlers often close over the host (bindings.md,
 * section 6). */
static void ta_napi_host_finalize(node_api_basic_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    ta_napi_host *host = (ta_napi_host *)data;
    ta_napi_host_release(host);
    ta_napi_host_unreference(host);
    free(host);
}

/* The datamodel the options name: quickjs, configured by `quickjs`
 * (ta_datamodel_ecmascript_config), or the native one (ROADMAP.md, M1,
 * step 6). */
static ta_status ta_napi_host_datamodel(ta_napi_host *host, napi_value options) {
    napi_env env = host->env;
    napi_value name = ta_napi_member(env, options, "datamodel");
    ta_string_view text = {NULL, 0};
    ta_status status = ta_napi_is_undefined(env, name) ? TA_OK : ta_napi_values_text(env, name, &text);
    int quickjs = status == TA_OK && text.length == 7 && !memcmp(text.pointer, "quickjs", 7);
    int native = status == TA_OK && (!text.length || (text.length == 6 && !memcmp(text.pointer, "native", 6)));
    free((void *)text.pointer);
    if (status != TA_OK) return status;
    if (!quickjs) {
        if (!native) return TA_ERROR_INVALID;
        host->native = ta_napi_datamodel_new(host->env);
        if (!host->native) return TA_ERROR_NO_MEMORY;
        host->binding = ta_napi_datamodel_binding(host->native);
        return TA_OK;
    }
    ta_datamodel_ecmascript_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    napi_value settings = ta_napi_member(env, options, "quickjs");
    napi_value realm = ta_napi_member(env, settings, "realm");
    if (!ta_napi_is_undefined(env, realm)) {
        status = ta_napi_values_text(env, realm, &text);
        if (status == TA_OK && text.length == 8 && !memcmp(text.pointer, "isolated", 8))
            config.realm = TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED;
        else if (status == TA_OK && !(text.length == 4 && !memcmp(text.pointer, "auto", 4)))
            status = TA_ERROR_INVALID;
        free((void *)text.pointer);
        if (status != TA_OK) return status;
    }
    host->quickjs = ta_datamodel_ecmascript_new(&config);
    if (!host->quickjs) return TA_ERROR_NO_MEMORY;
    host->binding = ta_datamodel_ecmascript_binding(host->quickjs);
    return TA_OK;
}

/* The ticks per second the config names (the caller's own value): the
 * clock's unit. Core has no way to say its default yet (ROADMAP.md,
 * "Requests to core"). */
static void ta_napi_host_ticks(ta_napi_host *host, napi_value config) {
    napi_env env = host->env;
    napi_value ticks = ta_napi_member(env, ta_napi_member(env, config, "system"), "ticksPerSecond");
    double value = 0;
    if (napi_get_value_double(env, ticks, &value) == napi_ok && value >= 1 && value < 18446744073709551616.0)
        host->ticks_per_second = (uint64_t)value;
}

static napi_value ta_napi_host_new(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    if (!ta_napi_call_read(env, info, &call)) return NULL;
    napi_value options = call.argv[0];
    ta_napi_host *host = (ta_napi_host *)calloc(1, sizeof *host);
    if (!host) return ta_napi_refused(env, "Host", TA_ERROR_NO_MEMORY);
    host->env = env;
    if (napi_wrap(env, call.self, host, ta_napi_host_finalize, NULL, NULL) != napi_ok) {
        free(host);
        return NULL;
    }
    napi_type_tag_object(env, call.self, &ta_napi_host_tag);
    ta_status status = ta_napi_host_datamodel(host, options);
    napi_value config = ta_napi_member(env, options, "config");
    ta_buffer bytes = {NULL, 0, 0};
    if (status == TA_OK) status = ta_napi_values_encode(env, config, &bytes);
    if (status != TA_OK) {
        free(bytes.pointer);
        ta_napi_host_release(host);
        return ta_napi_refused(env, "ta_host_create", status);
    }
    ta_buffer refusal = {malloc(65536), 65536, 0};
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = 1;
    environment.datamodels = &host->binding;
    environment.allocator = (ta_allocator){(void *)env, ta_napi_host_alloc, ta_napi_host_free_block};
    environment.error = refusal.pointer ? &refusal : NULL;
    status = ta_host_create((ta_bytes){bytes.pointer, bytes.length}, &environment, &host->host);
    free(bytes.pointer);
    if (status != TA_OK) {
        host->host = NULL;
        ta_napi_host_release(host);
        if (status == TA_ERROR_INVALID && refusal.length)
            ta_napi_throw_config(env, (ta_bytes){refusal.pointer, refusal.length});
        else
            ta_napi_throw_status(env, "ta_host_create", status);
        free(refusal.pointer);
        return NULL;
    }
    free(refusal.pointer);
    ta_napi_host_ticks(host, config);
    napi_value handlers = ta_napi_member(env, options, "on");
    if (!ta_napi_is_undefined(env, handlers)) napi_create_reference(env, handlers, 1, &host->handlers);
    napi_value clock = ta_napi_member(env, options, "clock");
    if (!ta_napi_is_undefined(env, clock)) {
        bool tagged = false;
        void *unwrapped = NULL;
        if (napi_check_object_type_tag(env, clock, &ta_napi_clock_tag, &tagged) != napi_ok || !tagged ||
            napi_unwrap(env, clock, &unwrapped) != napi_ok) {
            ta_napi_host_release(host);
            return ta_napi_refused(env, "Host: clock", TA_ERROR_INVALID);
        }
        status = ta_napi_clock_add((ta_napi_clock *)unwrapped, host);
        if (status != TA_OK) {
            ta_napi_host_release(host);
            return ta_napi_refused(env, "Host: clock", status);
        }
        host->clock = (ta_napi_clock *)unwrapped;
        napi_create_reference(env, clock, 1, &host->clock_object);
    }
    return call.self;
}

/* ---- images --------------------------------------------------------------------- */

/* compile(charts, {resolve}): the image bytes of ta_compile_charts. */
typedef struct ta_napi_resolving {
    napi_env env;
    napi_value resolve;
    char *text[64]; /* what resolve gave, freed after the compile */
    uint32_t count;
} ta_napi_resolving;

/* A static src="": the application's resolve({base, source}) gives
 * {name, text}, or undefined for none. */
static ta_status ta_napi_resolve(void *context, const ta_resolve_request *request, ta_source *out) {
    ta_napi_resolving *resolving = (ta_napi_resolving *)context;
    napi_env env = resolving->env;
    if (ta_napi_is_undefined(env, resolving->resolve) || resolving->count + 2 > 64) return TA_ERROR_INVALID;
    napi_value argument, base, source, result, global;
    napi_create_object(env, &argument);
    napi_create_string_utf8(env, request->base.pointer ? request->base.pointer : "", request->base.length, &base);
    napi_create_string_utf8(env, request->source.pointer ? request->source.pointer : "", request->source.length,
                            &source);
    napi_set_named_property(env, argument, "base", base);
    napi_set_named_property(env, argument, "source", source);
    napi_get_global(env, &global);
    if (napi_call_function(env, global, resolving->resolve, 1, &argument, &result) != napi_ok) return TA_ERROR_INVALID;
    if (ta_napi_is_undefined(env, result)) return TA_ERROR_INVALID;
    ta_string_view name, text;
    if (ta_napi_values_text(env, ta_napi_member(env, result, "name"), &name) != TA_OK) return TA_ERROR_INVALID;
    if (ta_napi_values_text(env, ta_napi_member(env, result, "text"), &text) != TA_OK) {
        free((void *)name.pointer);
        return TA_ERROR_INVALID;
    }
    resolving->text[resolving->count++] = (char *)name.pointer;
    resolving->text[resolving->count++] = (char *)text.pointer;
    out->name = name;
    out->text = text;
    return TA_OK;
}

/* The diagnostics of a compile, as {severity, code, line, column, file,
 * message} records. */
static napi_value ta_napi_diagnostics(napi_env env, const ta_compile_result *result) {
    const ta_diagnostic *diagnostics = NULL;
    uint32_t count = result ? ta_compile_diagnostics(result, &diagnostics) : 0;
    napi_value list;
    napi_create_array_with_length(env, count, &list);
    for (uint32_t i = 0; i < count; i++) {
        const ta_diagnostic *d = &diagnostics[i];
        napi_value record, value;
        napi_create_object(env, &record);
        napi_create_uint32(env, d->severity, &value);
        napi_set_named_property(env, record, "severity", value);
        napi_create_uint32(env, d->code, &value);
        napi_set_named_property(env, record, "code", value);
        napi_create_uint32(env, d->line, &value);
        napi_set_named_property(env, record, "line", value);
        napi_create_uint32(env, d->column, &value);
        napi_set_named_property(env, record, "column", value);
        napi_create_string_utf8(env, d->file.pointer ? d->file.pointer : "", d->file.length, &value);
        napi_set_named_property(env, record, "file", value);
        napi_create_string_utf8(env, d->message.pointer ? d->message.pointer : "", d->message.length, &value);
        napi_set_named_property(env, record, "message", value);
        napi_set_element(env, list, i, record);
    }
    return list;
}

/* The charts of compile()'s first argument, as ta_source views over
 * malloc'd text (free each name and text). */
typedef struct ta_napi_sources {
    ta_source *items;
    uint32_t count;
} ta_napi_sources;

static void ta_napi_sources_free(ta_napi_sources *sources) {
    for (uint32_t i = 0; i < sources->count; i++) {
        free((void *)sources->items[i].name.pointer);
        free((void *)sources->items[i].text.pointer);
    }
    free(sources->items);
}

static ta_status ta_napi_sources_read(napi_env env, napi_value charts, ta_napi_sources *out) {
    bool array = false;
    uint32_t count = 0;
    out->items = NULL;
    out->count = 0;
    if (napi_is_array(env, charts, &array) != napi_ok || !array || napi_get_array_length(env, charts, &count) != napi_ok ||
        !count)
        return TA_ERROR_INVALID;
    out->items = (ta_source *)calloc(count, sizeof *out->items);
    if (!out->items) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        napi_value chart;
        if (napi_get_element(env, charts, i, &chart) != napi_ok) return TA_ERROR_INVALID;
        out->count = i + 1;
        if (ta_napi_values_text(env, ta_napi_member(env, chart, "name"), &out->items[i].name) != TA_OK ||
            ta_napi_values_text(env, ta_napi_member(env, chart, "text"), &out->items[i].text) != TA_OK)
            return TA_ERROR_INVALID;
    }
    return TA_OK;
}

napi_value ta_napi_compile(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    if (!ta_napi_call_read(env, info, &call)) return NULL;
    ta_napi_sources sources;
    ta_status status = ta_napi_sources_read(env, call.argv[0], &sources);
    if (status != TA_OK) {
        ta_napi_sources_free(&sources);
        return ta_napi_refused(env, "compile", status);
    }
    ta_napi_resolving resolving;
    memset(&resolving, 0, sizeof resolving);
    resolving.env = env;
    resolving.resolve = ta_napi_member(env, call.argv[1], "resolve");
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = ta_runtime_malloc();
    options.resolve = ta_napi_resolve;
    options.resolve_context = &resolving;
    ta_compile_result *result = NULL;
    status = ta_compile_charts(&options, (ta_source_list){sources.items, sources.count}, &result);
    napi_value image = NULL;
    if (status == TA_OK) {
        ta_string_view bytes = ta_compile_image(result);
        if (ta_napi_values_uint8array(env, (ta_bytes){bytes.pointer, bytes.length}, &image) != napi_ok) image = NULL;
    } else if (!ta_napi_exception_pending(env)) {
        ta_napi_throw_status(env, "ta_compile_charts", status);
        napi_value thrown;
        if (napi_get_and_clear_last_exception(env, &thrown) == napi_ok) {
            napi_set_named_property(env, thrown, "diagnostics", ta_napi_diagnostics(env, result));
            napi_throw(env, thrown);
        }
    }
    if (result) ta_compile_free(result);
    for (uint32_t i = 0; i < resolving.count; i++)
        free(resolving.text[i]);
    ta_napi_sources_free(&sources);
    return image;
}

/* addImage(image): a copy of the bytes, opened (ta_image_open) and added
 * (ta_host_add_image); the segment. */
static napi_value ta_napi_host_add_image(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_bytes bytes;
    if (!ta_napi_values_bytes(env, call.argv[0], &bytes)) return ta_napi_refused(env, "Host.addImage", TA_ERROR_INVALID);
    ta_napi_image *image = (ta_napi_image *)calloc(1, sizeof *image);
    if (host->image_count == host->image_capacity) {
        uint32_t capacity = host->image_capacity ? host->image_capacity * 2 : 4;
        ta_napi_image **images = (ta_napi_image **)realloc(host->images, capacity * sizeof *images);
        if (images) {
            host->images = images;
            host->image_capacity = capacity;
        }
    }
    if (!image || host->image_count == host->image_capacity) {
        free(image);
        free((void *)bytes.pointer);
        return ta_napi_refused(env, "Host.addImage", TA_ERROR_NO_MEMORY);
    }
    image->bytes = (void *)bytes.pointer;
    image->length = bytes.length;
    ta_status status = ta_image_open(bytes, &image->image);
    if (status == TA_OK) status = ta_host_add_image(host->host, &image->image, &image->segment);
    if (status != TA_OK) {
        free(image->bytes);
        free(image);
        return ta_napi_refused(env, "ta_host_add_image", status);
    }
    image->live = 1;
    host->images[host->image_count++] = image;
    return ta_napi_bigint(env, image->segment);
}

/* boot(segment): the actors the image declares (ta_host_boot). */
static napi_value ta_napi_host_boot(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    uint64_t segment = 0;
    if (!host) return NULL;
    if (!ta_napi_id(env, call.argv[0], &segment) || segment > UINT32_MAX)
        return ta_napi_refused(env, "Host.boot", TA_ERROR_INVALID);
    ta_status status = ta_host_boot(host->host, (uint32_t)segment);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_boot", status);
    ta_napi_loop_input(host);
    return ta_napi_undefined(env);
}

/* ---- actors ---------------------------------------------------------------------- */

/* spawn(chart, {name, params, traceparent}): ta_host_chart (the current
 * version of the chart named), ta_host_spawn; the session. */
static napi_value ta_napi_host_spawn(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_string_view chart = {NULL, 0}, name = {NULL, 0}, traceparent = {NULL, 0};
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    ta_host_chart_ref reference;
    napi_value options = call.argv[1];
    ta_status status = ta_napi_values_text(env, call.argv[0], &chart);
    napi_value value = ta_napi_member(env, options, "name");
    if (status == TA_OK && !ta_napi_is_undefined(env, value)) status = ta_napi_values_text(env, value, &name);
    value = ta_napi_member(env, options, "traceparent");
    if (status == TA_OK && !ta_napi_is_undefined(env, value)) status = ta_napi_values_text(env, value, &traceparent);
    if (status == TA_OK) status = ta_host_chart(host->host, chart, &reference);
    if (status == TA_OK) status = ta_napi_values_payload(host, ta_napi_member(env, options, "params"), &spawn.params);
    uint64_t session = 0;
    if (status == TA_OK) {
        spawn.segment = reference.segment;
        spawn.chart = reference.chart;
        spawn.name = name;
        spawn.traceparent = traceparent;
        ta_napi_loop_advance(host);
        status = ta_host_spawn(host->host, &spawn, &session);
        if (status != TA_OK && spawn.params) host->binding.vtable->release(host->binding.datamodel, spawn.params);
    }
    free((void *)chart.pointer);
    free((void *)name.pointer);
    free((void *)traceparent.pointer);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_spawn", status);
    ta_napi_loop_input(host);
    return ta_napi_bigint(env, session);
}

/* An event of send() or sendMany(): its strings are malloc'd. */
static void ta_napi_event_free(ta_event_in *event) {
    free((void *)event->name.pointer);
    event->name.pointer = NULL;
}

static ta_status ta_napi_event_read(ta_napi_host *host, const napi_value *argv, ta_event_in *event) {
    napi_env env = host->env;
    memset(event, 0, sizeof *event);
    event->size = sizeof *event;
    if (!ta_napi_id(env, argv[0], &event->session)) return TA_ERROR_INVALID;
    ta_status status = ta_napi_values_text(env, argv[1], &event->name);
    if (status == TA_OK) status = ta_napi_values_payload(host, argv[2], &event->data);
    return status;
}

/* send(session, event, data): ta_host_send. */
static napi_value ta_napi_host_send(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_event_in event;
    ta_status status = ta_napi_event_read(host, call.argv, &event);
    if (status == TA_OK) {
        status = ta_host_send(host->host, &event);
        if (status != TA_OK && event.data) host->binding.vtable->release(host->binding.datamodel, event.data);
    }
    ta_napi_event_free(&event);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_send", status);
    ta_napi_loop_input(host);
    return ta_napi_undefined(env);
}

/* sendMany([{session, event, data}]): ta_host_send_many; the length of the
 * accepted prefix. */
static napi_value ta_napi_host_send_many(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    bool array = false;
    uint32_t count = 0;
    if (napi_is_array(env, call.argv[0], &array) != napi_ok || !array ||
        napi_get_array_length(env, call.argv[0], &count) != napi_ok)
        return ta_napi_refused(env, "Host.sendMany", TA_ERROR_INVALID);
    ta_event_in *events = (ta_event_in *)calloc(count ? count : 1, sizeof *events);
    if (!events) return ta_napi_refused(env, "Host.sendMany", TA_ERROR_NO_MEMORY);
    ta_status status = TA_OK;
    uint32_t read = 0;
    for (; read < count && status == TA_OK; read++) {
        napi_value item, argv[3];
        if (napi_get_element(env, call.argv[0], read, &item) != napi_ok) {
            status = TA_ERROR_INVALID;
            break;
        }
        argv[0] = ta_napi_member(env, item, "session");
        argv[1] = ta_napi_member(env, item, "event");
        argv[2] = ta_napi_member(env, item, "data");
        status = ta_napi_event_read(host, argv, &events[read]);
        if (status != TA_OK) {
            ta_napi_event_free(&events[read]);
            break;
        }
    }
    uint32_t taken = status == TA_OK ? ta_host_send_many(host->host, events, count) : 0;
    for (uint32_t i = 0; i < read; i++) {
        if (i >= taken && events[i].data) host->binding.vtable->release(host->binding.datamodel, events[i].data);
        ta_napi_event_free(&events[i]);
    }
    free(events);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_send_many", status);
    if (taken) ta_napi_loop_input(host);
    napi_value result;
    napi_create_uint32(env, taken, &result);
    return result;
}

/* terminate(session): ta_host_terminate. */
static napi_value ta_napi_host_terminate(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    uint64_t session = 0;
    if (!host) return NULL;
    if (!ta_napi_id(env, call.argv[0], &session)) return ta_napi_refused(env, "Host.terminate", TA_ERROR_INVALID);
    ta_status status = ta_host_terminate(host->host, session);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_terminate", status);
    ta_napi_loop_input(host);
    return ta_napi_undefined(env);
}

/* ---- peers ------------------------------------------------------------------------ */

/* addPeer({id, trusted}): ta_host_peer_add; the peer. */
static napi_value ta_napi_host_add_peer(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_peer_config peer;
    memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    ta_status status = ta_napi_values_text(env, ta_napi_member(env, call.argv[0], "id"), &peer.id);
    bool trusted = false;
    napi_get_value_bool(env, ta_napi_member(env, call.argv[0], "trusted"), &trusted);
    peer.flags = trusted ? TA_PEER_TRUSTED : 0;
    uint32_t handle = 0;
    if (status == TA_OK) status = ta_host_peer_add(host->host, &peer, &handle);
    free((void *)peer.id.pointer);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_peer_add", status);
    ta_napi_loop_input(host);
    return ta_napi_bigint(env, handle);
}

/* deliver(peer, envelope): ta_host_envelope_read, then ta_host_deliver, also
 * when the read failed with the envelope's size set (its data is not one
 * item: core refuses it as malformed and answers the sender, host.h); the
 * TA_ROUTE_* outcome. */
static napi_value ta_napi_host_deliver(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    uint64_t peer = 0;
    ta_bytes bytes;
    if (!ta_napi_id(env, call.argv[0], &peer) || peer > UINT32_MAX) return ta_napi_refused(env, "Host.deliver", TA_ERROR_INVALID);
    if (!ta_napi_values_bytes(env, call.argv[1], &bytes)) return ta_napi_refused(env, "Host.deliver", TA_ERROR_INVALID);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    ta_napi_loop_advance(host);
    ta_status status = ta_host_envelope_read(host->host, bytes, &envelope);
    int32_t route = status == TA_OK || envelope.size ? ta_host_deliver(host->host, (uint32_t)peer, &envelope) : status;
    free((void *)bytes.pointer);
    if (route < 0) return ta_napi_refused(env, "ta_host_deliver", route);
    ta_napi_loop_input(host);
    napi_value result;
    napi_create_int32(env, route, &result);
    return result;
}

/* ---- close ------------------------------------------------------------------------- */

/* close(): ta_host_free, and every reference the host holds is dropped
 * (bindings.md, section 6). From a handler, after the loop returns
 * (section 5). */
static napi_value ta_napi_host_close(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    if (!ta_napi_call_read(env, info, &call)) return NULL;
    bool tagged = false;
    void *data = NULL;
    if (napi_check_object_type_tag(env, call.self, &ta_napi_host_tag, &tagged) != napi_ok || !tagged ||
        napi_unwrap(env, call.self, &data) != napi_ok)
        return ta_napi_refused(env, "Host.close", TA_ERROR_INVALID);
    ta_napi_host *host = (ta_napi_host *)data;
    if (host->running) {
        host->closing = 1;
    } else if (!host->closed) {
        ta_napi_loop_disarm(host);
        ta_napi_host_release(host);
        ta_napi_host_unreference(host);
    }
    return ta_napi_undefined(env);
}

napi_value ta_napi_host_class(napi_env env) {
    napi_property_descriptor methods[] = {
        {"addImage", NULL, ta_napi_host_add_image, NULL, NULL, NULL, napi_default_method, NULL},
        {"boot", NULL, ta_napi_host_boot, NULL, NULL, NULL, napi_default_method, NULL},
        {"spawn", NULL, ta_napi_host_spawn, NULL, NULL, NULL, napi_default_method, NULL},
        {"send", NULL, ta_napi_host_send, NULL, NULL, NULL, napi_default_method, NULL},
        {"sendMany", NULL, ta_napi_host_send_many, NULL, NULL, NULL, napi_default_method, NULL},
        {"terminate", NULL, ta_napi_host_terminate, NULL, NULL, NULL, napi_default_method, NULL},
        {"request", NULL, ta_napi_request, NULL, NULL, NULL, napi_default_method, NULL},
        {"addPeer", NULL, ta_napi_host_add_peer, NULL, NULL, NULL, napi_default_method, NULL},
        {"deliver", NULL, ta_napi_host_deliver, NULL, NULL, NULL, napi_default_method, NULL},
        {"close", NULL, ta_napi_host_close, NULL, NULL, NULL, napi_default_method, NULL},
    };
    napi_value class = NULL;
    napi_define_class(env, "Host", NAPI_AUTO_LENGTH, ta_napi_host_new, NULL, sizeof methods / sizeof methods[0],
                      methods, &class);
    return class;
}

/* The Host a request() runs on. */
ta_napi_host *ta_napi_host_of(napi_env env, napi_callback_info info, ta_napi_call *call) {
    return ta_napi_host_this(env, info, call);
}
