/* The native "ecmascript" datamodel's vtable (tinyactors/datamodel.h;
 * AGENTS.md, "The native "ecmascript" datamodel"; structured as
 * tinyactors-cpython/src/datamodel/datamodel.c, sharing no code or text with
 * quickjs's datamodel). A chart is a row holding its record (datamodel.h);
 * a session a row holding its scope, a scope made by its chart's
 * constructor in the shared context, or, for a chart whose scripts
 * declare, the record of a context of its own. Every entry opens a handle
 * scope of its own (a run is one native call: handles would pile up until
 * it returns); an expression that throws is TA_ERROR_EXECUTION. Values
 * cross by copy, made with the receiving context's constructors (the
 * kit's `copy`); `_event` is a frozen object kept from bind_event to the
 * next one or destroy, its data the payload row's value itself in the
 * shared context. */
#include "datamodel.h"

#include <string.h>

/* ---- an entry ----------------------------------------------------------------- */

/* An entry's frame: its handle scope, and the evaluating datamodel In()
 * reads (restored at the end, for an entry inside another). */
typedef struct ta_napi_frame {
    ta_napi_native *native;
    napi_handle_scope scope;
    ta_napi_native *evaluating;
    ta_datamodel_environment *environment;
} ta_napi_frame;

static bool ta_napi_frame_open(ta_napi_native *native, ta_datamodel_environment *environment, ta_napi_frame *frame) {
    ta_napi_instance *instance = ta_napi_instance_of(native->host->env);
    frame->native = native;
    if (instance == NULL || napi_open_handle_scope(native->host->env, &frame->scope) != napi_ok) {
        napi_value ignored;
        napi_get_and_clear_last_exception(native->host->env, &ignored);
        return false;
    }
    frame->evaluating = instance->evaluating;
    frame->environment = native->environment;
    instance->evaluating = native;
    native->environment = environment;
    return true;
}

static void ta_napi_frame_close(ta_napi_frame *frame) {
    napi_env env = frame->native->host->env;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (instance != NULL) instance->evaluating = frame->evaluating;
    frame->native->environment = frame->environment;
    napi_close_handle_scope(env, frame->scope);
}

/* The exception pending, cleared: the entry's error.execution. */
static ta_status ta_napi_failed(napi_env env) {
    napi_value ignored;
    napi_get_and_clear_last_exception(env, &ignored);
    return TA_ERROR_EXECUTION;
}

/* The evaluation of `call`'s session (an open frame), or false. */
static bool ta_napi_evaluation_of(const ta_datamodel_call *call, ta_napi_evaluation *evaluation) {
    ta_napi_native *native = (ta_napi_native *)call->datamodel;
    napi_env env = native->host->env;
    memset(evaluation, 0, sizeof *evaluation);
    evaluation->native = native;
    evaluation->env = env;
    ta_napi_session_row *session = (ta_napi_session_row *)ta_table_get(&native->host->sessions, call->instance);
    ta_napi_chart_row *chart = session != NULL ? (ta_napi_chart_row *)ta_table_get(&native->host->charts, session->chart) : NULL;
    if (chart == NULL) return false;
    evaluation->row = *chart;
    evaluation->chart = &evaluation->row;
    napi_value held = ta_napi_native_get(native, session->scope), shared;
    if (held == NULL || (evaluation->record = ta_napi_native_get(native, chart->functions)) == NULL) return false;
    if (chart->flags & TA_NAPI_CHART_DECLARES) {
        evaluation->own = held;
        if (napi_get_element(env, held, TA_NAPI_OWN_KIT, &evaluation->kit) != napi_ok ||
            napi_get_named_property(env, evaluation->kit, "global", &evaluation->scope) != napi_ok)
            return false;
    } else {
        evaluation->scope = held;
        if ((shared = ta_napi_realm_shared(native)) == NULL ||
            napi_get_element(env, shared, TA_NAPI_REALM_KIT, &evaluation->kit) != napi_ok ||
            napi_get_element(env, shared, TA_NAPI_REALM_CONTEXT, &evaluation->context) != napi_ok)
            return false;
    }
    if (call->environment != NULL) ta_datamodel_environment_charge(call->environment, 1); /* fuel once per evaluation */
    return true;
}

/* An entry over a session: its frame and its evaluation. */
typedef struct ta_napi_entry {
    ta_napi_frame frame;
    ta_napi_evaluation evaluation;
} ta_napi_entry;

static bool ta_napi_enter_entry(const ta_datamodel_call *call, ta_napi_entry *entry) {
    if (!ta_napi_frame_open((ta_napi_native *)call->datamodel, call->environment, &entry->frame)) return false;
    if (ta_napi_evaluation_of(call, &entry->evaluation)) return true;
    napi_value ignored;
    napi_get_and_clear_last_exception(entry->frame.native->host->env, &ignored);
    ta_napi_frame_close(&entry->frame);
    return false;
}

/* A string the datamodel answers (eval_string, log): its UTF-8 in the
 * output, valid until the datamodel's next call. */
static ta_status ta_napi_string_out(ta_napi_native *native, napi_value value, ta_string_view *out) {
    napi_env env = native->host->env;
    ta_napi_room room = {&native->output, &native->host->allocator, {NULL, 0}};
    napi_value text;
    if (value == NULL || napi_coerce_to_string(env, value, &text) != napi_ok || !ta_napi_text_kept(env, text, &room))
        return ta_napi_failed(env);
    *out = room.view;
    return TA_OK;
}

/* ---- charts -------------------------------------------------------------------- */

/* The names a chart's scopes have from the start: its <data> ids, then the
 * locations its <foreach>es bind as item and index (an identifier the
 * session does not declare otherwise: SCXML 4.6). */
static napi_value ta_napi_chart_names(napi_env env, const ta_datamodel_chart *chart) {
    napi_value names, name;
    uint32_t count = 0;
    if (napi_create_array(env, &names) != napi_ok) return NULL;
    uint32_t data = ta_image_data_count(chart->image, chart->chart);
    for (uint32_t i = 0; i < data; i++) {
        ta_data_info info;
        if (ta_image_data(chart->image, (ta_chart_item){chart->chart, i}, &info) != TA_OK) continue;
        if (napi_create_string_utf8(env, info.id.pointer, info.id.length, &name) != napi_ok ||
            napi_set_element(env, names, count++, name) != napi_ok)
            return NULL;
    }
    uint32_t expressions = ta_image_expression_count(chart->image, chart->chart);
    for (uint32_t e = 0; e < expressions; e++) {
        ta_expression_info info;
        if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, e}, &info) != TA_OK ||
            info.kind != TA_EXPRESSION_ITEM)
            continue;
        ta_string_view item = info.source;
        while (item.length > 0 && (item.pointer[0] == ' ' || item.pointer[0] == '\t' || item.pointer[0] == '\n' || item.pointer[0] == '\r')) {
            item.pointer++;
            item.length--;
        }
        while (item.length > 0 && (item.pointer[item.length - 1] == ' ' || item.pointer[item.length - 1] == '\t' ||
                                   item.pointer[item.length - 1] == '\n' || item.pointer[item.length - 1] == '\r'))
            item.length--;
        bool identifier = item.length > 0;
        for (uint32_t i = 0; i < item.length && identifier; i++) {
            char c = item.pointer[i];
            identifier = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (i > 0 && c >= '0' && c <= '9');
        }
        bool known = false;
        if (!identifier || napi_create_string_utf8(env, item.pointer, item.length, &name) != napi_ok) continue;
        for (uint32_t i = 0; i < count && !known; i++) {
            napi_value other;
            if (napi_get_element(env, names, i, &other) != napi_ok || napi_strict_equals(env, name, other, &known) != napi_ok)
                return NULL;
        }
        if (!known && napi_set_element(env, names, count++, name) != napi_ok) return NULL;
    }
    return names;
}

/* A chart's record (datamodel.h): its scopes' constructor in the shared
 * context (none for a declaring chart), its caches, its names. */
typedef struct ta_napi_record_of {
    const ta_datamodel_chart *chart;
    const ta_napi_chart_of *declaring;
    napi_value names;
} ta_napi_record_of;

static napi_value ta_napi_chart_record(ta_napi_native *native, const ta_napi_record_of *of) {
    napi_env env = native->host->env;
    napi_value record, slot, shape, shared, kit;
    if (napi_create_array_with_length(env, TA_NAPI_CHART_RECORD, &record) != napi_ok) return NULL;
    bool declares = of->declaring->declares;
    if (napi_get_undefined(env, &shape) != napi_ok) return NULL;
    if (!declares) {
        if ((shared = ta_napi_realm_shared(native)) == NULL || napi_get_element(env, shared, TA_NAPI_REALM_KIT, &kit) != napi_ok ||
            (shape = ta_napi_kit(env, kit, &(ta_napi_kit_call){"shape", 1, {of->names}})) == NULL)
            return NULL;
    }
    if (napi_set_element(env, record, TA_NAPI_CHART_SHAPE, shape) != napi_ok) return NULL;
    for (uint32_t i = TA_NAPI_CHART_GETTERS; i < TA_NAPI_CHART_HOISTS; i++)
        if (napi_create_array(env, &slot) != napi_ok || napi_set_element(env, record, i, slot) != napi_ok) return NULL;
    /* a declaring chart's names, for each session's context: kept as the
     * shape; its scripts' declarations, made again in a woken session's */
    if (declares && (napi_set_element(env, record, TA_NAPI_CHART_SHAPE, of->names) != napi_ok ||
                     napi_set_element(env, record, TA_NAPI_CHART_HOISTS, of->declaring->hoists) != napi_ok ||
                     napi_set_element(env, record, TA_NAPI_CHART_FUNCTIONS, of->declaring->functions) != napi_ok))
        return NULL;
    return record;
}

static ta_status ta_napi_chart_attach(void *datamodel, const ta_datamodel_chart *chart, void **context) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    napi_env env = native->host->env;
    ta_napi_frame frame;
    if (!ta_napi_frame_open(native, NULL, &frame)) return TA_ERROR_INVALID;
    ta_status status = TA_OK;
    ta_napi_chart_of declaring = {chart->image, chart->chart, false, false, NULL, NULL};
    napi_value names = NULL, record = NULL;
    napi_ref reference = NULL;
    if (!ta_napi_realm_declares(native, &declaring) || (names = ta_napi_chart_names(env, chart)) == NULL ||
        (record = ta_napi_chart_record(native, &(ta_napi_record_of){chart, &declaring, names})) == NULL ||
        napi_create_reference(env, record, 1, &reference) != napi_ok)
        status = (ta_napi_failed(env), TA_ERROR_INVALID); /* a chart this datamodel cannot run */
    uint64_t handle = 0;
    ta_napi_chart_row *row = status == TA_OK ? (ta_napi_chart_row *)ta_table_add(&native->host->charts, &handle) : NULL;
    if (status == TA_OK && row == NULL) {
        napi_delete_reference(env, reference);
        status = TA_ERROR_NO_MEMORY;
    }
    if (row != NULL) {
        uint32_t flags = (declaring.declares ? TA_NAPI_CHART_DECLARES : 0u) | (declaring.lexical ? TA_NAPI_CHART_LEXICAL : 0u);
        *row = (ta_napi_chart_row){reference, chart->image, chart->chart, flags};
        *context = (void *)(uintptr_t)handle; /* the row's handle, never a pointer */
    }
    ta_napi_frame_close(&frame);
    return status;
}

static void ta_napi_chart_detach(void *datamodel, void *context) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    uint64_t handle = (uint64_t)(uintptr_t)context;
    ta_napi_chart_row *row = (ta_napi_chart_row *)ta_table_get(&native->host->charts, handle);
    if (row == NULL) return;
    napi_ref reference = row->functions;
    ta_table_remove(&native->host->charts, handle);
    ta_napi_native_drop(native, reference);
}

/* ---- sessions ------------------------------------------------------------------- */

/* The system variables of a session, its link (realm.js, `system`), made
 * once at create in the kit's context: core's views end with the call. */
static napi_value ta_napi_system(napi_env env, napi_value kit, const ta_datamodel_system_variables *variables) {
    napi_value sessionid, name, processors, entry, type, location;
    if (napi_create_string_utf8(env, variables->sessionid.pointer, variables->sessionid.length, &sessionid) != napi_ok ||
        napi_create_string_utf8(env, variables->name.pointer != NULL ? variables->name.pointer : "", variables->name.length,
                                &name) != napi_ok ||
        napi_create_array_with_length(env, variables->io_processor_count, &processors) != napi_ok)
        return NULL;
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        const ta_io_processor_entry *processor = &variables->io_processors[i];
        if (napi_create_string_utf8(env, processor->type.pointer, processor->type.length, &type) != napi_ok ||
            napi_create_string_utf8(env, processor->location.pointer, processor->location.length, &location) != napi_ok ||
            napi_create_array_with_length(env, 2, &entry) != napi_ok || napi_set_element(env, entry, 0, type) != napi_ok ||
            napi_set_element(env, entry, 1, location) != napi_ok || napi_set_element(env, processors, i, entry) != napi_ok)
            return NULL;
    }
    return ta_napi_kit(env, kit, &(ta_napi_kit_call){"system", 3, {sessionid, name, processors}});
}

/* The strings of _event the datamodel keeps: the three types, then the
 * session's I/O processors' types, which an event names by index (every
 * session of a host has the same ones). */
static bool ta_napi_keep_strings(ta_napi_native *native, const ta_datamodel_system_variables *variables) {
    if (native->strings != NULL || variables == NULL) return true;
    napi_env env = native->host->env;
    static const char *const types[] = {"platform", "internal", "external"};
    napi_value strings, string;
    if (napi_create_array(env, &strings) != napi_ok) return false;
    for (uint32_t i = 0; i < 3; i++)
        if (napi_create_string_utf8(env, types[i], NAPI_AUTO_LENGTH, &string) != napi_ok ||
            napi_set_element(env, strings, i, string) != napi_ok)
            return false;
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        ta_string_view type = variables->io_processors[i].type;
        if (napi_create_string_utf8(env, type.pointer, type.length, &string) != napi_ok ||
            napi_set_element(env, strings, 3 + i, string) != napi_ok)
            return false;
    }
    return napi_create_reference(env, strings, 1, &native->strings) == napi_ok;
}

/* A session's scope (shared chart) or the record of its own context
 * (declaring chart), with its link and its init params. */
typedef struct ta_napi_start_of {
    const ta_napi_chart_row *chart;
    const ta_datamodel_system_variables *variables;
    napi_value init; /* the params' value (of the shared context), or undefined */
} ta_napi_start_of;

static napi_value ta_napi_session_new(ta_napi_native *native, const ta_napi_start_of *start) {
    napi_env env = native->host->env;
    napi_value record = ta_napi_native_get(native, start->chart->functions), shape, kit, link, init = start->init, made;
    if (record == NULL || napi_get_element(env, record, TA_NAPI_CHART_SHAPE, &shape) != napi_ok) return NULL;
    if (!(start->chart->flags & TA_NAPI_CHART_DECLARES)) {
        napi_value shared = ta_napi_realm_shared(native);
        if (shared == NULL || napi_get_element(env, shared, TA_NAPI_REALM_KIT, &kit) != napi_ok ||
            (link = ta_napi_system(env, kit, start->variables)) == NULL ||
            napi_set_named_property(env, link, "init", init) != napi_ok || napi_new_instance(env, shape, 1, &link, &made) != napi_ok)
            return NULL;
        return made;
    }
    napi_value own = ta_napi_realm_new(native), slot, global;
    if (own == NULL || napi_get_element(env, own, TA_NAPI_REALM_KIT, &kit) != napi_ok ||
        (link = ta_napi_system(env, kit, start->variables)) == NULL)
        return NULL;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, init, &type) != napi_ok) return NULL;
    if (type != napi_undefined && (init = ta_napi_kit(env, kit, &(ta_napi_kit_call){"copy", 1, {init}})) == NULL) return NULL;
    if (napi_set_named_property(env, link, "init", init) != napi_ok ||
        (global = ta_napi_kit(env, kit, &(ta_napi_kit_call){"declare", 2, {shape, link}})) == NULL)
        return NULL;
    for (uint32_t i = TA_NAPI_OWN_GETTERS; i < TA_NAPI_OWN_RECORD; i++)
        if (napi_create_array(env, &slot) != napi_ok || napi_set_element(env, own, i, slot) != napi_ok) return NULL;
    return own;
}

static ta_status ta_napi_create(void *datamodel, const ta_datamodel_start *start, uint64_t *instance) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    napi_env env = native->host->env;
    ta_napi_frame frame;
    if (!ta_napi_frame_open(native, NULL, &frame)) {
        ta_napi_native_release(native, start->init); /* the init payload's reference is ours */
        return TA_ERROR_NO_MEMORY;
    }
    napi_value init = ta_napi_native_value(native, start->init);
    ta_napi_native_release(native, start->init);
    uint64_t chart_handle = (uint64_t)(uintptr_t)start->chart_context;
    ta_napi_chart_row *chart = (ta_napi_chart_row *)ta_table_get(&native->host->charts, chart_handle);
    ta_status status = TA_OK;
    napi_value made = NULL;
    napi_ref reference = NULL;
    if (chart == NULL || !ta_napi_keep_strings(native, start->system_variables) ||
        (made = ta_napi_session_new(native, &(ta_napi_start_of){chart, start->system_variables, init})) == NULL ||
        napi_create_reference(env, made, 1, &reference) != napi_ok)
        status = ta_napi_failed(env);
    uint64_t handle = 0;
    ta_napi_session_row *row = status == TA_OK ? (ta_napi_session_row *)ta_table_add(&native->host->sessions, &handle) : NULL;
    if (status == TA_OK && row == NULL) {
        napi_delete_reference(env, reference);
        status = TA_ERROR_NO_MEMORY;
    }
    if (row != NULL) {
        *row = (ta_napi_session_row){reference, chart_handle, TA_NIL, TA_NIL, TA_NIL};
        *instance = handle;
    }
    ta_napi_frame_close(&frame);
    return status;
}

static void ta_napi_destroy(const ta_datamodel_call *call) {
    ta_napi_native *native = (ta_napi_native *)call->datamodel;
    ta_napi_session_row *row = (ta_napi_session_row *)ta_table_get(&native->host->sessions, call->instance);
    if (row == NULL) return;
    napi_ref reference = row->scope;
    ta_table_remove(&native->host->sessions, call->instance);
    ta_napi_native_drop(native, reference);
}

/* The session's link: its system variables and params. */
static napi_value ta_napi_link(const ta_napi_evaluation *evaluation) {
    napi_value symbol, link;
    if (napi_get_named_property(evaluation->env, evaluation->kit, "LINK", &symbol) != napi_ok ||
        napi_get_property(evaluation->env, evaluation->scope, symbol, &link) != napi_ok)
        return NULL;
    return link;
}

static ta_status ta_napi_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    ta_napi_evaluation *evaluation = &entry.evaluation;
    napi_env env = evaluation->env;
    ta_data_info info;
    ta_status status = TA_OK;
    napi_value id, value = NULL, link, init, undefined;
    napi_get_undefined(env, &undefined);
    if (ta_image_data(evaluation->row.image, (ta_chart_item){evaluation->row.chart, declaration}, &info) != TA_OK ||
        napi_create_string_utf8(env, info.id.pointer, info.id.length, &id) != napi_ok) {
        ta_napi_frame_close(&entry.frame);
        return ta_napi_failed(env);
    }
    /* the spawn's or invoke's params override the <data> of their names */
    napi_valuetype type = napi_undefined;
    bool given = false;
    if ((link = ta_napi_link(evaluation)) != NULL && napi_get_named_property(env, link, "init", &init) == napi_ok &&
        napi_typeof(env, init, &type) == napi_ok && type == napi_object &&
        napi_has_own_property(env, init, id, &given) == napi_ok && given)
        napi_get_property(env, init, id, &value);
    if (value == NULL && info.expression != TA_NIL && (value = ta_napi_evaluate(evaluation, info.expression)) == NULL)
        status = ta_napi_failed(env);
    if (napi_set_property(env, evaluation->scope, id, value != NULL ? value : undefined) != napi_ok && status == TA_OK)
        status = ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

/* ---- _event --------------------------------------------------------------------- */

/* The string of `text` kept at `index` of a chart's list (its names,
 * invoke ids or origins): the one there when its text is the same, else a
 * new one, kept there when the place was empty. A text that differs (an id
 * generated per execution) gets a string of its own. */
typedef struct ta_napi_kept_at {
    napi_value list;
    uint32_t index;
} ta_napi_kept_at;

static napi_value ta_napi_kept(napi_env env, const ta_napi_kept_at *at, ta_string_view text) {
    napi_value made = NULL, kept;
    napi_valuetype type = napi_undefined;
    if (at->list != NULL && at->index != TA_NIL && napi_get_element(env, at->list, at->index, &kept) == napi_ok &&
        napi_typeof(env, kept, &type) == napi_ok && type == napi_string) {
        char room[256];
        size_t length = 0;
        if (text.length < sizeof room && napi_get_value_string_utf8(env, kept, room, sizeof room, &length) == napi_ok &&
            length == text.length && memcmp(room, text.pointer, length) == 0)
            return kept; /* the chart's string for this text */
    }
    if (napi_create_string_utf8(env, text.pointer, text.length, &made) != napi_ok) return NULL;
    if (at->list != NULL && at->index != TA_NIL && type == napi_undefined)
        napi_set_element(env, at->list, at->index, made); /* the first: kept */
    return made;
}

/* Optional text: undefined when empty. */
static napi_value ta_napi_optional(napi_env env, ta_string_view text) {
    napi_value value = NULL;
    if (text.length == 0) napi_get_undefined(env, &value);
    else napi_create_string_utf8(env, text.pointer, text.length, &value);
    return value;
}

static ta_status ta_napi_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    ta_napi_evaluation *evaluation = &entry.evaluation;
    ta_napi_native *native = evaluation->native;
    napi_env env = evaluation->env;
    napi_value fields, names, invokeids, origins, strings, field, event, link, undefined;
    napi_get_undefined(env, &undefined);
    bool ok = napi_create_array_with_length(env, 8, &fields) == napi_ok &&
              napi_get_element(env, evaluation->record, TA_NAPI_CHART_NAMES, &names) == napi_ok &&
              napi_get_element(env, evaluation->record, TA_NAPI_CHART_INVOKEIDS, &invokeids) == napi_ok &&
              napi_get_element(env, evaluation->record, TA_NAPI_CHART_ORIGINS, &origins) == napi_ok &&
              (strings = ta_napi_native_get(native, native->strings)) != NULL;
    /* name, type, sendid, origin, origintype, invokeid, data, raw */
    ok = ok && (field = ta_napi_kept(env, &(ta_napi_kept_at){names, view->name_id}, view->name)) != NULL &&
         napi_set_element(env, fields, 0, field) == napi_ok &&
         napi_get_element(env, strings, view->type % 3, &field) == napi_ok && napi_set_element(env, fields, 1, field) == napi_ok &&
         napi_set_element(env, fields, 2, ta_napi_optional(env, view->sendid)) == napi_ok;
    field = view->origin.length ? ta_napi_kept(env, &(ta_napi_kept_at){origins, view->invoke_slot}, view->origin) : undefined;
    ok = ok && field != NULL && napi_set_element(env, fields, 3, field) == napi_ok;
    /* the origintype: the I/O processor's type the datamodel keeps, when it is that one */
    field = view->origintype.length == 0 ? undefined
            : view->origintype_id != TA_NIL
                ? ta_napi_kept(env, &(ta_napi_kept_at){strings, 3 + view->origintype_id}, view->origintype)
                : ta_napi_optional(env, view->origintype);
    ok = ok && field != NULL && napi_set_element(env, fields, 4, field) == napi_ok;
    field = view->invokeid.length ? ta_napi_kept(env, &(ta_napi_kept_at){invokeids, view->invoke_slot}, view->invokeid)
                                  : undefined;
    ok = ok && field != NULL && napi_set_element(env, fields, 5, field) == napi_ok;
    /* the data: the payload row's value itself; a session of its own context gets its copy */
    field = ta_napi_native_value(native, view->data);
    if (ok && evaluation->own != NULL && view->data != 0)
        field = ta_napi_kit(env, evaluation->kit, &(ta_napi_kit_call){"copy", 1, {field}});
    ok = ok && field != NULL && napi_set_element(env, fields, 6, field) == napi_ok &&
         napi_set_element(env, fields, 7,
                          view->size >= sizeof *view ? ta_napi_optional(env, view->raw) : undefined) == napi_ok &&
         (event = ta_napi_kit(env, evaluation->kit, &(ta_napi_kit_call){"event", 1, {fields}})) != NULL;
    /* _event is read-only to the chart: a non-writable property, defined again */
    napi_property_descriptor property = {"_event", NULL, NULL, NULL, NULL, NULL, napi_enumerable | napi_configurable, NULL};
    property.value = ok ? event : NULL;
    ok = ok && napi_define_properties(env, evaluation->scope, 1, &property) == napi_ok;
    /* an event has come: the params were read by init_data */
    if (ok && (link = ta_napi_link(evaluation)) != NULL) napi_set_named_property(env, link, "init", undefined);
    ta_napi_session_row *row = ok ? (ta_napi_session_row *)ta_table_get(&native->host->sessions, call->instance) : NULL;
    if (row != NULL) { /* core's ids of the strings, for a blob */
        row->name_id = view->name_id;
        row->invoke_slot = view->invoke_slot;
        row->origintype_id = view->origintype_id;
    }
    ta_status status = ok ? TA_OK : ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

/* SCXML keeps _event bound to the last event (5.10): nothing to drop. */
static void ta_napi_detach_event(const ta_datamodel_call *call) {
    (void)call;
}

/* ---- evaluations ------------------------------------------------------------------ */

static ta_status ta_napi_eval_condition(const ta_datamodel_call *call, ta_expression e, int *out) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    napi_env env = entry.evaluation.env;
    napi_value value = ta_napi_evaluate(&entry.evaluation, e), truth;
    bool is = false;
    ta_status status = value != NULL && napi_coerce_to_bool(env, value, &truth) == napi_ok &&
                               napi_get_value_bool(env, truth, &is) == napi_ok
                           ? TA_OK
                           : ta_napi_failed(env);
    if (status == TA_OK) *out = is ? 1 : 0;
    ta_napi_frame_close(&entry.frame);
    return status;
}

static ta_status ta_napi_eval_string(const ta_datamodel_call *call, ta_expression e, ta_string_view *out) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    napi_value value = ta_napi_evaluate(&entry.evaluation, e);
    ta_status status = value != NULL ? ta_napi_string_out(entry.evaluation.native, value, out) : ta_napi_failed(entry.evaluation.env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

static ta_status ta_napi_assign_entry(const ta_datamodel_call *call, ta_expression location, ta_expression e) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    napi_value value = ta_napi_evaluate(&entry.evaluation, e);
    bool ok = value != NULL && ta_napi_assign(&entry.evaluation, &(ta_napi_assignment){location, value});
    ta_status status = ok ? TA_OK : ta_napi_failed(entry.evaluation.env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

static ta_status ta_napi_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view text) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    napi_env env = entry.evaluation.env;
    napi_value value = NULL;
    bool ok = napi_create_string_utf8(env, text.pointer != NULL ? text.pointer : "", text.length, &value) == napi_ok &&
              ta_napi_assign(&entry.evaluation, &(ta_napi_assignment){location, value});
    ta_status status = ok ? TA_OK : ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

static ta_status ta_napi_script_entry(const ta_datamodel_call *call, ta_expression e) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    ta_status status = ta_napi_evaluate(&entry.evaluation, e) != NULL ? TA_OK : ta_napi_failed(entry.evaluation.env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

/* <log>: an object (not a function) as its JSON, else as a string. */
static ta_status ta_napi_log(const ta_datamodel_call *call, ta_expression e, ta_string_view *out) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    napi_env env = entry.evaluation.env;
    napi_value value = ta_napi_evaluate(&entry.evaluation, e), stringify, json, undefined;
    napi_valuetype type = napi_undefined;
    ta_napi_instance *instance = ta_napi_instance_of(env);
    if (value != NULL && instance != NULL && napi_typeof(env, value, &type) == napi_ok && type == napi_object &&
        napi_get_reference_value(env, instance->stringify, &stringify) == napi_ok && napi_get_undefined(env, &undefined) == napi_ok) {
        if (napi_call_function(env, undefined, stringify, 1, &value, &json) == napi_ok &&
            napi_typeof(env, json, &type) == napi_ok && type == napi_string)
            value = json;
        else {
            napi_value ignored; /* a cycle: its text instead */
            napi_get_and_clear_last_exception(env, &ignored);
        }
    }
    ta_status status = value != NULL ? ta_napi_string_out(entry.evaluation.native, value, out) : ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

/* ---- <foreach> ------------------------------------------------------------------ */

static ta_status ta_napi_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    ta_napi_native *native = entry.evaluation.native;
    napi_env env = entry.evaluation.env;
    napi_value value = ta_napi_evaluate(&entry.evaluation, array), items, item;
    bool is = false;
    uint32_t length = 0;
    ta_status status = TA_OK;
    /* an array, copied shallowly: the loop walks the items it had */
    if (value == NULL || napi_is_array(env, value, &is) != napi_ok || !is || napi_get_array_length(env, value, &length) != napi_ok ||
        length > (uint32_t)INT32_MAX || napi_create_array_with_length(env, length, &items) != napi_ok)
        status = ta_napi_failed(env);
    for (uint32_t i = 0; status == TA_OK && i < length; i++)
        if (napi_get_element(env, value, i, &item) != napi_ok || napi_set_element(env, items, i, item) != napi_ok)
            status = ta_napi_failed(env);
    napi_ref reference = NULL;
    if (status == TA_OK && napi_create_reference(env, items, 1, &reference) != napi_ok) status = ta_napi_failed(env);
    uint64_t handle = 0;
    ta_napi_loop_row *row = status == TA_OK ? (ta_napi_loop_row *)ta_table_add(&native->host->loops, &handle) : NULL;
    if (status == TA_OK && row == NULL) {
        napi_delete_reference(env, reference);
        status = TA_ERROR_NO_MEMORY;
    }
    if (row != NULL) {
        row->items = reference;
        loop->iter = handle;
        loop->n = length;
    }
    ta_napi_frame_close(&entry.frame);
    return status;
}

static ta_status ta_napi_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    ta_napi_native *native = entry.evaluation.native;
    napi_env env = entry.evaluation.env;
    ta_napi_loop_row *row = (ta_napi_loop_row *)ta_table_get(&native->host->loops, loop->iter);
    napi_value items = row != NULL ? ta_napi_native_get(native, row->items) : NULL, item, index;
    bool ok = items != NULL && napi_get_element(env, items, loop->i, &item) == napi_ok &&
              ta_napi_assign(&entry.evaluation, &(ta_napi_assignment){loop->item, item});
    if (ok && loop->index != TA_NIL)
        ok = napi_create_uint32(env, loop->i, &index) == napi_ok &&
             ta_napi_assign(&entry.evaluation, &(ta_napi_assignment){loop->index, index});
    ta_status status = ok ? TA_OK : ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

static void ta_napi_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_napi_native *native = (ta_napi_native *)call->datamodel;
    ta_napi_loop_row *row = (ta_napi_loop_row *)ta_table_get(&native->host->loops, loop->iter);
    if (row == NULL) return;
    napi_ref reference = row->items;
    ta_table_remove(&native->host->loops, loop->iter);
    ta_napi_native_drop(native, reference);
}

/* ---- payloads ----------------------------------------------------------------------- */

/* A namelist entry's key: its location's text, trimmed. */
static napi_value ta_napi_location_name(const ta_napi_evaluation *evaluation, ta_expression location) {
    ta_expression_info info;
    napi_value name = NULL;
    if (!ta_napi_expression_of(&evaluation->row, location, &info)) return NULL;
    ta_string_view text = info.source;
    while (text.length > 0 && (text.pointer[0] == ' ' || text.pointer[0] == '\t' || text.pointer[0] == '\n' || text.pointer[0] == '\r')) {
        text.pointer++;
        text.length--;
    }
    while (text.length > 0 && (text.pointer[text.length - 1] == ' ' || text.pointer[text.length - 1] == '\t' ||
                               text.pointer[text.length - 1] == '\n' || text.pointer[text.length - 1] == '\r'))
        text.length--;
    napi_create_string_utf8(evaluation->env, text.pointer, text.length, &name);
    return name;
}

/* What a <send>, <invoke> or <donedata> captures, copied into the shared
 * context: actors share no mutable value (bindings.md, section 8). */
static ta_status ta_napi_capture(const ta_datamodel_call *call, const ta_datamodel_capture *capture, ta_payload *out) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_EXECUTION;
    ta_napi_evaluation *evaluation = &entry.evaluation;
    napi_env env = evaluation->env;
    napi_value shared = ta_napi_realm_shared(evaluation->native), kit = NULL, captured = NULL, names, values, name, value;
    bool ok = shared != NULL && napi_get_element(env, shared, TA_NAPI_REALM_KIT, &kit) == napi_ok;
    if (ok && capture->content != TA_NIL) {
        value = ta_napi_evaluate(evaluation, capture->content);
        captured = value != NULL ? ta_napi_kit(env, kit, &(ta_napi_kit_call){"copy", 1, {value}}) : NULL;
    } else if (ok) {
        uint32_t count = 0;
        ok = napi_create_array(env, &names) == napi_ok && napi_create_array(env, &values) == napi_ok;
        for (uint32_t i = 0; ok && i < capture->namelist_count; i++)
            ok = (name = ta_napi_location_name(evaluation, capture->namelist[i])) != NULL &&
                 (value = ta_napi_evaluate(evaluation, capture->namelist[i])) != NULL &&
                 napi_set_element(env, names, count, name) == napi_ok && napi_set_element(env, values, count++, value) == napi_ok;
        for (uint32_t i = 0; ok && i < capture->param_count; i++) {
            ta_string_view text = ta_image_string(evaluation->row.image, capture->params[2 * i]);
            ok = napi_create_string_utf8(env, text.pointer, text.length, &name) == napi_ok &&
                 (value = ta_napi_evaluate(evaluation, capture->params[2 * i + 1])) != NULL &&
                 napi_set_element(env, names, count, name) == napi_ok && napi_set_element(env, values, count++, value) == napi_ok;
        }
        captured = ok ? ta_napi_kit(env, kit, &(ta_napi_kit_call){"record", 2, {names, values}}) : NULL;
    }
    ta_status status = captured != NULL ? ta_napi_native_add(evaluation->native, captured, out) : ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

static ta_status ta_napi_custom(const ta_datamodel_call *call, uint32_t definition) {
    (void)call;
    (void)definition;
    return TA_ERROR_UNSUPPORTED;
}

/* ---- hibernation ----------------------------------------------------------------------- */

/* Appends `text` to the reason, truncated, NUL-terminated. */
static void ta_napi_reason_add(ta_napi_native *native, const char *text) {
    size_t at = strlen(native->reason);
    for (; *text != '\0' && at + 1 < sizeof native->reason; text++) native->reason[at++] = *text;
    native->reason[at] = '\0';
}

/* Why a value could not be stored: `what` ("variable 'name'", "_event"),
 * then the message of the pending exception, which is cleared. */
static void ta_napi_reason(ta_napi_native *native, const char *what, napi_value name) {
    napi_env env = native->host->env;
    napi_value error = NULL, message;
    napi_get_and_clear_last_exception(env, &error);
    native->reason[0] = '\0';
    ta_napi_reason_add(native, what);
    size_t at = strlen(native->reason), length = 0;
    if (name != NULL && at + 2 < sizeof native->reason &&
        napi_get_value_string_utf8(env, name, native->reason + at, sizeof native->reason - at - 1, &length) == napi_ok)
        native->reason[at + length] = '\0';
    ta_napi_reason_add(native, name != NULL ? "' cannot be stored: " : " cannot be stored: ");
    at = strlen(native->reason);
    napi_valuetype type = napi_undefined;
    if (error != NULL && napi_typeof(env, error, &type) == napi_ok && type == napi_object &&
        napi_get_named_property(env, error, "message", &message) == napi_ok && napi_typeof(env, message, &type) == napi_ok &&
        type == napi_string && at + 1 < sizeof native->reason &&
        napi_get_value_string_utf8(env, message, native->reason + at, sizeof native->reason - at, &length) == napi_ok)
        return;
    napi_get_and_clear_last_exception(env, &error);
    ta_napi_reason_add(native, "it is not a value of the value table");
}

/* What a refused document answers core: no memory, or a state that cannot
 * be stored. */
static ta_status ta_napi_unstored(const ta_napi_document *document) {
    return document->refused == TA_ERROR_NO_MEMORY ? TA_ERROR_NO_MEMORY : TA_ERROR_UNSUPPORTED;
}

static const char *const ta_napi_event_fields[] = {"name", "type", "sendid", "origin", "origintype", "invokeid", "data", "raw"};

/* _event as a blob holds it: its eight fields, then core's ids of its
 * name, its invoke and its origintype; null when no event came yet. */
typedef struct ta_napi_event_out {
    napi_value event;
    const ta_napi_session_row *row;
} ta_napi_event_out;

static bool ta_napi_event_write(ta_napi_native *native, const ta_napi_event_out *event, ta_napi_document *document) {
    napi_env env = native->host->env;
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, event->event, &type) != napi_ok) return false;
    if (type != napi_object) {
        ta_cbor_write_simple(&document->writer, TA_CBOR_NULL);
        return true;
    }
    ta_cbor_write_array(&document->writer, 11);
    for (uint32_t i = 0; i < 8; i++) {
        napi_value field;
        if (napi_get_named_property(env, event->event, ta_napi_event_fields[i], &field) != napi_ok ||
            !ta_napi_document_value(env, field, document)) {
            ta_napi_reason(native, "_event", NULL);
            return false;
        }
    }
    ta_cbor_write_uint(&document->writer, event->row->name_id);
    ta_cbor_write_uint(&document->writer, event->row->invoke_slot);
    ta_cbor_write_uint(&document->writer, event->row->origintype_id);
    return true;
}

/* A session's state written into the datamodel's output: its variables
 * and its _event as one CBOR map {"vars": {name: value}, "event": [...] |
 * null}, through the value table (no format that runs code when read:
 * bindings.md section 8). Then core calls destroy, which removes the row:
 * a hibernated session holds nothing in the engine. A value outside the
 * table keeps it resident (TA_ERROR_UNSUPPORTED, `reason` says which), and
 * so does a chart whose scripts declare with let, const or class, which a
 * new context cannot be given again. A declaring session's function
 * declarations are not written: the wake makes them again from the
 * chart's scripts. */
static ta_status ta_napi_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    ta_napi_native *native = (ta_napi_native *)call->datamodel;
    *out = (ta_bytes){NULL, 0};
    native->reason[0] = '\0';
    ta_napi_session_row *found = (ta_napi_session_row *)ta_table_get(&native->host->sessions, call->instance);
    if (found == NULL) return TA_ERROR_INVALID;
    ta_napi_session_row row = *found; /* a copy: JavaScript runs below */
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_INVALID;
    ta_napi_evaluation *evaluation = &entry.evaluation;
    napi_env env = evaluation->env;
    if (evaluation->row.flags & TA_NAPI_CHART_LEXICAL) {
        ta_napi_reason_add(native, "the chart's scripts declare with let, const or class at their top level: "
                                   "a new context cannot be given those bindings again, so its sessions stay resident");
        ta_napi_frame_close(&entry.frame);
        return TA_ERROR_UNSUPPORTED;
    }
    napi_value remade = NULL, variables = NULL, names, values, name, value, event;
    napi_valuetype type = napi_undefined;
    uint32_t count = 0;
    bool ok = napi_get_element(env, evaluation->record, TA_NAPI_CHART_FUNCTIONS, &remade) == napi_ok &&
              napi_typeof(env, remade, &type) == napi_ok && (type == napi_object || napi_create_array(env, &remade) == napi_ok) &&
              (variables = ta_napi_kit(env, evaluation->kit, &(ta_napi_kit_call){"stored", 2, {evaluation->scope, remade}})) != NULL &&
              napi_get_element(env, variables, 0, &names) == napi_ok && napi_get_element(env, variables, 1, &values) == napi_ok &&
              napi_get_array_length(env, names, &count) == napi_ok &&
              napi_get_named_property(env, evaluation->scope, "_event", &event) == napi_ok;
    if (!ok) {
        ta_napi_reason(native, "the session", NULL);
        ta_napi_frame_close(&entry.frame);
        return TA_ERROR_UNSUPPORTED;
    }
    ta_napi_document document;
    memset(&document, 0, sizeof document);
    document.room = (ta_napi_room){&native->output, &native->host->allocator, {NULL, 0}};
    document.arena = &native->host->arena;
    ta_napi_document_open(&document);
    ta_cbor_write_map(&document.writer, 2);
    ta_cbor_write_text(&document.writer, (ta_string_view){"vars", 4});
    ta_cbor_write_map(&document.writer, count);
    for (uint32_t i = 0; ok && i < count; i++) {
        ok = napi_get_element(env, names, i, &name) == napi_ok && napi_get_element(env, values, i, &value) == napi_ok &&
             ta_napi_document_value(env, name, &document);
        if (ok && !ta_napi_document_value(env, value, &document)) {
            ta_napi_reason(native, "variable '", name);
            ok = false;
        }
    }
    if (ok) {
        ta_cbor_write_text(&document.writer, (ta_string_view){"event", 5});
        ok = ta_napi_event_write(native, &(ta_napi_event_out){event, &row}, &document);
    }
    if (!ok && document.refused == TA_OK) document.refused = TA_ERROR_EXECUTION;
    bool kept = ta_napi_document_close(&document, ok);
    ta_status status = kept ? TA_OK : ta_napi_unstored(&document);
    if (kept) *out = document.out;
    else if (native->reason[0] == '\0') ta_napi_reason(native, "the session", NULL);
    napi_value ignored;
    napi_get_and_clear_last_exception(env, &ignored);
    ta_napi_frame_close(&entry.frame);
    return status;
}

/* `text` as the string kept at `index` of a chart's list, when its text is
 * the same (a string of the blob's is kept there when the place is empty). */
static napi_value ta_napi_kept_string(napi_env env, const ta_napi_kept_at *at, napi_value text) {
    napi_value kept;
    napi_valuetype type = napi_undefined, mine = napi_undefined;
    bool same = false;
    if (napi_typeof(env, text, &mine) != napi_ok || mine != napi_string || at->list == NULL || at->index == TA_NIL ||
        napi_get_element(env, at->list, at->index, &kept) != napi_ok || napi_typeof(env, kept, &type) != napi_ok)
        return text;
    if (type == napi_string && napi_strict_equals(env, kept, text, &same) == napi_ok && same) return kept;
    if (type == napi_undefined) napi_set_element(env, at->list, at->index, text);
    return text;
}

/* An id of a blob's event (a number), TA_NIL for anything else. */
static uint32_t ta_napi_blob_id(napi_env env, napi_value fields, uint32_t index) {
    napi_value value;
    napi_valuetype type = napi_undefined;
    uint32_t id = TA_NIL;
    if (napi_get_element(env, fields, index, &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_number || napi_get_value_uint32(env, value, &id) != napi_ok)
        return TA_NIL;
    return id;
}

/* A woken session's _event from the fields a blob holds, its strings
 * shared with the chart's (by core's ids, when the text is the same) and
 * the datamodel's, its data copied into the session's context. The ids go
 * to `row`. */
typedef struct ta_napi_event_in {
    napi_value fields; /* the blob's array of 11 */
    napi_value kit, record;
    ta_napi_session_row *row; /* out: the ids */
} ta_napi_event_in;

static napi_value ta_napi_event_from(ta_napi_native *native, const ta_napi_event_in *in) {
    napi_env env = native->host->env;
    napi_value fields, field, names, invokeids, origins, strings = ta_napi_native_get(native, native->strings), made[8];
    uint32_t length = 0;
    bool array = false;
    if (napi_is_array(env, in->fields, &array) != napi_ok || !array || napi_get_array_length(env, in->fields, &length) != napi_ok ||
        length != 11 || strings == NULL || napi_get_element(env, in->record, TA_NAPI_CHART_NAMES, &names) != napi_ok ||
        napi_get_element(env, in->record, TA_NAPI_CHART_INVOKEIDS, &invokeids) != napi_ok ||
        napi_get_element(env, in->record, TA_NAPI_CHART_ORIGINS, &origins) != napi_ok)
        return NULL;
    for (uint32_t i = 0; i < 8; i++)
        if (napi_get_element(env, in->fields, i, &made[i]) != napi_ok) return NULL;
    in->row->name_id = ta_napi_blob_id(env, in->fields, 8);
    in->row->invoke_slot = ta_napi_blob_id(env, in->fields, 9);
    in->row->origintype_id = ta_napi_blob_id(env, in->fields, 10);
    made[0] = ta_napi_kept_string(env, &(ta_napi_kept_at){names, in->row->name_id}, made[0]);
    for (uint32_t i = 0; i < 3; i++) { /* the type: one of the datamodel's three */
        bool same = false;
        if (napi_get_element(env, strings, i, &field) == napi_ok && napi_strict_equals(env, field, made[1], &same) == napi_ok && same)
            made[1] = field;
    }
    made[3] = ta_napi_kept_string(env, &(ta_napi_kept_at){origins, in->row->invoke_slot}, made[3]);
    uint32_t origintype = in->row->origintype_id < TA_NIL - 3 ? 3 + in->row->origintype_id : TA_NIL;
    made[4] = ta_napi_kept_string(env, &(ta_napi_kept_at){strings, origintype}, made[4]);
    made[5] = ta_napi_kept_string(env, &(ta_napi_kept_at){invokeids, in->row->invoke_slot}, made[5]);
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, made[6], &type) != napi_ok ||
        (type != napi_undefined && (made[6] = ta_napi_kit(env, in->kit, &(ta_napi_kit_call){"copy", 1, {made[6]}})) == NULL) ||
        napi_create_array_with_length(env, 8, &fields) != napi_ok)
        return NULL;
    for (uint32_t i = 0; i < 8; i++)
        if (napi_set_element(env, fields, i, made[i]) != napi_ok) return NULL;
    return ta_napi_kit(env, in->kit, &(ta_napi_kit_call){"event", 1, {fields}});
}

/* A session from a blob serialize wrote, for its chart now: a new scope
 * (or, for a declaring chart, a new context, its scripts' declarations
 * made again), the variables of the blob its chart declares (all of them
 * on a declaring session's global), and its _event. Every variable of the
 * blob is kept in the session's link as its `init` until the first event
 * or migrate takes it, as a spawn's params are: when the blob is of an
 * older version of the chart, migrate answers them as the old bindings. */
static ta_status ta_napi_deserialize(void *datamodel, const ta_datamodel_start *start, uint64_t *instance) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    napi_env env = native->host->env;
    ta_napi_frame frame;
    if (!ta_napi_frame_open(native, NULL, &frame)) return TA_ERROR_NO_MEMORY;
    uint64_t chart_handle = (uint64_t)(uintptr_t)start->chart_context;
    ta_napi_chart_row *found = (ta_napi_chart_row *)ta_table_get(&native->host->charts, chart_handle);
    ta_napi_chart_row chart = found != NULL ? *found : (ta_napi_chart_row){NULL, NULL, 0, 0};
    ta_napi_session_row row = {NULL, chart_handle, TA_NIL, TA_NIL, TA_NIL};
    napi_value state = NULL, vars = NULL, fields = NULL, made = NULL, record, kit, target, context, shared, undefined, event;
    napi_valuetype type = napi_undefined;
    bool ok = found != NULL && napi_get_undefined(env, &undefined) == napi_ok && ta_napi_keep_strings(native, start->system_variables) &&
              (record = ta_napi_native_get(native, chart.functions)) != NULL &&
              (state = ta_napi_value_read(env, start->state)) != NULL && napi_typeof(env, state, &type) == napi_ok &&
              type == napi_object && napi_get_named_property(env, state, "vars", &vars) == napi_ok &&
              napi_typeof(env, vars, &type) == napi_ok && type == napi_object &&
              napi_get_named_property(env, state, "event", &fields) == napi_ok &&
              (made = ta_napi_session_new(native, &(ta_napi_start_of){&chart, start->system_variables, vars})) != NULL;
    if (ok && (chart.flags & TA_NAPI_CHART_DECLARES)) /* its own context: the declarations again, then the values */
        ok = napi_get_element(env, made, TA_NAPI_OWN_KIT, &kit) == napi_ok &&
             napi_get_element(env, made, TA_NAPI_OWN_CONTEXT, &context) == napi_ok &&
             napi_get_named_property(env, kit, "global", &target) == napi_ok && ta_napi_realm_hoist(native, record, context);
    else if (ok)
        ok = (shared = ta_napi_realm_shared(native)) != NULL && napi_get_element(env, shared, TA_NAPI_REALM_KIT, &kit) == napi_ok &&
             (target = made) != NULL;
    ok = ok && ta_napi_kit(env, kit, &(ta_napi_kit_call){"restore", 2, {target, vars}}) != NULL &&
         napi_typeof(env, fields, &type) == napi_ok;
    if (ok && type != napi_null && type != napi_undefined) {
        event = ta_napi_event_from(native, &(ta_napi_event_in){fields, kit, record, &row});
        napi_property_descriptor property = {"_event", NULL, NULL, NULL, NULL, event, napi_enumerable | napi_configurable, NULL};
        ok = event != NULL && napi_define_properties(env, target, 1, &property) == napi_ok;
    }
    ta_status status = TA_OK;
    if (!ok || napi_create_reference(env, made, 1, &row.scope) != napi_ok) status = (ta_napi_failed(env), TA_ERROR_INVALID);
    uint64_t handle = 0;
    ta_napi_session_row *added = status == TA_OK ? (ta_napi_session_row *)ta_table_add(&native->host->sessions, &handle) : NULL;
    if (status == TA_OK && added == NULL) {
        napi_delete_reference(env, row.scope);
        status = TA_ERROR_NO_MEMORY;
    }
    if (added != NULL) {
        *added = row;
        *instance = handle;
    }
    ta_napi_frame_close(&frame);
    return status;
}

/* After deserialize, when the blob was written for an older version of
 * the chart: the old bindings, exactly the old chart's variables as the
 * blob held them, as one payload (an object of name -> value), taken from
 * the link; on a declaring session's global the old variables the new
 * chart does not declare are dropped. Core then initializes the new
 * chart's added <data> and raises error.platform.upgrade with it. */
static ta_status ta_napi_migrate(const ta_datamodel_call *call, ta_payload *bindings) {
    *bindings = 0;
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_INVALID;
    ta_napi_evaluation *evaluation = &entry.evaluation;
    napi_env env = evaluation->env;
    napi_value old = ta_napi_kit(env, evaluation->kit, &(ta_napi_kit_call){"upgrade", 1, {evaluation->scope}});
    napi_valuetype type = napi_undefined;
    ta_status status = TA_OK;
    if (old == NULL || napi_typeof(env, old, &type) != napi_ok) status = ta_napi_failed(env);
    else if (type == napi_object && !ta_napi_native_payload(evaluation->native, old, bindings)) {
        napi_value error = NULL;
        napi_get_and_clear_last_exception(env, &error);
        status = TA_ERROR_NO_MEMORY; /* the blob's values are the value table's: only memory refuses them */
    }
    ta_napi_frame_close(&entry.frame);
    return status;
}

/* ---- neutral values ------------------------------------------------------------------ */

/* A payload as one CBOR document in the output: to_wire's (`wire`), or the
 * value table's for payload_serialize; the document's refusal, or NULL
 * row's TA_ERROR_INVALID. */
typedef struct ta_napi_payload_out {
    ta_payload payload;
    bool wire;
    ta_bytes *out;
} ta_napi_payload_out;

static ta_status ta_napi_payload_document(ta_napi_native *native, const ta_napi_payload_out *payload) {
    napi_env env = native->host->env;
    ta_napi_frame frame;
    *payload->out = (ta_bytes){NULL, 0};
    if (ta_table_get(&native->host->payloads, payload->payload) == NULL) return TA_ERROR_INVALID;
    if (!ta_napi_frame_open(native, native->environment, &frame)) return TA_ERROR_NO_MEMORY;
    ta_napi_document document;
    memset(&document, 0, sizeof document);
    document.room = (ta_napi_room){&native->output, &native->host->allocator, {NULL, 0}};
    document.arena = &native->host->arena;
    document.wire = payload->wire;
    ta_napi_document_open(&document);
    bool ok = ta_napi_document_value(env, ta_napi_native_value(native, payload->payload), &document);
    if (!ok) ta_napi_failed(env);
    ta_status status = ta_napi_document_close(&document, ok) ? TA_OK : ta_napi_unstored(&document);
    if (status == TA_OK) *payload->out = document.out;
    ta_napi_frame_close(&frame);
    return status;
}

/* A payload as CBOR (values.txt's ECMAScript to_wire), in the output:
 * TA_ERROR_INVALID when it cannot be a neutral value. */
static ta_status ta_napi_to_wire(void *datamodel, ta_payload payload, ta_bytes *out) {
    ta_status status = ta_napi_payload_document((ta_napi_native *)datamodel, &(ta_napi_payload_out){payload, true, out});
    return status == TA_ERROR_UNSUPPORTED ? TA_ERROR_INVALID : status;
}

/* A payload as the value table's CBOR (a delayed send's in an export blob):
 * TA_ERROR_UNSUPPORTED for a value outside the table. */
static ta_status ta_napi_payload_serialize(void *datamodel, ta_payload payload, ta_bytes *out) {
    return ta_napi_payload_document((ta_napi_native *)datamodel, &(ta_napi_payload_out){payload, false, out});
}

/* A payload from CBOR: read, then copied into the shared context. */
static ta_status ta_napi_from_wire(void *datamodel, ta_bytes cbor, ta_payload *out) {
    ta_napi_native *native = (ta_napi_native *)datamodel;
    napi_env env = native->host->env;
    ta_napi_frame frame;
    if (!ta_napi_frame_open(native, native->environment, &frame)) return TA_ERROR_NO_MEMORY;
    napi_value value = ta_napi_value_read(env, cbor);
    ta_status status = value != NULL && ta_napi_native_payload(native, value, out) ? TA_OK : TA_ERROR_INVALID;
    if (status != TA_OK) ta_napi_failed(env);
    ta_napi_frame_close(&frame);
    return status;
}

/* A <data> variable for inspection (actor.context): the value itself as a
 * payload, which core writes out with to_wire at once and releases; not
 * a copy, which would refuse what to_wire writes (a function dropped, a
 * Date as its JSON). */
static ta_status ta_napi_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_napi_entry entry;
    if (!ta_napi_enter_entry(call, &entry)) return TA_ERROR_INVALID;
    ta_napi_evaluation *evaluation = &entry.evaluation;
    napi_env env = evaluation->env;
    bool declared = false;
    uint32_t count = ta_image_data_count(evaluation->row.image, evaluation->row.chart);
    for (uint32_t i = 0; i < count && !declared; i++) {
        ta_data_info info;
        declared = ta_image_data(evaluation->row.image, (ta_chart_item){evaluation->row.chart, i}, &info) == TA_OK &&
                   info.id.length == name.length && memcmp(info.id.pointer, name.pointer, name.length) == 0;
    }
    napi_value key, value;
    ta_status status = declared && napi_create_string_utf8(env, name.pointer, name.length, &key) == napi_ok &&
                               napi_get_property(env, evaluation->scope, key, &value) == napi_ok
                           ? ta_napi_native_add(evaluation->native, value, out)
                           : TA_ERROR_INVALID;
    if (status != TA_OK) ta_napi_failed(env);
    ta_napi_frame_close(&entry.frame);
    return status;
}

static const ta_datamodel_vtable ta_napi_vtable = {
    .size = sizeof(ta_datamodel_vtable),
    .flags = TA_DATAMODEL_EVENT_DETACHABLE,
    .name = "ecmascript",
    .chart_attach = ta_napi_chart_attach,
    .chart_detach = ta_napi_chart_detach,
    .create = ta_napi_create,
    .destroy = ta_napi_destroy,
    .init_data = ta_napi_init_data,
    .bind_event = ta_napi_bind_event,
    .detach_event = ta_napi_detach_event,
    .eval_condition = ta_napi_eval_condition,
    .eval_string = ta_napi_eval_string,
    .assign = ta_napi_assign_entry,
    .assign_string = ta_napi_assign_string,
    .script = ta_napi_script_entry,
    .log = ta_napi_log,
    .foreach_begin = ta_napi_foreach_begin,
    .foreach_item = ta_napi_foreach_item,
    .foreach_end = ta_napi_foreach_end,
    .capture = ta_napi_capture,
    .retain = ta_napi_native_retain,
    .release = ta_napi_native_release,
    .custom = ta_napi_custom,
    .serialize = ta_napi_serialize,
    .deserialize = ta_napi_deserialize,
    .migrate = ta_napi_migrate,
    .to_wire = ta_napi_to_wire,
    .from_wire = ta_napi_from_wire,
    .payload_serialize = ta_napi_payload_serialize,
    .payload_deserialize = ta_napi_from_wire, /* the same CBOR: one reference, copied into the shared context */
    .variable = ta_napi_variable,
};

const ta_datamodel_vtable *ta_napi_native_vtable(void) {
    return &ta_napi_vtable;
}
