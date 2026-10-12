/* extension.h — what the files of the tinyactors extension share: the
 * module's state, and each file's functions (AGENTS.md, "Layout").
 *
 * The extension is built twice from the same sources: abi3 against
 * CPython 3.13's limited API (Py_LIMITED_API, set by the build), and
 * without it for the free-threaded test interpreter (AGENTS.md, "C rules").
 * Every CPython function it calls is in both. */
#ifndef TA_PYTHON_EXTENSION_H
#define TA_PYTHON_EXTENSION_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string.h>

#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"
#include "tinyactors_runtime.h"

/* The module's state (PyModule_GetState): every type and object the
 * extension makes once per interpreter. No C globals (AGENTS.md, "No
 * mutable globals"). */
typedef struct ta_python_state {
    PyObject *error_type;     /* tinyactors.TinyactorsError */
    PyObject *address_type;   /* tinyactors.Address */
    PyObject *undefined_type; /* type(tinyactors.UNDEFINED) */
    PyObject *undefined;      /* tinyactors.UNDEFINED */
} ta_python_state;

/* The state of the module `module`. */
ta_python_state *ta_python_state_of(PyObject *module);

/* ---- errors (module.c) ---------------------------------------------------------- */

/* TinyactorsError for a core call that failed with `status`: its `status`
 * is set, `code`, `reason` and `path` are None, and its message names the
 * function and the status (docs/errors.md). */
void ta_python_raise_status(const ta_python_state *state, const char *function, ta_status status);
/* TinyactorsError from an error reply ({"code", "message", "data":
 * {"reason", "path"}}, docs/requests.md) as a Python dict: `code`,
 * `reason`, `path` and the message unchanged; `status` is the call's
 * when it failed (ta_host_create), None when it answered TA_OK with the
 * error in its reply (ta_host_request). */
typedef struct ta_python_refusal {
    PyObject *error; /* the reply's "error" map */
    ta_status status;
} ta_python_refusal;
void ta_python_raise_reply(const ta_python_state *state, const ta_python_refusal *refusal);

/* ---- values (values.c) ------------------------------------------------------------ */

int ta_python_values_init(PyObject *module, ta_python_state *state);
/* `value` as CBOR into `out` (ta_buffer: TA_ERROR_NO_MEMORY with
 * out->length what it needs, and no Python error). TA_ERROR_INVALID: a
 * value outside the table (docs/values.md). A Python error stays set when
 * Python's allocator failed. */
ta_status ta_python_values_write(const ta_python_state *state, PyObject *value, ta_buffer *out);
/* The same as a new bytes object. */
ta_status ta_python_values_bytes(const ta_python_state *state, PyObject *value, PyObject **out);
/* One CBOR item as a new Python value: ta_cbor_decode's status, or
 * TA_ERROR_INVALID for an item outside the table. */
ta_status ta_python_values_read(const ta_python_state *state, ta_bytes cbor, PyObject **out);
/* A new tinyactors.Address of `text`. */
PyObject *ta_python_address_from(const ta_python_state *state, ta_string_view text);
/* tinyactors.encode(value) and tinyactors.decode(data). */
PyObject *ta_python_encode(PyObject *module, PyObject *value);
PyObject *ta_python_decode(PyObject *module, PyObject *data);

#endif /* TA_PYTHON_EXTENSION_H */
