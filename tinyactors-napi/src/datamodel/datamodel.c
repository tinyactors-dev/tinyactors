/* datamodel.c — the native "ecmascript" datamodel: core's vtable
 * (tinyactors/datamodel.h) over the application's engine. Each session is
 * a scope object with a null prototype, as a session of quickjs's shared
 * realm is (tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:
 * 1173-1290): its <data> ids, _event and the read-only system variables
 * are its own properties, In() too, and every other name is the realm's.
 * Each entry opens a handle scope of its own (a run is one native call,
 * so handles would pile up until it returns), clears what an expression
 * threw and answers TA_ERROR_EXECUTION, so core raises error.execution.
 * What crosses to another actor is copied through the value table
 * (bindings.md, section 8, "Actors share no mutable value"). */
#include "datamodel.h"

#include <stdlib.h>
#include <string.h>

/* A chart attached, and the sessions of it: freed when both are gone
 * (sessions may outlive the detach). */
typedef struct ta_napi_attached {
    ta_napi_chart *chart;
    uint32_t uses;
} ta_napi_attached;

/* A session: its chart and what an entry hands back to core. */
typedef struct ta_napi_session {
    ta_napi_chart *chart;
    ta_napi_attached *attached;
    napi_ref init; /* the spawn's or invoke's params (an object), or NULL */
    char *string;  /* eval_string's and log's text, valid until the next call */
} ta_napi_session;

/* ---- entries ---------------------------------------------------------------------------- */

/* One entry into the datamodel for one session. */
typedef struct ta_napi_entry {
    ta_napi_datamodel *datamodel;
    ta_napi_session *session;
    napi_value scope;
    napi_handle_scope handles;
} ta_napi_entry;

static ta_status ta_napi_enter(const ta_datamodel_call *call, ta_napi_entry *entry) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)call->datamodel;
    entry->datamodel = datamodel;
    ta_napi_slot *slot = ta_napi_slots_get(&datamodel->sessions, call->instance);
    if (!slot || napi_open_handle_scope(datamodel->env, &entry->handles) != napi_ok) return TA_ERROR_EXECUTION;
    entry->session = (ta_napi_session *)slot->data;
    if (ta_napi_slot_value(datamodel, slot, &entry->scope) != napi_ok) {
        napi_close_handle_scope(datamodel->env, entry->handles);
        return TA_ERROR_EXECUTION;
    }
    datamodel->environment = call->environment;
    datamodel->chart = entry->session->chart;
    return TA_OK;
}

static ta_status ta_napi_leave(ta_napi_entry *entry, ta_status status) {
    ta_napi_datamodel *datamodel = entry->datamodel;
    napi_value thrown;
    if (ta_napi_exception_pending(datamodel->env)) napi_get_and_clear_last_exception(datamodel->env, &thrown);
    datamodel->environment = NULL;
    datamodel->chart = NULL;
    napi_close_handle_scope(datamodel->env, entry->handles);
    return status;
}

/* A napi call that failed: the expression threw. */
static ta_status ta_napi_executed(napi_status status) {
    return status == napi_ok ? TA_OK : TA_ERROR_EXECUTION;
}

/* ---- the realm's helpers --------------------------------------------------------------- */

static napi_value ta_napi_helper(ta_napi_datamodel *datamodel, const char *name) {
    napi_value helpers, helper = NULL;
    if (napi_get_reference_value(datamodel->env, datamodel->helpers, &helpers) == napi_ok)
        napi_get_named_property(datamodel->env, helpers, name, &helper);
    return helper;
}

/* realm[object][function](argument): JSON.parse, JSON.stringify. */
typedef struct ta_napi_realm_call {
    const char *object, *function;
    napi_value argument;
} ta_napi_realm_call;

static napi_status ta_napi_realm(napi_env env, const ta_napi_realm_call *call, napi_value *out) {
    napi_value global, object, function;
    napi_status status = napi_get_global(env, &global);
    if (status == napi_ok) status = napi_get_named_property(env, global, call->object, &object);
    if (status == napi_ok) status = napi_get_named_property(env, object, call->function, &function);
    return status == napi_ok ? napi_call_function(env, object, function, 1, &call->argument, out) : status;
}

/* ---- values of expressions ------------------------------------------------------------- */

/* <content> text (SCXML B.2): JSON if it parses, else its text with each
 * run of whitespace one space, trimmed (quickjs's content_value; its
 * small XML DOM is the W3C suite's, M2). */
static ta_status ta_napi_content(ta_napi_entry *entry, ta_string_view source, napi_value *out) {
    napi_env env = entry->datamodel->env;
    napi_value text;
    if (napi_create_string_utf8(env, source.pointer ? source.pointer : "", source.length, &text) != napi_ok)
        return TA_ERROR_EXECUTION;
    ta_napi_realm_call parse = {"JSON", "parse", text};
    if (ta_napi_realm(env, &parse, out) == napi_ok) return TA_OK;
    napi_value thrown;
    napi_get_and_clear_last_exception(env, &thrown);
    char *normal = (char *)malloc(source.length + 1);
    if (!normal) return TA_ERROR_NO_MEMORY;
    size_t length = 0;
    for (uint32_t i = 0; i < source.length; i++) {
        char c = source.pointer[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (length && normal[length - 1] != ' ') normal[length++] = ' ';
        } else {
            normal[length++] = c;
        }
    }
    while (length && normal[length - 1] == ' ')
        length--;
    ta_status status = ta_napi_executed(napi_create_string_utf8(env, normal, length, out));
    free(normal);
    return status;
}

/* The value of expression `expression` in the entry's session. */
static ta_status ta_napi_value_of(ta_napi_entry *entry, ta_expression expression, napi_value *out) {
    ta_napi_datamodel *datamodel = entry->datamodel;
    const ta_napi_chart *chart = entry->session->chart;
    if (expression >= chart->count) return TA_ERROR_EXECUTION;
    if (datamodel->environment) ta_datamodel_environment_charge(datamodel->environment, 1);
    if (chart->kinds[expression] == TA_EXPRESSION_CONTENT)
        return ta_napi_content(entry, ta_napi_chart_source(chart, expression), out);
    napi_value function, undefined;
    if (!chart->getters[expression] ||
        napi_get_reference_value(datamodel->env, chart->getters[expression], &function) != napi_ok)
        return TA_ERROR_EXECUTION;
    napi_get_undefined(datamodel->env, &undefined);
    return ta_napi_executed(napi_call_function(datamodel->env, undefined, function, 1, &entry->scope, out));
}

static int ta_napi_identifier_character(char c, uint32_t at) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (at && c >= '0' && c <= '9');
}

/* The name a location starts with: its first identifier, after spaces. */
static ta_string_view ta_napi_location_name(ta_string_view source) {
    while (source.length && (source.pointer[0] == ' ' || source.pointer[0] == '\t' || source.pointer[0] == '\n' ||
                             source.pointer[0] == '\r')) {
        source.pointer++;
        source.length--;
    }
    uint32_t length = 0;
    while (length < source.length && ta_napi_identifier_character(source.pointer[length], length))
        length++;
    source.length = length;
    return source;
}

static int ta_napi_named(ta_string_view name, const char *text) {
    return name.length == strlen(text) && !memcmp(name.pointer, text, name.length);
}

/* A location must start with one of the session's own variables, and not a
 * system variable (SCXML: assigning to an undeclared location is
 * error.execution; system variables are read-only), else the assignment
 * would land in the realm's global object: quickjs's shared_location_ok
 * (datamodel_ecmascript.c:417-445). */
static int ta_napi_location_ok(ta_napi_entry *entry, ta_expression location) {
    napi_env env = entry->datamodel->env;
    ta_string_view name = ta_napi_location_name(ta_napi_chart_source(entry->session->chart, location));
    if (!name.length || ta_napi_named(name, "_event") || ta_napi_named(name, "_sessionid") ||
        ta_napi_named(name, "_name") || ta_napi_named(name, "_ioprocessors") || ta_napi_named(name, "In"))
        return 0;
    napi_value key;
    bool own = false;
    return napi_create_string_utf8(env, name.pointer, name.length, &key) == napi_ok &&
           napi_has_own_property(env, entry->scope, key, &own) == napi_ok && own;
}

/* Assign `value` to location `location`. */
static ta_status ta_napi_assign_to(ta_napi_entry *entry, ta_expression location, napi_value value) {
    ta_napi_datamodel *datamodel = entry->datamodel;
    const ta_napi_chart *chart = entry->session->chart;
    if (location >= chart->count || !ta_napi_location_ok(entry, location) || !chart->setters[location])
        return TA_ERROR_EXECUTION;
    napi_value function, undefined, result, argv[2] = {entry->scope, value};
    if (napi_get_reference_value(datamodel->env, chart->setters[location], &function) != napi_ok)
        return TA_ERROR_EXECUTION;
    napi_get_undefined(datamodel->env, &undefined);
    return ta_napi_executed(napi_call_function(datamodel->env, undefined, function, 2, argv, &result));
}

/* A string core reads back, kept in the session until the next call. */
static ta_status ta_napi_string_out(ta_napi_entry *entry, napi_value value, ta_string_view *out) {
    ta_string_view text;
    ta_status status = ta_napi_values_text(entry->datamodel->env, value, &text);
    if (status != TA_OK) return TA_ERROR_EXECUTION;
    free(entry->session->string);
    entry->session->string = (char *)text.pointer;
    out->pointer = text.pointer ? text.pointer : "";
    out->length = text.length;
    return TA_OK;
}

/* ---- In() ---------------------------------------------------------------------------- */

/* In('id'): whether the evaluating session's configuration holds state id. */
static napi_value ta_napi_in(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    void *data = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, &data) != napi_ok) return NULL;
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    int in = 0;
    ta_string_view id;
    if (argc >= 1 && datamodel->environment && datamodel->chart &&
        ta_napi_values_text(env, argv[0], &id) == TA_OK) {
        uint32_t state = ta_image_state_by_name(datamodel->chart->image, datamodel->chart->chart, id);
        in = state != TA_NIL && ta_datamodel_environment_in(datamodel->environment, state);
        free((void *)id.pointer);
    }
    napi_get_boolean(env, in, &result);
    return result;
}

/* ---- the vtable ------------------------------------------------------------------------- */

static ta_status ta_napi_chart_attach(void *data, const ta_datamodel_chart *chart, void **context) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    ta_napi_attached *attached = (ta_napi_attached *)calloc(1, sizeof *attached);
    if (!attached) return TA_ERROR_NO_MEMORY;
    ta_status status = ta_napi_chart_compile(datamodel, chart, &attached->chart);
    if (status != TA_OK) {
        free(attached);
        return status;
    }
    attached->uses = 1;
    *context = attached;
    return TA_OK;
}

static void ta_napi_attached_unuse(ta_napi_datamodel *datamodel, ta_napi_attached *attached) {
    if (--attached->uses) return;
    ta_napi_chart_free(datamodel, attached->chart);
    free(attached);
}

static void ta_napi_chart_detach(void *data, void *context) {
    ta_napi_attached_unuse((ta_napi_datamodel *)data, (ta_napi_attached *)context);
}

/* A read-only, enumerable own property of the scope. */
typedef struct ta_napi_property {
    const char *name;
    napi_value value;
} ta_napi_property;

static void ta_napi_define(napi_env env, napi_value object, const ta_napi_property *property) {
    napi_property_descriptor descriptor;
    memset(&descriptor, 0, sizeof descriptor);
    descriptor.utf8name = property->name;
    descriptor.value = property->value;
    descriptor.attributes = napi_enumerable | napi_configurable;
    napi_define_properties(env, object, 1, &descriptor);
}

static napi_value ta_napi_text(napi_env env, ta_string_view text) {
    napi_value value = NULL;
    napi_create_string_utf8(env, text.pointer ? text.pointer : "", text.length, &value);
    return value;
}

/* _ioprocessors: each processor by its type, and SCXML's and BasicHTTP's
 * by their short names, as quickjs has them (datamodel_ecmascript.c:
 * 1247-1267). */
static napi_value ta_napi_io_processors(napi_env env, const ta_datamodel_system_variables *variables) {
    napi_value processors;
    napi_create_object(env, &processors);
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        const ta_io_processor_entry *entry = &variables->io_processors[i];
        napi_value processor, type;
        napi_create_object(env, &processor);
        napi_set_named_property(env, processor, "location", ta_napi_text(env, entry->location));
        type = ta_napi_text(env, entry->type);
        napi_set_property(env, processors, type, processor);
        static const char *const suffixes[] = {"#SCXMLEventProcessor", "#BasicHTTPEventProcessor"};
        static const char *const aliases[] = {"scxml", "basichttp"};
        for (size_t k = 0; k < 2; k++) {
            size_t length = strlen(suffixes[k]);
            if (entry->type.length > length &&
                !memcmp(entry->type.pointer + entry->type.length - length, suffixes[k], length))
                napi_set_named_property(env, processors, aliases[k], processor);
        }
    }
    return processors;
}

static ta_status ta_napi_create(void *data, const ta_datamodel_start *start, uint64_t *instance) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    napi_env env = datamodel->env;
    ta_napi_attached *attached = (ta_napi_attached *)start->chart_context;
    const ta_datamodel_system_variables *variables = start->system_variables;
    ta_napi_session *session = (ta_napi_session *)calloc(1, sizeof *session);
    napi_handle_scope handles;
    if (!session || napi_open_handle_scope(env, &handles) != napi_ok) {
        free(session);
        return TA_ERROR_NO_MEMORY;
    }
    ta_status status = TA_ERROR_NO_MEMORY;
    napi_value scope, undefined, in, function = ta_napi_helper(datamodel, "scope");
    napi_get_undefined(env, &undefined);
    if (function && napi_call_function(env, undefined, function, 0, NULL, &scope) == napi_ok &&
        napi_get_reference_value(env, datamodel->in, &in) == napi_ok) {
        ta_napi_property properties[] = {
            {"_sessionid", ta_napi_text(env, variables->sessionid)},
            {"_name", ta_napi_text(env, variables->name)},
            {"_ioprocessors", ta_napi_io_processors(env, variables)},
            {"In", in},
        };
        for (size_t i = 0; i < sizeof properties / sizeof properties[0]; i++)
            ta_napi_define(env, scope, &properties[i]);
        /* every <data> id is declared from the start (late binding:
         * undefined until its init_data) */
        uint32_t count = ta_image_data_count(attached->chart->image, attached->chart->chart);
        for (uint32_t i = 0; i < count; i++) {
            ta_data_info info;
            if (ta_image_data(attached->chart->image, (ta_chart_item){attached->chart->chart, i}, &info) != TA_OK)
                continue;
            napi_set_property(env, scope, ta_napi_text(env, info.id), undefined);
        }
        session->chart = attached->chart;
        ta_napi_slot *init = ta_napi_slots_get(&datamodel->payloads, start->init);
        napi_value params;
        if (init && ta_napi_slot_value(datamodel, init, &params) == napi_ok)
            napi_create_reference(env, params, 1, &session->init);
        ta_napi_slot_init slot = {scope, session};
        *instance = ta_napi_slots_add(datamodel, &datamodel->sessions, &slot);
        status = *instance ? TA_OK : TA_ERROR_NO_MEMORY;
    }
    if (start->init) ta_napi_slots_remove(datamodel, &datamodel->payloads, start->init); /* passed to us */
    napi_close_handle_scope(env, handles);
    if (status != TA_OK) {
        if (session->init) ta_napi_defer_drop(datamodel, session->init);
        free(session);
        return status;
    }
    attached->uses++;
    session->attached = attached;
    return TA_OK;
}

static void ta_napi_destroy(const ta_datamodel_call *call) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)call->datamodel;
    ta_napi_slot *slot = ta_napi_slots_get(&datamodel->sessions, call->instance);
    if (!slot) return;
    ta_napi_session *session = (ta_napi_session *)slot->data;
    ta_napi_slots_remove(datamodel, &datamodel->sessions, call->instance);
    if (session->init) ta_napi_defer_drop(datamodel, session->init);
    ta_napi_attached_unuse(datamodel, session->attached);
    free(session->string);
    free(session);
}

static ta_status ta_napi_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_env env = entry.datamodel->env;
    ta_data_info info;
    if (ta_image_data(entry.session->chart->image, (ta_chart_item){entry.session->chart->chart, declaration}, &info) !=
        TA_OK)
        return ta_napi_leave(&entry, TA_ERROR_EXECUTION);
    napi_value key = ta_napi_text(env, info.id), value, params;
    napi_get_undefined(env, &value);
    bool has = false;
    /* the spawn's or invoke's params override <data> of the same name */
    if (entry.session->init && napi_get_reference_value(env, entry.session->init, &params) == napi_ok &&
        napi_has_property(env, params, key, &has) == napi_ok && has) {
        napi_get_property(env, params, key, &value);
    } else if (info.expression != TA_NIL) {
        status = ta_napi_value_of(&entry, info.expression, &value);
        if (status != TA_OK) napi_get_undefined(env, &value);
    }
    napi_value thrown;
    if (ta_napi_exception_pending(env)) napi_get_and_clear_last_exception(env, &thrown);
    napi_set_property(env, entry.scope, key, value);
    return ta_napi_leave(&entry, status);
}

/* _event: name, type, sendid, origin, origintype, invokeid, data and raw,
 * read-only (quickjs's bind_event, datamodel_ecmascript.c:1370-1397). */
static ta_status ta_napi_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    ta_napi_datamodel *datamodel = entry.datamodel;
    napi_env env = datamodel->env;
    static const char *const types[] = {"platform", "internal", "external"};
    napi_value event, undefined, data;
    napi_get_undefined(env, &undefined);
    napi_create_object(env, &event);
    napi_set_named_property(env, event, "name", ta_napi_text(env, view->name));
    napi_value type;
    napi_create_string_utf8(env, types[view->type % 3], NAPI_AUTO_LENGTH, &type);
    napi_set_named_property(env, event, "type", type);
    const ta_string_view *texts[] = {&view->sendid, &view->origin, &view->origintype, &view->invokeid};
    static const char *const names[] = {"sendid", "origin", "origintype", "invokeid"};
    for (size_t i = 0; i < 4; i++)
        napi_set_named_property(env, event, names[i], texts[i]->length ? ta_napi_text(env, *texts[i]) : undefined);
    ta_napi_slot *payload = ta_napi_slots_get(&datamodel->payloads, view->data);
    if (!payload || ta_napi_slot_value(datamodel, payload, &data) != napi_ok) data = undefined;
    napi_set_named_property(env, event, "data", data);
    if (view->size >= sizeof *view && view->raw.length)
        napi_set_named_property(env, event, "raw", ta_napi_text(env, view->raw));
    ta_napi_property property = {"_event", event};
    ta_napi_define(env, entry.scope, &property);
    return ta_napi_leave(&entry, TA_OK);
}

static ta_status ta_napi_eval_condition(const ta_datamodel_call *call, ta_expression expression, int *out) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_value value, truth;
    bool b = false;
    status = ta_napi_value_of(&entry, expression, &value);
    if (status == TA_OK)
        status = ta_napi_executed(napi_coerce_to_bool(entry.datamodel->env, value, &truth) == napi_ok
                                      ? napi_get_value_bool(entry.datamodel->env, truth, &b)
                                      : napi_pending_exception);
    if (status == TA_OK) *out = b;
    return ta_napi_leave(&entry, status);
}

static ta_status ta_napi_eval_string(const ta_datamodel_call *call, ta_expression expression, ta_string_view *out) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_value value, text;
    status = ta_napi_value_of(&entry, expression, &value);
    if (status == TA_OK) status = ta_napi_executed(napi_coerce_to_string(entry.datamodel->env, value, &text));
    if (status == TA_OK) status = ta_napi_string_out(&entry, text, out);
    return ta_napi_leave(&entry, status);
}

static ta_status ta_napi_assign(const ta_datamodel_call *call, ta_expression location, ta_expression expression) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_value value;
    status = ta_napi_value_of(&entry, expression, &value);
    if (status == TA_OK) status = ta_napi_assign_to(&entry, location, value);
    return ta_napi_leave(&entry, status);
}

static ta_status ta_napi_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view text) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    status = ta_napi_assign_to(&entry, location, ta_napi_text(entry.datamodel->env, text));
    return ta_napi_leave(&entry, status);
}

static ta_status ta_napi_script(const ta_datamodel_call *call, ta_expression expression) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_value value;
    status = ta_napi_value_of(&entry, expression, &value);
    return ta_napi_leave(&entry, status);
}

/* <log expr>: an object as its JSON (a function as its text), anything else
 * as its string (quickjs's to_string_out with json_objects). */
static ta_status ta_napi_log(const ta_datamodel_call *call, ta_expression expression, ta_string_view *out) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_env env = entry.datamodel->env;
    napi_value value, text = NULL;
    status = ta_napi_value_of(&entry, expression, &value);
    if (status == TA_OK) {
        napi_valuetype type = napi_undefined;
        napi_typeof(env, value, &type);
        napi_valuetype text_type = napi_undefined;
        ta_napi_realm_call stringify = {"JSON", "stringify", value};
        if (type == napi_object && ta_napi_realm(env, &stringify, &text) == napi_ok &&
            napi_typeof(env, text, &text_type) == napi_ok && text_type == napi_string) {
        } else {
            napi_value thrown;
            if (ta_napi_exception_pending(env)) napi_get_and_clear_last_exception(env, &thrown);
            status = ta_napi_executed(napi_coerce_to_string(env, value, &text));
        }
    }
    if (status == TA_OK) status = ta_napi_string_out(&entry, text, out);
    return ta_napi_leave(&entry, status);
}

/* <foreach>: a shallow copy of the array's items, kept until foreach_end. */
static ta_status ta_napi_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    ta_napi_datamodel *datamodel = entry.datamodel;
    napi_env env = datamodel->env;
    napi_value value, copy;
    bool is_array = false;
    uint32_t length = 0;
    status = ta_napi_value_of(&entry, array, &value);
    if (status == TA_OK && (napi_is_array(env, value, &is_array) != napi_ok || !is_array ||
                            napi_get_array_length(env, value, &length) != napi_ok))
        status = TA_ERROR_EXECUTION;
    if (status == TA_OK) status = ta_napi_executed(napi_create_array_with_length(env, length, &copy));
    for (uint32_t i = 0; i < length && status == TA_OK; i++) {
        napi_value item;
        status = ta_napi_executed(napi_get_element(env, value, i, &item));
        if (status == TA_OK) status = ta_napi_executed(napi_set_element(env, copy, i, item));
    }
    uint32_t slot = 0;
    while (status == TA_OK && slot < datamodel->iteration_count && datamodel->iterations[slot])
        slot++;
    if (status == TA_OK && slot == datamodel->iteration_capacity) {
        uint32_t capacity = datamodel->iteration_capacity ? datamodel->iteration_capacity * 2 : 8;
        napi_ref *iterations = (napi_ref *)realloc(datamodel->iterations, capacity * sizeof *iterations);
        if (!iterations) status = TA_ERROR_NO_MEMORY;
        else {
            datamodel->iterations = iterations;
            datamodel->iteration_capacity = capacity;
        }
    }
    if (status == TA_OK) {
        if (slot == datamodel->iteration_count) datamodel->iteration_count++;
        datamodel->iterations[slot] = NULL;
        status = ta_napi_executed(napi_create_reference(env, copy, 1, &datamodel->iterations[slot]));
    }
    if (status == TA_OK) {
        loop->iter = (uint64_t)slot + 1;
        loop->n = length;
    }
    return ta_napi_leave(&entry, status);
}

/* <foreach> may introduce its item and index: a plain name not declared yet
 * is declared in the session (quickjs's declare_if_missing). */
static void ta_napi_declare(ta_napi_entry *entry, ta_expression location) {
    napi_env env = entry->datamodel->env;
    ta_string_view source = ta_napi_chart_source(entry->session->chart, location);
    ta_string_view name = ta_napi_location_name(source);
    uint32_t end = (uint32_t)(name.pointer - source.pointer) + name.length;
    while (end < source.length && (source.pointer[end] == ' ' || source.pointer[end] == '\t' || source.pointer[end] == '\n'))
        end++;
    if (!name.length || end != source.length) return;
    napi_value key = ta_napi_text(env, name), undefined;
    bool own = false;
    if (napi_has_own_property(env, entry->scope, key, &own) != napi_ok || own) return;
    napi_get_undefined(env, &undefined);
    napi_set_property(env, entry->scope, key, undefined);
}

static ta_status ta_napi_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    ta_napi_datamodel *datamodel = entry.datamodel;
    napi_env env = datamodel->env;
    napi_value items, item, index;
    if (!loop->iter || loop->iter > datamodel->iteration_count || !datamodel->iterations[loop->iter - 1] ||
        napi_get_reference_value(env, datamodel->iterations[loop->iter - 1], &items) != napi_ok ||
        napi_get_element(env, items, loop->i, &item) != napi_ok)
        return ta_napi_leave(&entry, TA_ERROR_EXECUTION);
    ta_napi_declare(&entry, loop->item);
    status = ta_napi_assign_to(&entry, loop->item, item);
    if (status == TA_OK && loop->index != TA_NIL) {
        ta_napi_declare(&entry, loop->index);
        napi_create_uint32(env, loop->i, &index);
        status = ta_napi_assign_to(&entry, loop->index, index);
    }
    return ta_napi_leave(&entry, status);
}

static void ta_napi_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)call->datamodel;
    if (!loop->iter || loop->iter > datamodel->iteration_count || !datamodel->iterations[loop->iter - 1]) return;
    ta_napi_defer_drop(datamodel, datamodel->iterations[loop->iter - 1]);
    datamodel->iterations[loop->iter - 1] = NULL;
}

/* A payload holding a copy of `value` through the value table: what
 * crosses to another actor shares nothing with the sender (bindings.md,
 * section 8). A value outside the table fails it. */
static ta_status ta_napi_payload_copy(ta_napi_datamodel *datamodel, napi_value value, ta_payload *out) {
    ta_buffer cbor;
    napi_value copy;
    ta_status status = ta_napi_values_encode(datamodel->env, value, &cbor);
    if (status != TA_OK) return status;
    status = ta_napi_values_decode(datamodel->env, (ta_bytes){cbor.pointer, cbor.length}, &copy);
    free(cbor.pointer);
    if (status != TA_OK) return status;
    ta_napi_slot_init slot = {copy, NULL};
    *out = ta_napi_slots_add(datamodel, &datamodel->payloads, &slot);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

/* <send>, <invoke> and <donedata> data: the content's value, or an object of
 * the namelist's and the params' values (quickjs's capture,
 * datamodel_ecmascript.c:1557-1598), copied. */
static ta_status ta_napi_capture(const ta_datamodel_call *call, const ta_datamodel_capture *capture, ta_payload *out) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_env env = entry.datamodel->env;
    napi_value value;
    if (capture->content != TA_NIL) {
        status = ta_napi_value_of(&entry, capture->content, &value);
    } else {
        status = ta_napi_executed(napi_create_object(env, &value));
        for (uint32_t i = 0; i < capture->namelist_count && status == TA_OK; i++) {
            napi_value item;
            status = ta_napi_value_of(&entry, capture->namelist[i], &item);
            if (status == TA_OK)
                status = ta_napi_executed(napi_set_property(
                    env, value, ta_napi_text(env, ta_napi_chart_source(entry.session->chart, capture->namelist[i])), item));
        }
        for (uint32_t i = 0; i < capture->param_count && status == TA_OK; i++) {
            napi_value item;
            status = ta_napi_value_of(&entry, capture->params[2 * i + 1], &item);
            if (status == TA_OK)
                status = ta_napi_executed(napi_set_property(
                    env, value, ta_napi_text(env, ta_image_string(entry.session->chart->image, capture->params[2 * i])), item));
        }
    }
    if (status == TA_OK) {
        status = ta_napi_payload_copy(entry.datamodel, value, out);
        if (status == TA_ERROR_INVALID) status = TA_ERROR_EXECUTION;
    }
    return ta_napi_leave(&entry, status);
}

static void ta_napi_retain(void *data, ta_payload payload) {
    ta_napi_slot *slot = ta_napi_slots_get(&((ta_napi_datamodel *)data)->payloads, payload);
    if (slot) slot->references++;
}

/* The last reference: the slot is freed and its napi_ref dropped once the
 * outer call into core returns (handles.c). */
static void ta_napi_release(void *data, ta_payload payload) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    ta_napi_slot *slot = ta_napi_slots_get(&datamodel->payloads, payload);
    if (slot && slot->references == 1) ta_napi_slots_remove(datamodel, &datamodel->payloads, payload);
    else if (slot) slot->references--;
}

static ta_status ta_napi_to_wire(void *data, ta_payload payload, ta_buffer *out) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    ta_napi_slot *slot = ta_napi_slots_get(&datamodel->payloads, payload);
    napi_handle_scope handles;
    if (!slot || napi_open_handle_scope(datamodel->env, &handles) != napi_ok) return TA_ERROR_INVALID;
    napi_value value;
    ta_buffer cbor = {NULL, 0, 0};
    ta_status status = ta_napi_slot_value(datamodel, slot, &value) == napi_ok
                           ? ta_napi_values_encode(datamodel->env, value, &cbor)
                           : TA_ERROR_INVALID;
    napi_value thrown;
    if (ta_napi_exception_pending(datamodel->env)) napi_get_and_clear_last_exception(datamodel->env, &thrown);
    napi_close_handle_scope(datamodel->env, handles);
    if (status == TA_OK) {
        out->length = cbor.length;
        if (cbor.length > out->capacity) status = TA_ERROR_NO_MEMORY;
        else if (cbor.length) memcpy(out->pointer, cbor.pointer, cbor.length);
    }
    free(cbor.pointer);
    return status;
}

static ta_status ta_napi_from_wire(void *data, ta_bytes cbor, ta_payload *out) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    napi_handle_scope handles;
    if (napi_open_handle_scope(datamodel->env, &handles) != napi_ok) return TA_ERROR_NO_MEMORY;
    napi_value value;
    ta_status status = ta_napi_values_decode(datamodel->env, cbor, &value);
    if (status == TA_OK) {
        ta_napi_slot_init slot = {value, NULL};
        *out = ta_napi_slots_add(datamodel, &datamodel->payloads, &slot);
        if (!*out) status = TA_ERROR_NO_MEMORY;
    }
    napi_close_handle_scope(datamodel->env, handles);
    return status;
}

/* actor.context: a variable of the session, as a new payload. */
static ta_status ta_napi_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return TA_ERROR_INVALID;
    napi_env env = entry.datamodel->env;
    napi_value key = ta_napi_text(env, name), value;
    bool own = false;
    if (napi_has_own_property(env, entry.scope, key, &own) != napi_ok || !own ||
        napi_get_property(env, entry.scope, key, &value) != napi_ok)
        return ta_napi_leave(&entry, TA_ERROR_INVALID);
    ta_napi_slot_init slot = {value, NULL};
    *out = ta_napi_slots_add(entry.datamodel, &entry.datamodel->payloads, &slot);
    return ta_napi_leave(&entry, *out ? TA_OK : TA_ERROR_NO_MEMORY);
}

static const ta_datamodel_vtable ta_napi_vtable = {
    sizeof(ta_datamodel_vtable),
    0,
    "ecmascript",
    ta_napi_chart_attach,
    ta_napi_chart_detach,
    ta_napi_create,
    ta_napi_destroy,
    ta_napi_init_data,
    ta_napi_bind_event,
    NULL, /* detach_event */
    ta_napi_eval_condition,
    ta_napi_eval_string,
    ta_napi_assign,
    ta_napi_assign_string,
    ta_napi_script,
    ta_napi_log,
    ta_napi_foreach_begin,
    ta_napi_foreach_item,
    ta_napi_foreach_end,
    ta_napi_capture,
    ta_napi_retain,
    ta_napi_release,
    NULL, /* custom */
    NULL, /* serialize: hibernation is M4's (ROADMAP.md) */
    NULL, /* deserialize */
    NULL, /* migrate */
    ta_napi_to_wire,
    ta_napi_from_wire,
    NULL, /* payload_serialize */
    NULL, /* payload_deserialize */
    ta_napi_variable,
};

/* ---- the datamodel ------------------------------------------------------------------------ */

static const char ta_napi_helpers_source[] = "({ scope() { return Object.create(null); } })";

ta_napi_datamodel *ta_napi_datamodel_new(napi_env env) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)calloc(1, sizeof *datamodel);
    if (!datamodel) return NULL;
    datamodel->env = env;
    napi_value source, helpers, in;
    if (napi_create_string_utf8(env, ta_napi_helpers_source, sizeof ta_napi_helpers_source - 1, &source) != napi_ok ||
        napi_run_script(env, source, &helpers) != napi_ok ||
        napi_create_reference(env, helpers, 1, &datamodel->helpers) != napi_ok ||
        napi_create_function(env, "In", NAPI_AUTO_LENGTH, ta_napi_in, datamodel, &in) != napi_ok ||
        napi_create_reference(env, in, 1, &datamodel->in) != napi_ok) {
        ta_napi_datamodel_free(datamodel);
        return NULL;
    }
    return datamodel;
}

/* After the host is freed: every session and payload is gone already. */
void ta_napi_datamodel_free(ta_napi_datamodel *datamodel) {
    if (!datamodel) return;
    for (uint32_t i = 0; i < datamodel->payloads.count; i++)
        if (datamodel->payloads.items[i].references && datamodel->payloads.items[i].value)
            ta_napi_defer_drop(datamodel, datamodel->payloads.items[i].value);
    for (uint32_t i = 0; i < datamodel->iteration_count; i++)
        if (datamodel->iterations[i]) ta_napi_defer_drop(datamodel, datamodel->iterations[i]);
    if (datamodel->helpers) ta_napi_defer_drop(datamodel, datamodel->helpers);
    if (datamodel->in) ta_napi_defer_drop(datamodel, datamodel->in);
    ta_napi_datamodel_drain(datamodel);
    free(datamodel->payloads.items);
    free(datamodel->sessions.items);
    free(datamodel->iterations);
    free(datamodel->drops);
    free(datamodel);
}

ta_datamodel_binding ta_napi_datamodel_binding(ta_napi_datamodel *datamodel) {
    ta_datamodel_binding binding = {&ta_napi_vtable, datamodel};
    return binding;
}
