/* datamodel.c — the "python" datamodel's vtable (tinyactors/datamodel.h):
 * one globals dict per session, payloads that are Python values, and the
 * value table (values.c) for to_wire and from_wire.
 *
 * In a session's globals:
 *   - each <data> id, None until it is initialized (late binding too);
 *   - _sessionid, _name (str), _ioprocessors ({type: {"location": ...}}),
 *     and _event once the first event is bound: a tinyactors.Event, read
 *     by attribute (name, type, sendid, origin, origintype, invokeid, data,
 *     raw; a field with no value is None, as is data without data);
 *   - In(state_id), SCXML's In() (ta_datamodel_environment_in);
 *   - __builtins__, the builtins module's dict.
 * These are the assumptions of the suite's stylesheet for this datamodel
 * (tinyactors-test/suites/w3c/confPython.xsl, its header).
 *
 * What crosses to another actor (<send> data, <donedata>, a session's
 * variable for actor.context) is copied through the value table; an
 * object outside it fails the capture with error.execution (bindings.md,
 * section 8). Hibernation, export and migration are later milestones'
 * (ROADMAP.md): serialize, deserialize, payload_serialize, migrate are
 * NULL, so core answers TA_ERROR_UNSUPPORTED. */
#include "datamodel/datamodel.h"

#define TA_PYTHON_DATAMODEL(call) ((ta_python_datamodel *)(call)->datamodel)

/* The session of a call, or NULL for a stale handle. */
static ta_python_session *ta_python_session_of_call(const ta_datamodel_call *call) {
    ta_python_slot *slot = ta_python_handle_slot(&TA_PYTHON_DATAMODEL(call)->sessions, call->instance);
    return slot ? (ta_python_session *)slot->item : NULL;
}

/* The payload's value, borrowed; NULL for a stale handle. */
static PyObject *ta_python_payload_object(ta_python_datamodel *datamodel, ta_payload payload) {
    ta_python_slot *slot = ta_python_handle_slot(&datamodel->payloads, payload);
    return slot ? (PyObject *)slot->item : NULL;
}

/* A new payload holding `value` (its reference is the payload's). */
static ta_status ta_python_payload_new(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out) {
    *out = ta_python_handle_add(&datamodel->payloads, value);
    if (*out) return TA_OK;
    ta_python_datamodel_defer(datamodel, value);
    return TA_ERROR_NO_MEMORY;
}

void ta_python_datamodel_defer(ta_python_datamodel *datamodel, PyObject *object) {
    if (!object) return;
    if (PyList_Append(datamodel->deferred, object) < 0) PyErr_Clear(); /* then dropped now */
    Py_DECREF(object);
}

int ta_python_datamodel_interrupted(const ta_python_datamodel *datamodel) {
    return datamodel->kept != NULL;
}

int ta_python_datamodel_settle(ta_python_datamodel *datamodel) {
    /* An ended session's globals are in a cycle with the functions and
     * classes defined in them (their __globals__): cleared here, outside
     * core, its variables go now instead of in a later collection, which
     * could come during a run. */
    if (!datamodel->ended || !datamodel->deferred) return 0;
    Py_ssize_t ended = PyList_Size(datamodel->ended);
    for (Py_ssize_t i = 0; i < ended; i++)
        PyDict_Clear(PyList_GetItem(datamodel->ended, i));
    if (ended > 0 && PyList_SetSlice(datamodel->ended, 0, ended, NULL) < 0) PyErr_Clear();
    Py_ssize_t count = PyList_Size(datamodel->deferred);
    if (count > 0 && PyList_SetSlice(datamodel->deferred, 0, count, NULL) < 0) PyErr_Clear();
    if (!datamodel->kept) return 0;
    PyErr_SetRaisedException(datamodel->kept);
    datamodel->kept = NULL;
    return -1;
}

/* ---- In() ------------------------------------------------------------------------------- */

/* In(state_id): whether the session of the evaluation in progress is in
 * that state (SCXML 5.9.1); outside an evaluation, False. */
static PyObject *ta_python_in(PyObject *capsule, PyObject *name) {
    ta_python_datamodel *datamodel = PyCapsule_GetPointer(capsule, NULL);
    if (!datamodel) return NULL;
    Py_ssize_t length = 0;
    const char *text = PyUnicode_Check(name) ? PyUnicode_AsUTF8AndSize(name, &length) : NULL;
    if (!text) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "In() takes a state's id");
        return NULL;
    }
    ta_python_session *session = datamodel->current;
    if (!datamodel->environment || !session) Py_RETURN_FALSE;
    uint32_t state = ta_image_state_by_name(session->chart->image, session->chart->chart,
                                            (ta_string_view){text, (uint32_t)length});
    if (state == TA_NIL) Py_RETURN_FALSE;
    return PyBool_FromLong(ta_datamodel_environment_in(datamodel->environment, state));
}

static PyMethodDef ta_python_in_method = {"In", ta_python_in, METH_O, "In(state_id) -> bool"};

/* ---- _event ------------------------------------------------------------------------------- */

static PyStructSequence_Field ta_python_event_fields[] = {
    {"name", NULL}, {"type", NULL},     {"sendid", NULL}, {"origin", NULL}, {"origintype", NULL},
    {"invokeid", NULL}, {"data", NULL}, {"raw", NULL},    {NULL, NULL},
};

static PyStructSequence_Desc ta_python_event_description = {
    "tinyactors.Event", "_event: the event being processed (SCXML 5.10.1), read by attribute.",
    ta_python_event_fields, 8};

int ta_python_event_init_type(PyObject *module, ta_python_state *state) {
    state->event_type = (PyObject *)PyStructSequence_NewType(&ta_python_event_description);
    if (!state->event_type) return -1;
    return PyModule_AddObjectRef(module, "Event", state->event_type);
}

/* A str of a view, or None when it is empty; text is valid UTF-8, never
 * replaced (bindings.md, section 9): NULL when it is not. */
static PyObject *ta_python_text_or_none(ta_string_view text) {
    if (!text.length) Py_RETURN_NONE;
    return PyUnicode_DecodeUTF8(text.pointer, (Py_ssize_t)text.length, "strict");
}

/* ---- charts and sessions --------------------------------------------------------------------- */

static ta_status ta_python_chart_attach(void *datamodel, const ta_datamodel_chart *chart, void **context) {
    ta_python_chart *attached = PyMem_RawMalloc(sizeof *attached);
    if (!attached) return TA_ERROR_NO_MEMORY;
    memset(attached, 0, sizeof *attached);
    attached->image = chart->image;
    attached->chart = chart->chart;
    ta_status status = ta_python_chart_compile((ta_python_datamodel *)datamodel, attached);
    if (status != TA_OK) {
        ta_python_chart_free(attached);
        PyMem_RawFree(attached);
        return status;
    }
    *context = attached;
    return TA_OK;
}

static void ta_python_chart_detach(void *datamodel, void *context) {
    (void)datamodel;
    ta_python_chart_free((ta_python_chart *)context);
    PyMem_RawFree(context);
}

static void ta_python_session_free(ta_python_datamodel *datamodel, ta_python_session *session) {
    if (session->globals && PyList_Append(datamodel->ended, session->globals) < 0) PyErr_Clear();
    ta_python_datamodel_defer(datamodel, session->globals); /* its variables' last drop: outside core */
    for (int i = 0; i < TA_PYTHON_SYSTEM_VARIABLES; i++)
        ta_python_datamodel_defer(datamodel, session->system[i]);
    ta_python_datamodel_defer(datamodel, session->init);
    PyMem_RawFree(session);
}

/* `name` = `value` in the globals, and kept as a system variable. */
typedef struct ta_python_binding {
    int variable;
    const char *name;
} ta_python_binding;

static int ta_python_bind_system(ta_python_session *session, const ta_python_binding *binding, PyObject *value) {
    if (!value || PyDict_SetItemString(session->globals, binding->name, value) < 0) {
        Py_XDECREF(value);
        return -1;
    }
    session->system[binding->variable] = value;
    return 0;
}

static PyObject *ta_python_ioprocessors(const ta_datamodel_system_variables *variables) {
    PyObject *processors = PyDict_New();
    for (uint32_t i = 0; processors && i < variables->io_processor_count; i++) {
        const ta_io_processor_entry *entry = &variables->io_processors[i];
        PyObject *type = PyUnicode_DecodeUTF8(entry->type.pointer, (Py_ssize_t)entry->type.length, "strict");
        PyObject *location =
            PyUnicode_DecodeUTF8(entry->location.pointer, (Py_ssize_t)entry->location.length, "strict");
        PyObject *record = location ? Py_BuildValue("{sO}", "location", location) : NULL;
        if (!type || !record || PyDict_SetItem(processors, type, record) < 0) Py_CLEAR(processors);
        Py_XDECREF(type);
        Py_XDECREF(location);
        Py_XDECREF(record);
    }
    return processors;
}

static int ta_python_session_fill(ta_python_datamodel *datamodel, ta_python_session *session,
                                  const ta_datamodel_start *start) {
    const ta_datamodel_system_variables *variables = start->system_variables;
    if (ta_python_bind_system(session, &(ta_python_binding){TA_PYTHON_BUILTINS, "__builtins__"},
                              Py_NewRef(datamodel->builtins)) < 0 ||
        ta_python_bind_system(session, &(ta_python_binding){TA_PYTHON_IN, "In"}, Py_NewRef(datamodel->in)) < 0 ||
        ta_python_bind_system(session, &(ta_python_binding){TA_PYTHON_SESSIONID, "_sessionid"},
                              ta_python_text_or_none(variables->sessionid)) < 0 ||
        ta_python_bind_system(session, &(ta_python_binding){TA_PYTHON_NAME, "_name"},
                              ta_python_text_or_none(variables->name)) < 0 ||
        ta_python_bind_system(session, &(ta_python_binding){TA_PYTHON_IOPROCESSORS, "_ioprocessors"},
                              ta_python_ioprocessors(variables)) < 0)
        return -1;
    /* every <data> id is declared from the start: None until initialized */
    uint32_t count = ta_image_data_count(session->chart->image, session->chart->chart);
    for (uint32_t i = 0; i < count; i++) {
        ta_data_info data;
        if (ta_image_data(session->chart->image, (ta_chart_item){session->chart->chart, i}, &data) != TA_OK) continue;
        PyObject *id = PyUnicode_DecodeUTF8(data.id.pointer, (Py_ssize_t)data.id.length, "strict");
        int set = id ? PyDict_SetItem(session->globals, id, Py_None) : -1;
        Py_XDECREF(id);
        if (set < 0) return -1;
    }
    PyObject *init = start->init ? ta_python_payload_object(datamodel, start->init) : NULL;
    if (init && PyDict_Check(init)) session->init = Py_NewRef(init);
    return 0;
}

static ta_status ta_python_create(void *data, const ta_datamodel_start *start, uint64_t *instance) {
    ta_python_datamodel *datamodel = (ta_python_datamodel *)data;
    ta_python_session *session = PyMem_RawMalloc(sizeof *session);
    if (!session) return TA_ERROR_NO_MEMORY;
    memset(session, 0, sizeof *session);
    session->chart = (ta_python_chart *)start->chart_context;
    session->globals = PyDict_New();
    if (!session->globals || ta_python_session_fill(datamodel, session, start) < 0) {
        PyErr_Clear();
        ta_python_session_free(datamodel, session);
        return TA_ERROR_NO_MEMORY;
    }
    *instance = ta_python_handle_add(&datamodel->sessions, session);
    if (*instance) return TA_OK;
    ta_python_session_free(datamodel, session);
    return TA_ERROR_NO_MEMORY;
}

static void ta_python_destroy(const ta_datamodel_call *call) {
    ta_python_session *session = ta_python_handle_remove(&TA_PYTHON_DATAMODEL(call)->sessions, call->instance);
    if (session) ta_python_session_free(TA_PYTHON_DATAMODEL(call), session);
}

/* ---- data, events, expressions ------------------------------------------------------------------ */

#define TA_PYTHON_EVALUATION(call, session)                                                        \
    (&(ta_python_evaluation){TA_PYTHON_DATAMODEL(call), (session), (call)->environment})

static ta_status ta_python_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_python_session *session = ta_python_session_of_call(call);
    ta_data_info data;
    if (!session || ta_image_data(session->chart->image, (ta_chart_item){session->chart->chart, declaration}, &data) !=
                        TA_OK)
        return TA_ERROR_EXECUTION;
    PyObject *id = PyUnicode_DecodeUTF8(data.id.pointer, (Py_ssize_t)data.id.length, "strict");
    if (!id) {
        PyErr_Clear();
        return TA_ERROR_EXECUTION;
    }
    PyObject *value = NULL;
    ta_status status = TA_OK;
    if (session->init && PyDict_GetItemRef(session->init, id, &value) < 0) PyErr_Clear(); /* the spawn's params win */
    if (!value && data.expression != TA_NIL)
        value = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), data.expression, &status);
    if (PyDict_SetItem(session->globals, id, value ? value : Py_None) < 0) {
        PyErr_Clear();
        status = TA_ERROR_EXECUTION;
    }
    Py_XDECREF(value);
    Py_DECREF(id);
    return status;
}

static ta_status ta_python_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    static const char *const types[] = {"platform", "internal", "external"};
    PyObject *event = PyStructSequence_New((PyTypeObject *)datamodel->state->event_type);
    if (!event) {
        PyErr_Clear();
        return TA_ERROR_NO_MEMORY;
    }
    PyObject *data = view->data ? ta_python_payload_object(datamodel, view->data) : NULL;
    PyObject *fields[8] = {
        PyUnicode_DecodeUTF8(view->name.pointer, (Py_ssize_t)view->name.length, "strict"),
        PyUnicode_FromString(types[view->type % 3]),
        ta_python_text_or_none(view->sendid),
        ta_python_text_or_none(view->origin),
        ta_python_text_or_none(view->origintype),
        ta_python_text_or_none(view->invokeid),
        Py_NewRef(data ? data : Py_None),
        ta_python_text_or_none(view->size >= sizeof *view ? view->raw : (ta_string_view){NULL, 0}),
    };
    int complete = 1;
    for (int i = 0; i < 8; i++) {
        if (!fields[i]) complete = 0;
        PyStructSequence_SetItem(event, i, fields[i] ? fields[i] : Py_NewRef(Py_None));
    }
    int memory = PyErr_ExceptionMatches(PyExc_MemoryError);
    PyErr_Clear();
    if (!complete) {
        ta_python_datamodel_defer(datamodel, event);
        return memory ? TA_ERROR_NO_MEMORY : TA_ERROR_INVALID;
    }
    PyObject *previous = session->system[TA_PYTHON_EVENT];
    if (PyDict_SetItemString(session->globals, "_event", event) < 0) {
        PyErr_Clear();
        ta_python_datamodel_defer(datamodel, event);
        return TA_ERROR_NO_MEMORY;
    }
    session->system[TA_PYTHON_EVENT] = event;
    ta_python_datamodel_defer(datamodel, previous);
    return TA_OK;
}

static ta_status ta_python_eval_condition(const ta_datamodel_call *call, ta_expression expression, int *out) {
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    ta_status status;
    PyObject *value = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), expression, &status);
    if (!value) return status;
    int truth = PyObject_IsTrue(value);
    Py_DECREF(value);
    if (truth < 0) {
        PyErr_Clear();
        return TA_ERROR_EXECUTION;
    }
    *out = truth;
    return TA_OK;
}

/* The value as text (str()), its UTF-8 valid until the next call. */
static ta_status ta_python_eval_text(const ta_datamodel_call *call, ta_expression expression, ta_string_view *out) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    ta_status status;
    PyObject *value = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), expression, &status);
    if (!value) return status;
    PyObject *text = PyObject_Str(value);
    ta_python_datamodel_defer(datamodel, value);
    Py_ssize_t length = 0;
    const char *utf8 = text ? PyUnicode_AsUTF8AndSize(text, &length) : NULL;
    if (!utf8) {
        PyErr_Clear();
        ta_python_datamodel_defer(datamodel, text);
        return TA_ERROR_EXECUTION;
    }
    ta_python_datamodel_defer(datamodel, datamodel->text);
    datamodel->text = text;
    *out = (ta_string_view){utf8, (uint32_t)length};
    return TA_OK;
}

static ta_status ta_python_assign(const ta_datamodel_call *call, ta_expression location, ta_expression value) {
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    ta_status status;
    PyObject *result = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), value, &status);
    if (!result) return status;
    status = ta_python_assign_to(TA_PYTHON_EVALUATION(call, session), &(ta_python_assignment){location, result, 0});
    Py_DECREF(result);
    return status;
}

static ta_status ta_python_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view value) {
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    PyObject *text = PyUnicode_DecodeUTF8(value.pointer, (Py_ssize_t)value.length, "strict");
    if (!text) {
        PyErr_Clear();
        return TA_ERROR_EXECUTION;
    }
    ta_status status =
        ta_python_assign_to(TA_PYTHON_EVALUATION(call, session), &(ta_python_assignment){location, text, 0});
    Py_DECREF(text);
    return status;
}

static ta_status ta_python_script(const ta_datamodel_call *call, ta_expression expression) {
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    return ta_python_execute(TA_PYTHON_EVALUATION(call, session), expression);
}

/* ---- <foreach> --------------------------------------------------------------------------------- */

/* The array (a list or a tuple), copied shallowly so the body may change
 * it (SCXML 4.6). */
static ta_status ta_python_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session) return TA_ERROR_EXECUTION;
    ta_status status;
    PyObject *value = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), array, &status);
    if (!value) return status;
    PyObject *items = PyList_Check(value) || PyTuple_Check(value) ? PySequence_List(value) : NULL;
    ta_python_datamodel_defer(datamodel, value);
    if (!items) {
        PyErr_Clear();
        return TA_ERROR_EXECUTION;
    }
    loop->n = (uint32_t)PyList_Size(items);
    loop->iter = ta_python_handle_add(&datamodel->loops, items);
    if (loop->iter) return TA_OK;
    ta_python_datamodel_defer(datamodel, items);
    return TA_ERROR_NO_MEMORY;
}

static ta_status ta_python_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_python_session *session = ta_python_session_of_call(call);
    ta_python_slot *slot = ta_python_handle_slot(&datamodel->loops, loop->iter);
    if (!session || !slot || loop->i >= (uint32_t)PyList_Size((PyObject *)slot->item)) return TA_ERROR_EXECUTION;
    PyObject *item = PyList_GetItem((PyObject *)slot->item, loop->i);
    ta_status status = ta_python_assign_to(TA_PYTHON_EVALUATION(call, session),
                                           &(ta_python_assignment){loop->item, item, 1});
    if (status == TA_OK && loop->index != TA_NIL) {
        PyObject *index = PyLong_FromUnsignedLong(loop->i);
        status = index ? ta_python_assign_to(TA_PYTHON_EVALUATION(call, session),
                                             &(ta_python_assignment){loop->index, index, 1})
                       : TA_ERROR_NO_MEMORY;
        Py_XDECREF(index);
    }
    PyErr_Clear();
    return status;
}

static void ta_python_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_python_datamodel_defer(datamodel, ta_python_handle_remove(&datamodel->loops, loop->iter));
}

/* ---- payloads ----------------------------------------------------------------------------------- */

/* The capture's value before its copy: <content>'s value, or a dict of the
 * namelist's locations and the params. */
static PyObject *ta_python_capture_value(const ta_datamodel_call *call, const ta_datamodel_capture *capture,
                                         ta_status *status) {
    ta_python_session *session = ta_python_session_of_call(call);
    *status = TA_ERROR_EXECUTION;
    if (!session) return NULL;
    if (capture->content != TA_NIL) return ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), capture->content, status);
    PyObject *map = PyDict_New();
    *status = map ? TA_OK : TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; map && i < capture->namelist_count; i++) {
        PyObject *value = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), capture->namelist[i], status);
        PyObject *name = ta_python_expression_source(session, capture->namelist[i]);
        PyObject *key = name ? PyObject_CallMethod(name, "strip", NULL) : NULL;
        if (!value || !key || PyDict_SetItem(map, key, value) < 0) Py_CLEAR(map);
        Py_XDECREF(key);
        Py_XDECREF(value);
    }
    for (uint32_t i = 0; map && i < capture->param_count; i++) {
        ta_string_view name = ta_image_string(session->chart->image, capture->params[2 * i]);
        PyObject *value = ta_python_evaluate(TA_PYTHON_EVALUATION(call, session), capture->params[2 * i + 1], status);
        PyObject *key = value ? PyUnicode_DecodeUTF8(name.pointer, (Py_ssize_t)name.length, "strict") : NULL;
        if (!value || !key || PyDict_SetItem(map, key, value) < 0) Py_CLEAR(map);
        Py_XDECREF(key);
        Py_XDECREF(value);
    }
    if (!map && *status == TA_OK) *status = TA_ERROR_EXECUTION;
    PyErr_Clear();
    return map;
}

static ta_status ta_python_capture(const ta_datamodel_call *call, const ta_datamodel_capture *capture, ta_payload *out) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_status status;
    PyObject *value = ta_python_capture_value(call, capture, &status);
    if (!value) return status;
    PyObject *copy = NULL;
    status = ta_python_values_copy(datamodel->state, value, &copy);
    ta_python_datamodel_defer(datamodel, value);
    if (status != TA_OK) return status == TA_ERROR_NO_MEMORY ? status : TA_ERROR_EXECUTION;
    return ta_python_payload_new(datamodel, copy, out);
}

static void ta_python_retain(void *data, ta_payload payload) {
    ta_python_slot *slot = ta_python_handle_slot(&((ta_python_datamodel *)data)->payloads, payload);
    if (slot) slot->count++;
}

/* The last release drops the value later, outside core (AGENTS.md, "Rules
 * the suite cannot see"): core calls release in ta_host_free, the ack,
 * requests and deliver too, where a finalizer must not reenter it. */
static void ta_python_release(void *data, ta_payload payload) {
    ta_python_datamodel *datamodel = (ta_python_datamodel *)data;
    ta_python_slot *slot = ta_python_handle_slot(&datamodel->payloads, payload);
    if (slot && --slot->count == 0) ta_python_datamodel_defer(datamodel, ta_python_handle_remove(&datamodel->payloads, payload));
}

static ta_status ta_python_to_wire(void *data, ta_payload payload, ta_buffer *out) {
    ta_python_datamodel *datamodel = (ta_python_datamodel *)data;
    PyObject *value = ta_python_payload_object(datamodel, payload);
    if (!value) return TA_ERROR_INVALID;
    ta_status status = ta_python_values_write(datamodel->state, value, out);
    PyErr_Clear();
    return status;
}

static ta_status ta_python_from_wire(void *data, ta_bytes cbor, ta_payload *out) {
    ta_python_datamodel *datamodel = (ta_python_datamodel *)data;
    PyObject *value = NULL;
    ta_status status = ta_python_values_read(datamodel->state, cbor, &value);
    PyErr_Clear();
    if (status != TA_OK) return status == TA_ERROR_NO_MEMORY ? status : TA_ERROR_INVALID;
    return ta_python_payload_new(datamodel, value, out);
}

/* actor.context: a copy of the variable, through the value table; never
 * __builtins__ (AGENTS.md, "Rules the suite cannot see"). */
static ta_status ta_python_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_python_datamodel *datamodel = TA_PYTHON_DATAMODEL(call);
    ta_python_session *session = ta_python_session_of_call(call);
    if (!session || (name.length == 12 && !memcmp(name.pointer, "__builtins__", 12))) return TA_ERROR_INVALID;
    PyObject *key = PyUnicode_DecodeUTF8(name.pointer, (Py_ssize_t)name.length, "strict"), *value = NULL;
    int found = key ? PyDict_GetItemRef(session->globals, key, &value) : -1;
    Py_XDECREF(key);
    PyErr_Clear();
    if (found <= 0) return TA_ERROR_INVALID;
    PyObject *copy = NULL;
    ta_status status = ta_python_values_copy(datamodel->state, value, &copy);
    Py_DECREF(value);
    if (status != TA_OK) return status;
    return ta_python_payload_new(datamodel, copy, out);
}

/* ---- the vtable ---------------------------------------------------------------------------------- */

static const ta_datamodel_vtable ta_python_vtable = {
    .size = sizeof(ta_datamodel_vtable),
    .flags = 0,
    .name = "python",
    .chart_attach = ta_python_chart_attach,
    .chart_detach = ta_python_chart_detach,
    .create = ta_python_create,
    .destroy = ta_python_destroy,
    .init_data = ta_python_init_data,
    .bind_event = ta_python_bind_event,
    .eval_condition = ta_python_eval_condition,
    .eval_string = ta_python_eval_text,
    .assign = ta_python_assign,
    .assign_string = ta_python_assign_string,
    .script = ta_python_script,
    .log = ta_python_eval_text,
    .foreach_begin = ta_python_foreach_begin,
    .foreach_item = ta_python_foreach_item,
    .foreach_end = ta_python_foreach_end,
    .capture = ta_python_capture,
    .retain = ta_python_retain,
    .release = ta_python_release,
    .to_wire = ta_python_to_wire,
    .from_wire = ta_python_from_wire,
    .variable = ta_python_variable,
};

ta_status ta_python_datamodel_payload(ta_python_datamodel *datamodel, PyObject *value, ta_payload *out) {
    PyObject *copy = NULL;
    ta_status status = ta_python_values_copy(datamodel->state, value, &copy);
    if (status != TA_OK) return status;
    return ta_python_payload_new(datamodel, copy, out);
}

PyObject *ta_python_datamodel_value(ta_python_datamodel *datamodel, ta_payload payload) {
    PyObject *value = ta_python_payload_object(datamodel, payload), *copy = NULL;
    if (!value) Py_RETURN_NONE;
    ta_status status = ta_python_values_copy(datamodel->state, value, &copy);
    if (status != TA_OK) ta_python_raise_status(datamodel->state, "to_wire", status);
    return copy;
}

const ta_datamodel_vtable *ta_python_datamodel_vtable(void) {
    return &ta_python_vtable;
}

ta_python_datamodel *ta_python_datamodel_new(const ta_python_state *state) {
    ta_python_datamodel *datamodel = PyMem_RawMalloc(sizeof *datamodel);
    if (!datamodel) return (ta_python_datamodel *)PyErr_NoMemory();
    memset(datamodel, 0, sizeof *datamodel);
    datamodel->state = state;
    PyObject *builtins = PyImport_ImportModule("builtins"), *json = PyImport_ImportModule("json");
    PyObject *capsule = PyCapsule_New(datamodel, NULL, NULL);
    datamodel->builtins = builtins ? Py_XNewRef(PyModule_GetDict(builtins)) : NULL;
    datamodel->json_loads = json ? PyObject_GetAttrString(json, "loads") : NULL;
    /* int('NaN') raises, so json.loads refuses the constants JSON has not */
    datamodel->refuse_constant = builtins ? PyObject_GetAttrString(builtins, "int") : NULL;
    datamodel->in = capsule ? PyCFunction_New(&ta_python_in_method, capsule) : NULL;
    datamodel->deferred = PyList_New(0);
    datamodel->ended = PyList_New(0);
    Py_XDECREF(builtins);
    Py_XDECREF(json);
    Py_XDECREF(capsule);
    if (!datamodel->builtins || !datamodel->json_loads || !datamodel->refuse_constant || !datamodel->in || !datamodel->deferred || !datamodel->ended) {
        ta_python_datamodel_free(datamodel);
        return NULL;
    }
    return datamodel;
}

void ta_python_datamodel_free(ta_python_datamodel *datamodel) {
    if (!datamodel) return;
    /* core destroyed every session and released every payload first
     * (ta_host_free); what a stale table still holds goes now */
    for (uint32_t i = 0; i < datamodel->payloads.count; i++)
        if (datamodel->payloads.slots[i].item) ta_python_datamodel_defer(datamodel, datamodel->payloads.slots[i].item);
    for (uint32_t i = 0; i < datamodel->loops.count; i++)
        if (datamodel->loops.slots[i].item) ta_python_datamodel_defer(datamodel, datamodel->loops.slots[i].item);
    for (uint32_t i = 0; i < datamodel->sessions.count; i++)
        if (datamodel->sessions.slots[i].item) ta_python_session_free(datamodel, datamodel->sessions.slots[i].item);
    ta_python_handles_free(&datamodel->payloads);
    ta_python_handles_free(&datamodel->loops);
    ta_python_handles_free(&datamodel->sessions);
    Py_CLEAR(datamodel->text);
    Py_CLEAR(datamodel->kept);
    ta_python_datamodel_settle(datamodel);
    PyErr_Clear();
    Py_CLEAR(datamodel->ended);
    Py_CLEAR(datamodel->deferred);
    Py_CLEAR(datamodel->builtins);
    Py_CLEAR(datamodel->json_loads);
    Py_CLEAR(datamodel->refuse_constant);
    Py_CLEAR(datamodel->in);
    PyMem_RawFree(datamodel);
}
