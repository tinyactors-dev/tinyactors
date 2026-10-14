/* The "python" datamodel's vtable (tinyactors/datamodel.h; AGENTS.md,
 * "The "python" datamodel"): a class per chart at chart_attach, an
 * instance per session, `_event` an immutable Event kept from bind_event
 * to the next one or destroy, payloads as rows of Python values, and
 * hibernation by the session row's handle. An expression that raises is
 * TA_ERROR_EXECUTION; a BaseException that is not an Exception is kept
 * and raised again after the run. */
#include "datamodel.h"
#include <string.h>

/* ---- the Event of _event ------------------------------------------------------------ */

typedef struct ta_python_event {
    PyObject_HEAD
    PyObject *name, *type, *sendid, *origin, *origintype, *invokeid, *data, *raw;
} ta_python_event;

static PyMemberDef ta_python_event_members[] = {
    {"name", Py_T_OBJECT_EX, offsetof(ta_python_event, name), Py_READONLY, "the event's name"},
    {"type", Py_T_OBJECT_EX, offsetof(ta_python_event, type), Py_READONLY, "\"platform\", \"internal\" or \"external\""},
    {"sendid", Py_T_OBJECT_EX, offsetof(ta_python_event, sendid), Py_READONLY, "the sendid, or None"},
    {"origin", Py_T_OBJECT_EX, offsetof(ta_python_event, origin), Py_READONLY, "the sender's address, or None"},
    {"origintype", Py_T_OBJECT_EX, offsetof(ta_python_event, origintype), Py_READONLY, "the origin's type, or None"},
    {"invokeid", Py_T_OBJECT_EX, offsetof(ta_python_event, invokeid), Py_READONLY, "the invocation's id, or None"},
    {"data", Py_T_OBJECT_EX, offsetof(ta_python_event, data), Py_READONLY, "the event's data, or None"},
    {"raw", Py_T_OBJECT_EX, offsetof(ta_python_event, raw), Py_READONLY, "the I/O processor's raw message, or None"},
    {NULL, 0, 0, 0, NULL}
};

static int ta_python_event_traverse(PyObject *self, visitproc visit, void *arg) {
    ta_python_event *event = (ta_python_event *)self;
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(event->data);
    return 0;
}

static int ta_python_event_clear(PyObject *self) {
    ta_python_event *event = (ta_python_event *)self;
    Py_CLEAR(event->name);
    Py_CLEAR(event->type);
    Py_CLEAR(event->sendid);
    Py_CLEAR(event->origin);
    Py_CLEAR(event->origintype);
    Py_CLEAR(event->invokeid);
    Py_CLEAR(event->data);
    Py_CLEAR(event->raw);
    return 0;
}

static void ta_python_event_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    ta_python_event_clear(self);
    freefunc ta_python_release = (freefunc)PyType_GetSlot(type, Py_tp_free);
    ta_python_release(self);
    Py_DECREF(type);
}

static PyType_Slot ta_python_event_slots[] = {
    {Py_tp_doc, "The current event of a \"python\" chart (_event), immutable."},
    {Py_tp_members, ta_python_event_members},
    {Py_tp_dealloc, ta_python_event_dealloc},
    {Py_tp_traverse, ta_python_event_traverse},
    {Py_tp_clear, ta_python_event_clear},
    {0, NULL}
};

static PyType_Spec ta_python_event_spec = {
    "tinyactors.Event", sizeof(ta_python_event), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    ta_python_event_slots
};

int ta_python_event_init_type(PyObject *module, ta_python_module_state *state) {
    state->event_type = PyType_FromModuleAndSpec(module, &ta_python_event_spec, NULL);
    return state->event_type ? PyModule_AddObjectRef(module, "Event", state->event_type) : -1;
}

/* ---- the datamodel of a host ------------------------------------------------------------ */

int ta_python_datamodel_init(ta_python_datamodel *datamodel, ta_python_host *host) {
    datamodel->host = host;
    datamodel->state = host->state;
    if (ta_python_rows_init(datamodel) < 0) return -1;
    return ta_python_namespace_init(datamodel);
}

void ta_python_datamodel_free(ta_python_datamodel *datamodel) {
    if (!datamodel->state) return;
    ta_python_rows_free(datamodel);
    Py_CLEAR(datamodel->namespace);
    Py_CLEAR(datamodel->raised);
    Py_CLEAR(datamodel->output);
    if (datamodel->wire.pointer) {
        const ta_allocator *allocator = &datamodel->host->allocator;
        allocator->free(allocator->context, datamodel->wire.pointer, datamodel->wire.capacity);
    }
    datamodel->wire = (ta_buffer){NULL, 0, 0};
    datamodel->state = NULL;
}

int ta_python_datamodel_traverse(ta_python_datamodel *datamodel, visitproc visit, void *arg) {
    Py_VISIT(datamodel->namespace);
    Py_VISIT(datamodel->raised);
    Py_VISIT(datamodel->output);
    return datamodel->state ? ta_python_rows_traverse(datamodel, visit, arg) : 0;
}

/* ---- an evaluation ------------------------------------------------------------------------ */

/* What an entry evaluates with: the session's instance and its chart's
 * functions, held (a row pointer would not survive an add that chart
 * code causes), and the environment In() reads. */
typedef struct ta_python_scope {
    ta_python_datamodel *datamodel;
    PyObject *instance;
    PyObject *functions;
    ta_python_session_row row; /* a copy, for its chart and flags */
    uint64_t handle;
    ta_datamodel_environment *saved;
} ta_python_scope;

static int ta_python_scope_enter(const ta_datamodel_call *call, ta_python_scope *scope) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    ta_python_chart_row *chart = row && row->instance ? ta_table_get(&datamodel->charts, row->chart) : NULL;
    if (!chart) return -1;
    scope->datamodel = datamodel;
    scope->row = *row;
    scope->handle = call->instance;
    scope->instance = Py_NewRef(row->instance);
    scope->functions = Py_NewRef(chart->functions);
    scope->saved = datamodel->environment;
    datamodel->environment = call->environment;
    if (call->environment) ta_datamodel_environment_charge(call->environment, 1); /* fuel once per evaluation */
    return 0;
}

static void ta_python_scope_leave(ta_python_scope *scope) {
    scope->datamodel->environment = scope->saved;
    Py_DECREF(scope->instance);
    Py_DECREF(scope->functions);
}

/* The exception set now as the entry's failure: error.execution for the
 * chart; a BaseException that is not an Exception (KeyboardInterrupt,
 * SystemExit) is kept for the driver, which raises the same object. */
static ta_status ta_python_failed(ta_python_datamodel *datamodel) {
    PyObject *raised = PyErr_GetRaisedException();
    if (raised && !PyErr_GivenExceptionMatches(raised, PyExc_Exception) && !datamodel->raised)
        datamodel->raised = Py_NewRef(raised);
    Py_XDECREF(raised);
    return TA_ERROR_EXECUTION;
}

/* The compiled form of expression `e` (borrowed), or NULL with the
 * exception that refused it set. */
static PyObject *ta_python_compiled(ta_python_scope *scope, ta_expression e) {
    if (e >= (ta_expression)PyTuple_Size(scope->functions)) {
        PyErr_SetString(PyExc_IndexError, "no such expression");
        return NULL;
    }
    PyObject *compiled = PyTuple_GetItem(scope->functions, e);
    if (PyExceptionInstance_Check(compiled)) { /* refused at chart_attach: error.execution now (W3C 312) */
        PyErr_SetObject((PyObject *)Py_TYPE(compiled), compiled);
        return NULL;
    }
    return compiled;
}

/* The value of expression `e`: a new reference, or NULL. */
static PyObject *ta_python_value(ta_python_scope *scope, ta_expression e) {
    PyObject *compiled = ta_python_compiled(scope, e);
    if (!compiled) return NULL;
    if (PyTuple_Check(compiled)) compiled = PyTuple_GetItem(compiled, 0); /* a location's getter */
    return PyObject_CallFunctionObjArgs(compiled, scope->instance, NULL);
}

/* Assign `value` (borrowed) to location `e`. */
static int ta_python_assign_value(ta_python_scope *scope, ta_expression e, PyObject *value) {
    PyObject *compiled = ta_python_compiled(scope, e);
    if (!compiled) return -1;
    if (!PyTuple_Check(compiled)) {
        PyErr_SetString(PyExc_TypeError, "not a location");
        return -1;
    }
    PyObject *done = PyObject_CallFunctionObjArgs(PyTuple_GetItem(compiled, 1), scope->instance, value, NULL);
    if (!done) return -1;
    Py_DECREF(done);
    return 0;
}

/* A string the datamodel answers (eval_string, log): the str's UTF-8,
 * held until the datamodel's next call. */
static ta_status ta_python_string_out(ta_python_datamodel *datamodel, PyObject *value, ta_string_view *out) {
    PyObject *text = value && PyUnicode_Check(value) ? Py_NewRef(value) : value ? PyObject_Str(value) : NULL;
    if (!text || ta_python_text(text, out) < 0) {
        Py_XDECREF(text);
        return ta_python_failed(datamodel);
    }
    ta_python_replace(&datamodel->output, text);
    return TA_OK;
}

/* ---- the vtable's entries ---------------------------------------------------------------- */

static ta_status ta_python_chart_attach(void *self, const ta_datamodel_chart *chart, void **context) {
    ta_python_datamodel *datamodel = self;
    ta_python_chart_row made = {NULL, NULL, NULL, 0, 0};
    if (ta_python_chart_compile(datamodel, &(ta_python_attach){chart->image, chart->chart}, &made) < 0) {
        PyErr_Clear(); /* a chart this datamodel cannot run: the image is refused */
        return TA_ERROR_INVALID;
    }
    uint64_t handle = 0;
    ta_python_chart_row *row = ta_table_add(&datamodel->charts, &handle);
    if (!row) {
        ta_python_drop(datamodel, made.chart);
        ta_python_drop(datamodel, made.functions);
        ta_python_drop(datamodel, made.data);
        return TA_ERROR_NO_MEMORY;
    }
    *row = made;
    *context = (void *)(uintptr_t)handle; /* the row's handle, never a pointer */
    return TA_OK;
}

static void ta_python_chart_detach(void *self, void *context) {
    ta_python_datamodel *datamodel = self;
    uint64_t handle = (uint64_t)(uintptr_t)context;
    ta_python_chart_row *row = ta_table_get(&datamodel->charts, handle);
    if (!row) return;
    ta_python_chart_row kept = *row;
    ta_table_remove(&datamodel->charts, handle);
    ta_python_drop(datamodel, kept.chart);
    ta_python_drop(datamodel, kept.functions);
    ta_python_drop(datamodel, kept.data);
}

static PyObject *ta_python_view_str(ta_string_view text) {
    return PyUnicode_DecodeUTF8(text.pointer ? text.pointer : "", text.length, "strict");
}

static int ta_python_ends_with(ta_string_view text, const char *suffix) {
    size_t length = strlen(suffix);
    return text.length >= length && !memcmp(text.pointer + text.length - length, suffix, length);
}

/* _ioprocessors: {type: {"location": ...}}, with the short names the W3C
 * tests use, as the ECMAScript datamodel has them. */
static PyObject *ta_python_io_processors(const ta_datamodel_system_variables *variables) {
    PyObject *processors = PyDict_New();
    for (uint32_t i = 0; processors && i < variables->io_processor_count; i++) {
        const ta_io_processor_entry *entry = &variables->io_processors[i];
        PyObject *location = ta_python_view_str(entry->location), *type = ta_python_view_str(entry->type);
        PyObject *value = location ? Py_BuildValue("{s:O}", "location", location) : NULL;
        int status = value && type && PyDict_SetItem(processors, type, value) == 0 ? 0 : -1;
        if (status == 0 && ta_python_ends_with(entry->type, "#SCXMLEventProcessor"))
            status = PyDict_SetItemString(processors, "scxml", value);
        if (status == 0 && ta_python_ends_with(entry->type, "#BasicHTTPEventProcessor"))
            status = PyDict_SetItemString(processors, "basichttp", value);
        Py_XDECREF(location);
        Py_XDECREF(type);
        Py_XDECREF(value);
        if (status < 0) Py_CLEAR(processors);
    }
    return processors;
}

/* The system variables a chart reads, made once at create (core's views
 * end with the call): (_sessionid, _name, _ioprocessors), None for the
 * ones it does not read; None when it reads none. */
static PyObject *ta_python_system(const ta_python_chart_row *chart, const ta_datamodel_system_variables *variables) {
    if (!chart->reads) return Py_NewRef(Py_None);
    PyObject *sessionid = chart->reads & TA_PYTHON_READS_SESSIONID ? ta_python_view_str(variables->sessionid) : Py_NewRef(Py_None);
    PyObject *name = chart->reads & TA_PYTHON_READS_NAME ? ta_python_view_str(variables->name) : Py_NewRef(Py_None);
    PyObject *processors = chart->reads & TA_PYTHON_READS_IOPROCESSORS ? ta_python_io_processors(variables) : Py_NewRef(Py_None);
    PyObject *system = sessionid && name && processors ? PyTuple_Pack(3, sessionid, name, processors) : NULL;
    Py_XDECREF(sessionid);
    Py_XDECREF(name);
    Py_XDECREF(processors);
    return system;
}

/* An instance of the chart's class: every <data> id None (unbound until
 * init_data), no _event yet, and the system variables. */
static PyObject *ta_python_instance(ta_python_datamodel *datamodel, const ta_python_chart_row *chart,
                                    const ta_datamodel_system_variables *variables) {
    (void)datamodel;
    PyObject *instance = PyObject_CallNoArgs(chart->chart);
    PyObject *system = instance ? ta_python_system(chart, variables) : NULL;
    int status = system && PyObject_SetAttrString(instance, "_ta_sys", system) == 0 &&
                         PyObject_SetAttrString(instance, "_event", Py_None) == 0
                     ? 0
                     : -1;
    for (Py_ssize_t i = 0; status == 0 && i < PyTuple_Size(chart->data); i++)
        status = PyObject_SetAttr(instance, PyTuple_GetItem(chart->data, i), Py_None);
    Py_XDECREF(system);
    if (status < 0) Py_CLEAR(instance);
    return instance;
}

static ta_status ta_python_create(void *self, const ta_datamodel_start *start, uint64_t *out) {
    ta_python_datamodel *datamodel = self;
    PyObject *init = start->init ? ta_python_payload_value(datamodel, start->init) : NULL;
    Py_XINCREF(init);
    ta_python_payload_release(datamodel, start->init); /* the init payload's reference is ours */
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, (uint64_t)(uintptr_t)start->chart_context);
    PyObject *instance = chart ? ta_python_instance(datamodel, chart, start->system_variables) : NULL;
    uint64_t chart_handle = (uint64_t)(uintptr_t)start->chart_context;
    if (!instance) {
        ta_python_drop(datamodel, init);
        return ta_python_failed(datamodel);
    }
    uint64_t handle = 0;
    ta_python_session_row *row = ta_table_add(&datamodel->sessions, &handle);
    if (!row) {
        ta_python_drop(datamodel, instance);
        ta_python_drop(datamodel, init);
        return TA_ERROR_NO_MEMORY;
    }
    row->instance = instance;
    row->chart = chart_handle;
    row->extra = init;
    *out = handle;
    return TA_OK;
}

static void ta_python_destroy(const ta_datamodel_call *call) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    if (!row) return;
    if (row->flags & TA_PYTHON_SESSION_SERIALIZED) { /* just hibernated: the row stays, the handle is its blob */
        row->flags = TA_PYTHON_SESSION_HIBERNATED;
        return;
    }
    ta_python_session_row kept = *row;
    ta_table_remove(&datamodel->sessions, call->instance);
    ta_python_drop(datamodel, kept.instance);
    ta_python_drop(datamodel, kept.extra);
}

static ta_status ta_python_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_datamodel *datamodel = scope.datamodel;
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, scope.row.chart);
    PyObject *id = chart && declaration < (uint32_t)PyTuple_Size(chart->data) ? Py_NewRef(PyTuple_GetItem(chart->data, declaration)) : NULL;
    ta_data_info info;
    ta_status status = TA_OK;
    PyObject *value = NULL;
    if (!id || !call->environment ||
        ta_image_data(ta_datamodel_environment_image(call->environment),
                      (ta_chart_item){ta_datamodel_environment_chart(call->environment), declaration}, &info) != TA_OK) {
        status = TA_ERROR_EXECUTION;
    } else {
        PyObject *given = NULL; /* the spawn's or invoke's params override <data> of their names */
        if (scope.row.extra && PyDict_Check(scope.row.extra) && PyDict_GetItemRef(scope.row.extra, id, &given) < 0) PyErr_Clear();
        if (given) value = given;
        else if (info.expression != TA_NIL) value = ta_python_value(&scope, info.expression);
        else value = Py_NewRef(Py_None);
        if (!value) status = ta_python_failed(datamodel);
    }
    if (id && PyObject_SetAttr(scope.instance, id, value ? value : Py_None) < 0 && status == TA_OK)
        status = ta_python_failed(datamodel);
    Py_XDECREF(value);
    Py_XDECREF(id);
    ta_python_scope_leave(&scope);
    return status;
}

static PyObject *ta_python_optional_str(ta_string_view text) {
    return text.length ? ta_python_view_str(text) : Py_NewRef(Py_None);
}

static ta_status ta_python_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    if (!row || !row->instance) return TA_ERROR_EXECUTION;
    PyObject *instance = Py_NewRef(row->instance);
    if (row->extra) { /* the params were read by init_data: an event has come */
        ta_python_drop(datamodel, row->extra);
        row->extra = NULL;
    }
    ta_python_event *event = (ta_python_event *)PyType_GenericAlloc((PyTypeObject *)datamodel->state->event_type, 0);
    ta_status status = TA_OK;
    if (event) {
        PyObject *data = view->data ? ta_python_payload_value(datamodel, view->data) : NULL;
        event->name = ta_python_view_str(view->name);
        event->type = Py_NewRef(PyTuple_GetItem(datamodel->state->event_kinds, view->type % 3));
        event->sendid = ta_python_optional_str(view->sendid);
        event->origin = ta_python_optional_str(view->origin);
        event->origintype = ta_python_optional_str(view->origintype);
        event->invokeid = ta_python_optional_str(view->invokeid);
        event->data = Py_NewRef(data ? data : Py_None); /* the payload's value itself (held by the Event) */
        event->raw = view->size >= sizeof *view ? ta_python_optional_str(view->raw) : Py_NewRef(Py_None);
        if (!event->name || !event->sendid || !event->origin || !event->origintype || !event->invokeid || !event->raw ||
            PyObject_SetAttrString(instance, "_event", (PyObject *)event) < 0)
            status = ta_python_failed(datamodel);
        Py_DECREF(event); /* the instance holds it now */
    } else status = ta_python_failed(datamodel);
    Py_DECREF(instance);
    return status;
}

/* SCXML keeps _event bound to the last event (5.10): nothing to drop. */
static void ta_python_detach_event(const ta_datamodel_call *call) {
    (void)call;
}

static ta_status ta_python_eval_condition(const ta_datamodel_call *call, ta_expression e, int *out) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    int truth = value ? PyObject_IsTrue(value) : -1;
    Py_XDECREF(value);
    ta_status status = truth < 0 ? ta_python_failed(scope.datamodel) : TA_OK;
    if (truth >= 0) *out = truth;
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_eval_string(const ta_datamodel_call *call, ta_expression e, ta_string_view *out) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    ta_status status = value ? ta_python_string_out(scope.datamodel, value, out) : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_assign(const ta_datamodel_call *call, ta_expression location, ta_expression e) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    ta_status status = value && ta_python_assign_value(&scope, location, value) == 0 ? TA_OK : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view text) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_view_str(text);
    ta_status status = value && ta_python_assign_value(&scope, location, value) == 0 ? TA_OK : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_script(const ta_datamodel_call *call, ta_expression e) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    PyObject *value = ta_python_value(&scope, e);
    ta_status status = value ? TA_OK : ta_python_failed(scope.datamodel);
    Py_XDECREF(value);
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_log(const ta_datamodel_call *call, ta_expression e, ta_string_view *out) {
    return ta_python_eval_string(call, e, out);
}

static ta_status ta_python_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_datamodel *datamodel = scope.datamodel;
    PyObject *value = ta_python_value(&scope, array);
    PyObject *items = value && (PyList_Check(value) || PyTuple_Check(value)) ? PySequence_Tuple(value) : NULL;
    if (value && !items && !PyErr_Occurred()) PyErr_SetString(PyExc_TypeError, "<foreach> takes a list or a tuple");
    Py_XDECREF(value);
    ta_status status = TA_OK;
    if (!items || PyTuple_Size(items) > (Py_ssize_t)INT32_MAX) {
        Py_XDECREF(items);
        status = ta_python_failed(datamodel);
    } else {
        uint64_t handle = 0;
        ta_python_loop_row *row = ta_table_add(&datamodel->loops, &handle);
        if (!row) {
            ta_python_drop(datamodel, items);
            status = TA_ERROR_NO_MEMORY;
        } else {
            row->items = items; /* a shallow copy, as the ECMAScript datamodel takes */
            loop->iter = handle;
            loop->n = (uint32_t)PyTuple_Size(items);
        }
    }
    ta_python_scope_leave(&scope);
    return status;
}

static ta_status ta_python_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_loop_row *row = ta_table_get(&scope.datamodel->loops, loop->iter);
    PyObject *items = row ? Py_NewRef(row->items) : NULL;
    int status = items && loop->i < (uint32_t)PyTuple_Size(items) ? 0 : -1;
    if (status == 0) status = ta_python_assign_value(&scope, loop->item, PyTuple_GetItem(items, loop->i));
    if (status == 0 && loop->index != TA_NIL) {
        PyObject *index = PyLong_FromUnsignedLong(loop->i);
        status = index ? ta_python_assign_value(&scope, loop->index, index) : -1;
        Py_XDECREF(index);
    }
    Py_XDECREF(items);
    ta_status result = status == 0 ? TA_OK : ta_python_failed(scope.datamodel);
    ta_python_scope_leave(&scope);
    return result;
}

static void ta_python_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_loop_row *row = ta_table_get(&datamodel->loops, loop->iter);
    if (!row) return;
    PyObject *items = row->items;
    ta_table_remove(&datamodel->loops, loop->iter);
    ta_python_drop(datamodel, items);
}

/* A namelist entry's key: its location's text. */
static PyObject *ta_python_location_name(const ta_datamodel_call *call, ta_expression location) {
    ta_expression_info info;
    if (!call->environment ||
        ta_image_expression(ta_datamodel_environment_image(call->environment),
                            (ta_chart_item){ta_datamodel_environment_chart(call->environment), location}, &info) != TA_OK) {
        PyErr_SetString(PyExc_RuntimeError, "no such location");
        return NULL;
    }
    PyObject *source = ta_python_view_str(info.source);
    PyObject *name = source ? PyObject_CallMethod(source, "strip", NULL) : NULL;
    Py_XDECREF(source);
    return name;
}

/* What a <send>, <invoke> or <donedata> captures, copied: actors share no
 * mutable value (bindings.md, section 8). */
static ta_status ta_python_capture(const ta_datamodel_call *call, const ta_datamodel_capture *capture, ta_payload *out) {
    ta_python_scope scope;
    if (ta_python_scope_enter(call, &scope) < 0) return TA_ERROR_EXECUTION;
    ta_python_datamodel *datamodel = scope.datamodel;
    PyObject *captured = NULL;
    if (capture->content != TA_NIL) {
        PyObject *value = ta_python_value(&scope, capture->content);
        captured = value ? ta_python_value_copy(datamodel->state, value) : NULL;
        Py_XDECREF(value);
    } else {
        captured = PyDict_New();
        const ta_image *image = call->environment ? ta_datamodel_environment_image(call->environment) : NULL;
        for (uint32_t i = 0; captured && i < capture->namelist_count; i++) {
            PyObject *name = ta_python_location_name(call, capture->namelist[i]);
            PyObject *value = name ? ta_python_value(&scope, capture->namelist[i]) : NULL;
            PyObject *copy = value ? ta_python_value_copy(datamodel->state, value) : NULL;
            if (!copy || PyDict_SetItem(captured, name, copy) < 0) Py_CLEAR(captured);
            Py_XDECREF(name);
            Py_XDECREF(value);
            Py_XDECREF(copy);
        }
        for (uint32_t i = 0; captured && i < capture->param_count; i++) {
            PyObject *name = image ? ta_python_view_str(ta_image_string(image, capture->params[2 * i])) : NULL;
            PyObject *value = name ? ta_python_value(&scope, capture->params[2 * i + 1]) : NULL;
            PyObject *copy = value ? ta_python_value_copy(datamodel->state, value) : NULL;
            if (!copy || PyDict_SetItem(captured, name, copy) < 0) Py_CLEAR(captured);
            Py_XDECREF(name);
            Py_XDECREF(value);
            Py_XDECREF(copy);
        }
    }
    ta_status status = TA_OK;
    if (!captured) status = ta_python_failed(datamodel);
    else if (ta_python_payload_add(datamodel, captured, out) < 0) {
        PyErr_Clear();
        status = TA_ERROR_NO_MEMORY;
    }
    ta_python_scope_leave(&scope);
    return status;
}

static void ta_python_retain(void *self, ta_payload payload) {
    ta_python_payload_retain(self, payload);
}

static void ta_python_release(void *self, ta_payload payload) {
    ta_python_payload_release(self, payload);
}

static ta_status ta_python_custom(const ta_datamodel_call *call, uint32_t definition) {
    (void)call;
    (void)definition;
    return TA_ERROR_UNSUPPORTED;
}

/* Hibernation keeps the handle: the blob is the session row's handle; the
 * instance stays on the Python heap (AGENTS.md). */
static ta_status ta_python_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    if (!row || !row->instance) return TA_ERROR_INVALID;
    row->flags |= TA_PYTHON_SESSION_SERIALIZED; /* destroy comes next and keeps the row */
    datamodel->serialized = call->instance;
    *out = (ta_bytes){&datamodel->serialized, sizeof datamodel->serialized};
    return TA_OK;
}

/* The bindings of the old version that the new chart does not declare,
 * and the instance of the new class with the slots both declare. */
static int ta_python_migrate_instance(ta_python_datamodel *datamodel, ta_python_session_row *row,
                                      const ta_datamodel_start *start) {
    ta_python_chart_row *chart = ta_table_get(&datamodel->charts, (uint64_t)(uintptr_t)start->chart_context);
    ta_python_chart_row *old = ta_table_get(&datamodel->charts, row->chart);
    PyObject *instance = chart ? ta_python_instance(datamodel, chart, start->system_variables) : NULL;
    PyObject *bindings = instance ? PyDict_New() : NULL;
    int status = bindings ? 0 : -1;
    for (Py_ssize_t i = 0; status == 0 && old && i < PyTuple_Size(old->data); i++) {
        PyObject *id = PyTuple_GetItem(old->data, i), *value = PyObject_GetAttr(row->instance, id);
        int shared = value ? PySequence_Contains(chart->data, id) : -1;
        if (shared < 0 || PyDict_SetItem(bindings, id, value) < 0) status = -1;
        else if (shared == 1) status = PyObject_SetAttr(instance, id, value);
        Py_XDECREF(value);
    }
    PyObject *event = status == 0 ? PyObject_GetAttrString(row->instance, "_event") : NULL;
    if (!event || PyObject_SetAttrString(instance, "_event", event) < 0) status = -1;
    Py_XDECREF(event);
    if (status < 0) {
        Py_XDECREF(instance);
        Py_XDECREF(bindings);
        return -1;
    }
    ta_python_drop(datamodel, row->instance);
    ta_python_drop(datamodel, row->extra);
    row->instance = instance;
    row->chart = (uint64_t)(uintptr_t)start->chart_context;
    row->extra = bindings;
    return 0;
}

static ta_status ta_python_deserialize(void *self, const ta_datamodel_start *start, uint64_t *out) {
    ta_python_datamodel *datamodel = self;
    uint64_t handle = 0;
    if (start->state.length != sizeof handle) return TA_ERROR_INVALID;
    memcpy(&handle, start->state.pointer, sizeof handle);
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, handle);
    if (!row || !(row->flags & TA_PYTHON_SESSION_HIBERNATED)) return TA_ERROR_INVALID;
    if (row->chart != (uint64_t)(uintptr_t)start->chart_context && ta_python_migrate_instance(datamodel, row, start) < 0) {
        ta_python_failed(datamodel);
        return TA_ERROR_INVALID;
    }
    row = ta_table_get(&datamodel->sessions, handle);
    row->flags = 0;
    *out = handle;
    return TA_OK;
}

static ta_status ta_python_migrate(const ta_datamodel_call *call, ta_payload *bindings) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    *bindings = 0;
    if (!row || !row->extra) return TA_OK;
    PyObject *taken = row->extra;
    row->extra = NULL;
    return ta_python_payload_add(datamodel, taken, bindings) < 0 ? (PyErr_Clear(), TA_ERROR_NO_MEMORY) : TA_OK;
}

/* A payload as CBOR, in a block of the host's allocator kept between
 * calls: a view until the datamodel's next call. */
static ta_status ta_python_to_wire(void *self, ta_payload payload, ta_bytes *out) {
    ta_python_datamodel *datamodel = self;
    PyObject *value = ta_python_payload_value(datamodel, payload);
    if (!value) return TA_ERROR_INVALID;
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer, &(ta_python_room){{datamodel->wire.pointer, datamodel->wire.capacity, 0}, &datamodel->host->allocator});
    Py_INCREF(value);
    int written = ta_python_value_write(datamodel->state, &writer, value);
    Py_DECREF(value);
    ta_status status = written < 0 ? TA_ERROR_INVALID : ta_cbor_writer_bytes(&writer, out);
    if (written < 0) PyErr_Clear();
    if (status != TA_OK) {
        ta_cbor_writer_free(&writer); /* a block it grew, never our room */
        return status == TA_ERROR_NO_MEMORY ? status : TA_ERROR_INVALID;
    }
    if (writer.bytes != (unsigned char *)datamodel->wire.pointer) { /* it grew: the block is the room now */
        const ta_allocator *allocator = &datamodel->host->allocator;
        if (datamodel->wire.pointer) allocator->free(allocator->context, datamodel->wire.pointer, datamodel->wire.capacity);
        datamodel->wire = (ta_buffer){writer.bytes, writer.capacity, 0};
    }
    return TA_OK;
}

static ta_status ta_python_from_wire(void *self, ta_bytes cbor, ta_payload *out) {
    ta_python_datamodel *datamodel = self;
    PyObject *value = ta_python_value_read(datamodel->state, cbor);
    if (!value) {
        PyErr_Clear();
        return TA_ERROR_INVALID;
    }
    if (ta_python_payload_add(datamodel, value, out) < 0) {
        PyErr_Clear();
        return TA_ERROR_NO_MEMORY;
    }
    return TA_OK;
}

static ta_status ta_python_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_python_datamodel *datamodel = call->datamodel;
    ta_python_session_row *row = ta_table_get(&datamodel->sessions, call->instance);
    ta_python_chart_row *chart = row && row->instance ? ta_table_get(&datamodel->charts, row->chart) : NULL;
    PyObject *id = chart ? ta_python_view_str(name) : NULL;
    int declared = id ? PySequence_Contains(chart->data, id) : 0;
    PyObject *value = declared == 1 ? PyObject_GetAttr(row->instance, id) : NULL;
    PyObject *copy = value ? ta_python_value_copy(datamodel->state, value) : NULL;
    Py_XDECREF(id);
    Py_XDECREF(value);
    if (!copy) {
        PyErr_Clear();
        return TA_ERROR_INVALID;
    }
    if (ta_python_payload_add(datamodel, copy, out) < 0) {
        PyErr_Clear();
        return TA_ERROR_NO_MEMORY;
    }
    return TA_OK;
}

static const ta_datamodel_vtable ta_python_vtable = {
    .size = sizeof(ta_datamodel_vtable),
    .flags = TA_DATAMODEL_EVENT_DETACHABLE,
    .name = "python",
    .chart_attach = ta_python_chart_attach,
    .chart_detach = ta_python_chart_detach,
    .create = ta_python_create,
    .destroy = ta_python_destroy,
    .init_data = ta_python_init_data,
    .bind_event = ta_python_bind_event,
    .detach_event = ta_python_detach_event,
    .eval_condition = ta_python_eval_condition,
    .eval_string = ta_python_eval_string,
    .assign = ta_python_assign,
    .assign_string = ta_python_assign_string,
    .script = ta_python_script,
    .log = ta_python_log,
    .foreach_begin = ta_python_foreach_begin,
    .foreach_item = ta_python_foreach_item,
    .foreach_end = ta_python_foreach_end,
    .capture = ta_python_capture,
    .retain = ta_python_retain,
    .release = ta_python_release,
    .custom = ta_python_custom,
    .serialize = ta_python_serialize,
    .deserialize = ta_python_deserialize,
    .migrate = ta_python_migrate,
    .to_wire = ta_python_to_wire,
    .from_wire = ta_python_from_wire,
    .payload_serialize = NULL, /* export of a "python" actor is not supported (AGENTS.md) */
    .payload_deserialize = NULL,
    .variable = ta_python_variable,
};

const ta_datamodel_vtable *ta_python_datamodel_vtable(void) {
    return &ta_python_vtable;
}
