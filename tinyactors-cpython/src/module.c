/* Public Python entry points for the first slice of the direct binding.
 * The test adapter uses this module as an application would; no test-only
 * codec or private core access is exposed. Host execution is still absent. */
#include "extension.h"

static const char ta_python_encode_doc[] =
    "encode($module, value, /)\n--\n\n"
    "Encode one Python value as independently owned CBOR bytes.\n\n"
    "Use for config, control messages, and values crossing a process boundary.\n"
    "The conformance adapter uses the same codec for its driver frames.\n"
    "Actor inputs in one process use structs and payload handles instead.\n\n"
    "Supported: exact built-in int (-2**64 through 2**64-1), float, str,\n"
    "bytes, list, dict with str keys, plus bool, None, Address and UNDEFINED.\n"
    "Address preserves tag 32; UNDEFINED preserves undefined, including\n"
    "inside containers. Container order\n"
    "is preserved. Each container is snapshotted separately; do not mutate\n"
    "a nested value during conversion if you need a consistent whole value.\n"
    "Subclasses and tuples are not supported.\n\n"
    "Raises TypeError for unsupported types or non-str map keys, OverflowError\n"
    "for integers or text outside core's length range, UnicodeError for lone\n"
    "surrogates, ValueError for excessive nesting (including cycles), and\n"
    "MemoryError on allocation failure. Calling before module execution raises\n"
    "RuntimeError. See docs/values.md for the complete mapping.";

static const char ta_python_decode_doc[] =
    "decode($module, data, /)\n--\n\n"
    "Decode exactly one supported CBOR item from exact built-in bytes.\n\n"
    "Use after obtaining a complete message; remove any transport framing\n"
    "first. Returned values own their text and bytes and outlive data.\n"
    "Arrays become lists, maps become dicts with str keys, and repeated\n"
    "map keys keep the last value. Wire nesting checks are core's checks.\n"
    "Tag 32 on text becomes Address; undefined becomes this module's\n"
    "UNDEFINED singleton. Other tags and simple values are refused.\n\n"
    "Raises TypeError for input other than bytes, ValueError for malformed,\n"
    "truncated, trailing, or unsupported CBOR, and MemoryError on allocation\n"
    "failure. Calling before module execution raises RuntimeError.\n"
    "This is a value codec, not a host or driver-message validator.";

static PyMethodDef ta_python_methods[] = {
    {"encode", ta_python_encode, METH_O, ta_python_encode_doc},
    {"decode", ta_python_decode, METH_O, ta_python_decode_doc},
    {NULL, NULL, 0, NULL}
};

/* Module methods can be called before an import loader executes Py_mod_exec.
 * Check both allocation and identities; a partial initialization must not
 * allow a codec call to read or publish an incomplete value table. */
ta_python_module_state *ta_python_codec_state(PyObject *module) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (state == NULL || state->address_type == NULL ||
        state->undefined_type == NULL || state->undefined == NULL) {
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, "tinyactors codec is not initialized");
        return NULL;
    }
    return state;
}

/* Module state owns the interpreter's identities; CPython needs to see all
 * its references when collecting cycles with module-associated heap types. */
static int ta_python_module_traverse(PyObject *module, visitproc visit, void *arg) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (state == NULL) return 0;
    Py_VISIT(state->address_type);
    Py_VISIT(state->undefined_type);
    Py_VISIT(state->undefined);
    return 0;
}

/* Clear partial initialization as well as normal module teardown. Exported
 * names have separate dictionary references; clear only the state's owned
 * references. */
static int ta_python_module_clear(PyObject *module) {
    ta_python_module_state *state = PyModule_GetState(module);
    if (state == NULL) return 0;
    Py_CLEAR(state->undefined);
    Py_CLEAR(state->undefined_type);
    Py_CLEAR(state->address_type);
    return 0;
}

/* CPython's m_free can run without an earlier clear (including init errors). */
static void ta_python_module_free(void *module) {
    ta_python_module_clear((PyObject *)module);
}

/* Registration tables contain no runtime identities. Exec publishes the
 * per-module types once; conversions then only borrow module state. */
static PyModuleDef_Slot ta_python_slots[] = {
    {Py_mod_exec, ta_python_value_types_init},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef ta_python_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "tinyactors",
    .m_doc = "Tinyactors direct binding: neutral-value codec and driver bootstrap.",
    .m_size = sizeof(ta_python_module_state),
    .m_methods = ta_python_methods,
    .m_slots = ta_python_slots,
    .m_traverse = ta_python_module_traverse,
    .m_clear = ta_python_module_clear,
    .m_free = ta_python_module_free
};

/* CPython's required import symbol. Return its multi-phase initialization
 * descriptor; no writer, host or interpreter-specific object is created
 * in static storage when the module is imported. */
PyMODINIT_FUNC PyInit_tinyactors(void) {
    return PyModuleDef_Init(&ta_python_module);
}
