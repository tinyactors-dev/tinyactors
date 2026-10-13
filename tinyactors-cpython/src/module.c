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
    "bytes, list, dict with str keys, plus bool and None. Container order\n"
    "is preserved. Each container is snapshotted separately; do not mutate\n"
    "a nested value during conversion if you need a consistent whole value.\n"
    "Subclasses, tuples, Address, and UNDEFINED are not supported yet.\n\n"
    "Raises TypeError for unsupported types or non-str map keys, OverflowError\n"
    "for integers or text outside core's length range, UnicodeError for lone\n"
    "surrogates, ValueError for excessive nesting (including cycles), and\n"
    "MemoryError on allocation failure. See docs/values.md for the subset.";

static const char ta_python_decode_doc[] =
    "decode($module, data, /)\n--\n\n"
    "Decode exactly one supported CBOR item from exact built-in bytes.\n\n"
    "Use after obtaining a complete message; remove any transport framing\n"
    "first. Returned values own their text and bytes and outlive data.\n"
    "Arrays become lists, maps become dicts with str keys, and repeated\n"
    "map keys keep the last value. Wire nesting checks are core's checks.\n"
    "Tags (including Address) and undefined are not supported yet.\n\n"
    "Raises TypeError for input other than bytes, ValueError for malformed,\n"
    "truncated, trailing, or unsupported CBOR, and MemoryError on allocation\n"
    "failure. This is a value codec, not a host or driver-message validator.";

static PyMethodDef ta_python_methods[] = {
    {"encode", ta_python_encode, METH_O, ta_python_encode_doc},
    {"decode", ta_python_decode, METH_O, ta_python_decode_doc},
    {NULL, NULL, 0, NULL}
};

/* CPython reads these registration tables; the extension never mutates
 * them. Per-call writers/readers and owned container snapshots carry the
 * conversion state, including on the free-threaded test interpreter. */
static PyModuleDef_Slot ta_python_slots[] = {
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef ta_python_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "tinyactors",
    .m_doc = "Tinyactors direct binding: initial CBOR bootstrap.",
    .m_size = 0,
    .m_methods = ta_python_methods,
    .m_slots = ta_python_slots
};

/* CPython's required import symbol. Return its multi-phase initialization
 * descriptor; no writer, host or interpreter-specific object is created
 * in static storage when the module is imported. */
PyMODINIT_FUNC PyInit_tinyactors(void) {
    return PyModuleDef_Init(&ta_python_module);
}
