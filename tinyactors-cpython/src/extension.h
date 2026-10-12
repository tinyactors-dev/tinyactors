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

#include "tinyactors.h"
#include "tinyactors_datamodel_ecmascript.h"
#include "tinyactors_runtime.h"

/* The module's state (PyModule_GetState): every type and object the
 * extension makes once per interpreter. No C globals (AGENTS.md, "No
 * mutable globals"). */
typedef struct ta_python_state {
    PyObject *error_type; /* tinyactors.TinyactorsError */
} ta_python_state;

/* The state of the module `module`. */
ta_python_state *ta_python_state_of(PyObject *module);

#endif /* TA_PYTHON_EXTENSION_H */
