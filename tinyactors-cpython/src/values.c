/* values.c — the value table (docs/values.md): Python objects to CBOR and
 * back, streamed item by item with core's ta_cbor_writer and
 * ta_cbor_reader (tinyactors/cbor.h; tinyactors-core/docs/bindings.md,
 * section 9; docs/adr/0010 in the repository root, decision 4).
 * The API (config, requests, replies, event data), the module's
 * tinyactors.encode and tinyactors.decode, and the "python" datamodel's
 * to_wire and from_wire all convert here; there is no second codec.
 *
 * Also the two objects the table adds to Python: tinyactors.Address (tag
 * 32 on text, an actor reference) and tinyactors.UNDEFINED (CBOR's
 * undefined, distinct from None). */
#include "extension.h"

/* ---- Address ------------------------------------------------------------------ */

typedef struct ta_python_address {
    PyObject_HEAD
    PyObject *text; /* a str */
} ta_python_address;

static PyObject *ta_python_address_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    static const char *const keywords[] = {"text", NULL};
    PyObject *text = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "U:Address", (char **)keywords, &text)) return NULL;
    ta_python_address *address = (ta_python_address *)PyType_GenericAlloc(type, 0);
    if (!address) return NULL;
    address->text = Py_NewRef(text);
    return (PyObject *)address;
}

static void ta_python_address_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    Py_CLEAR(((ta_python_address *)self)->text);
    ((freefunc)PyType_GetSlot(type, Py_tp_free))(self); /* the type's own free, as a heap type's dealloc does */
    Py_DECREF(type);
}

static PyObject *ta_python_address_str(PyObject *self) {
    return Py_NewRef(((ta_python_address *)self)->text);
}

static PyObject *ta_python_address_repr(PyObject *self) {
    return PyUnicode_FromFormat("Address(%R)", ((ta_python_address *)self)->text);
}

static Py_hash_t ta_python_address_hash(PyObject *self) {
    return PyObject_Hash(((ta_python_address *)self)->text);
}

static PyObject *ta_python_address_compare(PyObject *self, PyObject *other, int operation) {
    if (!PyObject_TypeCheck(other, Py_TYPE(self)) || (operation != Py_EQ && operation != Py_NE))
        Py_RETURN_NOTIMPLEMENTED;
    return PyObject_RichCompare(((ta_python_address *)self)->text, ((ta_python_address *)other)->text, operation);
}

static PyType_Slot ta_python_address_slots[] = {
    {Py_tp_doc, "An actor reference: an address, written as CBOR tag 32 on its text."},
    {Py_tp_new, ta_python_address_new},
    {Py_tp_dealloc, ta_python_address_dealloc},
    {Py_tp_str, ta_python_address_str},
    {Py_tp_repr, ta_python_address_repr},
    {Py_tp_hash, ta_python_address_hash},
    {Py_tp_richcompare, ta_python_address_compare},
    {0, NULL},
};

static PyType_Spec ta_python_address_spec = {
    .name = "tinyactors.Address",
    .basicsize = sizeof(ta_python_address),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = ta_python_address_slots,
};

/* ---- UNDEFINED ------------------------------------------------------------------ */

static PyObject *ta_python_undefined_repr(PyObject *self) {
    (void)self;
    return PyUnicode_FromString("tinyactors.UNDEFINED");
}

/* Falsy, as None and as ECMAScript's undefined are. */
static int ta_python_undefined_bool(PyObject *self) {
    (void)self;
    return 0;
}

static void ta_python_undefined_dealloc(PyObject *self) {
    PyTypeObject *type = Py_TYPE(self);
    ((freefunc)PyType_GetSlot(type, Py_tp_free))(self); /* the type's own free, as a heap type's dealloc does */
    Py_DECREF(type);
}

static PyType_Slot ta_python_undefined_slots[] = {
    {Py_tp_doc, "The type of tinyactors.UNDEFINED, CBOR's undefined: Python has one empty value, None, which is null."},
    {Py_tp_repr, ta_python_undefined_repr},
    {Py_nb_bool, ta_python_undefined_bool},
    {Py_tp_dealloc, ta_python_undefined_dealloc},
    {0, NULL},
};

static PyType_Spec ta_python_undefined_spec = {
    .name = "tinyactors.Undefined",
    .basicsize = sizeof(PyObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    .slots = ta_python_undefined_slots,
};

int ta_python_values_init(PyObject *module, ta_python_state *state) {
    state->address_type = PyType_FromModuleAndSpec(module, &ta_python_address_spec, NULL);
    state->undefined_type = PyType_FromModuleAndSpec(module, &ta_python_undefined_spec, NULL);
    if (!state->address_type || !state->undefined_type) return -1;
    state->undefined = PyType_GenericAlloc((PyTypeObject *)state->undefined_type, 0);
    if (!state->undefined) return -1;
    if (PyModule_AddObjectRef(module, "Address", state->address_type) < 0 ||
        PyModule_AddObjectRef(module, "UNDEFINED", state->undefined) < 0)
        return -1;
    return 0;
}

PyObject *ta_python_address_from(const ta_python_state *state, ta_string_view text) {
    PyObject *string = PyUnicode_DecodeUTF8(text.pointer, (Py_ssize_t)text.length, "strict");
    if (!string) return NULL;
    ta_python_address *address = (ta_python_address *)PyType_GenericAlloc((PyTypeObject *)state->address_type, 0);
    if (!address) {
        Py_DECREF(string);
        return NULL;
    }
    address->text = string;
    return (PyObject *)address;
}

/* ---- Python to CBOR ------------------------------------------------------------------------ */

/* A value being written item by item with a ta_cbor_writer (cbor.h). The
 * table's refusal sticks in `status`; the writer's own (no memory, text
 * that is not UTF-8) in the writer, read at the end. */
typedef struct ta_python_writing {
    const ta_python_state *state;
    ta_cbor_writer *writer;
    ta_status status;
} ta_python_writing;

/* Text: valid UTF-8, so a lone surrogate is refused, never replaced
 * (bindings.md, section 9). */
static void ta_python_write_text(ta_python_writing *writing, PyObject *text) {
    Py_ssize_t length = 0;
    const char *utf8 = PyUnicode_AsUTF8AndSize(text, &length);
    if (!utf8) {
        PyErr_Clear();
        writing->status = TA_ERROR_INVALID;
        return;
    }
    if ((size_t)length > UINT32_MAX) {
        writing->status = TA_ERROR_INVALID;
        return;
    }
    ta_cbor_write_text(writing->writer, (ta_string_view){utf8, (uint32_t)length});
}

/* An int, exact to 64 bits either way: -2^64 … 2^64-1. */
static void ta_python_write_integer(ta_python_writing *writing, PyObject *integer) {
    int overflow = 0;
    long long value = PyLong_AsLongLongAndOverflow(integer, &overflow);
    if (value == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        writing->status = TA_ERROR_INVALID;
        return;
    }
    if (!overflow) {
        ta_cbor_write_int(writing->writer, value);
        return;
    }
    /* Past 63 bits: n itself, or ~n (-1 - n) for a negative one. */
    PyObject *magnitude = overflow > 0 ? Py_NewRef(integer) : PyNumber_Invert(integer);
    unsigned long long bits = magnitude ? PyLong_AsUnsignedLongLong(magnitude) : 0;
    Py_XDECREF(magnitude);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        writing->status = TA_ERROR_INVALID;
        return;
    }
    if (overflow > 0) ta_cbor_write_uint(writing->writer, bits);
    else ta_cbor_write_negative(writing->writer, bits);
}

static void ta_python_write_value(ta_python_writing *writing, PyObject *value, uint32_t depth);

/* A list's or a tuple's items. */
static void ta_python_write_items(ta_python_writing *writing, PyObject *sequence, uint32_t depth) {
    Py_ssize_t count = PySequence_Size(sequence);
    ta_cbor_write_array(writing->writer, (uint32_t)count);
    for (Py_ssize_t i = 0; i < count && writing->status == TA_OK; i++) {
        PyObject *item = PyList_Check(sequence) ? PyList_GetItemRef(sequence, i) : PySequence_GetItem(sequence, i);
        if (!item) {
            PyErr_Clear();
            writing->status = TA_ERROR_INVALID;
            return;
        }
        ta_python_write_value(writing, item, depth);
        Py_DECREF(item);
    }
}

static void ta_python_write_map(ta_python_writing *writing, PyObject *map, uint32_t depth) {
    PyObject *items = PyDict_Items(map); /* a snapshot: another thread may change the dict */
    if (!items) {
        PyErr_Clear();
        writing->status = TA_ERROR_INVALID;
        return;
    }
    Py_ssize_t count = PyList_Size(items);
    ta_cbor_write_map(writing->writer, (uint32_t)count);
    for (Py_ssize_t i = 0; i < count && writing->status == TA_OK; i++) {
        PyObject *pair = PyList_GetItem(items, i);
        PyObject *key = PyTuple_GetItem(pair, 0);
        if (!PyUnicode_Check(key)) {
            writing->status = TA_ERROR_INVALID; /* a map's keys are text */
            break;
        }
        ta_python_write_text(writing, key);
        if (writing->status == TA_OK) ta_python_write_value(writing, PyTuple_GetItem(pair, 1), depth);
    }
    Py_DECREF(items);
}

/* One value and its children. `depth` counts the containers and tags
 * around it: past TA_CBOR_DEPTH_MAX, core's limit, the value is refused,
 * which also ends a cycle (values.txt "invalid cycles"). */
static void ta_python_write_value(ta_python_writing *writing, PyObject *value, uint32_t depth) {
    const ta_python_state *state = writing->state;
    ta_cbor_writer *writer = writing->writer;
    if (writing->status != TA_OK) return;
    if (depth > TA_CBOR_DEPTH_MAX) {
        writing->status = TA_ERROR_INVALID;
        return;
    }
    if (value == Py_None) {
        ta_cbor_write_simple(writer, TA_CBOR_NULL);
    } else if (value == state->undefined) {
        ta_cbor_write_simple(writer, TA_CBOR_UNDEFINED);
    } else if (PyBool_Check(value)) { /* before int: bool is a subclass of int */
        ta_cbor_write_simple(writer, value == Py_True ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    } else if (PyLong_Check(value)) {
        ta_python_write_integer(writing, value);
    } else if (PyFloat_Check(value)) {
        ta_cbor_write_float(writer, PyFloat_AsDouble(value));
    } else if (PyUnicode_Check(value)) {
        ta_python_write_text(writing, value);
    } else if (PyBytes_Check(value)) {
        ta_cbor_write_bytes(writer, (ta_bytes){PyBytes_AsString(value), (size_t)PyBytes_Size(value)});
    } else if (PyByteArray_Check(value)) {
        ta_cbor_write_bytes(writer, (ta_bytes){PyByteArray_AsString(value), (size_t)PyByteArray_Size(value)});
    } else if (PyObject_TypeCheck(value, (PyTypeObject *)state->address_type)) {
        ta_cbor_write_tag(writer, TA_CBOR_TAG_URI);
        ta_python_write_text(writing, ((ta_python_address *)value)->text);
    } else if (PyList_Check(value) || PyTuple_Check(value)) {
        ta_python_write_items(writing, value, depth + 1);
    } else if (PyDict_Check(value)) {
        ta_python_write_map(writing, value, depth + 1);
    } else {
        writing->status = TA_ERROR_INVALID; /* outside the table (docs/values.md) */
    }
}

ta_status ta_python_values_write(const ta_python_state *state, PyObject *value, ta_python_cbor *out) {
    ta_python_writing writing = {state, &out->writer, TA_OK};
    ta_python_write_value(&writing, value, 0);
    if (writing.status != TA_OK) return writing.status;
    return ta_cbor_writer_bytes(&out->writer, &out->bytes);
}

/* ---- CBOR to Python -------------------------------------------------------------------------- */

/* A value being read item by item with a ta_cbor_reader (cbor.h): the
 * reader's refusal (malformed, not UTF-8, too deep) or the table's (a
 * value outside it) sticks in `status`. */
typedef struct ta_python_reading {
    const ta_python_state *state;
    ta_cbor_reader reader;
    ta_status status;
} ta_python_reading;

/* The next item into *item: 0 with reading->status set when there is
 * none or the reader refused. */
static PyObject *ta_python_read_value(ta_python_reading *reading);

static int ta_python_read_item(ta_python_reading *reading, ta_cbor_item *item) {
    int32_t read = ta_cbor_read(&reading->reader, item);
    if (read == 1) return 1;
    reading->status = read < 0 ? (ta_status)read : TA_ERROR_INVALID;
    return 0;
}

static PyObject *ta_python_read_integer(const ta_cbor_item *item) {
    if (item->kind == TA_CBOR_UINT) return PyLong_FromUnsignedLongLong(item->value);
    if (item->value <= INT64_MAX) return PyLong_FromLongLong(-1 - (long long)item->value);
    PyObject *magnitude = PyLong_FromUnsignedLongLong(item->value);
    PyObject *integer = magnitude ? PyNumber_Invert(magnitude) : NULL; /* -1 - n */
    Py_XDECREF(magnitude);
    return integer;
}

static PyObject *ta_python_read_text(const ta_cbor_item *item) {
    return PyUnicode_DecodeUTF8((const char *)item->bytes.pointer, (Py_ssize_t)item->bytes.length, "strict");
}

static PyObject *ta_python_read_array(ta_python_reading *reading, uint32_t count) {
    PyObject *list = PyList_New(count);
    for (uint32_t i = 0; list && i < count; i++) {
        PyObject *item = ta_python_read_value(reading);
        if (!item) Py_CLEAR(list);
        else PyList_SetItem(list, i, item);
    }
    return list;
}

static PyObject *ta_python_read_map(ta_python_reading *reading, uint32_t count) {
    PyObject *map = PyDict_New();
    for (uint32_t i = 0; map && i < count; i++) {
        ta_cbor_item item;
        if (!ta_python_read_item(reading, &item) || item.kind != TA_CBOR_TEXT) {
            if (reading->status == TA_OK) reading->status = TA_ERROR_INVALID; /* a key that is not text */
            Py_CLEAR(map);
            break;
        }
        PyObject *key = ta_python_read_text(&item);
        PyObject *value = key ? ta_python_read_value(reading) : NULL;
        if (!value || PyDict_SetItem(map, key, value) < 0) Py_CLEAR(map);
        Py_XDECREF(key);
        Py_XDECREF(value);
    }
    return map;
}

/* The next value. NULL with reading->status set for one the reader or
 * the table refuses; with a Python error for Python's. */
static PyObject *ta_python_read_value(ta_python_reading *reading) {
    ta_cbor_item item;
    if (!ta_python_read_item(reading, &item)) return NULL;
    switch (item.kind) {
    case TA_CBOR_UINT:
    case TA_CBOR_NEGINT: return ta_python_read_integer(&item);
    case TA_CBOR_TEXT: return ta_python_read_text(&item);
    case TA_CBOR_BYTES: return PyBytes_FromStringAndSize((const char *)item.bytes.pointer, (Py_ssize_t)item.bytes.length);
    case TA_CBOR_ARRAY: return ta_python_read_array(reading, item.count);
    case TA_CBOR_MAP: return ta_python_read_map(reading, item.count);
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &item.value, sizeof number);
        return PyFloat_FromDouble(number);
    }
    case TA_CBOR_SIMPLE:
        if (item.value == TA_CBOR_FALSE) Py_RETURN_FALSE;
        if (item.value == TA_CBOR_TRUE) Py_RETURN_TRUE;
        if (item.value == TA_CBOR_NULL) Py_RETURN_NONE;
        if (item.value == TA_CBOR_UNDEFINED) return Py_NewRef(reading->state->undefined);
        break;
    case TA_CBOR_TAG: {
        ta_cbor_item text;
        if (item.value == TA_CBOR_TAG_URI && ta_python_read_item(reading, &text) && text.kind == TA_CBOR_TEXT)
            return ta_python_address_from(reading->state,
                                          (ta_string_view){(const char *)text.bytes.pointer, (uint32_t)text.bytes.length});
        break;
    }
    default: break;
    }
    if (reading->status == TA_OK) reading->status = TA_ERROR_INVALID; /* another tag or simple value */
    return NULL;
}

ta_status ta_python_values_read(const ta_python_state *state, ta_bytes cbor, PyObject **out) {
    ta_python_reading reading = {state, {0}, TA_OK};
    ta_cbor_reader_init(&reading.reader, cbor);
    *out = ta_python_read_value(&reading);
    ta_cbor_item rest;
    if (*out && ta_cbor_read(&reading.reader, &rest) != 0) { /* bytes after the item, or an error past it */
        reading.status = TA_ERROR_INVALID;
        Py_CLEAR(*out);
    }
    if (*out) return TA_OK;
    if (reading.status == TA_OK) return TA_ERROR_NO_MEMORY; /* Python's, with its error set */
    PyErr_Clear(); /* the reader's or the table's refusal, not Python's */
    return reading.status;
}

/* ---- copying through the table ----------------------------------------------------------- */

/* The copy of `value` at `depth` (containers around it), by the same rows
 * as writing it: what is outside the table, or nests past
 * TA_CBOR_DEPTH_MAX, is refused. */
typedef struct ta_python_copying {
    const ta_python_state *state;
    ta_status status;
} ta_python_copying;

static PyObject *ta_python_copy_value(ta_python_copying *copying, PyObject *value, uint32_t depth) {
    const ta_python_state *state = copying->state;
    if (depth > TA_CBOR_DEPTH_MAX) {
        copying->status = TA_ERROR_INVALID;
        return NULL;
    }
    if (value == Py_None || value == state->undefined || PyBool_Check(value))
        return Py_NewRef(value);
    if (PyLong_CheckExact(value) || PyFloat_CheckExact(value) || PyBytes_CheckExact(value) ||
        PyObject_TypeCheck(value, (PyTypeObject *)state->address_type))
        return Py_NewRef(value); /* immutable: shared */
    if (PyUnicode_Check(value)) { /* a subclass: its base, as written */
        PyObject *text = PyUnicode_CheckExact(value) ? Py_NewRef(value) : PyUnicode_FromObject(value);
        if (text && !PyUnicode_AsUTF8AndSize(text, NULL)) { /* a lone surrogate */
            PyErr_Clear();
            Py_CLEAR(text);
            copying->status = TA_ERROR_INVALID;
        }
        return text;
    }
    if (PyLong_Check(value)) return PyNumber_Index(value); /* a subclass: its base, as written */
    if (PyFloat_Check(value)) return PyFloat_FromDouble(PyFloat_AsDouble(value));
    if (PyBytes_Check(value) || PyByteArray_Check(value)) return PyBytes_FromObject(value);
    if (PyList_Check(value) || PyTuple_Check(value)) {
        PyObject *items = PySequence_List(value);
        Py_ssize_t count = items ? PyList_Size(items) : 0;
        for (Py_ssize_t i = 0; items && i < count; i++) {
            PyObject *item = ta_python_copy_value(copying, PyList_GetItem(items, i), depth + 1);
            if (!item) Py_CLEAR(items);
            else PyList_SetItem(items, i, item);
        }
        return items;
    }
    if (PyDict_Check(value)) {
        PyObject *pairs = PyDict_Items(value), *map = PyDict_New();
        Py_ssize_t count = pairs ? PyList_Size(pairs) : 0;
        for (Py_ssize_t i = 0; map && i < count; i++) {
            PyObject *pair = PyList_GetItem(pairs, i), *key = PyTuple_GetItem(pair, 0);
            if (!PyUnicode_Check(key)) {
                copying->status = TA_ERROR_INVALID;
                Py_CLEAR(map);
                break;
            }
            PyObject *text = ta_python_copy_value(copying, key, depth + 1);
            PyObject *item = text ? ta_python_copy_value(copying, PyTuple_GetItem(pair, 1), depth + 1) : NULL;
            if (!item || PyDict_SetItem(map, text, item) < 0) Py_CLEAR(map);
            Py_XDECREF(text);
            Py_XDECREF(item);
        }
        Py_XDECREF(pairs);
        return map;
    }
    copying->status = TA_ERROR_INVALID; /* outside the table (docs/values.md) */
    return NULL;
}

ta_status ta_python_values_copy(const ta_python_state *state, PyObject *value, PyObject **out) {
    ta_python_copying copying = {state, TA_OK};
    *out = ta_python_copy_value(&copying, value, 0);
    if (*out) return TA_OK;
    if (copying.status == TA_OK) copying.status = PyErr_ExceptionMatches(PyExc_MemoryError) ? TA_ERROR_NO_MEMORY
                                                                                            : TA_ERROR_INVALID;
    PyErr_Clear();
    return copying.status;
}

/* ---- tinyactors.encode, tinyactors.decode ------------------------------------------------ */

PyObject *ta_python_encode(PyObject *module, PyObject *value) {
    const ta_python_state *state = ta_python_state_of(module);
    ta_python_cbor cbor;
    ta_python_cbor_init(&cbor);
    ta_status status = ta_python_values_write(state, value, &cbor);
    PyObject *bytes = status == TA_OK ? PyBytes_FromStringAndSize(cbor.bytes.pointer, (Py_ssize_t)cbor.bytes.length)
                                      : NULL;
    ta_python_cbor_free(&cbor);
    if (status != TA_OK && !PyErr_Occurred()) ta_python_raise_status(state, "ta_cbor_writer_bytes", status);
    return bytes;
}

PyObject *ta_python_decode(PyObject *module, PyObject *data) {
    const ta_python_state *state = ta_python_state_of(module);
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyObject *value = NULL;
    ta_status status = ta_python_values_read(state, (ta_bytes){view.buf, (size_t)view.len}, &value);
    PyBuffer_Release(&view);
    if (status != TA_OK && !PyErr_Occurred()) ta_python_raise_status(state, "ta_cbor_read", status);
    return status == TA_OK ? value : NULL;
}
