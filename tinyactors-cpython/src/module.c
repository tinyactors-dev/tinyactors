/* module.c — the module `tinyactors`: its state, its types and functions,
 * made once per interpreter (multi-phase initialization, PEP 489). */
#include "extension.h"

ta_python_state *ta_python_state_of(PyObject *module) {
    return (ta_python_state *)PyModule_GetState(module);
}

/* ---- errors ------------------------------------------------------------------------ */

/* Core's statuses (tinyactors/types.h), by name: the module's constants,
 * and the message of an error with a bare status. */
typedef struct ta_python_status_name {
    ta_status status;
    const char *name;
} ta_python_status_name;

static const ta_python_status_name ta_python_status_names[] = {
    {TA_OK, "OK"},
    {TA_ERROR_EXECUTION, "ERROR_EXECUTION"},
    {TA_ERROR_COMMUNICATION, "ERROR_COMMUNICATION"},
    {TA_ERROR_INVALID, "ERROR_INVALID"},
    {TA_ERROR_NO_MEMORY, "ERROR_NO_MEMORY"},
    {TA_ERROR_STALE, "ERROR_STALE"},
    {TA_ERROR_STATE, "ERROR_STATE"},
    {TA_ERROR_UNSUPPORTED, "ERROR_UNSUPPORTED"},
    {TA_ERROR_FATAL, "ERROR_FATAL"},
    {TA_ERROR_ABORTED, "ERROR_ABORTED"},
};

static const char *ta_python_status_name_of(ta_status status) {
    for (size_t i = 0; i < sizeof ta_python_status_names / sizeof ta_python_status_names[0]; i++)
        if (ta_python_status_names[i].status == status) return ta_python_status_names[i].name;
    return "an unknown status";
}

/* The error with `message`, and its attributes from `fields` (NULL: None). */
typedef struct ta_python_error_fields {
    PyObject *status, *code, *reason, *path;
} ta_python_error_fields;

static void ta_python_raise(const ta_python_state *state, PyObject *message, const ta_python_error_fields *fields) {
    PyObject *error = message ? PyObject_CallFunctionObjArgs(state->error_type, message, NULL) : NULL;
    if (error) {
        PyObject *const values[] = {fields->status, fields->code, fields->reason, fields->path};
        static const char *const names[] = {"status", "code", "reason", "path"};
        for (size_t i = 0; i < 4; i++)
            if (PyObject_SetAttrString(error, names[i], values[i] ? values[i] : Py_None) < 0) Py_CLEAR(error);
    }
    if (error) PyErr_SetObject(state->error_type, error);
    Py_XDECREF(error);
}

void ta_python_raise_status(const ta_python_state *state, const char *function, ta_status status) {
    PyObject *message = PyUnicode_FromFormat("%s: TA_%s", function, ta_python_status_name_of(status));
    PyObject *number = PyLong_FromLong(status);
    if (number) ta_python_raise(state, message, &(ta_python_error_fields){number, NULL, NULL, NULL});
    Py_XDECREF(message);
    Py_XDECREF(number);
}

/* The member `name` of `map`, a borrowed reference or NULL. */
static PyObject *ta_python_member(PyObject *map, const char *name) {
    PyObject *value = NULL;
    if (!map || !PyDict_Check(map) || PyDict_GetItemStringRef(map, name, &value) <= 0) return NULL;
    Py_DECREF(value); /* the map holds it */
    return value;
}

void ta_python_raise_reply(const ta_python_state *state, const ta_python_refusal *refusal) {
    PyObject *data = ta_python_member(refusal->error, "data");
    PyObject *message = ta_python_member(refusal->error, "message");
    PyObject *status = refusal->status != TA_OK ? PyLong_FromLong(refusal->status) : NULL;
    ta_python_error_fields fields = {status, ta_python_member(refusal->error, "code"), ta_python_member(data, "reason"),
                                     ta_python_member(data, "path")};
    PyObject *text = message ? PyObject_Str(message) : PyObject_Repr(refusal->error);
    ta_python_raise(state, text, &fields);
    Py_XDECREF(text);
    Py_XDECREF(status);
}

/* ---- the module --------------------------------------------------------------------- */

static PyMethodDef ta_python_module_methods[] = {
    {"encode", (PyCFunction)ta_python_encode, METH_O,
     "encode(value) -> bytes\n\nThe value as one CBOR item, by the value table (docs/values.md)."},
    {"decode", (PyCFunction)ta_python_decode, METH_O,
     "decode(data) -> object\n\nOne CBOR item as a Python value, by the value table (docs/values.md)."},
    {NULL, NULL, 0, NULL},
};

static int ta_python_module_exec(PyObject *module) {
    ta_python_state *state = ta_python_state_of(module);
    static const char *const attributes[] = {"status", "code", "reason", "path"};
    PyObject *defaults = PyDict_New();
    for (size_t i = 0; defaults && i < 4; i++)
        if (PyDict_SetItemString(defaults, attributes[i], Py_None) < 0) Py_CLEAR(defaults);
    state->error_type = defaults ? PyErr_NewExceptionWithDoc("tinyactors.TinyactorsError",
                                                             "What core refused: status, or code, reason and path.",
                                                             NULL, defaults)
                                 : NULL;
    Py_XDECREF(defaults);
    if (!state->error_type || PyModule_AddObjectRef(module, "TinyactorsError", state->error_type) < 0) return -1;
    for (size_t i = 0; i < sizeof ta_python_status_names / sizeof ta_python_status_names[0]; i++)
        if (PyModule_AddIntConstant(module, ta_python_status_names[i].name, ta_python_status_names[i].status) < 0)
            return -1;
    /* core's TA_ROUTE_* (host.h), what Host.deliver returns */
    static const ta_python_status_name routes[] = {{TA_ROUTE_DELIVERED, "ROUTE_DELIVERED"},
                                                   {TA_ROUTE_RELAYED, "ROUTE_RELAYED"},
                                                   {TA_ROUTE_DUPLICATE, "ROUTE_DUPLICATE"},
                                                   {TA_ROUTE_REFUSED, "ROUTE_REFUSED"}};
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        if (PyModule_AddIntConstant(module, routes[i].name, routes[i].status) < 0) return -1;
    state->time = PyImport_ImportModule("time");
    if (!state->time || ta_python_values_init(module, state) < 0 || ta_python_host_init_type(module, state) < 0 ||
        ta_python_clock_init_type(module, state) < 0)
        return -1;
    return 0;
}

/* Py_VISIT names its parameters visit and arg (Include/objimpl.h). */
static int ta_python_module_traverse(PyObject *module, visitproc visit, void *arg) {
    ta_python_state *state = ta_python_state_of(module);
    Py_VISIT(state->error_type);
    Py_VISIT(state->address_type);
    Py_VISIT(state->undefined_type);
    Py_VISIT(state->undefined);
    Py_VISIT(state->host_type);
    Py_VISIT(state->clock_type);
    Py_VISIT(state->time);
    return 0;
}

static int ta_python_module_clear(PyObject *module) {
    ta_python_state *state = ta_python_state_of(module);
    Py_CLEAR(state->error_type);
    Py_CLEAR(state->address_type);
    Py_CLEAR(state->undefined_type);
    Py_CLEAR(state->undefined);
    Py_CLEAR(state->host_type);
    Py_CLEAR(state->clock_type);
    Py_CLEAR(state->time);
    return 0;
}

static void ta_python_module_free(void *module) {
    ta_python_module_clear((PyObject *)module);
}

static PyModuleDef_Slot ta_python_module_slots[] = {
    {Py_mod_exec, (void *)ta_python_module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL},
};

static PyModuleDef ta_python_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "tinyactors",
    .m_doc = "SCXML statecharts as actors: the tinyactors host, with chart logic in Python.",
    .m_size = sizeof(ta_python_state),
    .m_methods = ta_python_module_methods,
    .m_slots = ta_python_module_slots,
    .m_traverse = ta_python_module_traverse,
    .m_clear = ta_python_module_clear,
    .m_free = ta_python_module_free,
};

/* CPython's import finds the module by this name (PEP 489). */
PyMODINIT_FUNC PyInit_tinyactors(void) {
    return PyModuleDef_Init(&ta_python_module);
}
