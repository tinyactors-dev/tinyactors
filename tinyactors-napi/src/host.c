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

ta_allocator ta_napi_allocator(napi_env env) {
    ta_allocator allocator = {(void *)env, ta_napi_host_alloc, ta_napi_host_free_block};
    return allocator;
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
    if (!((ta_napi_host *)host)->running) ta_napi_datamodel_drain(&((ta_napi_host *)host)->native);
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

/* The images' references: dropped when their segment is freed, or here. */
static void ta_napi_images_free(ta_napi_host *host) {
    for (uint32_t i = 0; i < host->images.count; i++) {
        ta_napi_image *image = (ta_napi_image *)ta_napi_table_row(&host->images, i);
        if (!image) continue;
        napi_delete_reference(host->env, image->bytes);
        napi_delete_reference(host->env, image->view);
    }
    ta_napi_table_free(&host->images);
}

void ta_napi_image_free(ta_napi_host *host, uint32_t segment) {
    for (uint32_t i = 0; i < host->images.count; i++) {
        ta_napi_image *image = (ta_napi_image *)ta_napi_table_row(&host->images, i);
        if (!image || image->segment != segment) continue;
        napi_delete_reference(host->env, image->bytes);
        napi_delete_reference(host->env, image->view);
        ta_napi_table_remove(&host->images, ta_napi_table_handle(&host->images, i));
        return;
    }
}

/* The host's free path: core, its datamodel, the images, once. */
void ta_napi_host_release(ta_napi_host *host) {
    if (host->closed) return;
    host->closed = 1;
    if (host->clock) ta_napi_clock_forget(host->clock, host);
    host->clock = NULL;
    if (host->host) ta_host_free(host->host);
    host->host = NULL;
    if (host->quickjs) ta_datamodel_ecmascript_free(host->quickjs);
    host->quickjs = NULL;
    ta_napi_datamodel_release(&host->native);
    ta_napi_images_free(host);
}

/* Deletes the references a host holds; a finalizer may (napi_delete_reference
 * takes a node_api_basic_env: Node.js 22's include/node/js_native_api.h:339). */
static void ta_napi_host_unreference(ta_napi_host *host) {
    napi_ref *references[] = {&host->handlers, &host->clock_object, &host->wake, &host->timer, &host->immediate,
                              &host->error, &host->self};
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
    napi_value settings = ta_napi_member(env, options, "quickjs");
    napi_value names[2] = {ta_napi_member(env, options, "datamodel"), ta_napi_member(env, settings, "realm")};
    ta_string_view texts[2];
    ta_napi_texts reading = {names, 2, texts};
    char stack[64];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    ta_status status = ta_napi_values_texts(&room, &reading);
    ta_string_view name = texts[0], realm = texts[1];
    int quickjs = status == TA_OK && name.length == 7 && !memcmp(name.pointer, "quickjs", 7);
    int native = status == TA_OK && (!name.length || (name.length == 6 && !memcmp(name.pointer, "native", 6)));
    int isolated = realm.length == 8 && !memcmp(realm.pointer, "isolated", 8);
    int realm_known = !realm.length || isolated || (realm.length == 4 && !memcmp(realm.pointer, "auto", 4));
    ta_napi_room_free(&room);
    if (status != TA_OK) return status;
    if (!quickjs) {
        if (!native) return TA_ERROR_INVALID;
        if (!ta_napi_datamodel_init(&host->native, env)) return TA_ERROR_NO_MEMORY;
        host->binding = ta_napi_datamodel_binding(&host->native);
        return TA_OK;
    }
    ta_datamodel_ecmascript_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    if (!realm_known) return TA_ERROR_INVALID;
    if (isolated) config.realm = TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED;
    host->quickjs = ta_datamodel_ecmascript_new(&config);
    if (!host->quickjs) return TA_ERROR_NO_MEMORY;
    host->binding = ta_datamodel_ecmascript_binding(host->quickjs);
    return TA_OK;
}

static napi_value ta_napi_host_new(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    if (!ta_napi_call_read(env, info, &call)) return NULL;
    napi_value options = call.argv[0];
    ta_napi_host *host = (ta_napi_host *)calloc(1, sizeof *host);
    if (!host) return ta_napi_refused(env, "Host", TA_ERROR_NO_MEMORY);
    host->env = env;
    host->allocator = ta_napi_allocator(env);
    host->images.row_size = sizeof(ta_napi_image);
    if (napi_wrap(env, call.self, host, ta_napi_host_finalize, NULL, NULL) != napi_ok) {
        free(host);
        return NULL;
    }
    /* a reference of our own, weak: napi_wrap's would be Node's to keep
     * (deleting it while the object is wrapped breaks napi_unwrap) */
    napi_create_reference(env, call.self, 0, &host->self);
    napi_type_tag_object(env, call.self, &ta_napi_host_tag);
    ta_status status = ta_napi_host_datamodel(host, options);
    napi_value config = ta_napi_member(env, options, "config");
    ta_bytes bytes = {NULL, 0};
    ta_napi_encoder encoder;
    ta_napi_encoder_init(&encoder, env, NULL);
    if (status == TA_OK) {
        ta_napi_encoder_value(&encoder, config);
        status = ta_napi_encoder_bytes(&encoder, &bytes);
    }
    if (status != TA_OK) {
        ta_napi_encoder_free(&encoder);
        ta_napi_host_release(host);
        return ta_napi_refused(env, "ta_host_create", status);
    }
    /* a refused config's error: environment->error stays a ta_buffer (ADR
     * 0010, decision 2: no host exists to own it), as the Python binding's */
    char error[8192];
    ta_buffer refusal = {error, sizeof error, 0};
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = 1;
    environment.datamodels = &host->binding;
    environment.allocator = host->allocator;
    environment.error = &refusal;
    status = ta_host_create(bytes, &environment, &host->host);
    ta_napi_encoder_free(&encoder);
    if (status != TA_OK) {
        host->host = NULL;
        if (status == TA_ERROR_INVALID && refusal.length && refusal.length <= refusal.capacity)
            ta_napi_throw_config(env, (ta_bytes){refusal.pointer, refusal.length});
        else
            ta_napi_throw_status(env, "ta_host_create", status);
        ta_napi_host_release(host);
        return NULL;
    }
    status = ta_napi_host_read_ticks(host);
    if (status != TA_OK) {
        ta_napi_host_release(host);
        return ta_napi_refused(env, "host.config", status);
    }
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

/* compile(charts, {resolve}): the image bytes of ta_compile_charts. The
 * texts the compiler reads (the charts', and what resolve gives during the
 * compile) and the list of sources are held in ArrayBuffers of the call's
 * handle scope, so a resolve that runs JavaScript (another compile among
 * it) cannot move them. */
typedef struct ta_napi_resolving {
    napi_env env;
    napi_value resolve;
} ta_napi_resolving;

/* A static src="": the application's resolve({base, source}) gives
 * {name, text}, or undefined for none. */
static ta_status ta_napi_resolve(void *context, const ta_resolve_request *request, ta_source *out) {
    ta_napi_resolving *resolving = (ta_napi_resolving *)context;
    napi_env env = resolving->env;
    if (ta_napi_is_undefined(env, resolving->resolve)) return TA_ERROR_INVALID;
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
    ta_napi_held_text name, text;
    if (ta_napi_values_hold_text(env, ta_napi_member(env, result, "name"), &name) != TA_OK ||
        ta_napi_values_hold_text(env, ta_napi_member(env, result, "text"), &text) != TA_OK)
        return TA_ERROR_INVALID;
    out->name = name.text;
    out->text = text.text;
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

/* The charts of compile()'s first argument, as ta_source views in an
 * ArrayBuffer over texts held in ArrayBuffers. */
static ta_status ta_napi_sources_read(napi_env env, napi_value charts, ta_source_list *out) {
    bool array = false;
    uint32_t count = 0;
    void *data = NULL;
    napi_value list;
    out->pointer = NULL;
    out->count = 0;
    if (napi_is_array(env, charts, &array) != napi_ok || !array || napi_get_array_length(env, charts, &count) != napi_ok ||
        !count)
        return TA_ERROR_INVALID;
    if (napi_create_arraybuffer(env, (size_t)count * sizeof(ta_source), &data, &list) != napi_ok)
        return TA_ERROR_NO_MEMORY;
    ta_source *sources = (ta_source *)data;
    for (uint32_t i = 0; i < count; i++) {
        napi_value chart;
        ta_napi_held_text name, text;
        if (napi_get_element(env, charts, i, &chart) != napi_ok ||
            ta_napi_values_hold_text(env, ta_napi_member(env, chart, "name"), &name) != TA_OK ||
            ta_napi_values_hold_text(env, ta_napi_member(env, chart, "text"), &text) != TA_OK)
            return TA_ERROR_INVALID;
        sources[i].name = name.text;
        sources[i].text = text.text;
    }
    out->pointer = sources;
    out->count = count;
    return TA_OK;
}

napi_value ta_napi_compile(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    if (!ta_napi_call_read(env, info, &call)) return NULL;
    ta_source_list sources;
    ta_status status = ta_napi_sources_read(env, call.argv[0], &sources);
    if (status != TA_OK) return ta_napi_refused(env, "compile", status);
    ta_napi_resolving resolving = {env, ta_napi_member(env, call.argv[1], "resolve")};
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = ta_runtime_malloc();
    options.resolve = ta_napi_resolve;
    options.resolve_context = &resolving;
    ta_compile_result *result = NULL;
    status = ta_compile_charts(&options, sources, &result);
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
    return image;
}

/* addImage(image): the bytes, held (a reference to the caller's
 * Uint8Array or ArrayBuffer, which must not change while the segment
 * runs), opened (ta_image_open) into an ArrayBuffer the host also holds,
 * and added (ta_host_add_image), until TA_HOST_EFFECT_SEGMENT_FREED; the
 * segment. */
static napi_value ta_napi_host_add_image(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_bytes bytes;
    void *data = NULL;
    napi_value view;
    if (!ta_napi_values_bytes(env, call.argv[0], &bytes)) return ta_napi_refused(env, "Host.addImage", TA_ERROR_INVALID);
    if (napi_create_arraybuffer(env, sizeof(ta_image), &data, &view) != napi_ok)
        return ta_napi_refused(env, "Host.addImage", TA_ERROR_NO_MEMORY);
    ta_status status = ta_image_open(bytes, (ta_image *)data);
    uint32_t segment = 0;
    if (status == TA_OK) status = ta_host_add_image(host->host, (const ta_image *)data, &segment);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_add_image", status);
    uint64_t handle = ta_napi_table_add(&host->images);
    ta_napi_image *image = (ta_napi_image *)ta_napi_table_get(&host->images, handle);
    if (!image || napi_create_reference(env, call.argv[0], 1, &image->bytes) != napi_ok ||
        napi_create_reference(env, view, 1, &image->view) != napi_ok)
        return ta_napi_refused(env, "Host.addImage", TA_ERROR_NO_MEMORY); /* added: held by the call only */
    image->segment = segment;
    return ta_napi_bigint(env, segment);
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
 * version of the chart named), ta_host_spawn; the session. The params
 * first (a getter may run JavaScript), then the texts, in the call's
 * room. */
static napi_value ta_napi_host_spawn(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    ta_host_chart_ref reference;
    napi_value options = call.argv[1];
    ta_status status = ta_napi_values_payload(host, ta_napi_member(env, options, "params"), &spawn.params);
    napi_value names[3] = {call.argv[0], ta_napi_member(env, options, "name"), ta_napi_member(env, options, "traceparent")};
    ta_string_view texts[3];
    ta_napi_texts reading = {names, 3, texts};
    char stack[256];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    if (status == TA_OK && ta_napi_is_undefined(env, call.argv[0])) status = TA_ERROR_INVALID;
    if (status == TA_OK) status = ta_napi_values_texts(&room, &reading);
    if (status == TA_OK) status = ta_host_chart(host->host, texts[0], &reference);
    uint64_t session = 0;
    if (status == TA_OK) {
        spawn.segment = reference.segment;
        spawn.chart = reference.chart;
        spawn.name = texts[1];
        spawn.traceparent = texts[2];
        ta_napi_loop_advance(host);
        status = ta_host_spawn(host->host, &spawn, &session);
    }
    ta_napi_room_free(&room);
    if (status != TA_OK) {
        if (spawn.params) host->binding.vtable->release(host->binding.datamodel, spawn.params);
        return ta_napi_refused(env, "ta_host_spawn", status);
    }
    ta_napi_loop_input(host);
    return ta_napi_bigint(env, session);
}

/* send(session, event, data): ta_host_send. */
static napi_value ta_napi_host_send(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    ta_status status = ta_napi_id(env, call.argv[0], &event.session) ? TA_OK : TA_ERROR_INVALID;
    if (status == TA_OK) status = ta_napi_values_payload(host, call.argv[2], &event.data);
    ta_napi_texts reading = {&call.argv[1], 1, &event.name};
    char stack[128];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    if (status == TA_OK && ta_napi_is_undefined(env, call.argv[1])) status = TA_ERROR_INVALID;
    if (status == TA_OK) status = ta_napi_values_texts(&room, &reading);
    if (status == TA_OK) status = ta_host_send(host->host, &event);
    ta_napi_room_free(&room);
    if (status != TA_OK) {
        if (event.data) host->binding.vtable->release(host->binding.datamodel, event.data);
        return ta_napi_refused(env, "ta_host_send", status);
    }
    ta_napi_loop_input(host);
    return ta_napi_undefined(env);
}

/* sendMany([{session, event, data}]): ta_host_send_many in batches from
 * the stack; the length of the accepted prefix. Each item's session and
 * event are checked first, so a malformed list sends nothing. Then each
 * batch: its payloads made (a getter may run JavaScript, and call the
 * Host), its names read into the batch's room, sent. A payload refused in
 * a later batch is thrown after the turn the earlier ones started, as the
 * Python binding's send_many does. */
#define TA_NAPI_BATCH 64

/* The item at `index` (given): its session, and its event name, a string. */
typedef struct ta_napi_item {
    uint32_t index;
    napi_value object, event;
    uint64_t session;
} ta_napi_item;

static int ta_napi_item_read(napi_env env, napi_value list, ta_napi_item *out) {
    napi_valuetype type = napi_undefined;
    if (napi_get_element(env, list, out->index, &out->object) != napi_ok ||
        !ta_napi_id(env, ta_napi_member(env, out->object, "session"), &out->session))
        return 0;
    out->event = ta_napi_member(env, out->object, "event");
    return napi_typeof(env, out->event, &type) == napi_ok && type == napi_string;
}

/* One batch of `count` items from `start`: how many core took; *status the
 * first refusal. */
typedef struct ta_napi_batch {
    ta_napi_host *host;
    const ta_napi_call *call;
    uint32_t start, count;
    ta_status status;
} ta_napi_batch;

static uint32_t ta_napi_batch_send(ta_napi_batch *batch) {
    ta_napi_host *host = batch->host;
    napi_env env = host->env;
    ta_event_in events[TA_NAPI_BATCH];
    napi_value names[TA_NAPI_BATCH];
    ta_string_view texts[TA_NAPI_BATCH];
    uint32_t read = 0;
    for (; read < batch->count && batch->status == TA_OK; read++) {
        ta_napi_item item = {batch->start + read, NULL, NULL, 0};
        ta_event_in *event = &events[read];
        memset(event, 0, sizeof *event);
        event->size = sizeof *event;
        if (!ta_napi_item_read(env, batch->call->argv[0], &item)) {
            batch->status = TA_ERROR_INVALID;
            break;
        }
        event->session = item.session;
        names[read] = item.event;
        batch->status = ta_napi_values_payload(host, ta_napi_member(env, item.object, "data"), &event->data);
        if (batch->status != TA_OK) break;
    }
    char stack[1024];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    ta_napi_texts reading = {names, read, texts};
    if (batch->status == TA_OK) batch->status = ta_napi_values_texts(&room, &reading);
    for (uint32_t i = 0; batch->status == TA_OK && i < read; i++)
        events[i].name = texts[i];
    uint32_t taken = batch->status == TA_OK ? ta_host_send_many(host->host, events, read) : 0;
    ta_napi_room_free(&room);
    for (uint32_t i = taken; i < read; i++)
        if (events[i].data) host->binding.vtable->release(host->binding.datamodel, events[i].data);
    return taken;
}

static napi_value ta_napi_host_send_many(napi_env env, napi_callback_info info) {
    ta_napi_call call;
    ta_napi_host *host = ta_napi_host_this(env, info, &call);
    if (!host) return NULL;
    bool array = false;
    uint32_t count = 0;
    ta_napi_item item;
    if (napi_is_array(env, call.argv[0], &array) != napi_ok || !array ||
        napi_get_array_length(env, call.argv[0], &count) != napi_ok)
        return ta_napi_refused(env, "Host.sendMany", TA_ERROR_INVALID);
    for (item.index = 0; item.index < count; item.index++)
        if (!ta_napi_item_read(env, call.argv[0], &item)) return ta_napi_refused(env, "Host.sendMany", TA_ERROR_INVALID);
    uint32_t taken = 0;
    ta_napi_batch batch = {host, &call, 0, 0, TA_OK};
    for (; batch.start < count; batch.start += TA_NAPI_BATCH) {
        batch.count = count - batch.start < TA_NAPI_BATCH ? count - batch.start : TA_NAPI_BATCH;
        uint32_t sent = ta_napi_batch_send(&batch);
        taken += sent;
        if (batch.status != TA_OK || sent < batch.count) break;
    }
    if (taken) ta_napi_loop_input(host);
    if (batch.status != TA_OK) return ta_napi_refused(env, "ta_host_send_many", batch.status);
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
    napi_value id = ta_napi_member(env, call.argv[0], "id");
    bool trusted = false;
    napi_get_value_bool(env, ta_napi_member(env, call.argv[0], "trusted"), &trusted);
    peer.flags = trusted ? TA_PEER_TRUSTED : 0;
    ta_napi_texts reading = {&id, 1, &peer.id};
    char stack[128];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    ta_status status = ta_napi_is_undefined(env, id) ? TA_ERROR_INVALID : ta_napi_values_texts(&room, &reading);
    uint32_t handle = 0;
    if (status == TA_OK) status = ta_host_peer_add(host->host, &peer, &handle);
    ta_napi_room_free(&room);
    if (status != TA_OK) return ta_napi_refused(env, "ta_host_peer_add", status);
    ta_napi_loop_input(host);
    return ta_napi_bigint(env, handle);
}

/* deliver(peer, envelope): the bytes borrowed for the call;
 * ta_host_envelope_read, then ta_host_deliver, also
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
