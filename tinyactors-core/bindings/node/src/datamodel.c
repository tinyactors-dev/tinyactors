/* datamodel.c — the native "ecmascript" datamodel: JavaScript values behind references (binding.h).
 */
#include "binding.h"

/* ---- payloads of the native datamodel (refcounted references, free-listed) */

ta_payload payload_new(nd_system *system, napi_value v) {
    uint32_t i;
    if (system->free_payload) {
        i = system->free_payload - 1;
        system->free_payload = (uint32_t)(uintptr_t)system->payloads[i];
    } else {
        uint32_t c = system->payload_capacity;
        grow((void **)&system->payloads, &c, system->payload_count + 1, sizeof(napi_ref));
        grow((void **)&system->payload_refs, &system->payload_capacity, system->payload_count + 1,
             sizeof(uint32_t));
        i = system->payload_count++;
    }
    system->payloads[i] = make_ref(system->environment, v);
    system->payload_refs[i] = 1;
    system->live_payloads++;
    return (ta_payload)i + 1;
}

napi_value payload_get(nd_system *system, ta_payload payload) {
    if (!payload || payload > system->payload_count || !system->payload_refs[payload - 1])
        return undef(system->environment);
    return get_ref(system->environment, system->payloads[payload - 1]);
}

static void native_retain(void *d, ta_payload payload) {
    nd_system *system = (nd_system *)d;
    if (payload && payload <= system->payload_count && system->payload_refs[payload - 1])
        system->payload_refs[payload - 1]++;
}

static void native_release(void *d, ta_payload payload) {
    nd_system *system = (nd_system *)d;
    if (!payload || payload > system->payload_count || !system->payload_refs[payload - 1]) return;
    if (--system->payload_refs[payload - 1] == 0) {
        napi_delete_reference(system->environment, system->payloads[payload - 1]);
        system->payloads[payload - 1] = (napi_ref)(uintptr_t)system->free_payload;
        system->free_payload = (uint32_t)payload;
        system->live_payloads--;
    }
}

/* ---- the native "ecmascript" datamodel -------------------------------------------------- */

nd_instance *instance_at(nd_system *system, uint64_t handle) {
    return handle && handle <= system->instance_count && system->instances[handle - 1].live
               ? &system->instances[handle - 1]
               : NULL;
}

/* In(stateID): the session being evaluated */
napi_value js_in(napi_env environment, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    void *data;
    napi_get_cb_info(environment, info, &argc, argv, NULL, &data);
    nd_system *system = (nd_system *)data;
    nd_instance *instance = system->evaluating;
    size_t length = 0;
    char *id = argc ? get_string(environment, argv[0], &length) : NULL;
    int yes = 0;
    if (id && instance && instance->outside) {
        uint32_t state = ta_image_state_by_name(instance->chart->image, instance->chart->chart,
                                                view_of(id, length));
        yes = state != TA_NIL && ta_datamodel_environment_in(instance->outside, state);
    }
    free(id);
    return bool_value(environment, yes);
}

void define_hidden(napi_env environment, napi_value o, const char *name, napi_value v,
                   int writable) {
    napi_property_descriptor d;
    memset(&d, 0, sizeof d);
    d.utf8name = name;
    d.value = v;
    d.attributes = 4 | (writable ? 1 : 0); /* napi_configurable (| napi_writable) */
    napi_define_properties(environment, o, 1, &d);
}

/* A new scope: the system variables, read-only and not enumerable. */
napi_value scope_new(nd_system *system, napi_value sessionid, napi_value name,
                     napi_value io_processors) {
    napi_env environment = system->environment;
    napi_value scope;
    napi_create_object(environment, &scope);
    define_hidden(environment, scope, "_sessionid", sessionid, 0);
    define_hidden(environment, scope, "_name", name, 0);
    define_hidden(environment, scope, "_ioprocessors", io_processors, 0);
    define_hidden(environment, scope, "In", get_ref(environment, system->in_function), 0);
    define_hidden(environment, scope, "_event", undef(environment), 0);
    return scope;
}

static napi_value system_variables_scope(nd_system *system,
                                         const ta_datamodel_system_variables *sv) {
    napi_env environment = system->environment;
    napi_value io;
    napi_create_object(environment, &io);
    for (uint32_t i = 0; i < sv->io_processor_count; i++) {
        napi_value entry;
        napi_create_object(environment, &entry);
        set(environment, entry, "location", str_value(environment, sv->io_processors[i].location));
        napi_set_property(environment, io, str_value(environment, sv->io_processors[i].type),
                          entry);
    }
    return scope_new(system, str_value(environment, sv->sessionid),
                     str_value(environment, sv->name), io);
}

static nd_instance *instance_new(nd_system *system, nd_chart *chart, uint64_t serial,
                                 napi_value scope, uint64_t *out) {
    uint32_t i;
    if (system->free_instance) {
        i = system->free_instance - 1;
        system->free_instance = system->instances[i].next_free;
    } else {
        grow((void **)&system->instances, &system->instance_capacity, system->instance_count + 1,
             sizeof(nd_instance));
        i = system->instance_count++;
    }
    nd_instance *instance = &system->instances[i];
    memset(instance, 0, sizeof *instance);
    instance->chart = chart;
    instance->serial = serial;
    instance->live = 1;
    instance->scope = make_ref(system->environment, scope);
    system->live_instances++;
    *out = i + 1;
    return instance;
}

void instance_free(nd_system *system, uint64_t handle) {
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return;
    set_ref(system->environment, &instance->scope, NULL);
    set_ref(system->environment, &instance->input, NULL);
    free(instance->string);
    instance->string = NULL;
    instance->live = 0;
    instance->next_free = system->free_instance;
    system->free_instance = (uint32_t)handle;
    system->live_instances--;
}

static void throw_unknown(nd_system *system, nd_instance *instance, ta_expression e) {
    ta_expression_info info;
    char message[600];
    if (ta_image_expression(instance->chart->image, instance->chart->chart, e, &info) == TA_OK)
        snprintf(message, sizeof message, "no implementation for the expression `%.*s`",
                 (int)(info.source.length > 500 ? 500 : info.source.length), info.source.pointer);
    else
        snprintf(message, sizeof message, "unknown expression %u", e);
    napi_throw_error(system->environment, NULL, message);
}

/* Call expression e's function with (scope[, value]); NULL on an exception (recorded). */
static napi_value evaluate(nd_system *system, nd_instance *instance, ta_expression e,
                           napi_value value) {
    nd_chart *chart = instance->chart;
    if (e >= chart->count || !chart->function[e]) {
        throw_unknown(system, instance, e);
        take_exception(system, instance->serial);
        return NULL;
    }
    napi_value argv[2] = {get_ref(system->environment, instance->scope), value}, r = NULL;
    nd_instance *outer = system->evaluating;
    system->evaluating = instance;
    r = call(system->environment, get_ref(system->environment, chart->function[e]), value ? 2 : 1,
             argv);
    system->evaluating = outer;
    if (!r) take_exception(system, instance->serial);
    return r;
}

static ta_status native_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    nd_system *system = (nd_system *)d;
    napi_env environment = system->environment;
    nd_chart *c = chart_new(chart);
    if (!c) return TA_ERROR_NO_MEMORY;
    c->function = (napi_ref *)calloc(c->count ? c->count : 1, sizeof(napi_ref));
    /* resolve(source, texts, kinds) -> functions, aligned with expression ids */
    napi_value texts, kinds, argv[3], result;
    napi_create_array_with_length(environment, c->count, &texts);
    napi_create_array_with_length(environment, c->count, &kinds);
    for (uint32_t e = 0; e < c->count; e++) {
        ta_expression_info info;
        if (ta_image_expression(c->image, c->chart, e, &info) != TA_OK) continue;
        napi_set_element(environment, texts, e, str_value(environment, info.source));
        napi_set_element(environment, kinds, e, num(environment, info.kind));
    }
    argv[0] = str_value(environment, ta_image_chart_source(c->image, c->chart));
    argv[1] = texts;
    argv[2] = kinds;
    result = call(environment, get_ref(environment, system->resolve), 3, argv);
    if (!result) take_exception(system, 0); /* unresolved expressions fail when evaluated */
    for (uint32_t e = 0; result && e < c->count; e++) {
        napi_value f;
        if (napi_get_element(environment, result, e, &f) == napi_ok &&
            type_of(environment, f) == napi_function)
            c->function[e] = make_ref(environment, f);
    }
    chart_add(system, c);
    *context = c;
    return TA_OK;
}

static void native_detach(void *d, void *context) { /* a segment is freed (or the host) */
    nd_system *system = (nd_system *)d;
    nd_chart *chart = (nd_chart *)context;
    if (!chart) return;
    chart_remove(system, chart);
    for (uint32_t e = 0; e < chart->count; e++)
        if (chart->function[e]) napi_delete_reference(system->environment, chart->function[e]);
    free(chart->function);
    free(chart);
}

static ta_status native_create(void *d, void *context, const ta_datamodel_system_variables *sv,
                               ta_payload init, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    uint64_t serial = serial_of(sv->sessionid);
    nd_instance *instance =
        instance_new(system, (nd_chart *)context, serial, system_variables_scope(system, sv), out);
    if (init) instance->input = make_ref(system->environment, payload_get(system, init));
    native_release(system, init); /* the init payload is ours now */
    mark_changed(system, &instance->stamp, serial);
    return TA_OK;
}

static void native_destroy(void *d, uint64_t handle) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return;
    grave_add(system, instance->serial, get_ref(system->environment, instance->scope));
    instance_free(system, handle);
}

#define ENTER()                                                                                    \
    nd_system *system = (nd_system *)d;                                                            \
    nd_instance *instance = instance_at(system, handle);                                           \
    if (!instance) return TA_ERROR_EXECUTION;                                                      \
    instance->outside = outside

#define LEAVE(status)                                                                              \
    do {                                                                                           \
        instance->outside = NULL;                                                                  \
        return (status);                                                                           \
    } while (0)

/* <data id expr>: the spawn's input overrides it (a property of that name) */
static ta_status native_init_data(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                  uint32_t declaration) {
    ENTER();
    napi_env environment = system->environment;
    ta_data_info data;
    if (ta_image_data(instance->chart->image, instance->chart->chart, declaration, &data) != TA_OK)
        LEAVE(TA_ERROR_EXECUTION);
    napi_value key = str_value(environment, data.id), v = NULL;
    if (instance->input) {
        napi_value input = get_ref(environment, instance->input);
        if (type_of(environment, input) == napi_object &&
            napi_get_property(environment, input, key, &v) == napi_ok &&
            type_of(environment, v) == napi_undefined)
            v = NULL;
    }
    if (!v)
        v = data.expression == TA_NIL ? undef(environment)
                                      : evaluate(system, instance, data.expression, NULL);
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    napi_set_property(environment, get_ref(environment, instance->scope), key, v);
    LEAVE(TA_OK);
}

static const char *const event_types[] = {"platform", "internal", "external"};

static napi_value event_object(nd_system *system, const ta_event_view *view) {
    napi_env environment = system->environment;
    napi_value e;
    napi_create_object(environment, &e);
    set(environment, e, "name", str_value(environment, view->name));
    set(environment, e, "type",
        str_value(environment, view_of(event_types[view->type < 3 ? view->type : 0],
                                       strlen(event_types[view->type < 3 ? view->type : 0]))));
    if (view->sendid.length) set(environment, e, "sendid", str_value(environment, view->sendid));
    if (view->origin.length) set(environment, e, "origin", str_value(environment, view->origin));
    if (view->origintype.length)
        set(environment, e, "origintype", str_value(environment, view->origintype));
    if (view->invokeid.length)
        set(environment, e, "invokeid", str_value(environment, view->invokeid));
    if (view->raw.length) set(environment, e, "raw", str_value(environment, view->raw));
    if (view->data) set(environment, e, "data", payload_get(system, view->data));
    return e;
}

static ta_status native_bind_event(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                   const ta_event_view *view) {
    (void)outside;
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    define_hidden(system->environment, get_ref(system->environment, instance->scope), "_event",
                  event_object(system, view), 0);
    mark_changed(system, &instance->stamp, instance->serial);
    return TA_OK;
}

static ta_status native_condition(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                  ta_expression e, int *out) {
    ENTER();
    napi_value v = evaluate(system, instance, e, NULL), b;
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    bool r = false;
    napi_coerce_to_bool(system->environment, v, &b);
    napi_get_value_bool(system->environment, b, &r);
    *out = r;
    LEAVE(TA_OK);
}

static ta_status to_text(nd_system *system, nd_instance *instance, napi_value v, int json,
                         ta_string_view *out) {
    napi_env environment = system->environment;
    napi_value s = NULL;
    if (json && type_of(environment, v) == napi_object)
        s = call_global(environment, "JSON", "stringify", 1, &v);
    if (!s || type_of(environment, s) != napi_string) {
        clear_exception(environment);
        if (napi_coerce_to_string(environment, v, &s) != napi_ok) {
            take_exception(system, instance->serial);
            return TA_ERROR_EXECUTION;
        }
    }
    size_t length = 0;
    free(instance->string);
    instance->string = get_string(environment, s, &length);
    if (!instance->string) return TA_ERROR_EXECUTION;
    *out = view_of(instance->string, length);
    return TA_OK;
}

static ta_status native_string(void *d, uint64_t handle, ta_datamodel_environment *outside,
                               ta_expression e, ta_string_view *out) {
    ENTER();
    napi_value v = evaluate(system, instance, e, NULL);
    LEAVE(v ? to_text(system, instance, v, 0, out) : TA_ERROR_EXECUTION);
}

static ta_status native_log(void *d, uint64_t handle, ta_datamodel_environment *outside,
                            ta_expression e, ta_string_view *out) {
    ENTER();
    napi_value v = evaluate(system, instance, e, NULL);
    LEAVE(v ? to_text(system, instance, v, 1, out) : TA_ERROR_EXECUTION);
}

/* location expressions resolve to setters: f(scope, value) */
static ta_status native_assign(void *d, uint64_t handle, ta_datamodel_environment *outside,
                               ta_expression location, ta_expression value) {
    ENTER();
    napi_value v = evaluate(system, instance, value, NULL);
    if (!v || !evaluate(system, instance, location, v)) LEAVE(TA_ERROR_EXECUTION);
    LEAVE(TA_OK);
}

static ta_status native_assign_string(void *d, uint64_t handle, ta_expression location,
                                      ta_string_view value) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    return evaluate(system, instance, location, str_value(system->environment, value))
               ? TA_OK
               : TA_ERROR_EXECUTION;
}

static ta_status native_script(void *d, uint64_t handle, ta_datamodel_environment *outside,
                               ta_expression e) {
    ENTER();
    LEAVE(evaluate(system, instance, e, NULL) ? TA_OK : TA_ERROR_EXECUTION);
}

/* <foreach>: the array (a payload while it runs), its items assigned in turn */
static ta_status native_foreach_begin(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                      ta_expression array, uint64_t *iterator, uint32_t *n) {
    ENTER();
    napi_value v = evaluate(system, instance, array, NULL);
    bool is_array = false;
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    napi_is_array(system->environment, v, &is_array);
    if (!is_array) {
        napi_throw_type_error(system->environment, NULL, "<foreach array>: not an array");
        take_exception(system, instance->serial);
        LEAVE(TA_ERROR_EXECUTION);
    }
    napi_value copy = call_global(system->environment, "Array", "from", 1, &v); /* a shallow copy */
    if (!copy) LEAVE(TA_ERROR_EXECUTION);
    *n = array_length(system->environment, copy);
    *iterator = payload_new(system, copy);
    LEAVE(TA_OK);
}

/* item and index declare their variable when it is not declared (SCXML 4.6) */
static ta_status foreach_assign(nd_system *system, nd_instance *instance, ta_expression location,
                                napi_value value) {
    ta_expression_info info;
    int identifier = ta_image_expression(instance->chart->image, instance->chart->chart, location,
                                         &info) == TA_OK &&
                     info.source.length > 0;
    for (uint32_t k = 0; identifier && k < info.source.length; k++) {
        char c = info.source.pointer[k];
        identifier = c == '_' || c == '$' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (k && c >= '0' && c <= '9');
    }
    if (!identifier)
        return evaluate(system, instance, location, value) ? TA_OK : TA_ERROR_EXECUTION;
    napi_set_property(system->environment, get_ref(system->environment, instance->scope),
                      str_value(system->environment, info.source), value);
    return TA_OK;
}

static ta_status native_foreach_item(void *d, uint64_t handle, uint64_t iterator, uint32_t i,
                                     ta_expression item, ta_expression index) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    if (!instance) return TA_ERROR_EXECUTION;
    napi_value element;
    napi_get_element(system->environment, payload_get(system, iterator), i, &element);
    if (foreach_assign(system, instance, item, element) != TA_OK) return TA_ERROR_EXECUTION;
    if (index != TA_NIL &&
        foreach_assign(system, instance, index, num(system->environment, i)) != TA_OK)
        return TA_ERROR_EXECUTION;
    return TA_OK;
}

static void native_foreach_end(void *d, uint64_t handle, uint64_t iterator) {
    (void)handle;
    native_release(d, iterator);
}

/* namelist and <param>s: an object of their values; <content>: its value; a
 * trace's value (TA_CAPTURE_VALUE): read with no effect the chart could see */
static ta_status native_capture(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                const ta_datamodel_capture *capture, ta_payload *out) {
    ENTER();
    napi_env environment = system->environment;
    napi_value v = NULL, scope = get_ref(environment, instance->scope);
    const ta_image *image = instance->chart->image;
    uint32_t chart = instance->chart->chart;
    if (capture->kind == TA_CAPTURE_VALUE) {
        ta_expression_info info;
        if (ta_image_expression(image, chart, capture->content, &info) != TA_OK)
            LEAVE(TA_ERROR_EXECUTION);
        napi_get_property(environment, scope, str_value(environment, info.source), &v);
    } else if (capture->content != TA_NIL) {
        v = evaluate(system, instance, capture->content, NULL);
    } else {
        napi_create_object(environment, &v);
        for (uint32_t i = 0; v && i < capture->namelist_count; i++) {
            ta_expression_info info;
            napi_value x;
            if (ta_image_expression(image, chart, capture->namelist[i], &info) != TA_OK) {
                v = NULL;
                break;
            }
            napi_value key = str_value(environment, info.source);
            bool has = false;
            napi_get_property(environment, scope, key, &x);
            napi_value keys;
            napi_get_property_names(environment, scope, &keys); /* a namelist names declared data */
            uint32_t n = array_length(environment, keys);
            for (uint32_t k = 0; k < n && !has; k++) {
                napi_value name;
                napi_get_element(environment, keys, k, &name);
                napi_strict_equals(environment, name, key, &has);
            }
            if (!has) {
                napi_throw_error(environment, NULL, "namelist: not a declared location");
                take_exception(system, instance->serial);
                v = NULL;
                break;
            }
            napi_set_property(environment, v, key, x);
        }
        for (uint32_t i = 0; v && i < capture->param_count; i++) {
            napi_value x = evaluate(system, instance, capture->params[2 * i + 1], NULL);
            if (!x) {
                v = NULL;
                break;
            }
            napi_set_property(
                environment, v,
                str_value(environment, ta_image_string(image, capture->params[2 * i])), x);
        }
    }
    if (!v) LEAVE(TA_ERROR_EXECUTION);
    *out = payload_new(system, v);
    LEAVE(TA_OK);
}

/* hibernation and export: {"v": variables, "e": _event} as neutral values; a
 * variable that cannot be one (a function, a cycle) keeps the session resident */
ta_status scope_wire(nd_system *system, napi_value scope, text *t) {
    napi_env environment = system->environment;
    napi_value keys;
    napi_get_property_names(environment, scope, &keys);
    uint32_t n = array_length(environment, keys);
    t->length = 0;
    text_put(t, "{\"v\":{", 6);
    int written = 0;
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, value;
        napi_get_element(environment, keys, i, &key);
        napi_get_property(environment, scope, key, &value);
        size_t length = 0;
        char *k = get_string(environment, key, &length);
        napi_valuetype kind = type_of(environment, value);
        ta_status status = TA_OK;
        if (kind == napi_function || kind == napi_symbol) status = TA_ERROR_UNSUPPORTED;
        if (kind == napi_undefined) {
            free(k);
            continue;
        }
        if (status == TA_OK) {
            if (written++) text_char(t, ',');
            text_string(t, k ? k : "", length);
            text_char(t, ':');
            status = wire_value(environment, value, t, 1);
        }
        if (status != TA_OK) {
            snprintf(system->error, sizeof system->error, "variable '%.*s' cannot be stored: %s",
                     (int)(length > 200 ? 200 : length), k ? k : "",
                     kind == napi_function ? "it holds a function"
                                           : "it is not a neutral value (a cycle?)");
            free(k);
            return TA_ERROR_UNSUPPORTED;
        }
        free(k);
    }
    text_put(t, "},\"e\":", 6);
    napi_value event = property(environment, scope, "_event");
    if (wire_value(environment, event, t, 1) != TA_OK) text_put(t, "null", 4);
    text_char(t, '}');
    return t->failed ? TA_ERROR_NO_MEMORY : TA_OK;
}

static ta_status native_serialize(void *d, uint64_t handle, void *buffer, size_t capacity,
                                  size_t *length) {
    nd_system *system = (nd_system *)d;
    nd_instance *instance = instance_at(system, handle);
    *length = 0;
    if (!instance) return TA_ERROR_INVALID;
    ta_status status =
        scope_wire(system, get_ref(system->environment, instance->scope), &system->wire);
    if (status != TA_OK) return status;
    *length = system->wire.length;
    if (system->wire.length > capacity) return TA_ERROR_NO_MEMORY;
    memcpy(buffer, system->wire.bytes, system->wire.length);
    return TA_OK;
}

/* Copy the enumerable properties of `from` onto `to`. */
void copy_properties(napi_env environment, napi_value to, napi_value from) {
    if (type_of(environment, from) != napi_object) return;
    napi_value keys;
    napi_get_property_names(environment, from, &keys);
    uint32_t n = array_length(environment, keys);
    for (uint32_t i = 0; i < n; i++) {
        napi_value key, value;
        napi_get_element(environment, keys, i, &key);
        napi_get_property(environment, from, key, &value);
        napi_set_property(environment, to, key, value);
    }
}

static ta_status native_deserialize(void *d, void *context, const ta_datamodel_system_variables *sv,
                                    const void *buffer, size_t length, uint64_t *out) {
    nd_system *system = (nd_system *)d;
    napi_env environment = system->environment;
    napi_value state = value_of_wire(environment, view_of((const char *)buffer, length),
                                     get_ref(environment, system->refs));
    if (!state || type_of(environment, state) != napi_object) return TA_ERROR_INVALID;
    napi_value scope = system_variables_scope(system, sv);
    copy_properties(environment, scope, property(environment, state, "v"));
    napi_value event = property(environment, state, "e");
    if (type_of(environment, event) == napi_object)
        define_hidden(environment, scope, "_event", event, 0);
    instance_new(system, (nd_chart *)context, serial_of(sv->sessionid), scope, out);
    return TA_OK;
}

/* Live migration: the previous bindings are the old variables; those the new
 * chart does not declare go. */
static ta_status native_migrate(void *d, uint64_t handle, ta_datamodel_environment *outside,
                                ta_payload *bindings) {
    ENTER();
    napi_env environment = system->environment;
    napi_value old = get_ref(environment, instance->scope), previous, keys;
    napi_value scope =
        scope_new(system, property(environment, old, "_sessionid"),
                  property(environment, old, "_name"), property(environment, old, "_ioprocessors"));
    napi_create_object(environment, &previous);
    copy_properties(environment, previous, old);
    const ta_image *image = instance->chart->image;
    uint32_t chart = instance->chart->chart, declared = ta_image_data_count(image, chart);
    napi_get_property_names(environment, old, &keys);
    uint32_t n = array_length(environment, keys);
    for (uint32_t k = 0; k < n; k++) {
        napi_value key, v;
        napi_get_element(environment, keys, k, &key);
        size_t kl = 0;
        char *ks = get_string(environment, key, &kl);
        int keep = 0;
        for (uint32_t i = 0; i < declared && !keep && ks; i++) {
            ta_data_info data;
            keep = ta_image_data(image, chart, i, &data) == TA_OK && data.id.length == kl &&
                   !memcmp(data.id.pointer, ks, kl);
        }
        free(ks);
        if (keep) {
            napi_get_property(environment, old, key, &v);
            napi_set_property(environment, scope, key, v);
        }
    }
    set_ref(environment, &instance->scope, scope);
    *bindings = payload_new(system, previous);
    LEAVE(TA_OK);
}

static ta_status native_to_wire(void *d, ta_payload payload, char *buffer, size_t capacity,
                                size_t *length) {
    nd_system *system = (nd_system *)d;
    ta_status status = wire_of(system->environment, payload_get(system, payload), &system->wire);
    if (status != TA_OK) return status;
    *length = system->wire.length;
    if (system->wire.length > capacity) return TA_ERROR_NO_MEMORY;
    memcpy(buffer, system->wire.bytes, system->wire.length);
    return TA_OK;
}

static ta_status native_from_wire(void *d, ta_string_view json, ta_payload *out) {
    nd_system *system = (nd_system *)d;
    napi_value v =
        value_of_wire(system->environment, json, get_ref(system->environment, system->refs));
    if (!v) return TA_ERROR_INVALID;
    *out = payload_new(system, v);
    return TA_OK;
}

static ta_status native_payload_serialize(void *d, ta_payload payload, void *buffer,
                                          size_t capacity, size_t *length) {
    ta_status status = native_to_wire(d, payload, (char *)buffer, capacity, length);
    return status == TA_ERROR_INVALID ? TA_ERROR_UNSUPPORTED : status;
}

static ta_status native_payload_deserialize(void *d, const void *buffer, size_t length,
                                            ta_payload *out) {
    return native_from_wire(d, view_of((const char *)buffer, length), out);
}

const ta_datamodel_vtable native_vtable = {sizeof(ta_datamodel_vtable),
                                           0,
                                           "ecmascript",
                                           native_attach,
                                           native_detach,
                                           native_create,
                                           native_destroy,
                                           native_init_data,
                                           native_bind_event,
                                           NULL,
                                           native_condition,
                                           native_string,
                                           native_assign,
                                           native_assign_string,
                                           native_script,
                                           native_log,
                                           native_foreach_begin,
                                           native_foreach_item,
                                           native_foreach_end,
                                           native_capture,
                                           native_retain,
                                           native_release,
                                           NULL,
                                           native_serialize,
                                           native_deserialize,
                                           native_migrate,
                                           native_to_wire,
                                           native_from_wire,
                                           native_payload_serialize,
                                           native_payload_deserialize};
