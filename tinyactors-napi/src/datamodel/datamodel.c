/* datamodel.c — the native "ecmascript" datamodel: core's vtable
 * (tinyactors/datamodel.h) over the application's engine. Each session is
 * a scope object with a null prototype, as a session of quickjs's shared
 * realm is (tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:
 * 1173-1290): its <data> ids, _event and the read-only system variables
 * are its own properties, In() too, and every other name is the realm's.
 * Each entry opens a handle scope of its own (a run is one native call,
 * so handles would pile up until it returns), clears what an expression
 * threw and answers TA_ERROR_EXECUTION, so core raises error.execution.
 * What crosses to another actor is copied by the value table
 * (ta_napi_values_copy; bindings.md, section 8, "Actors share no mutable
 * value").
 *
 * Sessions, charts, payloads and iterations are rows of the datamodel's
 * tables (handles.c). Text an entry puts together is in a room of that
 * entry's own; what core reads back (to_wire's bytes, eval_string's and
 * log's strings) is the datamodel's last output, kept until its next call.
 * Nothing here calls an allocator (AGENTS.md, "The binding's memory"). */
#include "datamodel.h"

#include <string.h>

/* A chart's handle is its context in core (a void *): handles fit. */
_Static_assert(sizeof(void *) >= sizeof(uint64_t), "a chart's handle is its context");

/* ---- entries ---------------------------------------------------------------------------- */

/* One entry into the datamodel for one session: what it needs of the
 * session's and the chart's rows, read once, so no row's address is held
 * across a call into JavaScript (a table may grow meanwhile). */
typedef struct ta_napi_entry {
    ta_napi_datamodel *datamodel;
    ta_napi_chart_row chart; /* a copy: image, chart, count */
    napi_value scope, getters, setters;
    napi_value init; /* the session's params, or NULL */
    napi_handle_scope handles;
} ta_napi_entry;

static ta_status ta_napi_enter(const ta_datamodel_call *call, ta_napi_entry *entry) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)call->datamodel;
    napi_env env = datamodel->env;
    entry->datamodel = datamodel;
    entry->init = NULL;
    const ta_napi_session_row *session =
        (const ta_napi_session_row *)ta_napi_table_get(&datamodel->sessions, call->instance);
    const ta_napi_chart_row *chart =
        session ? (const ta_napi_chart_row *)ta_napi_table_get(&datamodel->charts, session->chart) : NULL;
    if (!chart || napi_open_handle_scope(env, &entry->handles) != napi_ok) return TA_ERROR_EXECUTION;
    entry->chart = *chart;
    if (napi_get_reference_value(env, session->scope, &entry->scope) != napi_ok ||
        napi_get_reference_value(env, chart->getters, &entry->getters) != napi_ok ||
        napi_get_reference_value(env, chart->setters, &entry->setters) != napi_ok ||
        (session->init && napi_get_reference_value(env, session->init, &entry->init) != napi_ok)) {
        napi_close_handle_scope(env, entry->handles);
        return TA_ERROR_EXECUTION;
    }
    datamodel->environment = call->environment;
    datamodel->chart = session->chart;
    return TA_OK;
}

static ta_status ta_napi_leave(ta_napi_entry *entry, ta_status status) {
    ta_napi_datamodel *datamodel = entry->datamodel;
    napi_value thrown;
    if (ta_napi_exception_pending(datamodel->env)) napi_get_and_clear_last_exception(datamodel->env, &thrown);
    datamodel->environment = NULL;
    datamodel->chart = 0;
    napi_close_handle_scope(datamodel->env, entry->handles);
    return status;
}

/* A napi call that failed: the expression threw. */
static ta_status ta_napi_executed(napi_status status) {
    return status == napi_ok ? TA_OK : TA_ERROR_EXECUTION;
}

static napi_value ta_napi_text(napi_env env, ta_string_view text) {
    napi_value value = NULL;
    napi_create_string_utf8(env, text.pointer ? text.pointer : "", text.length, &value);
    return value;
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
 * small XML DOM is the W3C suite's, M2), put together in a room of the
 * call's. */
static ta_status ta_napi_content(ta_napi_entry *entry, ta_string_view source, napi_value *out) {
    napi_env env = entry->datamodel->env;
    ta_napi_realm_call parse = {"JSON", "parse", ta_napi_text(env, source)};
    if (parse.argument && ta_napi_realm(env, &parse, out) == napi_ok) return TA_OK;
    napi_value thrown;
    napi_get_and_clear_last_exception(env, &thrown);
    char stack[256];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    if (!ta_napi_room_reserve(&room, (size_t)source.length + 1)) return TA_ERROR_NO_MEMORY;
    char *normal = room.pointer;
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
    ta_napi_room_free(&room);
    return status;
}

/* The function of expression `expression` in `functions` (the chart's
 * getters or setters), or NULL when it did not compile. */
static napi_value ta_napi_function(napi_env env, napi_value functions, ta_expression expression) {
    napi_value function;
    napi_valuetype type = napi_undefined;
    if (napi_get_element(env, functions, expression, &function) != napi_ok ||
        napi_typeof(env, function, &type) != napi_ok || type != napi_function)
        return NULL;
    return function;
}

/* The value of expression `expression` in the entry's session. */
static ta_status ta_napi_value_of(ta_napi_entry *entry, ta_expression expression, napi_value *out) {
    ta_napi_datamodel *datamodel = entry->datamodel;
    if (expression >= entry->chart.count) return TA_ERROR_EXECUTION;
    if (datamodel->environment) ta_datamodel_environment_charge(datamodel->environment, 1);
    ta_napi_expression found = ta_napi_chart_expression(&entry->chart, expression);
    if (found.kind == TA_EXPRESSION_CONTENT) return ta_napi_content(entry, found.source, out);
    napi_value function = ta_napi_function(datamodel->env, entry->getters, expression), undefined;
    if (!function) return TA_ERROR_EXECUTION;
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
    ta_string_view name = ta_napi_location_name(ta_napi_chart_expression(&entry->chart, location).source);
    if (!name.length || ta_napi_named(name, "_event") || ta_napi_named(name, "_sessionid") ||
        ta_napi_named(name, "_name") || ta_napi_named(name, "_ioprocessors") || ta_napi_named(name, "In"))
        return 0;
    napi_value key = ta_napi_text(env, name);
    bool own = false;
    return key && napi_has_own_property(env, entry->scope, key, &own) == napi_ok && own;
}

/* Assign `value` to location `location`. */
static ta_status ta_napi_assign_to(ta_napi_entry *entry, ta_expression location, napi_value value) {
    napi_env env = entry->datamodel->env;
    if (location >= entry->chart.count || !ta_napi_location_ok(entry, location)) return TA_ERROR_EXECUTION;
    napi_value function = ta_napi_function(env, entry->setters, location), undefined, result,
               argv[2] = {entry->scope, value};
    if (!function) return TA_ERROR_EXECUTION;
    napi_get_undefined(env, &undefined);
    return ta_napi_executed(napi_call_function(env, undefined, function, 2, argv, &result));
}

/* A string core reads back: a view of the datamodel's last output, valid
 * until the datamodel's next call (datamodel.h: "valid until the next call
 * on the same datamodel instance"). */
static ta_status ta_napi_string_out(ta_napi_entry *entry, napi_value value, ta_string_view *out) {
    ta_napi_texts texts = {&value, 1, out};
    entry->datamodel->output.length = 0;
    return ta_napi_values_texts(&entry->datamodel->output, &texts) == TA_OK ? TA_OK : TA_ERROR_EXECUTION;
}

/* ---- In() ---------------------------------------------------------------------------- */

/* In('id'): whether the evaluating session's configuration holds state id. */
static napi_value ta_napi_in(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], result = NULL;
    void *data = NULL;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, &data) != napi_ok) return NULL;
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    const ta_napi_chart_row *chart = (const ta_napi_chart_row *)ta_napi_table_get(&datamodel->charts, datamodel->chart);
    int in = 0;
    ta_string_view id;
    ta_napi_texts texts = {argv, 1, &id};
    char stack[128];
    ta_napi_room room;
    ta_napi_room_init(&room, env, (ta_buffer){stack, sizeof stack, 0});
    if (argc >= 1 && datamodel->environment && chart && ta_napi_values_texts(&room, &texts) == TA_OK) {
        uint32_t state = ta_image_state_by_name(chart->image, chart->chart, id);
        in = state != TA_NIL && ta_datamodel_environment_in(datamodel->environment, state);
    }
    ta_napi_room_free(&room);
    napi_get_boolean(env, in, &result);
    return result;
}

/* ---- payloads ------------------------------------------------------------------------- */

/* A new payload row holding `value` (one reference); 0 when out of memory. */
static ta_payload ta_napi_payload_add(ta_napi_datamodel *datamodel, napi_value value) {
    napi_env env = datamodel->env;
    napi_value holder;
    napi_ref reference = NULL;
    if (napi_create_array_with_length(env, 1, &holder) != napi_ok || napi_set_element(env, holder, 0, value) != napi_ok ||
        napi_create_reference(env, holder, 1, &reference) != napi_ok)
        return 0;
    uint64_t handle = ta_napi_table_add(&datamodel->payloads);
    ta_napi_payload_row *row = (ta_napi_payload_row *)ta_napi_table_get(&datamodel->payloads, handle);
    if (!row) {
        napi_delete_reference(env, reference);
        return 0;
    }
    row->holder = reference;
    return handle;
}

/* The value of payload `payload`; 0 when it is not a live one. */
static int ta_napi_payload_value(ta_napi_datamodel *datamodel, ta_payload payload, napi_value *out) {
    const ta_napi_payload_row *row = (const ta_napi_payload_row *)ta_napi_table_get(&datamodel->payloads, payload);
    napi_value holder;
    return row && napi_get_reference_value(datamodel->env, row->holder, &holder) == napi_ok &&
           napi_get_element(datamodel->env, holder, 0, out) == napi_ok;
}

/* The last reference: the row is freed and its napi_ref dropped once the
 * outer call into core returns (handles.c). */
static void ta_napi_payload_drop(ta_napi_datamodel *datamodel, ta_payload payload) {
    ta_napi_payload_row *row = (ta_napi_payload_row *)ta_napi_table_get(&datamodel->payloads, payload);
    if (!row) return;
    if (row->row.references > 1) {
        row->row.references--;
        return;
    }
    ta_napi_defer_drop(&datamodel->drops, row->holder);
    ta_napi_table_remove(&datamodel->payloads, payload);
}

/* ---- the vtable ------------------------------------------------------------------------- */

static ta_status ta_napi_chart_attach(void *data, const ta_datamodel_chart *chart, void **context) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    napi_handle_scope handles;
    if (napi_open_handle_scope(datamodel->env, &handles) != napi_ok) return TA_ERROR_NO_MEMORY;
    uint64_t handle = 0;
    ta_status status = ta_napi_chart_compile(datamodel, chart, &handle);
    napi_close_handle_scope(datamodel->env, handles);
    if (status == TA_OK) *context = (void *)(uintptr_t)handle;
    return status;
}

static void ta_napi_chart_detach(void *data, void *context) {
    ta_napi_chart_unuse((ta_napi_datamodel *)data, (uint64_t)(uintptr_t)context);
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

/* _ioprocessors: each processor by its type, and SCXML's and BasicHTTP's
 * by their short names, as quickjs has them (datamodel_ecmascript.c:
 * 1247-1267). */
static napi_value ta_napi_io_processors(napi_env env, const ta_datamodel_system_variables *variables) {
    napi_value processors;
    napi_create_object(env, &processors);
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        const ta_io_processor_entry *entry = &variables->io_processors[i];
        napi_value processor;
        napi_create_object(env, &processor);
        napi_set_named_property(env, processor, "location", ta_napi_text(env, entry->location));
        napi_set_property(env, processors, ta_napi_text(env, entry->type), processor);
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

/* A session's scope: the system variables, In(), and every <data> id
 * declared from the start (late binding: undefined until its init_data). */
static napi_value ta_napi_scope(ta_napi_datamodel *datamodel, const ta_datamodel_start *start) {
    napi_env env = datamodel->env;
    const ta_datamodel_system_variables *variables = start->system_variables;
    const ta_napi_chart_row *chart =
        (const ta_napi_chart_row *)ta_napi_table_get(&datamodel->charts, (uint64_t)(uintptr_t)start->chart_context);
    napi_value scope, undefined, in, function = ta_napi_helper(datamodel, "scope");
    napi_get_undefined(env, &undefined);
    if (!chart || !function || napi_call_function(env, undefined, function, 0, NULL, &scope) != napi_ok ||
        napi_get_reference_value(env, datamodel->in, &in) != napi_ok)
        return NULL;
    ta_napi_property properties[] = {
        {"_sessionid", ta_napi_text(env, variables->sessionid)},
        {"_name", ta_napi_text(env, variables->name)},
        {"_ioprocessors", ta_napi_io_processors(env, variables)},
        {"In", in},
    };
    for (size_t i = 0; i < sizeof properties / sizeof properties[0]; i++)
        ta_napi_define(env, scope, &properties[i]);
    uint32_t count = ta_image_data_count(chart->image, chart->chart);
    for (uint32_t i = 0; i < count; i++) {
        ta_data_info info;
        if (ta_image_data(chart->image, (ta_chart_item){chart->chart, i}, &info) == TA_OK)
            napi_set_property(env, scope, ta_napi_text(env, info.id), undefined);
    }
    return scope;
}

static ta_status ta_napi_create(void *data, const ta_datamodel_start *start, uint64_t *instance) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    napi_env env = datamodel->env;
    uint64_t chart = (uint64_t)(uintptr_t)start->chart_context;
    napi_handle_scope handles;
    if (napi_open_handle_scope(env, &handles) != napi_ok) return TA_ERROR_NO_MEMORY;
    napi_value scope = ta_napi_scope(datamodel, start), params;
    napi_ref scope_ref = NULL, init_ref = NULL;
    ta_status status = scope && napi_create_reference(env, scope, 1, &scope_ref) == napi_ok ? TA_OK : TA_ERROR_NO_MEMORY;
    if (status == TA_OK && start->init && ta_napi_payload_value(datamodel, start->init, &params))
        napi_create_reference(env, params, 1, &init_ref);
    if (start->init) ta_napi_payload_drop(datamodel, start->init); /* passed to us */
    uint64_t handle = status == TA_OK ? ta_napi_table_add(&datamodel->sessions) : 0;
    ta_napi_session_row *session = (ta_napi_session_row *)ta_napi_table_get(&datamodel->sessions, handle);
    ta_napi_chart_row *row = (ta_napi_chart_row *)ta_napi_table_get(&datamodel->charts, chart);
    napi_close_handle_scope(env, handles);
    if (!session || !row) {
        if (session) ta_napi_table_remove(&datamodel->sessions, handle);
        if (scope_ref) ta_napi_defer_drop(&datamodel->drops, scope_ref);
        if (init_ref) ta_napi_defer_drop(&datamodel->drops, init_ref);
        return TA_ERROR_NO_MEMORY;
    }
    session->scope = scope_ref;
    session->init = init_ref;
    session->chart = chart;
    row->row.references++; /* the session keeps its chart */
    *instance = handle;
    return TA_OK;
}

static void ta_napi_destroy(const ta_datamodel_call *call) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)call->datamodel;
    ta_napi_session_row *session = (ta_napi_session_row *)ta_napi_table_get(&datamodel->sessions, call->instance);
    if (!session) return;
    uint64_t chart = session->chart;
    ta_napi_defer_drop(&datamodel->drops, session->scope);
    if (session->init) ta_napi_defer_drop(&datamodel->drops, session->init);
    ta_napi_table_remove(&datamodel->sessions, call->instance);
    ta_napi_chart_unuse(datamodel, chart);
}

static ta_status ta_napi_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    napi_env env = entry.datamodel->env;
    ta_data_info info;
    if (ta_image_data(entry.chart.image, (ta_chart_item){entry.chart.chart, declaration}, &info) != TA_OK)
        return ta_napi_leave(&entry, TA_ERROR_EXECUTION);
    napi_value key = ta_napi_text(env, info.id), value;
    napi_get_undefined(env, &value);
    bool has = false;
    /* the spawn's or invoke's params override <data> of the same name */
    if (entry.init && napi_has_property(env, entry.init, key, &has) == napi_ok && has) {
        napi_get_property(env, entry.init, key, &value);
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
    napi_value event, undefined, data, type;
    napi_get_undefined(env, &undefined);
    napi_create_object(env, &event);
    napi_set_named_property(env, event, "name", ta_napi_text(env, view->name));
    napi_create_string_utf8(env, types[view->type % 3], NAPI_AUTO_LENGTH, &type);
    napi_set_named_property(env, event, "type", type);
    const ta_string_view *texts[] = {&view->sendid, &view->origin, &view->origintype, &view->invokeid};
    static const char *const names[] = {"sendid", "origin", "origintype", "invokeid"};
    for (size_t i = 0; i < 4; i++)
        napi_set_named_property(env, event, names[i], texts[i]->length ? ta_napi_text(env, *texts[i]) : undefined);
    if (!ta_napi_payload_value(datamodel, view->data, &data)) data = undefined;
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
        napi_valuetype type = napi_undefined, text_type = napi_undefined;
        napi_typeof(env, value, &type);
        ta_napi_realm_call stringify = {"JSON", "stringify", value};
        if (!(type == napi_object && ta_napi_realm(env, &stringify, &text) == napi_ok &&
              napi_typeof(env, text, &text_type) == napi_ok && text_type == napi_string)) {
            napi_value thrown;
            if (ta_napi_exception_pending(env)) napi_get_and_clear_last_exception(env, &thrown);
            status = ta_napi_executed(napi_coerce_to_string(env, value, &text));
        }
    }
    if (status == TA_OK) status = ta_napi_string_out(&entry, text, out);
    return ta_napi_leave(&entry, status);
}

/* <foreach>: a shallow copy of the array's items, a row of the iterations
 * table until foreach_end. */
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
    napi_ref items = NULL;
    if (status == TA_OK) status = ta_napi_executed(napi_create_reference(env, copy, 1, &items));
    uint64_t handle = status == TA_OK ? ta_napi_table_add(&datamodel->iterations) : 0;
    ta_napi_iteration_row *row = (ta_napi_iteration_row *)ta_napi_table_get(&datamodel->iterations, handle);
    if (status == TA_OK && !row) {
        napi_delete_reference(env, items);
        status = TA_ERROR_NO_MEMORY;
    }
    if (status == TA_OK) {
        row->items = items;
        loop->iter = handle;
        loop->n = length;
    }
    return ta_napi_leave(&entry, status);
}

/* <foreach> may introduce its item and index: a plain name not declared yet
 * is declared in the session (quickjs's declare_if_missing). */
static void ta_napi_declare(ta_napi_entry *entry, ta_expression location) {
    napi_env env = entry->datamodel->env;
    ta_string_view source = ta_napi_chart_expression(&entry->chart, location).source;
    ta_string_view name = ta_napi_location_name(source);
    uint32_t end = (uint32_t)(name.pointer - source.pointer) + name.length;
    while (end < source.length && (source.pointer[end] == ' ' || source.pointer[end] == '\t' || source.pointer[end] == '\n'))
        end++;
    if (!name.length || end != source.length) return;
    napi_value key = ta_napi_text(env, name), undefined;
    bool own = false;
    if (!key || napi_has_own_property(env, entry->scope, key, &own) != napi_ok || own) return;
    napi_get_undefined(env, &undefined);
    napi_set_property(env, entry->scope, key, undefined);
}

static ta_status ta_napi_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_napi_entry entry;
    ta_status status = ta_napi_enter(call, &entry);
    if (status != TA_OK) return status;
    ta_napi_datamodel *datamodel = entry.datamodel;
    napi_env env = datamodel->env;
    const ta_napi_iteration_row *row = (const ta_napi_iteration_row *)ta_napi_table_get(&datamodel->iterations, loop->iter);
    napi_value items, item, index;
    if (!row || napi_get_reference_value(env, row->items, &items) != napi_ok ||
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
    ta_napi_iteration_row *row = (ta_napi_iteration_row *)ta_napi_table_get(&datamodel->iterations, loop->iter);
    if (!row) return;
    ta_napi_defer_drop(&datamodel->drops, row->items);
    ta_napi_table_remove(&datamodel->iterations, loop->iter);
}

/* A payload holding a copy of `value` by the value table, without CBOR:
 * what crosses to another actor shares nothing with the sender
 * (bindings.md, section 8). A value outside the table fails it. */
static ta_status ta_napi_payload_copy(ta_napi_datamodel *datamodel, napi_value value, ta_payload *out) {
    napi_value copy;
    ta_status status = ta_napi_values_copy(datamodel->env, value, &copy);
    if (status != TA_OK) return status;
    *out = ta_napi_payload_add(datamodel, copy);
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
                    env, value, ta_napi_text(env, ta_napi_chart_expression(&entry.chart, capture->namelist[i]).source),
                    item));
        }
        for (uint32_t i = 0; i < capture->param_count && status == TA_OK; i++) {
            napi_value item;
            status = ta_napi_value_of(&entry, capture->params[2 * i + 1], &item);
            if (status == TA_OK)
                status = ta_napi_executed(napi_set_property(
                    env, value, ta_napi_text(env, ta_image_string(entry.chart.image, capture->params[2 * i])), item));
        }
    }
    if (status == TA_OK) {
        status = ta_napi_payload_copy(entry.datamodel, value, out);
        if (status == TA_ERROR_INVALID) status = TA_ERROR_EXECUTION;
    }
    return ta_napi_leave(&entry, status);
}

static void ta_napi_retain(void *data, ta_payload payload) {
    ta_napi_payload_row *row = (ta_napi_payload_row *)ta_napi_table_get(&((ta_napi_datamodel *)data)->payloads, payload);
    if (row) row->row.references++;
}

static void ta_napi_release(void *data, ta_payload payload) {
    ta_napi_payload_drop((ta_napi_datamodel *)data, payload);
}

/* The payload as CBOR, written once into the datamodel's last output (a
 * value that outgrew it is copied into it, which grows to the largest):
 * a view until the datamodel's next call (datamodel.h). Core calls it
 * outside a run with the run's rules, so a toJSON that calls the Host
 * back cannot reach this datamodel meanwhile (ADR 0010, decision 3). */
static ta_status ta_napi_to_wire(void *data, ta_payload payload, ta_bytes *out) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    napi_handle_scope handles;
    *out = (ta_bytes){NULL, 0};
    if (napi_open_handle_scope(datamodel->env, &handles) != napi_ok) return TA_ERROR_NO_MEMORY;
    napi_value value;
    ta_status status = TA_ERROR_INVALID;
    if (ta_napi_payload_value(datamodel, payload, &value)) {
        ta_napi_encoder encoder;
        ta_napi_encoder_init(&encoder, datamodel->env, &datamodel->output);
        ta_napi_encoder_value(&encoder, value);
        status = ta_napi_encoder_keep(&encoder, &datamodel->output, out);
        ta_napi_encoder_free(&encoder);
    }
    napi_value thrown;
    if (ta_napi_exception_pending(datamodel->env)) napi_get_and_clear_last_exception(datamodel->env, &thrown);
    napi_close_handle_scope(datamodel->env, handles);
    return status;
}

static ta_status ta_napi_from_wire(void *data, ta_bytes cbor, ta_payload *out) {
    ta_napi_datamodel *datamodel = (ta_napi_datamodel *)data;
    napi_handle_scope handles;
    if (napi_open_handle_scope(datamodel->env, &handles) != napi_ok) return TA_ERROR_NO_MEMORY;
    napi_value value;
    ta_status status = ta_napi_values_decode(datamodel->env, cbor, &value);
    if (status == TA_OK) {
        *out = ta_napi_payload_add(datamodel, value);
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
    if (!key || napi_has_own_property(env, entry.scope, key, &own) != napi_ok || !own ||
        napi_get_property(env, entry.scope, key, &value) != napi_ok)
        return ta_napi_leave(&entry, TA_ERROR_INVALID);
    *out = ta_napi_payload_add(entry.datamodel, value);
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

int ta_napi_datamodel_init(ta_napi_datamodel *datamodel, napi_env env) {
    memset(datamodel, 0, sizeof *datamodel);
    datamodel->env = env;
    datamodel->drops.env = env;
    ta_napi_room_init(&datamodel->output, env, (ta_buffer){NULL, 0, 0});
    datamodel->payloads.row_size = sizeof(ta_napi_payload_row);
    datamodel->sessions.row_size = sizeof(ta_napi_session_row);
    datamodel->charts.row_size = sizeof(ta_napi_chart_row);
    datamodel->iterations.row_size = sizeof(ta_napi_iteration_row);
    datamodel->live = 1;
    napi_value source, helpers, in;
    if (napi_create_string_utf8(env, ta_napi_helpers_source, sizeof ta_napi_helpers_source - 1, &source) != napi_ok ||
        napi_run_script(env, source, &helpers) != napi_ok ||
        napi_create_reference(env, helpers, 1, &datamodel->helpers) != napi_ok ||
        napi_create_function(env, "In", NAPI_AUTO_LENGTH, ta_napi_in, datamodel, &in) != napi_ok ||
        napi_create_reference(env, in, 1, &datamodel->in) != napi_ok) {
        ta_napi_datamodel_release(datamodel);
        return 0;
    }
    return 1;
}

/* The references of one table's live rows (at most two per row, by their
 * offsets; `second` 0 for none). */
typedef struct ta_napi_table_drops {
    ta_napi_table *table;
    size_t first, second;
} ta_napi_table_drops;

static void ta_napi_drop_rows(ta_napi_datamodel *datamodel, const ta_napi_table_drops *rows) {
    for (uint32_t i = 0; i < rows->table->count; i++) {
        char *row = (char *)ta_napi_table_row(rows->table, i);
        if (!row) continue;
        napi_ref first = *(napi_ref *)(row + rows->first);
        napi_ref second = rows->second ? *(napi_ref *)(row + rows->second) : NULL;
        if (first) ta_napi_defer_drop(&datamodel->drops, first);
        if (second) ta_napi_defer_drop(&datamodel->drops, second);
    }
    ta_napi_table_free(rows->table);
}

/* After the host is freed (core destroyed its sessions and detached its
 * charts): what is left is dropped, and the tables go, once. */
void ta_napi_datamodel_release(ta_napi_datamodel *datamodel) {
    if (!datamodel->live) return;
    datamodel->live = 0;
    const ta_napi_table_drops tables[] = {
        {&datamodel->payloads, offsetof(ta_napi_payload_row, holder), 0},
        {&datamodel->sessions, offsetof(ta_napi_session_row, scope), offsetof(ta_napi_session_row, init)},
        {&datamodel->charts, offsetof(ta_napi_chart_row, getters), offsetof(ta_napi_chart_row, setters)},
        {&datamodel->iterations, offsetof(ta_napi_iteration_row, items), 0},
    };
    for (size_t i = 0; i < sizeof tables / sizeof tables[0]; i++)
        ta_napi_drop_rows(datamodel, &tables[i]);
    if (datamodel->helpers) ta_napi_defer_drop(&datamodel->drops, datamodel->helpers);
    if (datamodel->in) ta_napi_defer_drop(&datamodel->drops, datamodel->in);
    datamodel->helpers = datamodel->in = NULL;
    ta_napi_drops_free(&datamodel->drops);
    ta_napi_room_free(&datamodel->output);
}

uint32_t ta_napi_datamodel_drain(ta_napi_datamodel *datamodel) {
    return datamodel->live ? ta_napi_drops_drain(&datamodel->drops) : 0;
}

ta_status ta_napi_datamodel_payload(ta_napi_datamodel *datamodel, napi_value value, ta_payload *out) {
    return ta_napi_payload_copy(datamodel, value, out);
}

ta_status ta_napi_datamodel_value(ta_napi_datamodel *datamodel, ta_payload payload, napi_value *out) {
    napi_value value;
    if (!ta_napi_payload_value(datamodel, payload, &value)) return napi_get_undefined(datamodel->env, out) == napi_ok ? TA_OK : TA_ERROR_NO_MEMORY;
    return ta_napi_values_copy(datamodel->env, value, out);
}

ta_datamodel_binding ta_napi_datamodel_binding(ta_napi_datamodel *datamodel) {
    ta_datamodel_binding binding = {&ta_napi_vtable, datamodel};
    return binding;
}
