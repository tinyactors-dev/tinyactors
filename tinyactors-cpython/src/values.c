/* The binding's single Python <-> CBOR conversion, behind public
 * tinyactors.encode/decode. The conformance adapter is the current user;
 * later config, control-plane and process-boundary paths must reuse this
 * conversion rather than add another codec. This does not implement SCXML
 * evaluation, payload tables or in-process event delivery.
 *
 * Core owns CBOR syntax, wire widths and cursor validation (cbor.h).
 * This file owns the Python type mapping (docs/values.md). Each call owns
 * its writer/reader; recursive helpers borrow their arguments. Python
 * objects allocated while reading are released on error or returned as
 * new references. No core-owned byte view escapes to Python. */
#include "extension.h"
#include <string.h>

static int ta_python_write(ta_cbor_writer *writer, PyObject *value, unsigned depth);

/* Write children of an array already opened by ta_python_write. items must
 * be that call's private list snapshot, held alive by the caller, not the
 * original mutable input. depth is the children's nesting depth.
 * Return 0 on success or -1 with an exception set; the caller closes the
 * array only on success and discards the writer on failure. */
static int ta_python_write_sequence(ta_cbor_writer *writer, PyObject *items, unsigned depth) {
    Py_ssize_t count = PyList_Size(items);
    for (Py_ssize_t i = 0; i < count; i++) {
        PyObject *item = PyList_GetItemRef(items, i);
        if (item == NULL) return -1;
        int status = ta_python_write(writer, item, depth);
        Py_DECREF(item);
        if (status < 0) return -1;
    }
    return 0;
}

/* Recursively append one Python value to an initialized writer. This is
 * the private encoder used by tinyactors.encode, not an adapter-only
 * helper. Start at depth 0 for the whole message; recursive calls append
 * inside the parent's open container and increase depth by one.
 *
 * value is borrowed and must remain alive throughout this call. Exact
 * built-in types avoid invoking arbitrary conversion hooks. Each list or
 * dict is snapshotted before visiting its children; that snapshot holds
 * child references, but does not freeze a whole nested value atomically.
 * Shared subtrees are encoded on each occurrence; cycles reach the depth
 * limit and raise. Bytes and text are copied by the writer during the
 * write, so their borrowed views need not survive this call.
 *
 * Return 0 on success; -1 with an exception on conversion or writer error.
 * An error can leave partial output: stop, discard the writer and free it.
 * Do not resume the writer, retry conversion into it, or serialize a
 * payload handle here. In-process actor sends keep their struct/handle
 * path; this codec belongs at actual CBOR boundaries. */
static int ta_python_write(ta_cbor_writer *writer, PyObject *value, unsigned depth) {
    if (depth > TA_CBOR_DEPTH_MAX) {
        PyErr_SetString(PyExc_ValueError, "CBOR nesting exceeds 64 levels");
        return -1;
    }
    if (value == Py_None) ta_cbor_write_simple(writer, TA_CBOR_NULL);
    else if (PyBool_Check(value)) ta_cbor_write_simple(writer, value == Py_True ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    else if (PyLong_CheckExact(value)) {
        PyObject *zero = PyLong_FromLong(0);
        if (zero == NULL) return -1;
        int negative = PyObject_RichCompareBool(value, zero, Py_LT);
        Py_DECREF(zero);
        if (negative < 0) return -1;
        PyObject *magnitude = negative ? PyNumber_Invert(value) : Py_NewRef(value);
        if (magnitude == NULL) return -1;
        unsigned long long number = PyLong_AsUnsignedLongLong(magnitude);
        Py_DECREF(magnitude);
        if (PyErr_Occurred()) return -1;
        if (negative) ta_cbor_write_negative(writer, number);
        else ta_cbor_write_uint(writer, number);
    } else if (PyFloat_CheckExact(value)) ta_cbor_write_float(writer, PyFloat_AsDouble(value));
    else if (PyUnicode_CheckExact(value)) {
        Py_ssize_t size;
        const char *text = PyUnicode_AsUTF8AndSize(value, &size);
        if (text == NULL) return -1;
        if ((size_t)size > UINT32_MAX) {
            PyErr_SetString(PyExc_OverflowError, "CBOR text exceeds the core string length limit");
            return -1;
        }
        ta_cbor_write_text(writer, (ta_string_view){text, (uint32_t)size});
    } else if (PyBytes_CheckExact(value)) {
        char *bytes;
        Py_ssize_t size;
        if (PyBytes_AsStringAndSize(value, &bytes, &size) < 0) return -1;
        ta_cbor_write_bytes(writer, (ta_bytes){bytes, (size_t)size});
    } else if (PyList_CheckExact(value)) {
        PyObject *items = PySequence_List(value);
        if (items == NULL) return -1;
        ta_cbor_write_array(writer, TA_CBOR_COUNT_LATER);
        int status = ta_python_write_sequence(writer, items, depth + 1);
        Py_DECREF(items);
        if (status < 0) return -1;
        ta_cbor_write_end(writer);
    } else if (PyDict_CheckExact(value)) {
        PyObject *pairs = PyDict_Items(value);
        if (pairs == NULL) return -1;
        ta_cbor_write_map(writer, TA_CBOR_COUNT_LATER);
        for (Py_ssize_t i = 0; i < PyList_Size(pairs); i++) {
            PyObject *pair = PyList_GetItem(pairs, i);
            PyObject *key = PyTuple_GetItem(pair, 0);
            if (!PyUnicode_CheckExact(key)) {
                PyErr_SetString(PyExc_TypeError, "CBOR map keys must be str");
                Py_DECREF(pairs);
                return -1;
            }
            if (ta_python_write(writer, key, depth + 1) < 0 ||
                ta_python_write(writer, PyTuple_GetItem(pair, 1), depth + 1) < 0) {
                Py_DECREF(pairs);
                return -1;
            }
        }
        Py_DECREF(pairs);
        ta_cbor_write_end(writer);
    } else {
        PyErr_SetString(PyExc_TypeError, "unsupported Python value for CBOR");
        return -1;
    }
    if (writer->status != TA_OK) {
        if (writer->status == TA_ERROR_NO_MEMORY) PyErr_NoMemory();
        else PyErr_SetString(PyExc_ValueError, "invalid CBOR value");
        return -1;
    }
    return 0;
}

/* Public METH_O adapter: see extension.h and encode's Python help text.
 * Copy the finished writer view into independent Python bytes before
 * freeing the writer. Both success and failure release the raw block. */
PyObject *ta_python_encode(PyObject *module, PyObject *value) {
    (void)module;
    ta_cbor_writer writer;
    ta_python_cbor_init(&writer);
    PyObject *result = NULL;
    if (ta_python_write(&writer, value, 0) == 0) {
        ta_bytes bytes;
        if (ta_cbor_writer_bytes(&writer, &bytes) == TA_OK)
            result = PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
        else PyErr_SetString(PyExc_ValueError, "incomplete CBOR value");
    }
    ta_cbor_writer_free(&writer);
    return result;
}

/* Consume one item, including its children, from a call-local reader.
 * The reader's immutable input bytes must remain alive throughout all
 * recursive calls. Return a new Python reference, or NULL with an
 * exception. Strings/bytes are copied; containers own inserted children.
 * Partial containers are released on error.
 *
 * This helper can read a child of an open array/map, so it does not check
 * for trailing bytes. The top-level ta_python_decode must do that after
 * its one successful read. On failure abandon the reader, whose cursor
 * may already have advanced; do not use it to recover a frame boundary.
 * Core validates syntax and wire nesting; this helper rejects wire types
 * the bootstrap has no Python mapping for. */
static PyObject *ta_python_read(ta_cbor_reader *reader) {
    ta_cbor_item item;
    if (ta_cbor_read(reader, &item) != 1) {
        PyErr_SetString(PyExc_ValueError, "malformed or incomplete CBOR");
        return NULL;
    }
    switch (item.kind) {
    case TA_CBOR_UINT: return PyLong_FromUnsignedLongLong(item.value);
    case TA_CBOR_NEGINT: {
        PyObject *magnitude = PyLong_FromUnsignedLongLong(item.value);
        if (magnitude == NULL) return NULL;
        PyObject *result = PyNumber_Invert(magnitude);
        Py_DECREF(magnitude);
        return result;
    }
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &item.value, sizeof(number));
        return PyFloat_FromDouble(number);
    }
    case TA_CBOR_BYTES: return PyBytes_FromStringAndSize(item.bytes.pointer, (Py_ssize_t)item.bytes.length);
    case TA_CBOR_TEXT: return PyUnicode_DecodeUTF8(item.bytes.pointer, (Py_ssize_t)item.bytes.length, "strict");
    case TA_CBOR_SIMPLE:
        if (item.value == TA_CBOR_NULL) return Py_NewRef(Py_None);
        if (item.value == TA_CBOR_FALSE || item.value == TA_CBOR_TRUE)
            return PyBool_FromLong(item.value == TA_CBOR_TRUE);
        break;
    case TA_CBOR_ARRAY: {
        PyObject *result = PyList_New(0);
        if (result == NULL) return NULL;
        for (uint32_t i = 0; i < item.count; i++) {
            PyObject *value = ta_python_read(reader);
            if (value == NULL) { Py_DECREF(result); return NULL; }
            int status = PyList_Append(result, value);
            Py_DECREF(value);
            if (status < 0) { Py_DECREF(result); return NULL; }
        }
        return result;
    }
    case TA_CBOR_MAP: {
        PyObject *result = PyDict_New();
        if (result == NULL) return NULL;
        for (uint32_t i = 0; i < item.count; i++) {
            PyObject *key = ta_python_read(reader);
            if (key == NULL) { Py_DECREF(result); return NULL; }
            if (!PyUnicode_CheckExact(key)) {
                PyErr_SetString(PyExc_ValueError, "CBOR map keys must be text");
                Py_DECREF(key); Py_DECREF(result); return NULL;
            }
            PyObject *value = ta_python_read(reader);
            int status = value == NULL ? -1 : PyDict_SetItem(result, key, value);
            Py_DECREF(key);
            Py_XDECREF(value);
            if (status < 0) { Py_DECREF(result); return NULL; }
        }
        return result;
    }
    }
    PyErr_SetString(PyExc_ValueError, "unsupported CBOR tag or simple value");
    return NULL;
}

/* Public METH_O adapter: see extension.h and decode's Python help text.
 * The Python call holds value alive while the reader borrows its bytes.
 * Requiring immutable bytes also avoids retaining a view of mutable input.
 * Only return a decoded object once core confirms the message is exhausted. */
PyObject *ta_python_decode(PyObject *module, PyObject *value) {
    (void)module;
    if (!PyBytes_CheckExact(value)) {
        PyErr_SetString(PyExc_TypeError, "decode requires bytes in the bootstrap codec");
        return NULL;
    }
    char *bytes;
    Py_ssize_t size;
    if (PyBytes_AsStringAndSize(value, &bytes, &size) < 0) return NULL;
    ta_cbor_reader reader;
    ta_cbor_reader_init(&reader, (ta_bytes){bytes, (size_t)size});
    PyObject *result = ta_python_read(&reader);
    if (result != NULL) {
        ta_cbor_item extra;
        if (ta_cbor_read(&reader, &extra) != 0) {
            Py_DECREF(result);
            result = NULL;
            PyErr_SetString(PyExc_ValueError, "CBOR input must contain exactly one item");
        }
    }
    return result;
}
