/* values.c — the value table (docs/values.md): Python objects to core's
 * CBOR token lists and back (tinyactors/cbor.h), one ta_cbor_encode or
 * ta_cbor_decode per value (tinyactors-core/docs/bindings.md, section 9).
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
    PyObject_Free(self);
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
    PyObject_Free(self);
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

/* ---- Python to tokens -------------------------------------------------------------- */

/* A token list being built: tokens, and the bytes their text and byte
 * tokens point into (`strings`). Past its capacity it goes on counting,
 * so a second pass with that much room fits (the ta_cbor_writer
 * convention, cbor.h). The first error sticks. */
typedef struct ta_python_tokens {
    const ta_python_state *state;
    ta_cbor_token *tokens;
    uint32_t capacity, count;
    ta_buffer strings;
    ta_status status;
} ta_python_tokens;

static void ta_python_token_add(ta_python_tokens *list, const ta_cbor_token *token) {
    if (list->count < list->capacity) list->tokens[list->count] = *token;
    list->count++;
}

/* A text or byte token over `bytes`, copied into the strings. */
static void ta_python_token_string(ta_python_tokens *list, uint32_t kind, ta_bytes bytes) {
    if (bytes.length > UINT32_MAX) {
        list->status = TA_ERROR_INVALID;
        return;
    }
    ta_cbor_token token = {kind, (uint32_t)bytes.length, list->strings.length};
    if (list->strings.length + bytes.length <= list->strings.capacity && bytes.length)
        memcpy((char *)list->strings.pointer + list->strings.length, bytes.pointer, bytes.length);
    list->strings.length += bytes.length;
    ta_python_token_add(list, &token);
}

/* Text: valid UTF-8, so a lone surrogate is refused, never replaced
 * (bindings.md, section 9). */
static void ta_python_token_text(ta_python_tokens *list, PyObject *text) {
    Py_ssize_t length = 0;
    const char *utf8 = PyUnicode_AsUTF8AndSize(text, &length);
    if (!utf8) {
        PyErr_Clear();
        list->status = TA_ERROR_INVALID;
        return;
    }
    ta_python_token_string(list, TA_CBOR_TEXT, (ta_bytes){utf8, (size_t)length});
}

/* An int, exact to 64 bits either way: -2^64 … 2^64-1. */
static void ta_python_token_integer(ta_python_tokens *list, PyObject *integer) {
    int overflow = 0;
    long long value = PyLong_AsLongLongAndOverflow(integer, &overflow);
    if (value == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        list->status = TA_ERROR_INVALID;
        return;
    }
    if (!overflow) {
        ta_cbor_token token = value >= 0 ? (ta_cbor_token){TA_CBOR_UINT, 0, (uint64_t)value}
                                         : (ta_cbor_token){TA_CBOR_NEGINT, 0, (uint64_t)(-1 - value)};
        ta_python_token_add(list, &token);
        return;
    }
    /* Past 63 bits: n itself, or ~n (-1 - n) for a negative one. */
    PyObject *magnitude = overflow > 0 ? Py_NewRef(integer) : PyNumber_Invert(integer);
    unsigned long long bits = magnitude ? PyLong_AsUnsignedLongLong(magnitude) : 0;
    Py_XDECREF(magnitude);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        list->status = TA_ERROR_INVALID;
        return;
    }
    ta_cbor_token token = {overflow > 0 ? TA_CBOR_UINT : TA_CBOR_NEGINT, 0, bits};
    ta_python_token_add(list, &token);
}

static void ta_python_token_value(ta_python_tokens *list, PyObject *value, uint32_t depth);

/* A container's items, `count` of them; the count is set by the caller. */
static void ta_python_token_items(ta_python_tokens *list, PyObject *sequence, uint32_t depth) {
    Py_ssize_t count = PySequence_Size(sequence);
    for (Py_ssize_t i = 0; i < count && list->status == TA_OK; i++) {
        PyObject *item = PyList_Check(sequence) ? PyList_GetItemRef(sequence, i) : PySequence_GetItem(sequence, i);
        if (!item) {
            PyErr_Clear();
            list->status = TA_ERROR_INVALID;
            return;
        }
        ta_python_token_value(list, item, depth);
        Py_DECREF(item);
    }
}

static void ta_python_token_map(ta_python_tokens *list, PyObject *map, uint32_t depth) {
    PyObject *items = PyDict_Items(map); /* a snapshot: the dict may change while it is written */
    if (!items) {
        PyErr_Clear();
        list->status = TA_ERROR_INVALID;
        return;
    }
    Py_ssize_t count = PyList_Size(items);
    ta_cbor_token token = {TA_CBOR_MAP, (uint32_t)count, 0};
    ta_python_token_add(list, &token);
    for (Py_ssize_t i = 0; i < count && list->status == TA_OK; i++) {
        PyObject *pair = PyList_GetItemRef(items, i);
        PyObject *key = pair ? PyTuple_GetItem(pair, 0) : NULL;
        if (!key || !PyUnicode_Check(key)) {
            list->status = TA_ERROR_INVALID; /* a map's keys are text */
        } else {
            ta_python_token_text(list, key);
            if (list->status == TA_OK) ta_python_token_value(list, PyTuple_GetItem(pair, 1), depth);
        }
        Py_XDECREF(pair);
    }
    Py_DECREF(items);
}

/* One value and its children. `depth` counts the containers and tags
 * around it: past TA_CBOR_DEPTH_MAX, core's limit, the value is refused,
 * which also ends a cycle (values.txt "invalid cycles"). */
static void ta_python_token_value(ta_python_tokens *list, PyObject *value, uint32_t depth) {
    const ta_python_state *state = list->state;
    if (list->status != TA_OK) return;
    if (depth > TA_CBOR_DEPTH_MAX) {
        list->status = TA_ERROR_INVALID;
        return;
    }
    if (value == Py_None) {
        ta_python_token_add(list, &(ta_cbor_token){TA_CBOR_SIMPLE, 0, TA_CBOR_NULL});
    } else if (value == state->undefined) {
        ta_python_token_add(list, &(ta_cbor_token){TA_CBOR_SIMPLE, 0, TA_CBOR_UNDEFINED});
    } else if (PyBool_Check(value)) { /* before int: bool is a subclass of int */
        ta_python_token_add(list, &(ta_cbor_token){TA_CBOR_SIMPLE, 0, value == Py_True ? TA_CBOR_TRUE : TA_CBOR_FALSE});
    } else if (PyLong_Check(value)) {
        ta_python_token_integer(list, value);
    } else if (PyFloat_Check(value)) {
        double number = PyFloat_AsDouble(value);
        uint64_t bits;
        memcpy(&bits, &number, sizeof bits);
        ta_python_token_add(list, &(ta_cbor_token){TA_CBOR_FLOAT, 0, bits});
    } else if (PyUnicode_Check(value)) {
        ta_python_token_text(list, value);
    } else if (PyBytes_Check(value)) {
        ta_python_token_string(list, TA_CBOR_BYTES, (ta_bytes){PyBytes_AsString(value), (size_t)PyBytes_Size(value)});
    } else if (PyByteArray_Check(value)) {
        ta_python_token_string(list, TA_CBOR_BYTES,
                               (ta_bytes){PyByteArray_AsString(value), (size_t)PyByteArray_Size(value)});
    } else if (PyObject_TypeCheck(value, (PyTypeObject *)state->address_type)) {
        ta_python_token_add(list, &(ta_cbor_token){TA_CBOR_TAG, 0, TA_CBOR_TAG_URI});
        ta_python_token_text(list, ((ta_python_address *)value)->text);
    } else if (PyList_Check(value) || PyTuple_Check(value)) {
        Py_ssize_t count = PySequence_Size(value);
        ta_python_token_add(list, &(ta_cbor_token){TA_CBOR_ARRAY, (uint32_t)count, 0});
        ta_python_token_items(list, value, depth + 1);
    } else if (PyDict_Check(value)) {
        ta_python_token_map(list, value, depth + 1);
    } else {
        list->status = TA_ERROR_INVALID; /* outside the table (docs/values.md) */
    }
}

/* The tokens of `value` into `list`, twice when the first pass did not
 * fit: then with room for what it counted. */
static ta_status ta_python_tokens_of(ta_python_tokens *list, PyObject *value) {
    for (int pass = 0; pass < 2; pass++) {
        list->count = 0;
        list->strings.length = 0;
        list->status = TA_OK;
        ta_python_token_value(list, value, 0);
        if (list->status != TA_OK) return list->status;
        if (list->count <= list->capacity && list->strings.length <= list->strings.capacity) return TA_OK;
        if (pass) break;
        ta_cbor_token *tokens = PyMem_Malloc(list->count * sizeof *tokens);
        void *strings = PyMem_Malloc(list->strings.length ? list->strings.length : 1);
        if (!tokens || !strings) {
            PyMem_Free(tokens);
            PyMem_Free(strings);
            PyErr_NoMemory();
            return TA_ERROR_NO_MEMORY;
        }
        list->tokens = tokens;
        list->capacity = list->count;
        list->strings = (ta_buffer){strings, list->strings.length, 0};
    }
    return TA_ERROR_INVALID; /* the value changed between the passes */
}

ta_status ta_python_values_write(const ta_python_state *state, PyObject *value, ta_buffer *out) {
    ta_cbor_token tokens[64];
    char strings[1024];
    ta_python_tokens list = {state, tokens, 64, 0, {strings, sizeof strings, 0}, TA_OK};
    ta_status status = ta_python_tokens_of(&list, value);
    if (status == TA_OK) {
        ta_cbor_document document = {{list.strings.pointer, list.strings.length}, list.tokens, list.count, list.count};
        status = ta_cbor_encode(&document, out);
    }
    if (list.tokens != tokens) {
        PyMem_Free(list.tokens);
        PyMem_Free(list.strings.pointer);
    }
    return status;
}

ta_status ta_python_values_bytes(const ta_python_state *state, PyObject *value, PyObject **out) {
    char room[1024];
    ta_buffer buffer = {room, sizeof room, 0};
    ta_status status = ta_python_values_write(state, value, &buffer);
    if (status == TA_ERROR_NO_MEMORY && !PyErr_Occurred()) {
        buffer = (ta_buffer){PyMem_Malloc(buffer.length), buffer.length, 0};
        status = buffer.pointer ? ta_python_values_write(state, value, &buffer) : TA_ERROR_NO_MEMORY;
        if (!buffer.pointer) PyErr_NoMemory();
    }
    if (status == TA_OK) *out = PyBytes_FromStringAndSize(buffer.pointer, (Py_ssize_t)buffer.length);
    if (buffer.pointer != room) PyMem_Free(buffer.pointer);
    if (status == TA_OK && !*out) return TA_ERROR_NO_MEMORY;
    return status;
}

/* ---- tokens to Python ------------------------------------------------------------------ */

typedef struct ta_python_reading {
    const ta_python_state *state;
    const ta_cbor_document *document;
    uint32_t at; /* the next token */
    ta_status status;
} ta_python_reading;

static PyObject *ta_python_read_value(ta_python_reading *reading);

static PyObject *ta_python_read_integer(const ta_cbor_token *token) {
    if (token->kind == TA_CBOR_UINT) return PyLong_FromUnsignedLongLong(token->value);
    if (token->value <= INT64_MAX) return PyLong_FromLongLong(-1 - (long long)token->value);
    PyObject *magnitude = PyLong_FromUnsignedLongLong(token->value);
    PyObject *integer = magnitude ? PyNumber_Invert(magnitude) : NULL; /* -1 - n */
    Py_XDECREF(magnitude);
    return integer;
}

static PyObject *ta_python_read_text(ta_python_reading *reading, uint32_t at) {
    ta_string_view text = ta_cbor_string(reading->document, at);
    return PyUnicode_DecodeUTF8(text.pointer, (Py_ssize_t)text.length, "strict");
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
        uint32_t at = reading->at;
        if (reading->document->tokens[at].kind != TA_CBOR_TEXT) {
            reading->status = TA_ERROR_INVALID; /* a key that is not text */
            Py_CLEAR(map);
            break;
        }
        reading->at++;
        PyObject *key = ta_python_read_text(reading, at);
        PyObject *value = key ? ta_python_read_value(reading) : NULL;
        if (!value || PyDict_SetItem(map, key, value) < 0) Py_CLEAR(map);
        Py_XDECREF(key);
        Py_XDECREF(value);
    }
    return map;
}

/* The value at reading->at, and past it. NULL with reading->status set
 * for a value outside the table; with a Python error for Python's. */
static PyObject *ta_python_read_value(ta_python_reading *reading) {
    const ta_cbor_token *token = &reading->document->tokens[reading->at];
    uint32_t at = reading->at++;
    switch (token->kind) {
    case TA_CBOR_UINT:
    case TA_CBOR_NEGINT: return ta_python_read_integer(token);
    case TA_CBOR_TEXT: return ta_python_read_text(reading, at);
    case TA_CBOR_BYTES: {
        ta_string_view bytes = ta_cbor_string(reading->document, at);
        return PyBytes_FromStringAndSize(bytes.pointer, (Py_ssize_t)bytes.length);
    }
    case TA_CBOR_ARRAY: return ta_python_read_array(reading, token->count);
    case TA_CBOR_MAP: return ta_python_read_map(reading, token->count);
    case TA_CBOR_FLOAT: {
        double number;
        memcpy(&number, &token->value, sizeof number);
        return PyFloat_FromDouble(number);
    }
    case TA_CBOR_SIMPLE:
        if (token->value == TA_CBOR_FALSE) Py_RETURN_FALSE;
        if (token->value == TA_CBOR_TRUE) Py_RETURN_TRUE;
        if (token->value == TA_CBOR_NULL) Py_RETURN_NONE;
        if (token->value == TA_CBOR_UNDEFINED) return Py_NewRef(reading->state->undefined);
        break;
    case TA_CBOR_TAG:
        if (token->value == TA_CBOR_TAG_URI && reading->document->tokens[reading->at].kind == TA_CBOR_TEXT)
            return ta_python_address_from(reading->state, ta_cbor_string(reading->document, reading->at++));
        break;
    default: break;
    }
    reading->status = TA_ERROR_INVALID; /* another tag or simple value */
    return NULL;
}

ta_status ta_python_values_read(const ta_python_state *state, ta_bytes cbor, PyObject **out) {
    ta_cbor_token room[256];
    ta_cbor_document document = {cbor, room, 256, 0};
    ta_status status = ta_cbor_decode(&document);
    if (status == TA_ERROR_NO_MEMORY) {
        document.tokens = PyMem_Malloc(document.count * sizeof *document.tokens);
        if (!document.tokens) {
            PyErr_NoMemory();
            return TA_ERROR_NO_MEMORY;
        }
        document.capacity = document.count;
        status = ta_cbor_decode(&document);
    }
    if (status == TA_OK) {
        ta_python_reading reading = {state, &document, 0, TA_OK};
        *out = ta_python_read_value(&reading);
        status = *out ? TA_OK : reading.status != TA_OK ? reading.status : TA_ERROR_NO_MEMORY;
        if (reading.status != TA_OK) PyErr_Clear(); /* the table's refusal, not Python's */
    }
    if (document.tokens != room) PyMem_Free(document.tokens);
    return status;
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
    PyObject *bytes = NULL;
    ta_status status = ta_python_values_bytes(state, value, &bytes);
    if (status != TA_OK && !PyErr_Occurred()) ta_python_raise_status(state, "ta_cbor_encode", status);
    return status == TA_OK ? bytes : NULL;
}

PyObject *ta_python_decode(PyObject *module, PyObject *data) {
    const ta_python_state *state = ta_python_state_of(module);
    Py_buffer view;
    if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) return NULL;
    PyObject *value = NULL;
    ta_status status = ta_python_values_read(state, (ta_bytes){view.buf, (size_t)view.len}, &value);
    PyBuffer_Release(&view);
    if (status != TA_OK && !PyErr_Occurred()) ta_python_raise_status(state, "ta_cbor_decode", status);
    return status == TA_OK ? value : NULL;
}
