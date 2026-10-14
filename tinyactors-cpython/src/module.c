/* The module tinyactors (PEP 489): its state per interpreter, its types
 * and functions. Everything a Host does is host.c and the files beside it. */
#include "extension.h"

int ta_python_event_init_type(PyObject *module, ta_python_module_state *state);

static const char ta_python_encode_doc[] =
    "encode($module, value, /)\n--\n\n"
    "Encode one Python value as independently owned CBOR bytes.\n\n"
    "Use for config, control messages, and values crossing a process boundary.\n"
    "The conformance adapter uses the same codec for its driver frames.\n"
    "Actor inputs in one process use structs and payload handles instead.\n\n"
    "Supported: exact built-in int (-2**64 through 2**64-1), float, str,\n"
    "bytes, list, dict with str keys, plus bool, None, Address and UNDEFINED.\n"
    "Address preserves tag 32; UNDEFINED preserves undefined, including\n"
    "inside containers. Container order is preserved. Subclasses and tuples\n"
    "are not supported.\n\n"
    "Raises TypeError for unsupported types or non-str map keys, OverflowError\n"
    "for integers or text outside core's length range, UnicodeError for lone\n"
    "surrogates, ValueError for excessive nesting (including cycles), and\n"
    "MemoryError on allocation failure. See docs/values.md.";

static const char ta_python_decode_doc[] =
    "decode($module, data, /)\n--\n\n"
    "Decode exactly one supported CBOR item from exact built-in bytes.\n\n"
    "Arrays become lists, maps become dicts with str keys, and repeated\n"
    "map keys keep the last value. Tag 32 on text becomes Address; undefined\n"
    "becomes this module's UNDEFINED singleton. Other tags and simple values\n"
    "are refused.\n\n"
    "Raises TypeError for input other than bytes, ValueError for malformed,\n"
    "truncated, trailing, or unsupported CBOR, and MemoryError on allocation\n"
    "failure.";

static const char ta_python_compile_doc[] =
    "compile_charts(sources, resolver=None)\n--\n\n"
    "Compile SCXML documents into one image (ta_compile_charts). `sources` is\n"
    "a list of (name, text) pairs, the first the default boot chart.\n"
    "`resolver`, an object with resolve(base, source) -> str, reads a static\n"
    "src= at compile time; it refuses by raising ResolveError. Returns a\n"
    "Compiled; a compilation with an error raises CompileError with every\n"
    "diagnostic.";

static PyMethodDef ta_python_methods[] = {
    {"encode", ta_python_encode, METH_O, ta_python_encode_doc},
    {"decode", ta_python_decode, METH_O, ta_python_decode_doc},
    {"compile_charts", (PyCFunction)(void (*)(void))ta_python_compile_charts, METH_FASTCALL | METH_KEYWORDS,
     ta_python_compile_doc},
    {NULL, NULL, 0, NULL}
};

/* Module methods can be called before an import loader executes Py_mod_exec. */
ta_python_module_state *ta_python_codec_state(PyObject *module) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (state == NULL || state->address_type == NULL || state->undefined == NULL || state->support == NULL) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "tinyactors is not initialized");
        return NULL;
    }
    return state;
}

struct PyModuleDef ta_python_module;

ta_python_module_state *ta_python_state_of_type(PyTypeObject *type) {
    PyObject *module = PyType_GetModuleByDef(type, &ta_python_module);
    return module ? ta_python_codec_state(module) : NULL;
}

#define TA_PYTHON_STATE_FIELDS(apply)                                                                           \
    apply(address_type) apply(undefined_type) apply(undefined) apply(host_type) apply(clock_type)                \
    apply(compiled_type) apply(chart_type) apply(send_type) apply(entry_type) apply(envelope_type)               \
    apply(migration_type)                                                                                       \
    apply(event_type) apply(support) apply(error) apply(compile_error) apply(resolve_error) apply(diagnostic)    \
    apply(builtins) apply(attach) apply(check_file) apply(event_kinds) apply(event_key) apply(time_ns) apply(coroutine_function)

static int ta_python_module_traverse(PyObject *module, visitproc visit, void *arg) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (state == NULL) return 0;
#define TA_PYTHON_VISIT(field) Py_VISIT(state->field);
    TA_PYTHON_STATE_FIELDS(TA_PYTHON_VISIT)
#undef TA_PYTHON_VISIT
    return 0;
}

static int ta_python_module_clear(PyObject *module) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (state == NULL) return 0;
#define TA_PYTHON_CLEAR(field) Py_CLEAR(state->field);
    TA_PYTHON_STATE_FIELDS(TA_PYTHON_CLEAR)
#undef TA_PYTHON_CLEAR
    return 0;
}

static void ta_python_module_free(void *module) {
    ta_python_module_clear((PyObject *)module);
}

/* The types and objects, once per module instance. */
static int ta_python_module_exec(PyObject *module) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (ta_python_value_types_init(module) < 0 || ta_python_support_init(module, state) < 0 ||
        ta_python_host_init_type(module, state) < 0 || ta_python_clock_init_type(module, state) < 0 ||
        ta_python_compile_init_type(module, state) < 0 || ta_python_effects_init_types(module, state) < 0 ||
        ta_python_event_init_type(module, state) < 0)
        return -1;
    return 0;
}

static PyModuleDef_Slot ta_python_slots[] = {
    {Py_mod_exec, ta_python_module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

struct PyModuleDef ta_python_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "tinyactors",
    .m_doc = "tinyactors: SCXML actors in a host, with charts in Python (the \"python\" datamodel) or ECMAScript.",
    .m_size = sizeof(ta_python_module_state),
    .m_methods = ta_python_methods,
    .m_slots = ta_python_slots,
    .m_traverse = ta_python_module_traverse,
    .m_clear = ta_python_module_clear,
    .m_free = ta_python_module_free
};

/* CPython's import finds the module by this name (PEP 489). */
PyMODINIT_FUNC PyInit_tinyactors(void) {
    return PyModuleDef_Init(&ta_python_module);
}
