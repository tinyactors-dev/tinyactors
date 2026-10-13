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

/* Start an empty, call-local writer before converting a Python value.
 * The writer grows through CPython's raw allocator. Read its finished
 * bytes with ta_cbor_writer_bytes, consume or copy that view before
 * ta_cbor_writer_free, and free the writer on every exit, including errors.
 * This bootstrap has no Host arena yet; never share this writer between
 * calls or retain it on a host. Implemented in host.c with the allocator. */
void ta_python_cbor_init(ta_cbor_writer *writer);

/* CPython METH_O entry for tinyactors.encode(value): convert one supported
 * Python value to independent bytes. The module argument is unused in this
 * stateless slice. See docs/values.md for the supported subset and errors.
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
