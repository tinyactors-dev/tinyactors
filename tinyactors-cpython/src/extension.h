/* Private declarations shared by the extension's C files. Python callers
 * use tinyactors.encode/decode; the conformance adapter imports those same
 * public methods. This is not an additional public C API.
 *
 * Each entry point runs with an attached CPython thread state. Python
 * arguments are borrowed for the duration of the call; a returned object
 * is a new reference, or NULL with a Python exception set. */
#ifndef TA_PYTHON_EXTENSION_H
#define TA_PYTHON_EXTENSION_H

#include <Python.h>
#include "tinyactors.h"

/* One module instance's codec identities, isolated between interpreters.
 * The module owns these references;
 * its traverse/clear slots release cycles formed by heap types retaining
 * their defining module. Conversion calls borrow this state while their
 * module argument keeps it alive. Never store these identities globally. */
typedef struct ta_python_module_state {
    PyObject *address_type;
    PyObject *undefined_type;
    PyObject *undefined;
} ta_python_module_state;

/* An immutable actor-reference value. text is an owned, exact Unicode
 * object; .text and str(value) give that same text, without URI parsing.
 * GC also visits the heap type, which retains its defining module. */
typedef struct ta_python_address {
    PyObject_HEAD
    PyObject *text;
} ta_python_address;

/* Install this module's Address type and UNDEFINED singleton during
 * Py_mod_exec. Return 0 or -1 with an exception; module teardown releases
 * any state installed before a failure. No Host or core image is involved. */
int ta_python_value_types_init(PyObject *module);

/* Borrow initialized codec identities for an entry point. A module created
 * with module_from_spec may expose its methods before Py_mod_exec: refuse
 * that state with RuntimeError instead of dereferencing absent identities.
 * Return NULL with an exception, or state kept alive by the module argument. */
ta_python_module_state *ta_python_codec_state(PyObject *module);

/* Build a new Address from already validated exact Unicode text. Used by
 * the decoder after core accepts tag 32 followed by text. Borrow state and
 * text, return a new reference or NULL. The new object owns a text reference;
 * it never borrows bytes from the reader's input. */
PyObject *ta_python_address_from_text(ta_python_module_state *state, PyObject *text);

/* Start an empty, call-local writer before converting a Python value.
 * The writer grows through CPython's raw allocator. Read its finished
 * bytes with ta_cbor_writer_bytes, consume or copy that view before
 * ta_cbor_writer_free, and free the writer on every exit, including errors.
 * This bootstrap has no Host arena yet; never share this writer between
 * calls or retain it on a host. Implemented in host.c with the allocator. */
void ta_python_cbor_init(ta_cbor_writer *writer);

/* CPython METH_O entry for tinyactors.encode(value): convert one supported
 * Python value to independent bytes. The module argument selects the
 * codec state supplying this module's Address/UNDEFINED identities.
 * See docs/values.md for the supported values and errors.
 * This codec is for CBOR boundaries: config, posted control messages and
 * values entering/leaving the process. In-process actor inputs use structs
 * and payload handles; do not encode every event on the hot path. */
PyObject *ta_python_encode(PyObject *module, PyObject *value);

/* CPython METH_O entry for tinyactors.decode(data): require exact built-in
 * bytes containing one complete supported CBOR item. Returned strings and
 * byte strings own their contents, so they outlive data. Refuse trailing
 * data and unsupported wire types; never use this to parse a frame stream
 * directly (the adapter must first remove its length prefix). */
PyObject *ta_python_decode(PyObject *module, PyObject *value);

#endif
