/* module.c — the module `tinyactors`: its state, its types and functions,
 * made once per interpreter (multi-phase initialization, PEP 489). */
#include "extension.h"

ta_python_state *ta_python_state_of(PyObject *module) {
    return (ta_python_state *)PyModule_GetState(module);
}

static int ta_python_module_exec(PyObject *module) {
    ta_python_state *state = ta_python_state_of(module);
    state->error_type = PyErr_NewException("tinyactors.TinyactorsError", NULL, NULL);
    if (!state->error_type || PyModule_AddObjectRef(module, "TinyactorsError", state->error_type) < 0) return -1;
    return 0;
}

/* Py_VISIT names its parameters visit and arg (Include/objimpl.h). */
static int ta_python_module_traverse(PyObject *module, visitproc visit, void *arg) {
    ta_python_state *state = ta_python_state_of(module);
    Py_VISIT(state->error_type);
    return 0;
}

static int ta_python_module_clear(PyObject *module) {
    ta_python_state *state = ta_python_state_of(module);
    Py_CLEAR(state->error_type);
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
    .m_slots = ta_python_module_slots,
    .m_traverse = ta_python_module_traverse,
    .m_clear = ta_python_module_clear,
    .m_free = ta_python_module_free,
};

/* CPython's import finds the module by this name (PEP 489). */
PyMODINIT_FUNC PyInit_tinyactors(void) {
    return PyModuleDef_Init(&ta_python_module);
}
